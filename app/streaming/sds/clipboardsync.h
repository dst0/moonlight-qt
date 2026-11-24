#pragma once

// secure-desktop-stream: clipboard validation (DESIGN 5.1, S13) and echo suppression
// (DESIGN 5.6). Pure logic, no platform clipboard access.

#include "sdstypes.h"

#include <QByteArray>
#include <QString>

namespace Sds {

enum class ClipboardCheck {
    Ok,
    Empty,
    TooLarge,      // more bytes than the effective limit
    InvalidUtf8,
    ContainsNul,
    BadPng,        // signature or IHDR malformed
    PngTooLarge,   // width * height * 4 above kPngMaxDecodedBytes
};

ClipboardCheck validateClipboardText(const QByteArray& utf8, qint64 maxBytes);

// Checks the PNG signature and the IHDR chunk (length, type, CRC, dimensions, bit depth and
// colour type) before any decoder sees the data.
ClipboardCheck validateClipboardPng(const QByteArray& png, qint64 maxBytes,
                                    quint32* width = nullptr, quint32* height = nullptr);

QString describeClipboardCheck(ClipboardCheck check, bool image);

// Removes representations that fail validation. Returns the checks for text and image.
void sanitizeClipboardItem(ClipboardItem& item, const Limits& limits,
                           ClipboardCheck* textCheck, ClipboardCheck* imageCheck);

// Tracks the platform clipboard change counter so that content the client wrote itself
// (received from the host) is not pushed back, and identical content is not re-sent.
class ClipboardEchoGuard
{
public:
    // Starts tracking from the current counter: whatever is on the clipboard now does not
    // count as a change.
    void reset(qint64 currentChangeCount);

    // Call on every poll. Returns true only if the clipboard changed since the last poll and
    // the change was not produced by noteOwnWrite().
    bool pollChanged(qint64 changeCount);

    // Call after writing host content to the clipboard, with the resulting change count.
    void noteOwnWrite(qint64 changeCountAfterWrite);

    // Content-level de-duplication (e.g. a clipboard manager re-copying the same data).
    bool isDuplicateOfLastSynced(const ClipboardItem& item) const;
    void noteSynced(const ClipboardItem& item);

    qint64 lastSeen() const { return m_LastSeen; }

    static QByteArray contentHash(const ClipboardItem& item);

private:
    qint64 m_LastSeen = -1;
    qint64 m_LastWritten = -1;
    QByteArray m_LastSyncedHash;
};

} // namespace Sds
