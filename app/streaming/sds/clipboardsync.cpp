#include "clipboardsync.h"
#include "filesanitize.h"

#include <QCryptographicHash>

#include <array>
#include <cstring>

namespace Sds {

namespace {

quint32 readBe32(const unsigned char* p)
{
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

quint32 crc32(const unsigned char* data, size_t len)
{
    // Function-local static: initialized once, thread-safe (used from the worker and
    // the main thread).
    static const std::array<quint32, 256> table = []() {
        std::array<quint32, 256> t {};
        for (quint32 n = 0; n < 256; n++) {
            quint32 c = n;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[n] = c;
        }
        return t;
    }();

    quint32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

} // namespace

ClipboardCheck validateClipboardText(const QByteArray& utf8, qint64 maxBytes)
{
    if (utf8.isEmpty()) {
        return ClipboardCheck::Empty;
    }
    if (utf8.size() > maxBytes) {
        return ClipboardCheck::TooLarge;
    }
    if (utf8.contains('\0')) {
        return ClipboardCheck::ContainsNul;
    }
    if (!isValidUtf8(utf8)) {
        return ClipboardCheck::InvalidUtf8;
    }
    return ClipboardCheck::Ok;
}

ClipboardCheck validateClipboardPng(const QByteArray& png, qint64 maxBytes, quint32* width, quint32* height)
{
    static const unsigned char kSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

    if (png.isEmpty()) {
        return ClipboardCheck::Empty;
    }
    if (png.size() > maxBytes) {
        return ClipboardCheck::TooLarge;
    }

    // Signature (8) + IHDR length (4) + type (4) + data (13) + CRC (4)
    if (png.size() < 33) {
        return ClipboardCheck::BadPng;
    }

    const unsigned char* p = reinterpret_cast<const unsigned char*>(png.constData());
    if (memcmp(p, kSignature, sizeof(kSignature)) != 0) {
        return ClipboardCheck::BadPng;
    }
    if (readBe32(p + 8) != 13 || memcmp(p + 12, "IHDR", 4) != 0) {
        return ClipboardCheck::BadPng;
    }
    if (crc32(p + 12, 4 + 13) != readBe32(p + 29)) {
        return ClipboardCheck::BadPng;
    }

    quint32 w = readBe32(p + 16);
    quint32 h = readBe32(p + 20);
    unsigned char bitDepth = p[24];
    unsigned char colorType = p[25];
    unsigned char compression = p[26];
    unsigned char filter = p[27];
    unsigned char interlace = p[28];

    if (w == 0 || h == 0 || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu) {
        return ClipboardCheck::BadPng;
    }

    bool depthOk;
    switch (colorType) {
    case 0: depthOk = bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8 || bitDepth == 16; break;
    case 3: depthOk = bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8; break;
    case 2:
    case 4:
    case 6: depthOk = bitDepth == 8 || bitDepth == 16; break;
    default: depthOk = false; break;
    }
    if (!depthOk || compression != 0 || filter != 0 || interlace > 1) {
        return ClipboardCheck::BadPng;
    }

    if (static_cast<quint64>(w) * static_cast<quint64>(h) * 4u > static_cast<quint64>(kPngMaxDecodedBytes)) {
        return ClipboardCheck::PngTooLarge;
    }

    if (width) *width = w;
    if (height) *height = h;
    return ClipboardCheck::Ok;
}

QString describeClipboardCheck(ClipboardCheck check, bool image)
{
    switch (check) {
    case ClipboardCheck::Ok:
        return QString();
    case ClipboardCheck::Empty:
        return image ? QStringLiteral("Clipboard image is empty") : QStringLiteral("Clipboard text is empty");
    case ClipboardCheck::TooLarge:
        return image ? QStringLiteral("Clipboard image too large") : QStringLiteral("Clipboard text too large");
    case ClipboardCheck::InvalidUtf8:
        return QStringLiteral("Clipboard text is not valid UTF-8");
    case ClipboardCheck::ContainsNul:
        return QStringLiteral("Clipboard text contains NUL characters");
    case ClipboardCheck::BadPng:
        return QStringLiteral("Clipboard image is not a valid PNG");
    case ClipboardCheck::PngTooLarge:
        return QStringLiteral("Clipboard image dimensions too large");
    }
    return QString();
}

void sanitizeClipboardItem(ClipboardItem& item, const Limits& limits,
                           ClipboardCheck* textCheck, ClipboardCheck* imageCheck)
{
    ClipboardCheck t = ClipboardCheck::Empty;
    ClipboardCheck i = ClipboardCheck::Empty;

    if (!item.text.isEmpty()) {
        t = validateClipboardText(item.text, limits.clipboardTextMax);
        if (t != ClipboardCheck::Ok) {
            item.text.clear();
        }
    }
    if (!item.png.isEmpty()) {
        i = validateClipboardPng(item.png, limits.clipboardImageMax);
        if (i != ClipboardCheck::Ok) {
            item.png.clear();
        }
    }

    if (textCheck) *textCheck = t;
    if (imageCheck) *imageCheck = i;
}

void ClipboardEchoGuard::reset(qint64 currentChangeCount)
{
    m_LastSeen = currentChangeCount;
    m_LastWritten = -1;
}

bool ClipboardEchoGuard::pollChanged(qint64 changeCount)
{
    if (changeCount == m_LastSeen) {
        return false;
    }
    m_LastSeen = changeCount;
    return changeCount != m_LastWritten;
}

void ClipboardEchoGuard::noteOwnWrite(qint64 changeCountAfterWrite)
{
    m_LastWritten = changeCountAfterWrite;
    m_LastSeen = changeCountAfterWrite;
}

QByteArray ClipboardEchoGuard::contentHash(const ClipboardItem& item)
{
    QCryptographicHash h(QCryptographicHash::Sha256);
    QByteArray len;
    len.setNum(item.text.size());
    h.addData(QByteArrayLiteral("t"));
    h.addData(len);
    h.addData(QByteArrayLiteral(":"));
    h.addData(item.text);
    len.setNum(item.png.size());
    h.addData(QByteArrayLiteral("p"));
    h.addData(len);
    h.addData(QByteArrayLiteral(":"));
    h.addData(item.png);
    return h.result();
}

bool ClipboardEchoGuard::isDuplicateOfLastSynced(const ClipboardItem& item) const
{
    return !m_LastSyncedHash.isEmpty() && contentHash(item) == m_LastSyncedHash;
}

void ClipboardEchoGuard::noteSynced(const ClipboardItem& item)
{
    m_LastSyncedHash = contentHash(item);
}

} // namespace Sds
