#pragma once

// secure-desktop-stream: file name sanitizer (DESIGN 6.4) and the client download folder
// (DESIGN 6.5): exclusive part files, never-overwrite renames, quarantine attribute.

#include <QByteArray>
#include <QString>
#include <QUuid>

namespace Sds {

constexpr int kMaxNameBytes = 200;
constexpr int kMaxCollisionIndex = 999;
// An extension longer than this is not preserved when an overlong name is shortened.
constexpr int kMaxPreservedExtensionBytes = 32;

// Sanitizes a file name received from the peer (or taken from a local path before sending).
// Returns the NFC-normalized, rewritten name, or a null QString if the name must be rejected
// (empty, "." or "..", unpaired surrogates, nothing left after rewriting).
QString sanitizeFileName(const QString& name);

// Same, for raw bytes: rejects input that is not strictly valid UTF-8.
QString sanitizeFileNameUtf8(const QByteArray& utf8);

// Strict UTF-8 validation (no overlongs, no surrogates, nothing above U+10FFFF).
bool isValidUtf8(const QByteArray& bytes);

// Returns the candidate name for collision index n: n == 0 gives the name itself,
// n >= 1 gives "stem (n).ext" (split at the last dot), shortened so that the result stays
// within kMaxNameBytes UTF-8 bytes.
QString collisionName(const QString& sanitizedName, int n);

// Value written to com.apple.quarantine: "0081;<hex unix time>;Moonlight;<UUID>".
QByteArray quarantineValue(qint64 unixTime, const QUuid& eventId);

// The client download folder (~/Downloads/Moonlight). All file operations are relative to
// a directory descriptor opened with O_NOFOLLOW, so a symlinked folder is refused and a
// name can never resolve outside it.
class DownloadDir
{
public:
    explicit DownloadDir(const QString& path);
    ~DownloadDir();

    DownloadDir(const DownloadDir&) = delete;
    DownloadDir& operator=(const DownloadDir&) = delete;

    // Creates the folder (mode 0700) if missing and opens it. Refuses a symlink, a
    // non-directory or a folder owned by another user.
    bool open(QString* error);
    bool isOpen() const { return m_DirFd >= 0; }
    QString path() const { return m_Path; }

    // Creates a new hidden part file ".<random>.part" with
    // O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC and mode 0600. Returns the fd or -1.
    int createPartFile(QString* partName, QString* error);

    // Marks the part file as downloaded (com.apple.quarantine), removes any execute or
    // group/other write bits (mode 0644), closes it and moves it to the first free name
    // from collisionName(sanitizedName, 0..kMaxCollisionIndex) without ever replacing an
    // existing entry. On success *finalName holds the chosen name. On failure the part
    // file is removed. The fd is always closed.
    bool finalizePartFile(int fd, const QString& partName, const QString& sanitizedName,
                          QString* finalName, QString* error);

    // Closes and removes a part file.
    void discardPartFile(int fd, const QString& partName);

    // Moves 'from' to 'to' inside the folder only if 'to' does not exist (no replace).
    // Returns 0 or an errno value (EEXIST if the target exists).
    int renameNoReplace(const QString& from, const QString& to);

private:
    QString m_Path;
    int m_DirFd;
};

// Sets the quarantine attribute on an open file. No-op (returns true) on non-Apple systems.
bool setQuarantineAttribute(int fd, QString* error);

} // namespace Sds
