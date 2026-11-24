// secure-desktop-stream: NSPasteboard clipboard backend (DESIGN 5.6).
//
// Main thread only. Written without alloc/init so it is correct with and without ARC: every
// Cocoa object comes from a convenience constructor and lives in the local autorelease pool.

#include "clipboard_platform.h"
#include "clipboardsync.h"
#include "fileclipboard.h"
#include "filesanitize.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <sys/acl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#import <AppKit/AppKit.h>

namespace Sds {

namespace {

// Generating a TIFF copy for TIFF-only apps decodes the image; only do it for moderate sizes.
constexpr qint64 kMaxTiffDecodedBytes = 128LL * 1024 * 1024;

// At most four retired 8-MiB stages plus one current stage; native file managers
// can finish an already-started local read for 60 seconds after retirement.
constexpr qint64 kFileReadGraceMs = 60000;
struct FileStage {
    QString path;
    int directory = -1;
    struct stat identity{};
    QList<QPair<QByteArray, struct stat>> entries;
    ~FileStage()
    {
        if (directory < 0) return;
        for (const auto& entry : entries) {
            struct stat current{};
            if (::fstatat(directory, entry.first.constData(), &current, AT_SYMLINK_NOFOLLOW) == 0 &&
                    current.st_dev == entry.second.st_dev && current.st_ino == entry.second.st_ino &&
                    S_ISREG(current.st_mode)) ::unlinkat(directory, entry.first.constData(), 0);
        }
        ::close(directory);
        struct stat current{};
        if (::lstat(QFile::encodeName(path).constData(), &current) == 0 &&
                S_ISDIR(current.st_mode) && current.st_dev == identity.st_dev && current.st_ino == identity.st_ino)
            ::rmdir(QFile::encodeName(path).constData());
    }
};

// Shared across sessions so reconnecting repeatedly cannot accumulate an unbounded
// number of grace-period caches. Accessed only on the main thread.
using StageDeadline = std::chrono::steady_clock::time_point;

QList<QPair<StageDeadline, std::shared_ptr<FileStage>>>& retiredFileStages()
{
    static QList<QPair<StageDeadline, std::shared_ptr<FileStage>>> stages;
    return stages;
}

bool makePrivate(int fd, mode_t mode)
{
    acl_t empty = ::acl_init(0);
    if (empty == nullptr) return false;
    const bool ok = ::acl_set_fd(fd, empty) == 0;
    ::acl_free(empty);
    return ok && ::fchmod(fd, mode) == 0;
}

void reapFileStages()
{
    auto& stages = retiredFileStages();
    const auto now = std::chrono::steady_clock::now();
    for (auto it = stages.begin(); it != stages.end();) {
        if (it->first <= now) {
            it = stages.erase(it);
        }
        else {
            ++it;
        }
    }

    QCoreApplication* app = QCoreApplication::instance();
    if (app == nullptr) {
        return;
    }

    static QPointer<QTimer> timer;
    if (stages.isEmpty()) {
        if (timer) {
            timer->stop();
        }
        return;
    }

    if (!timer) {
        timer = new QTimer(app);
        timer->setSingleShot(true);
        timer->setTimerType(Qt::PreciseTimer);
        QObject::connect(timer, &QTimer::timeout, app, []() { reapFileStages(); });
    }

    StageDeadline earliest = stages.first().first;
    for (const auto& entry : std::as_const(stages)) {
        if (entry.first < earliest) {
            earliest = entry.first;
        }
    }
    const auto remaining = earliest - now;
    const qint64 remainingMs = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    timer->start(static_cast<int>(std::max<qint64>(1, remainingMs)));
}

int openVerifiedFileClipboardRoot(QString* rootPathOut, bool createIfMissing, QString* error)
{
    const QString cacheLoc = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheLoc.isEmpty()) {
        if (error) *error = QStringLiteral("Cannot determine cache location");
        return -1;
    }

    const QString rootPath = cacheLoc + QStringLiteral("/file-clipboard");
    if (createIfMissing) {
        QDir().mkpath(cacheLoc);
        if (::mkdir(QFile::encodeName(rootPath).constData(), 0700) != 0 && errno != EEXIST) {
            if (error) *error = QStringLiteral("Cannot create clipboard root: %1").arg(QString::fromLocal8Bit(strerror(errno)));
            return -1;
        }
    }

    int rootFd = ::open(QFile::encodeName(rootPath).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (rootFd < 0) {
        if (error) *error = QStringLiteral("Cannot open clipboard root: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        return -1;
    }

    struct stat st{};
    if (::fstat(rootFd, &st) != 0 || !S_ISDIR(st.st_mode) ||
            st.st_uid != ::geteuid() || !makePrivate(rootFd, 0700)) {
        if (error) *error = QStringLiteral("Cannot protect clipboard root: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        ::close(rootFd);
        return -1;
    }

    if (rootPathOut != nullptr) {
        const QString canonical = QFileInfo(rootPath).canonicalFilePath();
        *rootPathOut = canonical.isEmpty() ? rootPath : canonical;
    }
    return rootFd;
}

void sweepLeftoverFileStages()
{
    int rootFd = openVerifiedFileClipboardRoot(nullptr, false, nullptr);
    if (rootFd < 0) {
        return;
    }

    int dupFd = ::dup(rootFd);
    DIR* dir = dupFd >= 0 ? ::fdopendir(dupFd) : nullptr;
    if (dir == nullptr) {
        if (dupFd >= 0) ::close(dupFd);
        ::close(rootFd);
        return;
    }

    const auto& activeRetired = retiredFileStages();
    while (struct dirent* ent = ::readdir(dir)) {
        if (std::strncmp(ent->d_name, ".sds-file-clipboard-", 20) != 0) {
            continue;
        }

        struct stat st{};
        if (::fstatat(rootFd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
                !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid()) {
            continue;
        }

        bool inUse = false;
        for (const auto& active : activeRetired) {
            if (active.second && active.second->identity.st_dev == st.st_dev &&
                    active.second->identity.st_ino == st.st_ino) {
                inUse = true;
                break;
            }
        }
        if (inUse) {
            continue;
        }

        int subFd = ::openat(rootFd, ent->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (subFd < 0) {
            continue;
        }
        struct stat subSt{};
        if (::fstat(subFd, &subSt) == 0 && S_ISDIR(subSt.st_mode) &&
                subSt.st_dev == st.st_dev && subSt.st_ino == st.st_ino) {
            int subDup = ::dup(subFd);
            DIR* subDir = subDup >= 0 ? ::fdopendir(subDup) : nullptr;
            if (subDir != nullptr) {
                while (struct dirent* child = ::readdir(subDir)) {
                    if (std::strcmp(child->d_name, ".") == 0 || std::strcmp(child->d_name, "..") == 0) {
                        continue;
                    }
                    struct stat childSt{};
                    if (::fstatat(subFd, child->d_name, &childSt, AT_SYMLINK_NOFOLLOW) == 0 &&
                            !S_ISDIR(childSt.st_mode)) {
                        ::unlinkat(subFd, child->d_name, 0);
                    }
                }
                ::closedir(subDir);
            }
            else if (subDup >= 0) {
                ::close(subDup);
            }
        }
        ::close(subFd);
        ::unlinkat(rootFd, ent->d_name, AT_REMOVEDIR);
    }

    ::closedir(dir);
    ::close(rootFd);
}

bool createPrivateStage(std::shared_ptr<FileStage>& out, QString* error)
{
    reapFileStages();
    if (retiredFileStages().size() >= 4) {
        if (error) *error = QStringLiteral("File clipboard is busy; copy again shortly");
        return false;
    }

    QString rootPath;
    int rootFd = openVerifiedFileClipboardRoot(&rootPath, true, error);
    if (rootFd < 0) {
        return false;
    }

    QTemporaryDir temporary(rootPath + QStringLiteral("/.sds-file-clipboard-XXXXXX"));
    if (!temporary.isValid()) {
        ::close(rootFd);
        if (error) *error = QStringLiteral("Cannot stage clipboard files");
        return false;
    }

    const QString stageName = QFileInfo(temporary.path()).fileName();
    auto stage = std::make_shared<FileStage>();
    stage->path = QDir(rootPath).filePath(stageName);
    stage->directory = ::openat(rootFd, QFile::encodeName(stageName).constData(),
                                O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    ::close(rootFd);
    if (stage->directory < 0 || ::fstat(stage->directory, &stage->identity) != 0 ||
            !S_ISDIR(stage->identity.st_mode) || stage->identity.st_uid != ::geteuid() ||
            !makePrivate(stage->directory, 0700)) {
        if (error) {
            *error = QStringLiteral("Cannot protect clipboard stage: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        }
        return false;
    }

    temporary.setAutoRemove(false);
    out = std::move(stage);
    return true;
}

bool writeVerifiedStageFile(FileStage& stage, const QString& fileName, const QByteArray& bytes, QString* error)
{
    const auto name = QFile::encodeName(fileName);
    int fd = ::openat(stage.directory, name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (error) *error = QStringLiteral("Cannot create clipboard file: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }

    struct stat identity{};
    if (::fstat(fd, &identity) != 0) {
        ::close(fd);
        ::unlinkat(stage.directory, name.constData(), 0);
        return false;
    }
    stage.entries.append({name, identity});

    bool ok = makePrivate(fd, 0600);
    qint64 offset = 0;
    while (ok && offset < bytes.size()) {
        const auto n = ::write(fd, bytes.constData() + offset, size_t(bytes.size() - offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        offset += n;
    }
    ok = ok && setQuarantineAttribute(fd, error) && ::fsync(fd) == 0;
    ::close(fd);
    if (!ok && error && error->isEmpty()) {
        *error = QStringLiteral("Cannot protect or persist clipboard file: %1").arg(QString::fromLocal8Bit(strerror(errno)));
    }
    return ok;
}

void trackExistingStagedPaths(FileStage& stage, const QStringList& stagedPaths)
{
    for (const QString& fullPath : stagedPaths) {
        const QByteArray name = QFile::encodeName(QFileInfo(fullPath).fileName());
        if (name.isEmpty()) continue;
        struct stat st{};
        if (::fstatat(stage.directory, name.constData(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISREG(st.st_mode)) {
            stage.entries.append({name, st});
        }
    }
}

void publishConcreteImages(NSPasteboard* pb, const QByteArray& pngBytes)
{
    NSData* png = [NSData dataWithBytes:pngBytes.constData() length:static_cast<NSUInteger>(pngBytes.size())];
    [pb setData:png forType:NSPasteboardTypePNG];

    quint32 w = 0, h = 0;
    if (validateClipboardPng(pngBytes, pngBytes.size(), &w, &h) == ClipboardCheck::Ok &&
            static_cast<qint64>(w) * h * 4 <= kMaxTiffDecodedBytes) {
        NSBitmapImageRep* rep = [NSBitmapImageRep imageRepWithData:png];
        NSData* tiff = rep != nil ? [rep TIFFRepresentation] : nil;
        if (tiff != nil) {
            [pb setData:tiff forType:NSPasteboardTypeTIFF];
        }
    }
}

bool extractPasteboardFilePaths(NSPasteboard* pb, QStringList* pathsOut, QString* error)
{
    pathsOut->clear();
    NSArray* urls = [pb readObjectsForClasses:@[[NSURL class]]
                                      options:@{NSPasteboardURLReadingFileURLsOnlyKey:@YES}];
    if ([urls count] == 0 || [urls count] > kFileClipboardCountMax) {
        if (error) *error = QStringLiteral("Copy up to 16 regular files, not folders");
        return false;
    }
    QStringList paths;
    for (NSURL* url in urls) {
        if (![url isFileURL] || ([url host] != nil && [[url host] length] != 0 &&
                ![[url host] isEqualToString:@"localhost"])) {
            return false;
        }
        const auto path = QString::fromNSString([url path]);
        if (path.contains(QStringLiteral("/.sds-file-clipboard-"))) {
            return false;
        }
        paths.append(path);
    }
    *pathsOut = std::move(paths);
    return true;
}

struct LazyOfferBinding
{
    QString offerId;
    QString kind;
    Sds::IClipboardBackend::LazyDemandCallback onDemand;
    std::shared_ptr<Sds::LazyTransferResult> transfer;
    std::mutex stateMutex;
    bool demandFired = false;
    bool providerFulfilled = false;
    bool abandoned = false;
};

struct LazyBindingHolder
{
    std::shared_ptr<LazyOfferBinding> binding;
};

} // namespace
} // namespace Sds

@interface SdsLazyPasteboardProvider : NSObject <NSPasteboardItemDataProvider>
{
    void* _holder;
    NSUInteger _entryIndex;
}
- (instancetype)initWithBinding:(const std::shared_ptr<Sds::LazyOfferBinding>&)binding entryIndex:(NSUInteger)index;
- (void)abandon;
@end

@implementation SdsLazyPasteboardProvider

- (instancetype)initWithBinding:(const std::shared_ptr<Sds::LazyOfferBinding>&)binding entryIndex:(NSUInteger)index
{
    self = [super init];
    if (self != nil) {
        _holder = new Sds::LazyBindingHolder{binding};
        _entryIndex = index;
    }
    return self;
}

- (void)dealloc
{
    delete static_cast<Sds::LazyBindingHolder*>(_holder);
    _holder = nullptr;
#if !__has_feature(objc_arc)
    [super dealloc];
#endif
}

- (void)abandon
{
    auto* holder = static_cast<Sds::LazyBindingHolder*>(_holder);
    if (holder == nullptr || !holder->binding) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(holder->binding->stateMutex);
        holder->binding->abandoned = true;
    }
    if (holder->binding->transfer) {
        holder->binding->transfer->cv.notify_all();
    }
}

- (void)pasteboard:(NSPasteboard*)pb item:(NSPasteboardItem*)item provideDataForType:(NSPasteboardType)type
{
    (void)pb;
    auto* holder = static_cast<Sds::LazyBindingHolder*>(_holder);
    if (holder == nullptr || !holder->binding) {
        return;
    }
    const std::shared_ptr<Sds::LazyOfferBinding> b = holder->binding;
    Sds::IClipboardBackend::LazyDemandCallback demandCb;
    QString offerId;
    bool shouldTrigger = false;
    {
        std::lock_guard<std::mutex> lock(b->stateMutex);
        if (b->abandoned) {
            return;
        }
        if (!b->demandFired) {
            b->demandFired = true;
            shouldTrigger = true;
            demandCb = b->onDemand;
            offerId = b->offerId;
        }
    }
    if (shouldTrigger && demandCb) {
        demandCb(offerId);
    }

    if (!b->transfer) {
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4500);
    bool ok = false;
    QStringList stagedPaths;
    QByteArray png;
    for (;;) {
        bool done = false;
        bool abandoned = false;
        {
            std::lock_guard<std::mutex> sLock(b->stateMutex);
            abandoned = b->abandoned;
        }
        if (abandoned) {
            return;
        }
        {
            std::unique_lock<std::mutex> tLock(b->transfer->mutex);
            if (b->transfer->finished) {
                done = true;
                ok = b->transfer->ok;
                stagedPaths = b->transfer->stagedPaths;
                png = b->transfer->png;
            }
            else if (std::chrono::steady_clock::now() < deadline) {
                b->transfer->cv.wait_for(tLock, std::chrono::milliseconds(5));
            }
        }
        if (done || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        @autoreleasepool {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
        }
    }

    if (!ok) {
        return;
    }

    @autoreleasepool {
        if ([type isEqualToString:NSPasteboardTypeFileURL]) {
            if (_entryIndex < static_cast<NSUInteger>(stagedPaths.size())) {
                NSURL* url = [NSURL fileURLWithPath:stagedPaths.at(static_cast<qsizetype>(_entryIndex)).toNSString()];
                if (url != nil) {
                    {
                        std::lock_guard<std::mutex> lock(b->stateMutex);
                        b->providerFulfilled = true;
                    }
                    [item setData:[url dataRepresentation] forType:NSPasteboardTypeFileURL];
                }
            }
        }
        else if ([type isEqualToString:NSPasteboardTypePNG]) {
            if (png.isEmpty() && !stagedPaths.isEmpty()) {
                QFile f(stagedPaths.first());
                if (f.open(QIODevice::ReadOnly)) {
                    png = f.readAll();
                }
            }
            if (!png.isEmpty()) {
                {
                    std::lock_guard<std::mutex> lock(b->stateMutex);
                    b->providerFulfilled = true;
                }
                NSData* data = [NSData dataWithBytes:png.constData() length:static_cast<NSUInteger>(png.size())];
                [item setData:data forType:NSPasteboardTypePNG];
            }
        }
    }
}

@end

namespace Sds {
namespace {

class MacClipboard : public IClipboardBackend
{
public:
    explicit MacClipboard(const QString& privateName) :
        m_PrivateName(privateName)
    {
        sweepLeftoverFileStages();
    }

    ~MacClipboard() override
    {
        retireFiles();
        if (!m_PrivateName.isEmpty()) {
            @autoreleasepool {
                [pasteboard() releaseGlobally];
            }
        }
    }

    bool isAvailable() const override
    {
        return true;
    }

    qint64 changeCount() override
    {
        @autoreleasepool {
            return static_cast<qint64>([pasteboard() changeCount]);
        }
    }

    ReadResult read(qint64 textMax, qint64 imageMax) override
    {
        ReadResult result;

        @autoreleasepool {
            NSPasteboard* pb = pasteboard();
            if (m_OwnedLazyChangeCount >= 0 && static_cast<qint64>([pb changeCount]) == m_OwnedLazyChangeCount) {
                return result;
            }
            if ([[pb types] containsObject:NSPasteboardTypeFileURL] ||
                    [[pb types] containsObject:NSFilenamesPboardType]) return result;

            NSString* str = [pb stringForType:NSPasteboardTypeString];
            if (str != nil) {
                NSData* utf8 = [str dataUsingEncoding:NSUTF8StringEncoding];
                if (utf8 != nil && [utf8 length] > 0) {
                    if (static_cast<qint64>([utf8 length]) > textMax) {
                        result.textTooLarge = true;
                    }
                    else {
                        result.item.text = QByteArray(static_cast<const char*>([utf8 bytes]),
                                                      static_cast<qsizetype>([utf8 length]));
                    }
                }
            }

            NSData* png = [pb dataForType:NSPasteboardTypePNG];
            if (png == nil) {
                NSData* tiff = [pb dataForType:NSPasteboardTypeTIFF];
                if (tiff != nil) {
                    NSBitmapImageRep* rep = [NSBitmapImageRep imageRepWithData:tiff];
                    if (rep != nil) {
                        qint64 decoded = static_cast<qint64>([rep pixelsWide]) * static_cast<qint64>([rep pixelsHigh]) * 4;
                        if (decoded <= 0 || decoded > kPngMaxDecodedBytes) {
                            result.imageTooLarge = true;
                        }
                        else {
                            png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                        }
                    }
                }
            }

            if (png != nil && [png length] > 0) {
                if (static_cast<qint64>([png length]) > imageMax) {
                    result.imageTooLarge = true;
                }
                else {
                    result.item.png = QByteArray(static_cast<const char*>([png bytes]),
                                                 static_cast<qsizetype>([png length]));
                }
            }
        }

        return result;
    }

    qint64 write(const ClipboardItem& item) override
    {
        retireFiles();
        @autoreleasepool {
            NSPasteboard* pb = pasteboard();
            [pb clearContents];

            if (item.hasText()) {
                // Validated by the caller: UTF-8 without NUL, so the C string is complete
                NSString* str = [NSString stringWithUTF8String:item.text.constData()];
                if (str != nil) {
                    [pb setString:str forType:NSPasteboardTypeString];
                }
            }

            if (item.hasImage()) {
                publishConcreteImages(pb, item.png);
            }

            return static_cast<qint64>([pb changeCount]);
        }
    }

    bool hasFileSelection() override
    {
        reapStages();
        @autoreleasepool {
            NSPasteboard* pb = pasteboard();
            if (m_OwnedLazyChangeCount >= 0 && static_cast<qint64>([pb changeCount]) == m_OwnedLazyChangeCount) {
                return false;
            }
            return [[pb types] containsObject:NSPasteboardTypeFileURL] ||
                    [[pb types] containsObject:NSFilenamesPboardType];
        }
    }

    QByteArray readFiles(QString* error) override
    {
        @autoreleasepool {
            NSPasteboard* pb = pasteboard();
            if (m_OwnedLazyChangeCount >= 0 && static_cast<qint64>([pb changeCount]) == m_OwnedLazyChangeCount) {
                return {};
            }
            QStringList paths;
            if (!extractPasteboardFilePaths(pb, &paths, error)) {
                return {};
            }
            return snapshotClipboardFiles(paths, error);
        }
    }

    bool readFileEntries(QList<ClipboardEntryMeta>* entriesOut, QString* error) override
    {
        entriesOut->clear();
        @autoreleasepool {
            NSPasteboard* pb = pasteboard();
            if (m_OwnedLazyChangeCount >= 0 && static_cast<qint64>([pb changeCount]) == m_OwnedLazyChangeCount) {
                return false;
            }
            QStringList paths;
            if (!extractPasteboardFilePaths(pb, &paths, error)) {
                return false;
            }
            return inspectClipboardFiles(paths, kClientFileMax, entriesOut, error);
        }
    }

    bool readLazyImage(qint64 minBytes, qint64 maxBytes, QString* tempPathOut,
                       qint64* sizeOut, QString* error) override
    {
        tempPathOut->clear();
        *sizeOut = 0;
        @autoreleasepool {
            NSPasteboard* pb = pasteboard();
            if (m_OwnedLazyChangeCount >= 0 && static_cast<qint64>([pb changeCount]) == m_OwnedLazyChangeCount) {
                return false;
            }
            if ([[pb types] containsObject:NSPasteboardTypeFileURL] ||
                    [[pb types] containsObject:NSFilenamesPboardType]) {
                return false;
            }

            NSData* png = [pb dataForType:NSPasteboardTypePNG];
            if (png == nil) {
                NSData* tiff = [pb dataForType:NSPasteboardTypeTIFF];
                if (tiff != nil) {
                    NSBitmapImageRep* rep = [NSBitmapImageRep imageRepWithData:tiff];
                    if (rep != nil) {
                        qint64 decoded = static_cast<qint64>([rep pixelsWide]) * static_cast<qint64>([rep pixelsHigh]) * 4;
                        if (decoded > 0 && decoded <= kPngMaxDecodedBytes) {
                            png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
                        }
                    }
                }
            }
            if (png == nil || [png length] == 0) {
                return false;
            }
            const qint64 len = static_cast<qint64>([png length]);
            if (len <= minBytes) {
                return false;
            }
            if (len > maxBytes) {
                if (error) *error = QStringLiteral("Clipboard image is too large");
                return false;
            }

            QByteArray bytes(static_cast<const char*>([png bytes]), static_cast<qsizetype>(len));
            std::shared_ptr<FileStage> stage;
            if (!createPrivateStage(stage, error)) {
                return false;
            }
            if (!writeVerifiedStageFile(*stage, QStringLiteral("clipboard.png"), bytes, error)) {
                return false;
            }
            if (m_LocalImageStage) {
                retiredFileStages().append({std::chrono::steady_clock::now() + std::chrono::milliseconds(kFileReadGraceMs), m_LocalImageStage});
            }
            m_LocalImageStage = stage;
            *tempPathOut = QDir(stage->path).filePath(QStringLiteral("clipboard.png"));
            *sizeOut = len;
            return true;
        }
    }

    QString createLazyStageDir(QString* error) override
    {
        std::shared_ptr<FileStage> stage;
        if (!createPrivateStage(stage, error)) {
            return {};
        }
        m_PendingStages.insert(stage->path, stage);
        return stage->path;
    }

    void retireStageDir(const QString& stageDir) override
    {
        auto stage = m_PendingStages.take(stageDir);
        if (stage) {
            retiredFileStages().append({std::chrono::steady_clock::now() + std::chrono::milliseconds(kFileReadGraceMs), stage});
            reapStages();
        }
    }

    qint64 writeLazyOffer(const LazyClipboardOffer& offer,
                          std::shared_ptr<LazyTransferResult> transfer,
                          LazyDemandCallback onDemand,
                          QString* error) override
    {
        if (offer.offerId.isEmpty() || offer.entries.isEmpty()) {
            if (error) *error = QStringLiteral("Empty clipboard offer");
            return -1;
        }
        reapStages();
        retireOwnedPasteboard(false);

        auto binding = std::make_shared<LazyOfferBinding>();
        binding->offerId = offer.offerId;
        binding->kind = offer.kind;
        binding->onDemand = std::move(onDemand);
        binding->transfer = std::move(transfer);

        @autoreleasepool {
            NSMutableArray* items = [NSMutableArray array];
            if (offer.kind == QLatin1String("files")) {
                for (qsizetype i = 0; i < offer.entries.size(); ++i) {
                    SdsLazyPasteboardProvider* provider =
                            [[SdsLazyPasteboardProvider alloc] initWithBinding:binding
                                                                    entryIndex:static_cast<NSUInteger>(i)];
#if !__has_feature(objc_arc)
                    [provider retain];
#endif
                    m_LazyProviders.push_back(provider);
                    NSPasteboardItem* item = [[[NSPasteboardItem alloc] init] autorelease];
                    [item setDataProvider:provider forTypes:@[NSPasteboardTypeFileURL]];
                    [items addObject:item];
                }
            }
            else if (offer.kind == QLatin1String("image")) {
                SdsLazyPasteboardProvider* provider =
                        [[SdsLazyPasteboardProvider alloc] initWithBinding:binding entryIndex:0];
#if !__has_feature(objc_arc)
                [provider retain];
#endif
                m_LazyProviders.push_back(provider);
                NSPasteboardItem* item = [[[NSPasteboardItem alloc] init] autorelease];
                [item setDataProvider:provider forTypes:@[NSPasteboardTypePNG]];
                [items addObject:item];
            }
            else {
                if (error) *error = QStringLiteral("Unsupported lazy clipboard kind");
                return -1;
            }

            NSPasteboard* pb = pasteboard();
            [pb clearContents];
            if (![pb writeObjects:items]) {
                releaseLazyProviders();
                if (error) *error = QStringLiteral("Cannot publish lazy clipboard offer");
                return -1;
            }
            m_ActiveLazyBinding = binding;
            m_OwnedLazyChangeCount = static_cast<qint64>([pb changeCount]);
            if (error) error->clear();
            return m_OwnedLazyChangeCount;
        }
    }

    bool completeLazyOffer(const QString& offerId,
                           bool ok,
                           const QStringList& stagedPaths,
                           const QByteArray& png,
                           const QString& error,
                           qint64* newChangeCountOut,
                           bool* needRepasteToastOut) override
    {
        if (newChangeCountOut) *newChangeCountOut = -1;
        if (needRepasteToastOut) *needRepasteToastOut = false;

        std::shared_ptr<FileStage> adoptedStage;
        if (!stagedPaths.isEmpty()) {
            const QString stageDir = QFileInfo(stagedPaths.first()).absolutePath();
            adoptedStage = m_PendingStages.take(stageDir);
            if (adoptedStage) {
                trackExistingStagedPaths(*adoptedStage, stagedPaths);
            }
        }

        if (!m_ActiveLazyBinding || m_ActiveLazyBinding->offerId != offerId) {
            if (adoptedStage) {
                retiredFileStages().append({std::chrono::steady_clock::now() + std::chrono::milliseconds(kFileReadGraceMs), adoptedStage});
                reapStages();
            }
            return false;
        }

        bool demandFired = false;
        bool providerFulfilled = false;
        {
            std::lock_guard<std::mutex> lock(m_ActiveLazyBinding->stateMutex);
            demandFired = m_ActiveLazyBinding->demandFired;
            providerFulfilled = m_ActiveLazyBinding->providerFulfilled;
        }

        if (adoptedStage) {
            if (m_Stage) {
                retiredFileStages().append({std::chrono::steady_clock::now() + std::chrono::milliseconds(kFileReadGraceMs), m_Stage});
            }
            m_Stage = adoptedStage;
        }

        if (providerFulfilled) {
            return true;
        }

        @autoreleasepool {
            NSPasteboard* pb = pasteboard();
            if (static_cast<qint64>([pb changeCount]) != m_OwnedLazyChangeCount) {
                return false;
            }

            if (!ok) {
                Q_UNUSED(error);
                [pb clearContents];
                m_ActiveLazyBinding.reset();
                m_OwnedLazyChangeCount = -1;
                releaseLazyProviders();
                return false;
            }

            if (demandFired) {
                if (m_ActiveLazyBinding->kind == QLatin1String("files") && !stagedPaths.isEmpty()) {
                    NSMutableArray* urls = [NSMutableArray array];
                    for (const QString& path : stagedPaths) {
                        [urls addObject:[NSURL fileURLWithPath:path.toNSString()]];
                    }
                    [pb clearContents];
                    if ([pb writeObjects:urls]) {
                        const qint64 cc = static_cast<qint64>([pb changeCount]);
                        m_OwnedLazyChangeCount = -1;
                        m_FileWriteGeneration = cc;
                        m_ActiveLazyBinding.reset();
                        releaseLazyProviders();
                        if (newChangeCountOut) *newChangeCountOut = cc;
                        if (needRepasteToastOut) *needRepasteToastOut = true;
                        return true;
                    }
                }
                else if (m_ActiveLazyBinding->kind == QLatin1String("image")) {
                    QByteArray img = png;
                    if (img.isEmpty() && !stagedPaths.isEmpty()) {
                        QFile f(stagedPaths.first());
                        if (f.open(QIODevice::ReadOnly)) img = f.readAll();
                    }
                    if (!img.isEmpty()) {
                        [pb clearContents];
                        publishConcreteImages(pb, img);
                        const qint64 cc = static_cast<qint64>([pb changeCount]);
                        m_OwnedLazyChangeCount = -1;
                        m_ActiveLazyBinding.reset();
                        releaseLazyProviders();
                        if (newChangeCountOut) *newChangeCountOut = cc;
                        if (needRepasteToastOut) *needRepasteToastOut = true;
                        return true;
                    }
                }
            }
        }
        return true;
    }

    qint64 writeFiles(const QByteArray& bundle, qint64 expectedGeneration, QString* error) override
    {
        *error = QStringLiteral("Cannot create a private clipboard stage");
        reapStages();
        QList<ClipboardFile> files;
        if (!decodeFileClipboard(bundle, &files)) {
            *error = QStringLiteral("Rejected invalid file clipboard");
            return -1;
        }
        std::shared_ptr<FileStage> stage;
        if (!createPrivateStage(stage, error)) {
            return -1;
        }
        @autoreleasepool {
            NSMutableArray* urls = [NSMutableArray array];
            for (const auto& file : files) {
                if (!writeVerifiedStageFile(*stage, file.name, file.data, error)) {
                    return -1;
                }
                [urls addObject:[NSURL fileURLWithPath:QDir(stage->path).filePath(file.name).toNSString()]];
            }
            NSPasteboard* pb = pasteboard();
            // File staging can take time; preserve a Copy made by another app during it.
            if (static_cast<qint64>([pb changeCount]) != expectedGeneration) {
                *error = QStringLiteral("Local clipboard changed; copied files were preserved");
                return -1;
            }
            retireFiles();
            [pb clearContents];
            if (![pb writeObjects:urls]) { *error = QStringLiteral("Cannot publish file clipboard"); return -1; }
            m_Stage = stage;
            m_FileWriteGeneration = static_cast<qint64>([pb changeCount]);
            error->clear();
            return m_FileWriteGeneration;
        }
    }

    void retireFiles() override
    {
        retireOwnedPasteboard(true);
    }

private:
    void retireOwnedPasteboard(bool retirePendingStages)
    {
        if (m_OwnedLazyChangeCount >= 0 || m_ActiveLazyBinding) {
            @autoreleasepool {
                NSPasteboard* pb = pasteboard();
                if (m_OwnedLazyChangeCount >= 0 && static_cast<qint64>([pb changeCount]) == m_OwnedLazyChangeCount) {
                    [pb clearContents];
                }
            }
            m_ActiveLazyBinding.reset();
            m_OwnedLazyChangeCount = -1;
            releaseLazyProviders();
        }
        if (m_FileWriteGeneration >= 0) {
            @autoreleasepool {
                NSPasteboard* pb = pasteboard();
                if (static_cast<qint64>([pb changeCount]) == m_FileWriteGeneration) {
                    [pb clearContents];
                }
            }
            m_FileWriteGeneration = -1;
        }
        if (m_Stage) {
            retiredFileStages().append({std::chrono::steady_clock::now() + std::chrono::milliseconds(kFileReadGraceMs), m_Stage});
            m_Stage.reset();
        }
        if (m_LocalImageStage) {
            retiredFileStages().append({std::chrono::steady_clock::now() + std::chrono::milliseconds(kFileReadGraceMs), m_LocalImageStage});
            m_LocalImageStage.reset();
        }
        if (retirePendingStages) {
            for (const auto& pending : std::as_const(m_PendingStages)) {
                if (pending) {
                    retiredFileStages().append({std::chrono::steady_clock::now() + std::chrono::milliseconds(kFileReadGraceMs), pending});
                }
            }
            m_PendingStages.clear();
        }
        reapStages();
    }
    void releaseLazyProviders()
    {
        for (SdsLazyPasteboardProvider* provider : m_LazyProviders) {
            [provider abandon];
#if !__has_feature(objc_arc)
            [provider release];
#endif
        }
        m_LazyProviders.clear();
    }

    void reapStages()
    {
        reapFileStages();
    }
    // Must be called inside an autorelease pool
    NSPasteboard* pasteboard() const
    {
        if (m_PrivateName.isEmpty()) {
            return [NSPasteboard generalPasteboard];
        }
        return [NSPasteboard pasteboardWithName:m_PrivateName.toNSString()];
    }

    QString m_PrivateName;
    std::shared_ptr<FileStage> m_Stage;
    std::shared_ptr<FileStage> m_LocalImageStage;
    QHash<QString, std::shared_ptr<FileStage>> m_PendingStages;
    qint64 m_FileWriteGeneration = -1;
    qint64 m_OwnedLazyChangeCount = -1;
    std::shared_ptr<LazyOfferBinding> m_ActiveLazyBinding;
    std::vector<SdsLazyPasteboardProvider*> m_LazyProviders;
};

} // namespace

std::unique_ptr<IClipboardBackend> createPlatformClipboard(const QString& privatePasteboardName)
{
    return std::make_unique<MacClipboard>(privatePasteboardName);
}

} // namespace Sds
