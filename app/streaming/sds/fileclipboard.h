#pragma once

#include "sdstypes.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace Sds {

constexpr int kFileClipboardCountMax = 16;
constexpr qint64 kFileClipboardBytesMax = 8 * 1024 * 1024;
constexpr qint64 kFileClipboardWireMax = 12 * 1024 * 1024;

struct ClipboardFile {
    QString name;
    QByteArray data;
};

// Strict bounded v1 wire format. Names are basenames, never paths; duplicate NFC/case
// aliases, noncanonical base64, unknown fields, and invalid Unicode are rejected.
bool decodeFileClipboard(const QByteArray& json, QList<ClipboardFile>* files);
QByteArray encodeFileClipboard(const QList<ClipboardFile>& files);

// Opens a regular source file with descriptor-relative traversal. On macOS, root-owned OS
// top-level symlinks (/tmp, /var, /etc -> private/*) are permitted while all subsequent
// directory components and the leaf require O_NOFOLLOW.
int openVerifiedSourceFile(const QString& path,
                           QString* normalizedNameOut,
                           qint64* sizeOut,
                           bool requireSameUid,
                           QString* error);

// Inspects regular file metadata (name + byte size) without reading file payloads.
bool inspectClipboardFiles(const QStringList& paths,
                           qint64 maxTotalBytes,
                           QList<ClipboardEntryMeta>* entriesOut,
                           QString* error);

// Snapshot user-owned regular files using descriptor-relative no-follow traversal.
// No path or source metadata is serialized. Failure rejects the entire selection.
QByteArray snapshotClipboardFiles(const QStringList& paths, QString* error);

} // namespace Sds
