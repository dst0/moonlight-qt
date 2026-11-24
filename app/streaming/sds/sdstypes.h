#pragma once

// secure-desktop-stream (SDS): types shared by the clipboard and file transfer client.
// See docs/DESIGN.md (sections 4, 5 and 6) in the secure-desktop-stream repository.

#include <QByteArray>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

namespace Sds {

// Client-side hard caps. The effective limit is always min(host limit, client cap), so a
// malicious or misconfigured host cannot make the client allocate unbounded memory.
constexpr qint64 kClientTextMax = 16LL * 1024 * 1024;
constexpr qint64 kClientImageMax = 64LL * 1024 * 1024;
constexpr qint64 kClientChunkMax = 16LL * 1024 * 1024;
constexpr qint64 kClientFileMax = 64LL * 1024 * 1024 * 1024;
// PNG header sanity check: width * height * 4 must not exceed this (DESIGN 5.1).
constexpr qint64 kPngMaxDecodedBytes = 256LL * 1024 * 1024;
// Upper bound for JSON responses (capabilities, events, outbox listing).
constexpr qint64 kMaxJsonResponse = 1024 * 1024;
// Free space the client keeps on its own disk when downloading outbox files.
constexpr qint64 kClientMinFreeSpace = 1024LL * 1024 * 1024;
// At most this many outbox entries are considered per listing.
constexpr int kMaxOutboxEntries = 1000;

// Permission names (DESIGN 4.2).
constexpr const char* kPermClipboardRead = "clipboard_read";
constexpr const char* kPermClipboardWrite = "clipboard_write";
constexpr const char* kPermFileUpload = "file_upload";
constexpr const char* kPermFileDownload = "file_download";

struct Limits
{
    qint64 clipboardTextMax = 1048576;
    qint64 clipboardImageMax = 16777216;
    qint64 fileMax = 4294967296LL;
    qint64 chunkMax = 4194304;
    int uploadsMaxConcurrent = 2;
};

struct Capabilities
{
    int version = 0;
    QSet<QString> features;
    QSet<QString> permissions;
    Limits limits;          // already clamped to the client caps
    bool streamActive = false;

    bool has(const char* permission) const
    {
        return permissions.contains(QString::fromLatin1(permission));
    }

    bool hasFeature(const char* feature) const
    {
        return features.contains(QString::fromLatin1(feature));
    }
};

struct ClipboardEntryMeta
{
    QString id;
    QString name;
    qint64 size = 0;
    QString sourcePath;
};

struct LazyClipboardOffer
{
    QString offerId;
    QString kind; // "files" or "image"
    QString mime; // "image/png" when kind == "image"
    QList<ClipboardEntryMeta> entries;
    qint64 localGeneration = -1;
    bool fromHost = false;
};

struct LazyTransferResult
{
    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    bool ok = false;
    QStringList stagedPaths;
    QByteArray png;
    QString error;
};

// A clipboard item with up to two representations (DESIGN 5.1).
struct ClipboardItem
{
    QByteArray text; // UTF-8, empty means "no text representation"
    QByteArray png;  // PNG file bytes, empty means "no image representation"

    bool hasText() const { return !text.isEmpty(); }
    bool hasImage() const { return !png.isEmpty(); }
    bool isEmpty() const { return text.isEmpty() && png.isEmpty(); }
};

// Events delivered by the client worker to its listener. They are produced on the worker
// thread; the listener must hand them over to its own thread.
struct ClientEvent
{
    enum Type {
        // Capabilities are known and the first (immediate) event poll was processed.
        // hostProvidedClipboard says whether the host announced an initial clipboard.
        Ready,
        // Capabilities were refetched after a "capabilities" event.
        CapabilitiesChanged,
        // Host clipboard content, already validated against the effective limits.
        ClipboardReceived,
        // Transfer progress (percent 0..100).
        TransferProgress,
        // Transfer finished, ok or failed. For downloads, path is the saved file.
        TransferFinished,
        // A user-facing message (errors, refusals).
        Notice,
        // The client stopped for good (stream ended, feature unavailable, pin failure).
        Stopped,
        // Baseline/poll completion for native files; publication requires unchanged localGeneration.
        FileClipboardObserved,
        // Metadata-only lazy clipboard offer received from host (zero bytes fetched until paste).
        LazyClipboardOfferReceived,
        // Host requested streaming bytes for our previously offered lazy clipboard offer.
        LazyClipboardPullRequested,
        // On-demand lazy download for a host offer finished (ok or failed).
        LazyClipboardFinished,
    };

    Type type = Notice;
    Capabilities caps;
    bool hostProvidedClipboard = false;
    ClipboardItem clipboard;
    quint64 hostSeq = 0;
    QByteArray fileBundle;
    qint64 localGeneration = -1;
    LazyClipboardOffer lazyOffer;
    QString offerId;
    QStringList pullTargets;
    QStringList stagedPaths;
    QString name;
    int percent = 0;
    bool upload = false;
    bool ok = false;
    QString path;
    QString message;
};

class ClientListener
{
public:
    virtual ~ClientListener() = default;

    // Called on the client's worker thread. Implementations must not block for long and
    // must not call back into the client synchronously.
    virtual void onSdsClientEvent(const ClientEvent& event) = 0;
};

// The operations the session-side controller needs from the network client. Implemented by
// SdsClient; tests substitute a fake.
class IClient
{
public:
    virtual ~IClient() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void sendClipboard(const ClipboardItem& item) = 0;
    virtual void sendFile(const QString& path) = 0;
    virtual void downloadOutboxNow() = 0;
    virtual void exchangeFileClipboard(const QByteArray&, qint64) {}
    virtual void publishLazyClipboardOffer(const LazyClipboardOffer&) {}
    virtual void cancelLazyClipboardOffer(const QString&) {}
    virtual void fulfillLazyClipboardPull(const LazyClipboardOffer&) {}
    virtual void fetchLazyClipboardOffer(const LazyClipboardOffer&,
                                         const QString& /*stageDir*/,
                                         std::shared_ptr<LazyTransferResult> /*result*/) {}
    virtual void sendDroppedFiles(double /*nx*/, double /*ny*/, const QStringList& /*paths*/) {}
};

// Platform clipboard access (NSPasteboard on macOS). Main thread only.
class IClipboardBackend
{
public:
    using LazyDemandCallback = std::function<void(const QString& offerId)>;

    struct ReadResult
    {
        ClipboardItem item;
        bool textTooLarge = false;
        bool imageTooLarge = false;
    };

    virtual ~IClipboardBackend() = default;

    virtual bool isAvailable() const = 0;
    virtual qint64 changeCount() = 0;
    // Reads the current clipboard. Representations larger than the given limits are
    // dropped (and flagged), never truncated.
    virtual ReadResult read(qint64 textMax, qint64 imageMax) = 0;
    // Replaces the clipboard with the item and returns the resulting change count.
    virtual qint64 write(const ClipboardItem& item) = 0;
    virtual bool hasFileSelection() { return false; }
    virtual QByteArray readFiles(QString*) { return {}; }
    virtual qint64 writeFiles(const QByteArray&, qint64, QString*) { return -1; }
    virtual bool readFileEntries(QList<ClipboardEntryMeta>*, QString*) { return false; }
    virtual bool readLazyImage(qint64 /*minBytes*/, qint64 /*maxBytes*/, QString* /*tempPathOut*/,
                               qint64* /*sizeOut*/, QString* /*error*/) { return false; }
    virtual QString createLazyStageDir(QString*) { return {}; }
    virtual void retireStageDir(const QString&) {}
    virtual qint64 writeLazyOffer(const LazyClipboardOffer&,
                                  std::shared_ptr<LazyTransferResult>,
                                  LazyDemandCallback,
                                  QString*) { return -1; }
    virtual bool completeLazyOffer(const QString& /*offerId*/,
                                   bool /*ok*/,
                                   const QStringList& /*stagedPaths*/,
                                   const QByteArray& /*png*/,
                                   const QString& /*error*/,
                                   qint64* /*newChangeCountOut*/,
                                   bool* /*needRepasteToastOut*/)
    {
        return false;
    }
    // Releases only our owned file clipboard and stages; retains a bounded native-read grace.
    virtual void retireFiles() {}
};

// Maps window-space pointer coordinates (px, py) to [0.0, 1.0] normalized coordinates
// within the aspect-ratio-fitted video region (matching StreamUtils::scaleSourceToDestinationSurface).
inline void mapPointToNormalizedVideoRect(int windowW,
                                          int windowH,
                                          int streamW,
                                          int streamH,
                                          int px,
                                          int py,
                                          double* nxOut,
                                          double* nyOut)
{
    double nx = 0.5;
    double ny = 0.5;
    if (windowW > 0 && windowH > 0) {
        int dstX = 0, dstY = 0, dstW = windowW, dstH = windowH;
        if (streamW > 0 && streamH > 0) {
            const int scaledH = static_cast<int>(std::ceil(static_cast<double>(windowW) * streamH / streamW));
            const int scaledW = static_cast<int>(std::ceil(static_cast<double>(windowH) * streamW / streamH));
            if (scaledH > windowH) {
                dstX += (windowW - scaledW) / 2;
                dstW = scaledW;
            }
            else {
                dstY += (windowH - scaledH) / 2;
                dstH = scaledH;
            }
        }
        if (dstW > 0 && dstH > 0) {
            nx = std::clamp(static_cast<double>(px - dstX) / static_cast<double>(dstW), 0.0, 1.0);
            ny = std::clamp(static_cast<double>(py - dstY) / static_cast<double>(dstH), 0.0, 1.0);
        }
    }
    if (nxOut) *nxOut = nx;
    if (nyOut) *nyOut = ny;
}

} // namespace Sds
