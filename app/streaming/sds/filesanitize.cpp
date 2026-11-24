#include "filesanitize.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QVector>

#include <cerrno>
#include <cstring>

#ifdef Q_OS_UNIX
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

#ifdef Q_OS_DARWIN
#include <membership.h>
#include <stdio.h> // renameatx_np
#include <sys/acl.h>
#include <sys/xattr.h>
#endif

namespace Sds {

namespace {

// Mode bits do not reflect macOS extended ACL grants. Check the opened object,
// including inherited entries, before trusting its contents or its directory.
bool hasPrivateMutationAcl(int fd)
{
#ifdef Q_OS_DARWIN
    acl_t acl = ::acl_get_fd(fd);
    // Darwin reports ENOENT when the object has no extended ACL.
    if (acl == nullptr) return errno == ENOENT;
    if (::acl_valid(acl) != 0) { ::acl_free(acl); return false; }
    acl_entry_t entry;
    int result = ::acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
    bool safe = true;
    // Darwin returns 0 for an entry and -1/EINVAL at the end, unlike POSIX.
    while (result == 0 && safe) {
        acl_tag_t tag;
        acl_permset_t permissions;
        if (::acl_get_tag_type(entry, &tag) != 0 || ::acl_get_permset(entry, &permissions) != 0) {
            safe = false;
            break;
        }
        if (tag == ACL_EXTENDED_ALLOW) {
            bool mutates = false;
            for (acl_perm_t permission : {ACL_WRITE_DATA, ACL_APPEND_DATA, ACL_DELETE_CHILD, ACL_DELETE,
                    ACL_WRITE_ATTRIBUTES, ACL_WRITE_EXTATTRIBUTES, ACL_WRITE_SECURITY, ACL_CHANGE_OWNER}) {
                int allowed = ::acl_get_perm_np(permissions, permission);
                if (allowed < 0) { safe = false; break; }
                mutates = mutates || allowed == 1;
            }
            if (mutates && safe) {
                uuid_t owner;
                void* principal = ::acl_get_qualifier(entry);
                safe = principal != nullptr && ::mbr_uid_to_uuid(::geteuid(), owner) == 0 &&
                        std::memcmp(principal, owner, sizeof(owner)) == 0;
                if (principal != nullptr) ::acl_free(principal);
            }
        }
        else if (tag != ACL_EXTENDED_DENY) {
            safe = false;
        }
        result = ::acl_get_entry(acl, ACL_NEXT_ENTRY, &entry);
    }
    const int endError = errno;
    ::acl_free(acl);
    return safe && result == -1 && endError == EINVAL;
#else
    Q_UNUSED(fd);
    return true;
#endif
}

bool isForbiddenCodePoint(char32_t c)
{
    // C0 controls (incl. NUL) and DEL
    if (c < 0x20 || c == 0x7F) {
        return true;
    }
    // C1 controls
    if (c >= 0x80 && c <= 0x9F) {
        return true;
    }
    switch (c) {
    case U'/':
    case U'\\':
    case U':':
    case U'*':
    case U'?':
    case U'"':
    case U'<':
    case U'>':
    case U'|':
        return true;
    default:
        break;
    }
    // Bidirectional formatting characters can disguise an extension
    // ("photo<U+202E>gpj.exe" displays as "photoexe.jpg").
    if (c == 0x061C || c == 0x200E || c == 0x200F ||
            (c >= 0x202A && c <= 0x202E) ||
            (c >= 0x2066 && c <= 0x2069)) {
        return true;
    }
    return false;
}

// Decodes UTF-16 into code points; returns false on unpaired surrogates.
bool toCodePoints(const QString& s, QVector<char32_t>& out)
{
    out.clear();
    out.reserve(s.size());
    for (qsizetype i = 0; i < s.size(); i++) {
        char16_t u = s.at(i).unicode();
        if (QChar::isHighSurrogate(u)) {
            if (i + 1 >= s.size() || !QChar::isLowSurrogate(s.at(i + 1).unicode())) {
                return false;
            }
            out.append(QChar::surrogateToUcs4(u, s.at(i + 1).unicode()));
            i++;
        }
        else if (QChar::isLowSurrogate(u)) {
            return false;
        }
        else {
            out.append(u);
        }
    }
    return true;
}

int utf8Length(char32_t c)
{
    if (c < 0x80) return 1;
    if (c < 0x800) return 2;
    if (c < 0x10000) return 3;
    return 4;
}

int utf8Length(const QVector<char32_t>& cps)
{
    int len = 0;
    for (char32_t c : cps) {
        len += utf8Length(c);
    }
    return len;
}

QString fromCodePoints(const QVector<char32_t>& cps)
{
    return QString::fromUcs4(cps.constData(), cps.size());
}

void stripTrailingDotsAndSpaces(QVector<char32_t>& cps)
{
    while (!cps.isEmpty() && (cps.last() == U'.' || cps.last() == U' ')) {
        cps.removeLast();
    }
}

bool isReservedDeviceName(const QVector<char32_t>& cps)
{
    // The part before the first dot, with trailing spaces ignored, compared
    // case-insensitively ("con.txt", "Aux .log", "LPT1").
    QVector<char32_t> stem;
    for (char32_t c : cps) {
        if (c == U'.') {
            break;
        }
        stem.append(c);
    }
    while (!stem.isEmpty() && stem.last() == U' ') {
        stem.removeLast();
    }

    QString upper = fromCodePoints(stem).toUpper();
    static const char* const kNames[] = { "CON", "PRN", "AUX", "NUL" };
    for (const char* n : kNames) {
        if (upper == QLatin1String(n)) {
            return true;
        }
    }
    if (upper.size() == 4 && (upper.startsWith(QLatin1String("COM")) || upper.startsWith(QLatin1String("LPT")))) {
        QChar d = upper.at(3);
        // COM1-9 / LPT1-9, plus the superscript digits Windows also treats as reserved
        if ((d >= QLatin1Char('1') && d <= QLatin1Char('9')) ||
                d == QChar(0x00B9) || d == QChar(0x00B2) || d == QChar(0x00B3)) {
            return true;
        }
    }
    return false;
}

void truncateToBytes(QVector<char32_t>& cps, int maxBytes)
{
    if (utf8Length(cps) <= maxBytes) {
        return;
    }

    // Keep a short extension if there is one (split at the last dot, not at index 0)
    int lastDot = -1;
    for (int i = cps.size() - 1; i > 0; i--) {
        if (cps[i] == U'.') {
            lastDot = i;
            break;
        }
    }

    QVector<char32_t> ext;
    QVector<char32_t> stem = cps;
    if (lastDot > 0) {
        ext = cps.mid(lastDot);
        if (utf8Length(ext) <= kMaxPreservedExtensionBytes) {
            stem = cps.mid(0, lastDot);
        }
        else {
            ext.clear();
        }
    }

    int extBytes = utf8Length(ext);
    int stemBytes = utf8Length(stem);
    while (!stem.isEmpty() && stemBytes + extBytes > maxBytes) {
        stemBytes -= utf8Length(stem.last());
        stem.removeLast();
    }

    cps = stem + ext;
}

} // namespace

bool isValidUtf8(const QByteArray& bytes)
{
    const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.constData());
    const qsizetype n = bytes.size();
    qsizetype i = 0;

    while (i < n) {
        unsigned char c = p[i];
        if (c < 0x80) {
            i++;
            continue;
        }

        int extra;
        char32_t cp;
        char32_t minCp;
        if (c >= 0xC2 && c <= 0xDF) {
            extra = 1; cp = c & 0x1F; minCp = 0x80;
        }
        else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2; cp = c & 0x0F; minCp = 0x800;
        }
        else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3; cp = c & 0x07; minCp = 0x10000;
        }
        else {
            // 0x80-0xC1 (continuation or overlong lead) and 0xF5-0xFF
            return false;
        }

        // The continuation bytes p[i+1] .. p[i+extra] must exist
        if (i + extra >= n) {
            return false;
        }

        for (int k = 1; k <= extra; k++) {
            unsigned char cc = p[i + k];
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }

        if (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }

        i += extra + 1;
    }

    return true;
}

QString sanitizeFileName(const QString& name)
{
    if (name.isEmpty()) {
        return QString();
    }

    QVector<char32_t> cps;
    if (!toCodePoints(name, cps)) {
        // Unpaired surrogates cannot be represented in UTF-8
        return QString();
    }

    // macOS hands out NFD names; both sides store and compare NFC
    QString nfc = fromCodePoints(cps).normalized(QString::NormalizationForm_C);
    if (!toCodePoints(nfc, cps)) {
        return QString();
    }

    if (nfc == QLatin1String(".") || nfc == QLatin1String("..")) {
        return QString();
    }

    for (char32_t& c : cps) {
        if (isForbiddenCodePoint(c)) {
            c = U'_';
        }
    }

    // No hidden files, no option-like names, and no Windows reserved device names.
    // Re-apply after truncation because dropping characters before an extension or at
    // the end can expose a leading dot/dash or a reserved stem.
    for (;;) {
        stripTrailingDotsAndSpaces(cps);
        if (cps.isEmpty()) {
            return QString();
        }

        if (cps.first() == U'.' || cps.first() == U'-') {
            cps.first() = U'_';
        }

        if (isReservedDeviceName(cps)) {
            cps.prepend(U'_');
        }

        if (utf8Length(cps) <= kMaxNameBytes) {
            break;
        }
        truncateToBytes(cps, kMaxNameBytes);
    }

    return fromCodePoints(cps);
}

QString sanitizeFileNameUtf8(const QByteArray& utf8)
{
    if (!isValidUtf8(utf8)) {
        return QString();
    }
    return sanitizeFileName(QString::fromUtf8(utf8));
}

QString collisionName(const QString& sanitizedName, int n)
{
    if (n <= 0) {
        return sanitizedName;
    }

    QVector<char32_t> cps;
    if (!toCodePoints(sanitizedName, cps) || cps.isEmpty()) {
        return QString();
    }

    int lastDot = -1;
    for (int i = cps.size() - 1; i > 0; i--) {
        if (cps[i] == U'.') {
            lastDot = i;
            break;
        }
    }

    QVector<char32_t> stem = lastDot > 0 ? cps.mid(0, lastDot) : cps;
    QVector<char32_t> ext = lastDot > 0 ? cps.mid(lastDot) : QVector<char32_t>();
    if (utf8Length(ext) > kMaxPreservedExtensionBytes) {
        stem = cps;
        ext.clear();
    }

    QString suffixStr = QStringLiteral(" (%1)").arg(n);
    QVector<char32_t> suffix;
    toCodePoints(suffixStr, suffix);

    int fixedBytes = utf8Length(suffix) + utf8Length(ext);
    int stemBytes = utf8Length(stem);
    while (!stem.isEmpty() && stemBytes + fixedBytes > kMaxNameBytes) {
        stemBytes -= utf8Length(stem.last());
        stem.removeLast();
    }
    if (stem.isEmpty()) {
        return QString();
    }

    return fromCodePoints(stem + suffix + ext);
}

QByteArray quarantineValue(qint64 unixTime, const QUuid& eventId)
{
    // Flags 0x0081: downloaded file, checked by Gatekeeper before first launch/open.
    return QByteArray("0081;") +
            QByteArray::number(static_cast<qulonglong>(unixTime), 16).rightJustified(8, '0') +
            QByteArray(";Moonlight;") +
            eventId.toString(QUuid::WithoutBraces).toUpper().toLatin1();
}

bool setQuarantineAttribute(int fd, QString* error)
{
#ifdef Q_OS_DARWIN
    QByteArray value = quarantineValue(static_cast<qint64>(::time(nullptr)), QUuid::createUuid());
    if (fsetxattr(fd, "com.apple.quarantine", value.constData(), static_cast<size_t>(value.size()), 0, 0) != 0) {
        if (error) {
            *error = QStringLiteral("cannot set quarantine attribute: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        }
        return false;
    }
    return true;
#else
    Q_UNUSED(fd);
    Q_UNUSED(error);
    return true;
#endif
}

#ifdef Q_OS_UNIX

DownloadDir::DownloadDir(const QString& path) :
    m_Path(QDir::cleanPath(path)),
    m_DirFd(-1)
{
}

DownloadDir::~DownloadDir()
{
    if (m_DirFd >= 0) {
        ::close(m_DirFd);
    }
}

bool DownloadDir::open(QString* error)
{
    if (m_DirFd >= 0) {
        return true;
    }

    QFileInfo info(m_Path);
    if (!info.isAbsolute()) {
        if (error) *error = QStringLiteral("download folder must be an absolute path");
        return false;
    }

    // The parent (normally ~/Downloads) may legitimately be a symlink; the folder itself
    // may not.
    QDir().mkpath(info.absolutePath());

    QByteArray p = QFile::encodeName(m_Path);
    if (::mkdir(p.constData(), 0700) != 0 && errno != EEXIST) {
        if (error) *error = QStringLiteral("cannot create %1: %2").arg(m_Path, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }

    int fd = ::open(p.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (error) {
            *error = (errno == ELOOP || errno == ENOTDIR) ?
                        QStringLiteral("%1 is not a real directory").arg(m_Path) :
                        QStringLiteral("cannot open %1: %2").arg(m_Path, QString::fromLocal8Bit(strerror(errno)));
        }
        return false;
    }

    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() ||
            (st.st_mode & 0022) != 0 || !hasPrivateMutationAcl(fd)) {
        if (error) *error = QStringLiteral("%1 must be owned by the current user and not writable by other users").arg(m_Path);
        ::close(fd);
        return false;
    }

    m_DirFd = fd;

    // Remove part files left behind by a crash more than a day ago
    int dupFd = ::dup(m_DirFd);
    if (dupFd >= 0) {
        DIR* dir = ::fdopendir(dupFd);
        if (dir != nullptr) {
            static const QRegularExpression kPartRe(QStringLiteral("^\\.[0-9a-f]{32}\\.part$"));
            time_t now = ::time(nullptr);
            struct dirent* ent;
            while ((ent = ::readdir(dir)) != nullptr) {
                QString n = QFile::decodeName(ent->d_name);
                if (!kPartRe.match(n).hasMatch()) {
                    continue;
                }
                struct stat pst;
                if (::fstatat(m_DirFd, ent->d_name, &pst, AT_SYMLINK_NOFOLLOW) == 0 &&
                        S_ISREG(pst.st_mode) && pst.st_uid == ::geteuid() &&
                        now - pst.st_mtime > 24 * 60 * 60) {
                    ::unlinkat(m_DirFd, ent->d_name, 0);
                }
            }
            ::closedir(dir);
        }
        else {
            ::close(dupFd);
        }
    }

    return true;
}

int DownloadDir::createPartFile(QString* partName, QString* error)
{
    if (m_DirFd < 0) {
        if (error) *error = QStringLiteral("download folder is not open");
        return -1;
    }
    if (!hasPrivateMutationAcl(m_DirFd)) {
        if (error) *error = QStringLiteral("download directory ACL permits another user to change files");
        return -1;
    }

    for (int attempt = 0; attempt < 8; attempt++) {
        quint64 r[2];
        QRandomGenerator::system()->fillRange(r);
        QString name = QStringLiteral(".%1%2.part")
                .arg(r[0], 16, 16, QLatin1Char('0'))
                .arg(r[1], 16, 16, QLatin1Char('0'));
        QByteArray n = QFile::encodeName(name);

        int fd = ::openat(m_DirFd, n.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) {
            if (!hasPrivateMutationAcl(fd)) {
                if (error) *error = QStringLiteral("part file inherited an unsafe ACL");
                ::close(fd);
                ::unlinkat(m_DirFd, n.constData(), 0);
                return -1;
            }
            *partName = name;
            return fd;
        }
        if (errno != EEXIST) {
            if (error) *error = QStringLiteral("cannot create part file: %1").arg(QString::fromLocal8Bit(strerror(errno)));
            return -1;
        }
    }

    if (error) *error = QStringLiteral("cannot create a unique part file");
    return -1;
}

int DownloadDir::renameNoReplace(const QString& from, const QString& to)
{
    if (m_DirFd < 0) {
        return EBADF;
    }

    QByteArray f = QFile::encodeName(from);
    QByteArray t = QFile::encodeName(to);
    if (f.contains('/') || t.contains('/') || t.isEmpty()) {
        return EINVAL;
    }

#ifdef Q_OS_DARWIN
    if (::renameatx_np(m_DirFd, f.constData(), m_DirFd, t.constData(), RENAME_EXCL) == 0) {
        return 0;
    }
    if (errno != ENOTSUP) {
        return errno;
    }
    // Filesystem without RENAME_EXCL: link() never replaces an existing entry either
    if (::linkat(m_DirFd, f.constData(), m_DirFd, t.constData(), 0) != 0) {
        return errno;
    }
    ::unlinkat(m_DirFd, f.constData(), 0);
    return 0;
#else
    // link() never replaces an existing entry (including a dangling symlink)
    if (::linkat(m_DirFd, f.constData(), m_DirFd, t.constData(), 0) != 0) {
        return errno;
    }
    ::unlinkat(m_DirFd, f.constData(), 0);
    return 0;
#endif
}

bool DownloadDir::finalizePartFile(int fd, const QString& partName, const QString& sanitizedName,
                                   QString* finalName, QString* error)
{
    if (sanitizedName.isEmpty() || sanitizeFileName(sanitizedName) != sanitizedName) {
        if (error) *error = QStringLiteral("refusing an unsanitized file name");
        discardPartFile(fd, partName);
        return false;
    }

    struct stat verified;
    if (::fstat(fd, &verified) != 0 || !S_ISREG(verified.st_mode) ||
            verified.st_uid != ::geteuid() || verified.st_nlink != 1 ||
            !hasPrivateMutationAcl(fd) || !hasPrivateMutationAcl(m_DirFd)) {
        if (error) *error = QStringLiteral("download part file identity is invalid");
        discardPartFile(fd, partName);
        return false;
    }

    // Quarantine before the file becomes visible under its final name
    if (!setQuarantineAttribute(fd, error)) {
        discardPartFile(fd, partName);
        return false;
    }

    // Never executable, private to the current user
    if (::fchmod(fd, 0600) != 0) {
        if (error) *error = QStringLiteral("cannot set file mode: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        discardPartFile(fd, partName);
        return false;
    }

    if (::fsync(fd) != 0) {
        if (error) *error = QStringLiteral("cannot persist the received file");
        discardPartFile(fd, partName);
        return false;
    }

    for (int n = 0; n <= kMaxCollisionIndex; n++) {
        QString candidate = collisionName(sanitizedName, n);
        if (candidate.isEmpty()) {
            continue;
        }

        // Keep the verified descriptor alive until publication. The private directory
        // excludes other-user writers; reject a substituted name rather than publishing it.
        struct stat current;
        const QByteArray part = QFile::encodeName(partName);
        if (::fstatat(m_DirFd, part.constData(), &current, AT_SYMLINK_NOFOLLOW) != 0 ||
                !S_ISREG(current.st_mode) || current.st_dev != verified.st_dev ||
                current.st_ino != verified.st_ino || current.st_nlink != 1) {
            if (error) *error = QStringLiteral("download part file was replaced");
            discardPartFile(fd, partName);
            return false;
        }
        int err = renameNoReplace(partName, candidate);
        if (err == 0) {
            ::close(fd);
            *finalName = candidate;
            return true;
        }
        if (err != EEXIST) {
            if (error) *error = QStringLiteral("cannot move file into place: %1").arg(QString::fromLocal8Bit(strerror(err)));
            discardPartFile(fd, partName);
            return false;
        }
    }

    if (error) *error = QStringLiteral("no free file name for %1").arg(sanitizedName);
    discardPartFile(fd, partName);
    return false;
}

void DownloadDir::discardPartFile(int fd, const QString& partName)
{
    if (fd >= 0) {
        ::close(fd);
    }
    if (m_DirFd >= 0 && !partName.isEmpty()) {
        ::unlinkat(m_DirFd, QFile::encodeName(partName).constData(), 0);
    }
}

#else // !Q_OS_UNIX

// Downloads are only implemented for POSIX clients (the fork ships on macOS).

DownloadDir::DownloadDir(const QString& path) : m_Path(path), m_DirFd(-1) {}
DownloadDir::~DownloadDir() {}

bool DownloadDir::open(QString* error)
{
    if (error) *error = QStringLiteral("downloads are not supported on this platform");
    return false;
}

int DownloadDir::createPartFile(QString*, QString* error)
{
    if (error) *error = QStringLiteral("downloads are not supported on this platform");
    return -1;
}

bool DownloadDir::finalizePartFile(int, const QString&, const QString&, QString*, QString* error)
{
    if (error) *error = QStringLiteral("downloads are not supported on this platform");
    return false;
}

void DownloadDir::discardPartFile(int, const QString&) {}

int DownloadDir::renameNoReplace(const QString&, const QString&)
{
    return EINVAL;
}

#endif

} // namespace Sds
