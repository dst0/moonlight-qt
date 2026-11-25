#include "fileclipboard.h"
#include "filesanitize.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cerrno>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Sds {
namespace {
// QJsonDocument otherwise silently collapses duplicate members. Parse only the
// already-syntax-validated string/key boundaries and reject duplicates in each object.
bool uniqueMembers(const QByteArray& json)
{
    QList<QSet<QString>> objects;
    int depth = 0;
    int totalKeys = 0;
    for (qsizetype i = 0; i < json.size(); ++i) {
        if (json[i] == '{') {
            if (++depth > 3) return false;
            objects.append(QSet<QString>());
        }
        else if (json[i] == '}') {
            if (--depth < 0 || objects.isEmpty()) return false;
            objects.removeLast();
        }
        else if (json[i] == '[') {
            if (++depth > 3) return false;
        }
        else if (json[i] == ']') {
            if (--depth < 0) return false;
        }
        else if (json[i] == '"') {
            const auto start = i++;
            while (i < json.size() && json[i] != '"') {
                if (json[i] == '\\') ++i;
                ++i;
            }
            if (i >= json.size()) return false;
            auto next = i + 1;
            while (next < json.size() && (json[next] == ' ' || json[next] == '\t' ||
                                         json[next] == '\r' || json[next] == '\n')) ++next;
            if (next < json.size() && json[next] == ':') {
                if (objects.isEmpty()) return false;
                if (++totalKeys > 33) return false;
                if (i - start + 1 > 64) return false;
                const auto token = QByteArray("[") + json.mid(start, i - start + 1) + ']';
                const auto key = QJsonDocument::fromJson(token).array().first().toString();
                if (objects.last().contains(key)) return false;
                objects.last().insert(key);
            }
        }
    }
    return depth == 0 && objects.isEmpty();
}

bool validName(const QString& name)
{
    return !name.isEmpty() && !name.startsWith(QLatin1Char('.')) &&
            !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\')) &&
            sanitizeFileName(name) == name && name.toUtf8().size() <= kMaxNameBytes;
}
#ifdef Q_OS_UNIX
bool isAllowedRootSystemSymlink(const QString& topComponent)
{
#ifdef Q_OS_DARWIN
    if (topComponent != QLatin1String("tmp") &&
            topComponent != QLatin1String("var") &&
            topComponent != QLatin1String("etc")) {
        return false;
    }
    const QByteArray rootPath = QByteArrayLiteral("/") + QFile::encodeName(topComponent);
    struct stat st{};
    return ::lstat(rootPath.constData(), &st) == 0 && S_ISLNK(st.st_mode) && st.st_uid == 0;
#else
    Q_UNUSED(topComponent);
    return false;
#endif
}

bool matchStatIdentity(const struct stat& before, const struct stat& after)
{
    bool ok = before.st_dev == after.st_dev &&
              before.st_ino == after.st_ino &&
              before.st_size == after.st_size &&
              before.st_mtime == after.st_mtime &&
              before.st_ctime == after.st_ctime;
#ifdef Q_OS_DARWIN
    ok = ok && before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
         before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
    ok = ok && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
         before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
    return ok;
}
#endif
} // namespace

int openVerifiedSourceFile(const QString& path,
                           QString* normalizedNameOut,
                           qint64* sizeOut,
                           bool requireSameUid,
                           QString* error)
{
    auto fail = [error](const QString& msg) {
        if (error) {
            *error = msg;
        }
        return -1;
    };

#ifdef Q_OS_UNIX
    if (!path.startsWith(QLatin1Char('/')) || path.contains(QChar(0))) {
        return fail(QStringLiteral("Invalid local file path"));
    }
    const auto parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return fail(QStringLiteral("Invalid local file path"));
    }

    int directory = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        return fail(QStringLiteral("Cannot traverse root directory"));
    }

    for (int i = 0; i + 1 < parts.size() && directory >= 0; ++i) {
        int next = -1;
        if (parts[i] != QLatin1String(".") && parts[i] != QLatin1String("..")) {
            int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
            if (!(i == 0 && isAllowedRootSystemSymlink(parts[i]))) {
                flags |= O_NOFOLLOW;
            }
            next = ::openat(directory, QFile::encodeName(parts[i]).constData(), flags);
        }
        ::close(directory);
        directory = next;
    }
    if (directory < 0) {
        return fail(QStringLiteral("Cannot access parent directory"));
    }

    const auto& rawName = parts.last();
    const auto name = sanitizeFileName(rawName.normalized(QString::NormalizationForm_C));
    if (!validName(name)) {
        ::close(directory);
        return fail(QStringLiteral("%1 has an unsupported file name").arg(rawName));
    }

    int fd = ::openat(directory, QFile::encodeName(rawName).constData(),
                      O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    ::close(directory);
    if (fd < 0) {
        return fail((errno == ELOOP || errno == EISDIR || errno == ENXIO)
                        ? QStringLiteral("%1 is not a regular file").arg(name)
                        : QStringLiteral("Cannot open %1").arg(name));
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            (requireSameUid && st.st_uid != ::geteuid())) {
        const bool badKind = ::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode);
        ::close(fd);
        return fail(badKind
                        ? QStringLiteral("%1 is not a regular file").arg(name)
                        : QStringLiteral("Only regular files you own can be transferred"));
    }

    if (normalizedNameOut != nullptr) {
        *normalizedNameOut = name;
    }
    if (sizeOut != nullptr) {
        *sizeOut = static_cast<qint64>(st.st_size);
    }
    if (error != nullptr) {
        error->clear();
    }
    return fd;
#else
    Q_UNUSED(path);
    Q_UNUSED(normalizedNameOut);
    Q_UNUSED(sizeOut);
    Q_UNUSED(requireSameUid);
    return fail(QStringLiteral("Unsupported platform"));
#endif
}

bool inspectClipboardFiles(const QStringList& paths,
                           qint64 maxTotalBytes,
                           QList<ClipboardEntryMeta>* entriesOut,
                           QString* error)
{
    entriesOut->clear();
    if (paths.isEmpty() || paths.size() > kFileClipboardCountMax) {
        if (error) {
            *error = QStringLiteral("Copy up to %1 regular files, not folders").arg(kFileClipboardCountMax);
        }
        return false;
    }

    QList<ClipboardEntryMeta> result;
    QSet<QString> seenNames;
    qint64 total = 0;
#ifdef Q_OS_UNIX
    for (int i = 0; i < paths.size(); ++i) {
        QString name;
        qint64 size = 0;
        int fd = openVerifiedSourceFile(paths[i], &name, &size, true, error);
        if (fd < 0) {
            return false;
        }
        ::close(fd);

        const QString key = name.normalized(QString::NormalizationForm_C).toCaseFolded();
        if (seenNames.contains(key)) {
            if (error) {
                *error = QStringLiteral("Duplicate file names cannot be copied together");
            }
            return false;
        }
        if (maxTotalBytes > 0 && (size > maxTotalBytes - total)) {
            if (error) {
                *error = QStringLiteral("Selected files exceed host clipboard limit");
            }
            return false;
        }
        seenNames.insert(key);
        total += size;
        result.append({QStringLiteral("e%1").arg(i), name, size, paths[i]});
    }

    *entriesOut = std::move(result);
    if (error != nullptr) {
        error->clear();
    }
    return true;
#else
    Q_UNUSED(maxTotalBytes);
    Q_UNUSED(result);
    Q_UNUSED(seenNames);
    Q_UNUSED(total);
    if (error != nullptr) {
        *error = QStringLiteral("Unsupported platform");
    }
    return false;
#endif
}

QByteArray snapshotClipboardFiles(const QStringList& paths, QString* error)
{
    *error = QStringLiteral("Only up to 16 regular files totaling 8 MiB can be copied between computers");
    if (paths.isEmpty() || paths.size() > kFileClipboardCountMax) return {};
    QList<ClipboardFile> files;
    qint64 remaining = kFileClipboardBytesMax;
#ifdef Q_OS_UNIX
    for (const auto& path : paths) {
        QString name;
        qint64 size = 0;
        QString openErr;
        int fd = openVerifiedSourceFile(path, &name, &size, true, &openErr);
        if (fd < 0) {
            return {};
        }

        struct stat before{}, after{};
        bool valid = ::fstat(fd, &before) == 0 && size >= 0 && size <= remaining;
        QByteArray data;
        if (valid) {
            data.resize(size);
            qint64 offset = 0;
            while (offset < data.size()) {
                const auto n = ::read(fd, data.data() + offset, size_t(data.size() - offset));
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) { valid = false; break; }
                offset += n;
            }
            char extra;
            valid = valid && ::read(fd, &extra, 1) == 0 && ::fstat(fd, &after) == 0 &&
                    matchStatIdentity(before, after);
        }
        ::close(fd);
        if (!valid) return {};
        remaining -= data.size();
        files.append({name, data});
    }
    *error = QStringLiteral("Only up to 16 regular files totaling 8 MiB can be copied between computers");
    auto result = encodeFileClipboard(files);
    if (!result.isEmpty()) error->clear();
    return result;
#else
    return {};
#endif
}

bool decodeFileClipboard(const QByteArray& json, QList<ClipboardFile>* files)
{
    files->clear();
    if (json.isEmpty() || json.size() > kFileClipboardWireMax || !isValidUtf8(json)) return false;
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const auto root = doc.object();
    if (root.size() != 1 || !root.value(QStringLiteral("files")).isArray()) return false;
    const auto entries = root.value(QStringLiteral("files")).toArray();
    if (entries.isEmpty() || entries.size() > kFileClipboardCountMax) return false;
    if (!uniqueMembers(json)) return false;
    QList<ClipboardFile> result;
    QSet<QString> names;
    qint64 total = 0;
    for (const auto& entry : entries) {
        if (!entry.isObject()) return false;
        const auto object = entry.toObject();
        if (object.size() != 2 || !object.value(QStringLiteral("name")).isString() ||
                !object.value(QStringLiteral("data")).isString()) return false;
        const auto name = object.value(QStringLiteral("name")).toString();
        const auto key = name.normalized(QString::NormalizationForm_C).toCaseFolded();
        if (!validName(name) || names.contains(key)) return false;
        names.insert(key);
        const auto encoded = object.value(QStringLiteral("data")).toString().toUtf8();
        if (encoded.size() > ((kFileClipboardBytesMax - total + 2) / 3) * 4) return false;
        const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.toBase64() != encoded) return false;
        total += decoded.decoded.size();
        if (total > kFileClipboardBytesMax) return false;
        result.append({name, decoded.decoded});
    }
    *files = result;
    return true;
}

QByteArray encodeFileClipboard(const QList<ClipboardFile>& files)
{
    QJsonArray entries;
    qint64 total = 0;
    if (files.isEmpty() || files.size() > kFileClipboardCountMax) return {};
    for (const auto& file : files) {
        total += file.data.size();
        if (total > kFileClipboardBytesMax) return {};
        entries.append(QJsonObject{{QStringLiteral("name"), file.name},
                                   {QStringLiteral("data"), QString::fromLatin1(file.data.toBase64())}});
    }
    auto result = QJsonDocument(QJsonObject{{QStringLiteral("files"), entries}}).toJson(QJsonDocument::Compact);
    QList<ClipboardFile> checked;
    return decodeFileClipboard(result, &checked) ? result : QByteArray();
}

} // namespace Sds
