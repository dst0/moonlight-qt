#include "sdsclient.h"
#include "lanpolicy.h"
#include "certpinning.h"

#include "streaming/sds/clipboardsync.h"
#include "streaming/sds/filesanitize.h"
#include "streaming/sds/fileclipboard.h"
#include "streaming/video/adaptiverefinement_codec.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslConfiguration>
#include <QSslError>
#include <QStorageInfo>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace Sds;

namespace {

constexpr int kJsonTimeoutMs = 15000;
constexpr int kClipboardTimeoutMs = 20000;
constexpr int kChunkIdleTimeoutMs = 60000;
constexpr int kCompleteTimeoutMs = 120000;
constexpr int kMaxChunkAttempts = 5;
constexpr int kMaxUploadConflicts = 8;
constexpr int kMaxCreateAttempts = 5;
constexpr int kMaxClipboardAttempts = 3;
constexpr int kMaxEventsPerPoll = 256;
constexpr qint64 kHashSliceBytes = 4LL * 1024 * 1024;
constexpr int kMaxQueuedDownloads = 1000;
constexpr int kMaxTrackedOutboxIds = 10000;
constexpr int kMinListingIntervalMs = 2000;
constexpr int kMinIdlePollMs = 1000;
constexpr int kMinPollIntervalMs = 200;
constexpr int kMaxCompleteAttempts = 3;
constexpr qint64 kReadBufferBytes = 256 * 1024;

const char* const kTooLargeProp = "sdsTooLarge";

const QString kTypeText = QStringLiteral("text/plain");
const QString kTypePng = QStringLiteral("image/png");

bool isSafeSdsToken(const QString& token)
{
    if (token.isEmpty() || token.size() > 128) {
        return false;
    }
    for (QChar c : token) {
        if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
              (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
              (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              c == QLatin1Char('-') || c == QLatin1Char('_'))) {
            return false;
        }
    }
    return true;
}

bool isLowerHex(const QString& s, int len)
{
    if (s.size() != len) {
        return false;
    }
    for (QChar c : s) {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) {
            return false;
        }
    }
    return true;
}

// JSON numbers are doubles; accept only non-negative integers that a double represents exactly.
bool jsonToInt64(const QJsonValue& v, qint64* out)
{
    if (!v.isDouble()) {
        return false;
    }
    double d = v.toDouble();
    if (!(d >= 0) || d > 9007199254740992.0 || std::floor(d) != d) {
        return false;
    }
    *out = static_cast<qint64>(d);
    return true;
}

QString elide(const QString& name)
{
    constexpr int kMax = 40;
    if (name.size() <= kMax) {
        return name;
    }
    return name.left(kMax / 2 - 1) + QStringLiteral("...") + name.right(kMax / 2 - 2);
}

QString formatBytes(qint64 bytes)
{
    if (bytes >= 1024LL * 1024 * 1024) {
        return QStringLiteral("%1 GiB").arg(bytes / double(1024LL * 1024 * 1024), 0, 'f', 1);
    }
    if (bytes >= 1024 * 1024) {
        return QStringLiteral("%1 MiB").arg(bytes / double(1024 * 1024), 0, 'f', 1);
    }
    return QStringLiteral("%1 KiB").arg((bytes + 1023) / 1024);
}

bool parseCapabilities(const QJsonObject& o, Capabilities& caps)
{
    caps = Capabilities();
    caps.version = o.value(QStringLiteral("version")).toInt(0);

    const QJsonArray feats = o.value(QStringLiteral("features")).toArray();
    for (const QJsonValue& f : feats) {
        if (f.isString()) {
            caps.features.insert(f.toString());
        }
    }

    const QJsonArray perms = o.value(QStringLiteral("permissions")).toArray();
    for (const QJsonValue& p : perms) {
        if (p.isString()) {
            caps.permissions.insert(p.toString());
        }
    }

    const QJsonObject l = o.value(QStringLiteral("limits")).toObject();
    auto limit = [&l](const char* key, qint64 def, qint64 cap) {
        qint64 v;
        if (!jsonToInt64(l.value(QLatin1String(key)), &v) || v <= 0) {
            v = def;
        }
        return std::min(v, cap);
    };

    caps.limits.clipboardTextMax = limit("clipboard_text_max", 1048576, kClientTextMax);
    caps.limits.clipboardImageMax = limit("clipboard_image_max", 16777216, kClientImageMax);
    caps.limits.fileMax = limit("file_max", 4294967296LL, kClientFileMax);
    caps.limits.chunkMax = limit("chunk_max", 4194304, kClientChunkMax);
    caps.limits.uploadsMaxConcurrent = static_cast<int>(limit("uploads_max_concurrent", 2, 64));
    caps.streamActive = o.value(QStringLiteral("stream_active")).toBool(false);

    // Only protocol version 1 is understood; anything else disables the features.
    return caps.version == 1;
}

#ifdef Q_OS_UNIX
bool writeAll(int fd, const char* data, qint64 len)
{
    while (len > 0) {
        ssize_t n = ::write(fd, data, static_cast<size_t>(len));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += n;
        len -= n;
    }
    return true;
}
#endif

struct HttpResult
{
    int status = 0; // 0: no HTTP response (transport error, abort, TLS failure)
    QNetworkReply::NetworkError netError = QNetworkReply::NoError;
    QByteArray body;
    QJsonObject json;
    QString errorCode;
    bool pinFailure = false;
    bool tooLarge = false;
    int retryAfterSec = -1;
    QHash<QByteArray, QByteArray> headers; // lower-case names

    bool ok() const { return status >= 200 && status < 300; }
    QByteArray header(const char* name) const { return headers.value(QByteArray(name)); }
};

} // namespace

class SdsWorker : public QObject
{
public:
    SdsWorker(const SdsClient::Config& config, ClientListener* listener) :
        m_Cfg(config),
        m_Listener(listener)
    {
    }

    // Everything below runs on the worker thread.

    void init()
    {
        if (!LanPolicy::isPermittedConnectionTarget(QHostAddress(m_Cfg.host))) {
            stopForGood(QStringLiteral("Clipboard and file transfer require a private LAN IP address"), true);
            return;
        }
        m_Nam = new QNetworkAccessManager(this);
        m_Nam->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        m_Nam->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);

        // Share the paired-host certificate policy with NvHTTP.
        m_SslConfig = QSslConfiguration::defaultConfiguration();
        m_SslConfig.setLocalCertificate(m_Cfg.clientCert);
        m_SslConfig.setPrivateKey(m_Cfg.clientKey);
        m_SslConfig = CertPinning::restrictToPinned(m_SslConfig, m_Cfg.serverCert);
        m_SslConfig.setPeerVerifyMode(QSslSocket::VerifyPeer);

        if (m_Cfg.serverCert.isNull() || m_Cfg.httpsPort == 0 || m_Cfg.host.isEmpty()) {
            stopForGood(QStringLiteral("Clipboard and file transfer need a paired host"), true);
            return;
        }

        fetchCapabilities(true);
    }

    void shutdown()
    {
        m_Shutdown = true;
        m_CapsInFlight = false;
        m_CapsPending = false;
        abortAll();
        if (m_Upload) {
            m_Upload->file.close();
            m_Upload.reset();
        }
        if (m_Download) {
            if (m_Dir) {
                m_Dir->discardPartFile(m_Download->fd, m_Download->partName);
            }
            m_Download.reset();
        }
        m_BatchUpload.reset();
        m_DropQueue.clear();
        abortLazyFetchLocked(QStringLiteral("Cancelled"));
        m_Dir.reset();
        delete m_Nam;
        m_Nam = nullptr;
    }

    void doSendClipboard(ClipboardItem item)
    {
        if (!canRun() || !m_HaveCaps || !m_Cfg.clipboardEnabled || !m_Caps.has(kPermClipboardWrite)) {
            return;
        }

        ClipboardCheck tc, ic;
        sanitizeClipboardItem(item, m_Caps.limits, &tc, &ic);
        if (tc != ClipboardCheck::Ok && tc != ClipboardCheck::Empty) {
            notice(describeClipboardCheck(tc, false));
        }
        if (ic != ClipboardCheck::Ok && ic != ClipboardCheck::Empty) {
            notice(describeClipboardCheck(ic, true));
        }
        if (item.isEmpty()) {
            return;
        }

        quint64 seq = ++m_ClientSeq;

        // Only the newest clipboard matters: drop representations not yet sent
        m_ClipQueue.clear();
        if (item.hasText()) {
            m_ClipQueue.append({ seq, QByteArrayLiteral("text/plain;charset=utf-8"), item.text, 0 });
        }
        if (item.hasImage()) {
            m_ClipQueue.append({ seq, QByteArrayLiteral("image/png"), item.png, 0 });
        }
        processClipQueue();
    }

    void doExchangeFileClipboard(QByteArray bundle, qint64 generation)
    {
        const bool enabled = canRun() && m_HaveCaps && m_Caps.streamActive &&
                m_Cfg.fileClipboardEnabled && m_Cfg.clipboardEnabled && m_Cfg.filesEnabled &&
                ((m_Caps.has(kPermClipboardRead) && m_Caps.has(kPermFileDownload)) ||
                 (m_Caps.has(kPermClipboardWrite) && m_Caps.has(kPermFileUpload)));
        if (!enabled) { m_FileEpoch.clear(); m_PendingFiles.clear(); return; }
        if (!m_PendingFiles.isEmpty() && generation != m_PendingFileGeneration) m_PendingFiles.clear();
        if (!bundle.isEmpty()) {
            QList<ClipboardFile> checked;
            if (!m_Caps.has(kPermClipboardWrite) || !m_Caps.has(kPermFileUpload) ||
                    m_FileEpoch.isEmpty() || !decodeFileClipboard(bundle, &checked)) return;
            m_PendingFiles = bundle;
            m_PendingFileGeneration = generation;
            m_PendingFileSeq = m_FileSeq;
            m_PendingFileEpoch = m_FileEpoch;
        }
        if (m_FileBusy) return;
        const bool sending = !m_PendingFiles.isEmpty();
        if (sending && (m_PendingFileSeq != m_FileSeq || m_PendingFileEpoch != m_FileEpoch)) {
            m_PendingFiles.clear();
            notice(QStringLiteral("File clipboard changed on the host; copy the files again"));
            return;
        }
        auto body = m_PendingFiles;
        m_PendingFiles.clear();
        QUrlQuery query;
        QList<QPair<QByteArray, QByteArray>> headers;
        if (!m_FileEpoch.isEmpty()) {
            headers.append({QByteArrayLiteral("X-SDS-Clipboard-Epoch"), m_FileEpoch});
            if (sending) headers.append({QByteArrayLiteral("X-SDS-Clipboard-Seq"), QByteArray::number(m_FileSeq)});
            else query.addQueryItem(QStringLiteral("after"), QString::number(m_FileSeq));
        }
        const auto epoch = m_FileEpoch;
        const auto revision = m_FileRevision;
        m_FileBusy = true;
        send(sending ? "PUT" : "GET", QStringLiteral("/sds/v1/file-clipboard"), query, body,
             sending ? QByteArrayLiteral("application/json") : QByteArray(), headers,
             kClipboardTimeoutMs, kFileClipboardWireMax,
             [this, sending, generation, epoch, revision](const HttpResult& r) {
            m_FileBusy = false;
            if (r.pinFailure) { pinFailure(); return; }
            // Capabilities may have changed while this request was in flight.
            if (!m_Cfg.fileClipboardEnabled || m_FileEpoch != epoch || revision != m_FileRevision ||
                    (!(m_Caps.has(kPermClipboardRead) && m_Caps.has(kPermFileDownload)) &&
                     !(m_Caps.has(kPermClipboardWrite) && m_Caps.has(kPermFileUpload)))) return;
            const auto receivedEpoch = r.header("x-sds-clipboard-epoch");
            const auto seqText = r.header("x-sds-clipboard-seq");
            bool ok = false;
            const auto seq = seqText.toULongLong(&ok);
            const bool epochValid = !receivedEpoch.isEmpty() && receivedEpoch.size() <= 128 &&
                    std::all_of(receivedEpoch.begin(), receivedEpoch.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
            });
            if (r.status == 409 || (!epoch.isEmpty() && receivedEpoch != epoch)) {
                m_FileEpoch.clear();
                m_PendingFiles.clear();
                ClientEvent reset;
                reset.type = ClientEvent::FileClipboardObserved;
                reset.localGeneration = -2;
                emitEvent(reset);
                if (sending) notice(QStringLiteral("File clipboard changed; copy the files again"));
                return;
            }
            if ((r.status != 200 && r.status != 204) || !ok || seqText != QByteArray::number(seq) ||
                    !epochValid || r.tooLarge || r.netError != QNetworkReply::NoError) {
                m_FileEpoch.clear();
                m_PendingFiles.clear();
                return;
            }
            if ((sending || epoch.isEmpty()) && r.status != 204) return;
            if (!epoch.isEmpty() && seq < m_FileSeq) { m_FileEpoch.clear(); return; }
            if (r.status == 200 && seq <= m_FileSeq) return;
            m_FileEpoch = receivedEpoch;
            m_FileSeq = seq;
            ClientEvent ev;
            ev.type = ClientEvent::FileClipboardObserved;
            ev.localGeneration = generation;
            if (!sending && !epoch.isEmpty() && r.status == 200 &&
                    m_Caps.has(kPermClipboardRead) && m_Caps.has(kPermFileDownload)) {
                QList<ClipboardFile> checked;
                if (!decodeFileClipboard(r.body, &checked)) {
                    notice(QStringLiteral("Rejected invalid file clipboard"));
                    return;
                }
                ev.fileBundle = r.body;
            }
            emitEvent(ev);
        });
    }

    void doSendFile(const QString& path)
    {
        if (!canRun()) {
            notice(QStringLiteral("File transfer is not available"));
            return;
        }
        if (!m_Cfg.filesEnabled) {
            notice(QStringLiteral("File transfer is disabled"));
            return;
        }
        if (m_HaveCaps && !m_Caps.has(kPermFileUpload)) {
            notice(QStringLiteral("The host does not allow sending files"));
            return;
        }

        m_UploadQueue.append(path);
        startNextUpload();
    }

    void doDownloadOutbox(bool manual)
    {
        if (!canRun()) {
            if (manual) {
                notice(QStringLiteral("File transfer is not available"));
            }
            return;
        }
        requestListing(manual);
    }

    void doSendDroppedFiles(double nx, double ny, const QStringList& paths)
    {
        if (paths.isEmpty()) {
            return;
        }
        if (!canRun() || !m_Cfg.filesEnabled) {
            notice(QStringLiteral("File transfer is not available"));
            return;
        }
        if (m_HaveCaps && !m_Caps.has(kPermFileUpload)) {
            notice(QStringLiteral("The host does not allow sending files"));
            return;
        }
        if (!m_Cfg.xdndDropEnabled) {
            for (const QString& path : paths) {
                doSendFile(path);
            }
            return;
        }
        m_DropQueue.append({std::clamp(nx, 0.0, 1.0), std::clamp(ny, 0.0, 1.0), paths});
        startNextDrop();
    }

    void doPublishLazyClipboardOffer(const LazyClipboardOffer& offer)
    {
        if (!lazyClipboardUsable() || !m_Caps.has(kPermClipboardWrite) || !m_Caps.has(kPermFileUpload)) {
            return;
        }
        if (!isSafeSdsToken(offer.offerId) || offer.entries.isEmpty()) {
            return;
        }

        if (m_BatchUpload && m_BatchUpload->isClipboardOffer) {
            const QString oldId = m_BatchUpload->remoteId;
            m_BatchUpload.reset();
            if (!oldId.isEmpty() && oldId != offer.offerId) {
                send("POST", QStringLiteral("/sds/v1/clipboard-offer/%1/cancel").arg(oldId),
                     QUrlQuery(), QByteArray(), QByteArray(), {}, kJsonTimeoutMs, kMaxJsonResponse, [](const HttpResult&) {});
            }
        }
        m_ClientOfferId = offer.offerId;

        QJsonArray entriesArr;
        for (const ClipboardEntryMeta& entry : offer.entries) {
            entriesArr.append(QJsonObject{
                {QStringLiteral("id"), entry.id},
                {QStringLiteral("name"), entry.name},
                {QStringLiteral("size"), static_cast<double>(entry.size)}
            });
        }
        QJsonObject body{
            {QStringLiteral("offer_id"), offer.offerId},
            {QStringLiteral("kind"), offer.kind},
            {QStringLiteral("entries"), entriesArr}
        };
        if (offer.kind == QLatin1String("image") && !offer.mime.isEmpty()) {
            body.insert(QStringLiteral("mime"), offer.mime);
        }

        const QString offerId = offer.offerId;
        send("PUT", QStringLiteral("/sds/v1/clipboard-offer"), QUrlQuery(),
             QJsonDocument(body).toJson(QJsonDocument::Compact),
             QByteArrayLiteral("application/json"), {}, kClipboardTimeoutMs, kMaxJsonResponse,
             [this, offerId](const HttpResult& r) {
            if (r.pinFailure) {
                pinFailure();
                return;
            }
            if (!r.ok() && m_ClientOfferId == offerId) {
                m_ClientOfferId.clear();
            }
        });
    }

    void doCancelLazyClipboardOffer(const QString& offerId)
    {
        if (!isSafeSdsToken(offerId)) {
            return;
        }
        if (m_BatchUpload && m_BatchUpload->isClipboardOffer && m_BatchUpload->remoteId == offerId) {
            m_BatchUpload.reset();
        }
        if (m_ClientOfferId == offerId) {
            m_ClientOfferId.clear();
        }
        if (canRun()) {
            send("POST", QStringLiteral("/sds/v1/clipboard-offer/%1/cancel").arg(offerId),
                 QUrlQuery(), QByteArray(), QByteArray(), {}, kJsonTimeoutMs, kMaxJsonResponse,
                 [](const HttpResult&) {});
        }
    }

    void doFulfillLazyClipboardPull(const LazyClipboardOffer& offer)
    {
        if (!lazyClipboardUsable() || !m_Caps.has(kPermClipboardWrite) || !m_Caps.has(kPermFileUpload)) {
            return;
        }
        if (offer.offerId != m_ClientOfferId || offer.entries.isEmpty()) {
            return;
        }
        if (m_BatchUpload && m_BatchUpload->isClipboardOffer) {
            m_BatchUpload.reset();
        }
        startBatchUpload(true, offer.offerId, 0.5, 0.5, offer.entries);
    }

    void doFetchLazyClipboardOffer(const LazyClipboardOffer& offer,
                                   const QString& stageDir,
                                   std::shared_ptr<LazyTransferResult> result)
    {
        auto completeFail = [&offer, &result, this](const QString& msg) {
            if (result) {
                {
                    std::lock_guard<std::mutex> lock(result->mutex);
                    result->ok = false;
                    result->error = msg;
                    result->finished = true;
                }
                result->cv.notify_all();
            }
            ClientEvent ev;
            ev.type = ClientEvent::LazyClipboardFinished;
            ev.offerId = offer.offerId;
            ev.lazyOffer = offer;
            ev.ok = false;
            ev.message = msg;
            emitEvent(ev);
        };

        if (!canRun() || !lazyClipboardUsable() ||
                !m_Caps.has(kPermClipboardRead) || !m_Caps.has(kPermFileDownload)) {
            completeFail(QStringLiteral("Lazy clipboard is not available"));
            return;
        }
        if (!isSafeSdsToken(offer.offerId) || offer.entries.isEmpty()) {
            completeFail(QStringLiteral("Invalid clipboard offer"));
            return;
        }

        abortLazyFetchLocked(QStringLiteral("Superseded"));

        auto fetch = std::make_unique<LazyFetch>();
        fetch->offer = offer;
        fetch->stageDir = stageDir;
        fetch->result = std::move(result);
        for (const ClipboardEntryMeta& entry : offer.entries) {
            fetch->totalBytes += entry.size;
        }

        const qint64 maxMemImage = std::min(m_Caps.limits.clipboardImageMax, kClientImageMax);
        const bool needsStageFiles = (offer.kind == QLatin1String("files")) ||
                                     (offer.kind == QLatin1String("image") && fetch->totalBytes > maxMemImage);
        if (needsStageFiles) {
            if (stageDir.isEmpty()) {
                completeFail(QStringLiteral("Missing clipboard staging directory"));
                return;
            }
            fetch->dir = std::make_unique<DownloadDir>(stageDir);
            QString err;
            if (!fetch->dir->open(&err)) {
                completeFail(err.isEmpty() ? QStringLiteral("Cannot open clipboard stage") : err);
                return;
            }
        }

        m_LazyFetch = std::move(fetch);
        startNextLazyFetchEntry();
    }

private:
    struct ClipPut
    {
        quint64 seq;
        QByteArray contentType;
        QByteArray data;
        int attempts;
    };

    struct HostClip
    {
        quint64 seq = 0;
        QStringList types;
    };

    struct Upload
    {
        QString path;
        QString name;     // sanitized name sent to the host
        QString display;
        QFile file;
        qint64 size = 0;
        QCryptographicHash firstPass { QCryptographicHash::Sha256 };
        qint64 hashed = 0;
        QByteArray sha;   // lower-case hex of the first pass
        QString id;
        qint64 chunk = 0;
        qint64 offset = 0;
        // Hash over the bytes the host acknowledged, in order. Invalidated if the host
        // asks us to resend from an earlier offset.
        QCryptographicHash acked { QCryptographicHash::Sha256 };
        bool ackedValid = true;
        int attempts = 0;
        int conflicts = 0;
        int createAttempts = 0;
        int completeAttempts = 0;
        int lastPercent = -1;
    };

    struct OutboxEntry
    {
        QString id;
        QString name;
        qint64 size;
    };

    struct Download
    {
        OutboxEntry entry;
        QString sanitized;
        int fd = -1;
        QString partName;
        qint64 offset = 0;
        qint64 rangeStart = 0;
        qint64 rangeExpected = 0;
        qint64 rangeReceived = 0;
        // One range (at most chunk_max, clamped to 16 MiB) is kept in memory and only
        // written and hashed once it is complete, so a failed range is simply dropped.
        QByteArray rangeData;
        bool headersChecked = false;
        QString failure;
        QByteArray expectedSha;
        QCryptographicHash hash { QCryptographicHash::Sha256 };
        int attempts = 0;
        int lastPercent = -1;
    };

    using Callback = std::function<void(const HttpResult&)>;

    bool canRun() const { return !m_Shutdown && !m_Stopped && m_Nam != nullptr; }

    // ---- events to the listener ---------------------------------------------------------

    void emitEvent(const ClientEvent& ev)
    {
        if (!m_Shutdown && m_Listener != nullptr) {
            m_Listener->onSdsClientEvent(ev);
        }
    }

    void notice(const QString& message)
    {
        if (message.isEmpty()) {
            return;
        }
        qInfo() << "SDS:" << message;
        ClientEvent ev;
        ev.type = ClientEvent::Notice;
        ev.message = message;
        emitEvent(ev);
    }

    void noticeOnce(const QString& key, const QString& message)
    {
        if (!m_NoticeKeys.contains(key)) {
            m_NoticeKeys.insert(key);
            notice(message);
        }
    }

    void stopForGood(const QString& message, bool visible)
    {
        if (m_Stopped) {
            return;
        }
        m_Stopped = true;
        m_CapsInFlight = false;
        m_CapsPending = false;
        qInfo() << "SDS: stopping:" << message;

        abortAll();
        if (m_Upload) {
            ClientEvent ev;
            ev.type = ClientEvent::TransferFinished;
            ev.upload = true;
            ev.name = m_Upload->display;
            ev.message = message;
            emitEvent(ev);
            m_Upload.reset();
        }
        if (m_Download) {
            if (m_Dir) {
                m_Dir->discardPartFile(m_Download->fd, m_Download->partName);
            }
            m_Download.reset();
        }
        m_UploadQueue.clear();
        m_DownloadQueue.clear();
        m_ClipQueue.clear();
        m_DropQueue.clear();
        m_BatchUpload.reset();
        abortLazyFetchLocked(message.isEmpty() ? QStringLiteral("Stream ended") : message);
        m_ClientOfferId.clear();

        if (visible) {
            notice(message);
        }

        ClientEvent ev;
        ev.type = ClientEvent::Stopped;
        ev.message = message;
        emitEvent(ev);
    }

    void pinFailure()
    {
        stopForGood(QStringLiteral("Host certificate does not match the paired host"), true);
    }

    int backoffMs(int failures) const
    {
        qint64 ms = m_Cfg.retryBaseMs;
        for (int i = 1; i < failures && ms < m_Cfg.retryMaxMs; i++) {
            ms *= 2;
        }
        return static_cast<int>(std::min<qint64>(ms, m_Cfg.retryMaxMs));
    }

    int retryAfterMs(const HttpResult& r, int fallbackMs) const
    {
        if (r.retryAfterSec >= 0) {
            return std::min(r.retryAfterSec, m_Cfg.retryAfterMaxSec) * 1000;
        }
        return fallbackMs;
    }

    template <typename F> void later(int ms, F&& f)
    {
        QTimer::singleShot(ms, this, [this, f = std::forward<F>(f)]() {
            if (!m_Shutdown && !m_Stopped) {
                f();
            }
        });
    }

    // ---- HTTP plumbing ------------------------------------------------------------------

    QNetworkRequest makeRequest(const QString& path, const QUrlQuery& query, int timeoutMs) const
    {
        QUrl url;
        url.setScheme(QStringLiteral("https"));
        url.setHost(m_Cfg.host);
        url.setPort(m_Cfg.httpsPort);
        url.setPath(path);
        if (!query.isEmpty()) {
            url.setQuery(query);
        }

        QNetworkRequest req(url);
        req.setSslConfiguration(m_SslConfig);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        req.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
        req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        req.setHeader(QNetworkRequest::UserAgentHeader, QByteArrayLiteral("Moonlight-SDS/1"));
        if (timeoutMs > 0) {
            // Idle timeout: aborts if no data moves for this long
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
            req.setTransferTimeout(timeoutMs);
#else
            Q_UNUSED(timeoutMs);
#endif
        }
        return req;
    }

    void attachPinning(QNetworkReply* reply)
    {
        CertPinning::enforce(reply, m_Cfg.serverCert);
    }

    void track(QNetworkReply* reply, qint64 maxBody)
    {
        m_Replies.insert(reply);
        attachPinning(reply);
        if (maxBody > 0) {
            reply->setReadBufferSize(maxBody + 1);
            connect(reply, &QIODevice::readyRead, this, [reply, maxBody]() {
                if (reply->bytesAvailable() > maxBody) {
                    reply->setProperty(kTooLargeProp, true);
                    reply->abort();
                }
            });
            connect(reply, &QNetworkReply::downloadProgress, this, [reply, maxBody](qint64 received, qint64 total) {
                if (received > maxBody || total > maxBody) {
                    reply->setProperty(kTooLargeProp, true);
                    reply->abort();
                }
            });
        }
    }

    HttpResult collect(QNetworkReply* reply, bool readBody) const
    {
        HttpResult r;
        r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.netError = reply->error();
        r.pinFailure = CertPinning::wasRejected(reply);
        r.tooLarge = reply->property(kTooLargeProp).toBool();
        if (r.pinFailure) {
            r.status = 0;
        }

        const auto pairs = reply->rawHeaderPairs();
        for (const auto& p : pairs) {
            r.headers.insert(p.first.toLower(), p.second);
        }

        bool ok;
        int ra = r.header("retry-after").trimmed().toInt(&ok);
        if (ok && ra >= 0) {
            r.retryAfterSec = ra;
        }

        if (readBody && !r.tooLarge) {
            r.body = reply->readAll();
            // Parse JSON even if the host omits the content type; raw bodies (clipboard
            // content, file data) are still taken from r.body.
            if (!r.body.isEmpty() && r.body.size() <= kMaxJsonResponse &&
                    (r.header("content-type").startsWith("application/json") || r.body.trimmed().startsWith('{'))) {
                QJsonParseError err;
                QJsonDocument doc = QJsonDocument::fromJson(r.body, &err);
                if (err.error == QJsonParseError::NoError && doc.isObject()) {
                    r.json = doc.object();
                    r.errorCode = r.json.value(QStringLiteral("error")).toString();
                }
            }
        }
        return r;
    }

    QNetworkReply* send(const QByteArray& verb, const QString& path, const QUrlQuery& query,
                        const QByteArray& body, const QByteArray& contentType,
                        const QList<QPair<QByteArray, QByteArray>>& headers,
                        int timeoutMs, qint64 maxBody, Callback cb)
    {
        QNetworkRequest req = makeRequest(path, query, timeoutMs);
        if (!contentType.isEmpty()) {
            req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
        }
        for (const auto& h : headers) {
            req.setRawHeader(h.first, h.second);
        }

        QNetworkReply* reply;
        if (verb == "GET") {
            reply = m_Nam->get(req);
        }
        else if (verb == "DELETE") {
            reply = m_Nam->deleteResource(req);
        }
        else if (verb == "POST") {
            // QNAM sends Content-Length for byte-array bodies (never chunked encoding)
            reply = m_Nam->post(req, body);
        }
        else if (verb == "PUT") {
            reply = m_Nam->put(req, body);
        }
        else {
            reply = m_Nam->sendCustomRequest(req, verb, body);
        }

        track(reply, maxBody);
        connect(reply, &QNetworkReply::finished, this, [this, reply, cb = std::move(cb)]() {
            m_Replies.remove(reply);
            reply->deleteLater();
            if (m_Shutdown || m_Stopped) {
                return;
            }
            cb(collect(reply, true));
        });
        return reply;
    }

    void abortAll()
    {
        const auto replies = m_Replies;
        for (QNetworkReply* r : replies) {
            r->abort();
        }
        m_Replies.clear();
        m_PollReply = nullptr;
    }

    // ---- capabilities and events --------------------------------------------------------

    void fetchCapabilities(bool initial)
    {
        if (m_CapsInFlight) {
            if (!initial) {
                m_CapsPending = true;
            }
            return;
        }
        m_CapsInFlight = true;
        m_CapsPending = false;
        sendCapabilities(initial);
    }

    void sendCapabilities(bool initial)
    {
        if (!canRun()) {
            m_CapsInFlight = false;
            m_CapsPending = false;
            return;
        }
        send("GET", QStringLiteral("/sds/v1/capabilities"), QUrlQuery(), QByteArray(), QByteArray(), {},
             kJsonTimeoutMs, kMaxJsonResponse, [this, initial](const HttpResult& r) {
            if (r.pinFailure) {
                m_CapsInFlight = false;
                m_CapsPending = false;
                pinFailure();
                return;
            }

            if (r.status == 200) {
                Capabilities caps;
                if (!parseCapabilities(r.json, caps)) {
                    m_CapsInFlight = false;
                    m_CapsPending = false;
                    stopForGood(QStringLiteral("Unsupported clipboard/file transfer protocol version"), true);
                    return;
                }
                if (caps.streamActive) {
                    m_Caps = caps;
                    m_HaveCaps = true;
                    m_CapsFailures = 0;
                    m_CapsInFlight = false;
                    if (initial) {
                        // Immediate poll: picks up the initial host clipboard announcement
                        poll(0);
                        startNextUpload();
                        startNextDrop();
                    }
                    else {
                        m_FileEpoch.clear();
                        ++m_FileRevision;
                        m_PendingFiles.clear();
                        m_ClientOfferId.clear();
                        if (m_BatchUpload && m_BatchUpload->isClipboardOffer) {
                            m_BatchUpload.reset();
                        }
                        ClientEvent ev;
                        ev.type = ClientEvent::CapabilitiesChanged;
                        ev.caps = m_Caps;
                        emitEvent(ev);
                    }
                    if (m_CapsPending && canRun()) {
                        m_CapsPending = false;
                        fetchCapabilities(false);
                    }
                    return;
                }
                // stream_active == false: same as 409 below
            }

            switch (r.status) {
            case 401:
                m_CapsInFlight = false;
                m_CapsPending = false;
                stopForGood(QStringLiteral("The host does not recognize this client"), true);
                return;
            case 403:
            case 404:
                m_CapsInFlight = false;
                m_CapsPending = false;
                // Feature switched off on the host; stay quiet until the user asks for it
                stopForGood(QStringLiteral("Clipboard and file transfer are disabled on the host"), false);
                return;
            case 429:
                later(retryAfterMs(r, backoffMs(++m_CapsFailures)), [this, initial]() { sendCapabilities(initial); });
                return;
            default:
                // 409 no_active_stream, transport errors, 5xx
                later(backoffMs(++m_CapsFailures), [this, initial]() { sendCapabilities(initial); });
                return;
            }
        });
    }

    void poll(int timeoutSec)
    {
        if (!canRun() || m_PollReply != nullptr) {
            return;
        }

        QUrlQuery q;
        q.addQueryItem(QStringLiteral("after"), QString::number(m_LastEventId));
        q.addQueryItem(QStringLiteral("timeout"), QString::number(timeoutSec));

        m_PollTimer.start();
        m_PollReply = send("GET", QStringLiteral("/sds/v1/events"), q, QByteArray(), QByteArray(), {},
                           (timeoutSec + 15) * 1000, kMaxJsonResponse, [this, timeoutSec](const HttpResult& r) {
            m_PollReply = nullptr;
            onPollResult(r, timeoutSec);
        });
    }

    void sendReady()
    {
        if (m_ReadySent) {
            return;
        }
        m_ReadySent = true;
        ClientEvent ev;
        ev.type = ClientEvent::Ready;
        ev.caps = m_Caps;
        ev.hostProvidedClipboard = m_InitialClipboardAnnounced;
        emitEvent(ev);
    }

    void onPollResult(const HttpResult& r, int timeoutSec)
    {
        if (r.pinFailure) {
            pinFailure();
            return;
        }

        if (r.status == 200 && r.json.value(QStringLiteral("events")).isArray()) {
            const QJsonArray arr = r.json.value(QStringLiteral("events")).toArray();

            bool advanced = false;
            qint64 lastId = -1;
            if (r.json.contains(QStringLiteral("last_id")) &&
                    jsonToInt64(r.json.value(QStringLiteral("last_id")), &lastId) &&
                    static_cast<quint64>(lastId) < m_LastEventId) {
                // The host restarted its event numbering
                m_LastEventId = 0;
                advanced = true;
            }

            QList<QPair<qint64, QJsonObject>> events;
            for (const QJsonValue& v : arr) {
                if (events.size() >= kMaxEventsPerPoll) {
                    break;
                }
                QJsonObject ev = v.toObject();
                qint64 id;
                if (!jsonToInt64(ev.value(QStringLiteral("id")), &id) || id <= 0) {
                    continue;
                }
                events.append(qMakePair(id, ev));
            }
            std::sort(events.begin(), events.end(),
                      [](const QPair<qint64, QJsonObject>& a, const QPair<qint64, QJsonObject>& b) {
                return a.first < b.first;
            });

            for (const auto& ev : events) {
                if (static_cast<quint64>(ev.first) <= m_LastEventId) {
                    continue;
                }
                m_LastEventId = static_cast<quint64>(ev.first);
                advanced = true;
                handleEvent(ev.second);
                if (!canRun()) {
                    return;
                }
            }

            sendReady();

            const qint64 elapsedMs = m_PollTimer.elapsed();
            if (elapsedMs < kMinPollIntervalMs) {
                ++m_FastPolls;
            }
            else {
                m_FastPolls = 0;
            }

            // A host answering a long poll at once without new events would make the
            // client spin; back off instead.
            if (!advanced && timeoutSec > 0 && elapsedMs < kMinIdlePollMs) {
                later(backoffMs(++m_PollFailures), [this]() { poll(m_Cfg.longPollTimeoutSec); });
                return;
            }

            m_PollFailures = 0;
            if (m_FastPolls >= 2) {
                const int delayMs = std::max(1, kMinPollIntervalMs - static_cast<int>(elapsedMs));
                later(delayMs, [this]() { poll(m_Cfg.longPollTimeoutSec); });
                return;
            }
            poll(m_Cfg.longPollTimeoutSec);
            return;
        }

        const qint64 elapsedMs = m_PollTimer.elapsed();
        if (elapsedMs < kMinPollIntervalMs) {
            ++m_FastPolls;
        }
        else {
            m_FastPolls = 0;
        }

        switch (r.status) {
        case 401:
            stopForGood(QStringLiteral("The host does not recognize this client"), true);
            return;
        case 403:
        case 404:
            stopForGood(QStringLiteral("Clipboard and file transfer are disabled on the host"), false);
            return;
        case 429: {
            int delayMs = retryAfterMs(r, backoffMs(++m_PollFailures));
            if (m_FastPolls >= 2) {
                delayMs = std::max(delayMs, kMinPollIntervalMs);
            }
            later(delayMs, [this]() { poll(m_ReadySent ? m_Cfg.longPollTimeoutSec : 0); });
            return;
        }
        default:
            // 409 no_active_stream (not yet / no longer), transport errors, 5xx, bad JSON
            later(backoffMs(++m_PollFailures), [this]() { poll(m_ReadySent ? m_Cfg.longPollTimeoutSec : 0); });
            return;
        }
    }

    void handleEvent(const QJsonObject& ev)
    {
        const QString type = ev.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("clipboard")) {
            onClipboardEvent(ev);
        }
        else if (type == QLatin1String("outbox")) {
            if (m_Cfg.filesEnabled && m_Cfg.autoDownload && m_Caps.has(kPermFileDownload)) {
                requestListing(false);
            }
        }
        else if (type == QLatin1String("stream_ended")) {
            stopForGood(QStringLiteral("Stream ended"), false);
        }
        else if (type == QLatin1String("clipboard_offer")) {
            onLazyClipboardOfferEvent(ev);
        }
        else if (type == QLatin1String("clipboard_pull")) {
            onLazyClipboardPullEvent(ev);
        }
        else if (type == QLatin1String("capabilities")) {
            fetchCapabilities(false);
        }
        // Unknown event types are ignored (forward compatibility)
    }

    bool lazyClipboardUsable() const
    {
        return canRun() && m_HaveCaps && m_Caps.streamActive &&
               m_Cfg.fileClipboardEnabled && m_Cfg.clipboardEnabled && m_Cfg.filesEnabled &&
               m_Caps.hasFeature("lazy_clipboard");
    }

    void onLazyClipboardOfferEvent(const QJsonObject& ev)
    {
        if (!lazyClipboardUsable() || !m_Caps.has(kPermClipboardRead) || !m_Caps.has(kPermFileDownload)) {
            return;
        }

        const QString offerId = ev.value(QStringLiteral("offer_id")).toString();
        const QString kind = ev.value(QStringLiteral("kind")).toString();
        const QString mime = ev.value(QStringLiteral("mime")).toString();
        const QJsonArray arr = ev.value(QStringLiteral("entries")).toArray();
        if (!isSafeSdsToken(offerId) || arr.isEmpty()) {
            return;
        }
        if (kind == QLatin1String("files")) {
            if (arr.size() > kFileClipboardCountMax) {
                return;
            }
        }
        else if (kind == QLatin1String("image")) {
            if (arr.size() != 1 || mime != QLatin1String("image/png")) {
                return;
            }
        }
        else {
            return;
        }

        LazyClipboardOffer offer;
        offer.offerId = offerId;
        offer.kind = kind;
        offer.mime = mime;
        offer.fromHost = true;

        qint64 total = 0;
        QSet<QString> seenNames;
        for (const QJsonValue& val : arr) {
            if (!val.isObject()) {
                return;
            }
            const QJsonObject obj = val.toObject();
            const QString id = obj.value(QStringLiteral("id")).toString();
            const QString name = sanitizeFileName(obj.value(QStringLiteral("name")).toString());
            qint64 size = -1;
            if (!isSafeSdsToken(id) || name.isEmpty() || !jsonToInt64(obj.value(QStringLiteral("size")), &size)) {
                return;
            }
            const QString key = name.normalized(QString::NormalizationForm_C).toCaseFolded();
            if (seenNames.contains(key)) {
                return;
            }
            seenNames.insert(key);
            if (size > m_Caps.limits.fileMax - total) {
                notice(QStringLiteral("Host clipboard item exceeds maximum transfer size"));
                return;
            }
            total += size;
            offer.entries.append({id, name, size, QString()});
        }

        if (!m_ReadySent) {
            m_InitialClipboardAnnounced = true;
        }

        ClientEvent out;
        out.type = ClientEvent::LazyClipboardOfferReceived;
        out.lazyOffer = std::move(offer);
        emitEvent(out);
    }

    void onLazyClipboardPullEvent(const QJsonObject& ev)
    {
        if (!lazyClipboardUsable() || !m_Caps.has(kPermClipboardWrite) || !m_Caps.has(kPermFileUpload)) {
            return;
        }
        const QString offerId = ev.value(QStringLiteral("offer_id")).toString();
        if (!isSafeSdsToken(offerId)) {
            return;
        }
        QStringList targets;
        const QJsonArray arr = ev.value(QStringLiteral("targets")).toArray();
        for (const QJsonValue& t : arr) {
            if (t.isString()) {
                targets.append(t.toString());
            }
        }
        ClientEvent out;
        out.type = ClientEvent::LazyClipboardPullRequested;
        out.offerId = offerId;
        out.pullTargets = std::move(targets);
        emitEvent(out);
    }

    // ---- clipboard ----------------------------------------------------------------------

    void onClipboardEvent(const QJsonObject& ev)
    {
        if (!m_Cfg.clipboardEnabled || !m_Caps.has(kPermClipboardRead)) {
            return;
        }

        qint64 seq;
        if (!jsonToInt64(ev.value(QStringLiteral("seq")), &seq) || seq <= 0) {
            return;
        }

        const QJsonArray types = ev.value(QStringLiteral("types")).toArray();
        const QJsonObject sizes = ev.value(QStringLiteral("sizes")).toObject();

        QStringList wanted;
        for (const QString& t : { kTypeText, kTypePng }) {
            if (!types.contains(QJsonValue(t))) {
                continue;
            }
            bool image = t == kTypePng;
            qint64 limit = image ? m_Caps.limits.clipboardImageMax : m_Caps.limits.clipboardTextMax;
            qint64 size;
            if (jsonToInt64(sizes.value(t), &size) && size > limit) {
                notice(image ? QStringLiteral("Host clipboard image too large") :
                               QStringLiteral("Host clipboard text too large"));
                continue;
            }
            wanted.append(t);
        }
        if (wanted.isEmpty()) {
            return;
        }

        if (!m_ReadySent) {
            m_InitialClipboardAnnounced = true;
        }

        m_PendingHostClip.seq = static_cast<quint64>(seq);
        m_PendingHostClip.types = wanted;
        m_HostClipPending = true;
        if (!m_HostClipBusy) {
            startHostClipboardFetch();
        }
    }

    void startHostClipboardFetch()
    {
        if (!m_HostClipPending) {
            m_HostClipBusy = false;
            return;
        }
        m_ActiveHostClip = m_PendingHostClip;
        m_HostClipPending = false;
        m_HostClipBusy = true;
        m_HostClipTypeIndex = 0;
        m_HostClipAttempts = 0;
        m_HostClipItem = ClipboardItem();
        fetchNextHostClipType();
    }

    void abandonHostClip()
    {
        m_HostClipBusy = false;
        startHostClipboardFetch();
    }

    void fetchNextHostClipType()
    {
        if (!canRun()) {
            return;
        }

        // A newer announcement supersedes the item being fetched
        if (m_HostClipPending) {
            abandonHostClip();
            return;
        }

        if (m_HostClipTypeIndex >= m_ActiveHostClip.types.size()) {
            if (!m_HostClipItem.isEmpty()) {
                ClientEvent ev;
                ev.type = ClientEvent::ClipboardReceived;
                ev.clipboard = m_HostClipItem;
                ev.hostSeq = m_ActiveHostClip.seq;
                emitEvent(ev);
            }
            abandonHostClip();
            return;
        }

        const QString type = m_ActiveHostClip.types.at(m_HostClipTypeIndex);
        const bool image = type == kTypePng;
        const qint64 limit = image ? m_Caps.limits.clipboardImageMax : m_Caps.limits.clipboardTextMax;
        const quint64 seq = m_ActiveHostClip.seq;

        QUrlQuery q;
        q.addQueryItem(QStringLiteral("seq"), QString::number(seq));
        q.addQueryItem(QStringLiteral("type"), type);

        send("GET", QStringLiteral("/sds/v1/clipboard"), q, QByteArray(), QByteArray(), {},
             kClipboardTimeoutMs, limit, [this, image, limit, seq](const HttpResult& r) {
            if (r.pinFailure) {
                pinFailure();
                return;
            }
            if (r.tooLarge || r.status == 413) {
                notice(image ? QStringLiteral("Host clipboard image too large") :
                               QStringLiteral("Host clipboard text too large"));
                m_HostClipTypeIndex++;
                fetchNextHostClipType();
                return;
            }

            switch (r.status) {
            case 200: {
                QByteArray hostSeq = r.header("x-sds-host-seq").trimmed();
                if (hostSeq.isEmpty() || hostSeq.toULongLong() != seq) {
                    // Content for a different clipboard generation: wait for the next event
                    abandonHostClip();
                    return;
                }
                ClipboardCheck check = image ? validateClipboardPng(r.body, limit) :
                                               validateClipboardText(r.body, limit);
                if (check == ClipboardCheck::Ok) {
                    (image ? m_HostClipItem.png : m_HostClipItem.text) = r.body;
                }
                else if (check != ClipboardCheck::Empty) {
                    notice(QStringLiteral("Rejected host clipboard: %1").arg(describeClipboardCheck(check, image)));
                }
                m_HostClipTypeIndex++;
                m_HostClipAttempts = 0;
                fetchNextHostClipType();
                return;
            }
            case 410:
                // Changed again since the announcement; the next event brings the new seq
                abandonHostClip();
                return;
            case 404:
                m_HostClipTypeIndex++;
                fetchNextHostClipType();
                return;
            case 403:
                noticeOnce(QStringLiteral("clipboard_read"), QStringLiteral("The host does not allow reading its clipboard"));
                abandonHostClip();
                return;
            case 429:
                if (++m_HostClipAttempts <= kMaxClipboardAttempts) {
                    later(retryAfterMs(r, 1000), [this]() { fetchNextHostClipType(); });
                    return;
                }
                abandonHostClip();
                return;
            default:
                qWarning() << "SDS: clipboard fetch failed:" << r.status << r.netError << r.errorCode;
                abandonHostClip();
                return;
            }
        });
    }

    void processClipQueue()
    {
        if (m_ClipBusy || m_ClipQueue.isEmpty() || !canRun()) {
            return;
        }

        ClipPut put = m_ClipQueue.takeFirst();
        m_ClipBusy = true;

        QList<QPair<QByteArray, QByteArray>> headers;
        headers.append(qMakePair(QByteArrayLiteral("X-SDS-Client-Seq"), QByteArray::number(put.seq)));

        send("PUT", QStringLiteral("/sds/v1/clipboard"), QUrlQuery(), put.data, put.contentType, headers,
             kClipboardTimeoutMs, kMaxJsonResponse, [this, put](const HttpResult& r) mutable {
            if (r.pinFailure) {
                pinFailure();
                return;
            }

            bool retry = false;
            int delayMs = 1000;
            if (r.ok()) {
                // Sent. X-SDS-Host-Seq is informational for the client.
            }
            else if (r.status == 403) {
                noticeOnce(QStringLiteral("clipboard_write"), QStringLiteral("The host does not allow setting its clipboard"));
                m_ClipQueue.clear();
            }
            else if (r.status == 413) {
                notice(QStringLiteral("Clipboard too large for the host"));
            }
            else if (r.status == 415 || r.status == 400) {
                notice(QStringLiteral("The host rejected the clipboard content"));
            }
            else if (r.status == 429) {
                retry = true;
                delayMs = retryAfterMs(r, 1000);
            }
            else if (r.status == 0 || r.status >= 500) {
                retry = true;
            }
            // 409 no_active_stream: nothing to do

            if (retry && ++put.attempts < kMaxClipboardAttempts) {
                // Keep m_ClipBusy set so the representations stay in order
                later(delayMs, [this, put]() {
                    m_ClipBusy = false;
                    bool superseded = !m_ClipQueue.isEmpty() && m_ClipQueue.first().seq > put.seq;
                    if (!superseded && put.seq == m_ClientSeq) {
                        m_ClipQueue.prepend(put);
                    }
                    processClipQueue();
                });
                return;
            }

            m_ClipBusy = false;
            processClipQueue();
        });
    }

    // ---- uploads ------------------------------------------------------------------------

    void emitProgress(const QString& name, bool upload, int percent)
    {
        ClientEvent ev;
        ev.type = ClientEvent::TransferProgress;
        ev.name = name;
        ev.upload = upload;
        ev.percent = percent;
        emitEvent(ev);
    }

    void startNextUpload()
    {
        while (!m_Upload && !m_UploadQueue.isEmpty() && m_HaveCaps && canRun()) {
            const QString path = m_UploadQueue.takeFirst();
            QFileInfo fi(path);

            if (!m_Caps.has(kPermFileUpload)) {
                notice(QStringLiteral("The host does not allow sending files"));
                continue;
            }

            // The sanitized name is what the host stores; it is also safe to display
            const QString name = sanitizeFileName(fi.fileName());
            if (name.isEmpty()) {
                notice(QStringLiteral("Cannot send a file with an invalid name"));
                continue;
            }
            const QString display = elide(name);

            auto u = std::make_unique<Upload>();
            u->path = path;
            u->name = name;
            u->display = display;

#ifdef Q_OS_UNIX
            QString verifiedName;
            qint64 verifiedSize = 0;
            QString openErr;
            int fd = openVerifiedSourceFile(path, &verifiedName, &verifiedSize, false, &openErr);
            if (fd < 0) {
                notice(openErr.isEmpty() ? QStringLiteral("Cannot read %1").arg(display) : openErr);
                continue;
            }
            if (!u->file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
                ::close(fd);
                notice(QStringLiteral("Cannot read %1").arg(display));
                continue;
            }
            u->size = verifiedSize;
#else
            // Refuse directories and special files before open(): opening a FIFO would block
            if (!fi.isFile()) {
                notice(QStringLiteral("%1 is not a regular file").arg(display));
                continue;
            }
            u->file.setFileName(path);
            if (!u->file.open(QIODevice::ReadOnly)) {
                notice(QStringLiteral("Cannot read %1").arg(display));
                continue;
            }
            u->size = u->file.size();
#endif

            if (u->size > m_Caps.limits.fileMax) {
                notice(QStringLiteral("%1 is too large to send (limit %2)").arg(display, formatBytes(m_Caps.limits.fileMax)));
                continue;
            }

            m_Upload = std::move(u);
            emitProgress(m_Upload->display, true, 0);
            later(0, [this]() { uploadHashStep(); });
        }
    }

    void uploadFail(const QString& message, bool cancelOnHost)
    {
        if (!m_Upload) {
            return;
        }

        if (cancelOnHost && !m_Upload->id.isEmpty()) {
            send("DELETE", QStringLiteral("/sds/v1/files/uploads/") + m_Upload->id, QUrlQuery(),
                 QByteArray(), QByteArray(), {}, kJsonTimeoutMs, kMaxJsonResponse, [](const HttpResult&) {});
        }

        ClientEvent ev;
        ev.type = ClientEvent::TransferFinished;
        ev.upload = true;
        ev.ok = false;
        ev.name = m_Upload->display;
        ev.message = message;
        m_Upload.reset();
        emitEvent(ev);

        startNextUpload();
    }

    // First pass: the host needs the SHA-256 in the create request. Hashed in slices so the
    // worker keeps serving the event poll and clipboard while a large file is read.
    void uploadHashStep()
    {
        Upload* u = m_Upload.get();
        if (u == nullptr) {
            return;
        }

        qint64 want = std::min(kHashSliceBytes, u->size - u->hashed);
        if (want > 0) {
            QByteArray buf = u->file.read(want);
            if (buf.size() != want) {
                uploadFail(QStringLiteral("Cannot read %1").arg(u->display), false);
                return;
            }
            u->firstPass.addData(buf);
            u->hashed += buf.size();
        }

        if (u->hashed < u->size) {
            later(0, [this]() { uploadHashStep(); });
            return;
        }

        u->sha = u->firstPass.result().toHex();
        uploadCreate();
    }

    void uploadCreate()
    {
        Upload* u = m_Upload.get();
        QJsonObject o;
        o.insert(QStringLiteral("name"), u->name);
        o.insert(QStringLiteral("size"), static_cast<double>(u->size));
        o.insert(QStringLiteral("sha256"), QString::fromLatin1(u->sha));

        send("POST", QStringLiteral("/sds/v1/files/uploads"), QUrlQuery(),
             QJsonDocument(o).toJson(QJsonDocument::Compact), QByteArrayLiteral("application/json"), {},
             kJsonTimeoutMs, kMaxJsonResponse, [this](const HttpResult& r) {
            Upload* u = m_Upload.get();
            if (u == nullptr) {
                return;
            }
            if (r.pinFailure) {
                pinFailure();
                return;
            }

            if (r.ok()) {
                QString id = r.json.value(QStringLiteral("id")).toString();
                qint64 chunk = 0, offset = 0;
                if (!isLowerHex(id, 32)) {
                    uploadFail(QStringLiteral("Unexpected response from the host"), false);
                    return;
                }
                if (!jsonToInt64(r.json.value(QStringLiteral("chunk_max")), &chunk) || chunk <= 0) {
                    chunk = m_Caps.limits.chunkMax;
                }
                if (!jsonToInt64(r.json.value(QStringLiteral("offset")), &offset) || offset > u->size) {
                    offset = 0;
                }
                u->id = id;
                u->chunk = std::min({ chunk, m_Caps.limits.chunkMax, kClientChunkMax });
                u->offset = offset;
                if (offset != 0) {
                    u->ackedValid = false;
                }
                uploadSendChunk();
                return;
            }

            if (r.status == 403) {
                uploadFail(QStringLiteral("The host does not allow sending files"), false);
            }
            else if (r.status == 413) {
                uploadFail(QStringLiteral("%1 is too large for the host").arg(u->display), false);
            }
            else if (r.status == 507 || r.errorCode == QLatin1String("insufficient_space")) {
                uploadFail(QStringLiteral("The host does not have enough free space for %1").arg(u->display), false);
            }
            else if ((r.status == 429 || r.status == 0 || r.status >= 500) && ++u->createAttempts < kMaxCreateAttempts) {
                int delay = r.status == 429 ? retryAfterMs(r, 2000) : backoffMs(u->createAttempts);
                later(delay, [this]() {
                    if (m_Upload) {
                        uploadCreate();
                    }
                });
            }
            else if (r.status == 409) {
                uploadFail(QStringLiteral("Cannot send %1: the stream is not active").arg(u->display), false);
            }
            else {
                uploadFail(QStringLiteral("Cannot send %1 (host error %2)").arg(u->display).arg(r.status), false);
            }
        });
    }

    void uploadSendChunk()
    {
        Upload* u = m_Upload.get();
        if (u == nullptr || !canRun()) {
            return;
        }

        if (u->offset >= u->size) {
            uploadComplete();
            return;
        }

        const qint64 start = u->offset;
        const qint64 len = std::min(u->chunk, u->size - start);
        if (!u->file.seek(start)) {
            uploadFail(QStringLiteral("Cannot read %1").arg(u->display), true);
            return;
        }
        const QByteArray data = u->file.read(len);
        if (data.size() != len) {
            uploadFail(QStringLiteral("%1 changed while it was being sent").arg(u->display), true);
            return;
        }

        QUrlQuery q;
        q.addQueryItem(QStringLiteral("offset"), QString::number(start));

        send("PUT", QStringLiteral("/sds/v1/files/uploads/") + u->id, q, data,
             QByteArrayLiteral("application/octet-stream"), {}, kChunkIdleTimeoutMs, kMaxJsonResponse,
             [this, start, data](const HttpResult& r) {
            Upload* u = m_Upload.get();
            if (u == nullptr) {
                return;
            }
            if (r.pinFailure) {
                pinFailure();
                return;
            }

            const qint64 end = start + data.size();
            qint64 hostOffset = -1;
            bool haveOffset = jsonToInt64(r.json.value(QStringLiteral("offset")), &hostOffset);

            if (r.ok() || (r.status == 409 && r.errorCode != QLatin1String("no_active_stream"))) {
                if (!haveOffset || hostOffset > u->size) {
                    uploadFail(QStringLiteral("Unexpected response from the host"), true);
                    return;
                }
                if (hostOffset == end) {
                    // Our chunk is on the host (for a 409: it arrived but the reply was lost)
                    if (u->ackedValid) {
                        u->acked.addData(data);
                    }
                }
                else {
                    // Resynchronize from the host's offset
                    if (hostOffset != start || r.ok()) {
                        u->ackedValid = false;
                    }
                    if (++u->conflicts > kMaxUploadConflicts) {
                        uploadFail(QStringLiteral("Sending %1 failed: the host keeps losing data").arg(u->display), true);
                        return;
                    }
                }
                u->offset = hostOffset;
                u->attempts = 0;

                int percent = u->size > 0 ? static_cast<int>(u->offset * 100 / u->size) : 100;
                if (percent != u->lastPercent) {
                    u->lastPercent = percent;
                    emitProgress(u->display, true, percent);
                }
                uploadSendChunk();
                return;
            }

            if (r.status == 404) {
                uploadFail(QStringLiteral("The host discarded the upload of %1").arg(u->display), false);
            }
            else if (r.status == 409) {
                uploadFail(QStringLiteral("Cannot send %1: the stream is not active").arg(u->display), false);
            }
            else if (r.status == 413) {
                uploadFail(QStringLiteral("The host rejected a chunk of %1 as too large").arg(u->display), true);
            }
            else if (r.status == 507 || r.errorCode == QLatin1String("insufficient_space")) {
                uploadFail(QStringLiteral("The host does not have enough free space for %1").arg(u->display), true);
            }
            else if ((r.status == 429 || r.status == 0 || r.status >= 500) && ++u->attempts <= kMaxChunkAttempts) {
                // Network error: retry the same offset; a 409 tells us if it already landed
                int delay = r.status == 429 ? retryAfterMs(r, 1000) : backoffMs(u->attempts);
                later(delay, [this]() { uploadSendChunk(); });
            }
            else {
                uploadFail(QStringLiteral("Sending %1 failed").arg(u->display), true);
            }
        });
    }

    void uploadComplete()
    {
        Upload* u = m_Upload.get();
        if (u->ackedValid && u->acked.result().toHex() != u->sha) {
            uploadFail(QStringLiteral("%1 changed while it was being sent").arg(u->display), true);
            return;
        }

        send("POST", QStringLiteral("/sds/v1/files/uploads/%1/complete").arg(u->id), QUrlQuery(), QByteArray(),
             QByteArrayLiteral("application/octet-stream"), {}, kCompleteTimeoutMs, kMaxJsonResponse,
             [this](const HttpResult& r) {
            Upload* u = m_Upload.get();
            if (u == nullptr) {
                return;
            }
            if (r.pinFailure) {
                pinFailure();
                return;
            }

            if (r.ok()) {
                QString finalName = sanitizeFileName(r.json.value(QStringLiteral("name")).toString());
                ClientEvent ev;
                ev.type = ClientEvent::TransferFinished;
                ev.upload = true;
                ev.ok = true;
                ev.name = finalName.isEmpty() ? u->display : elide(finalName);
                m_Upload.reset();
                emitEvent(ev);
                startNextUpload();
                return;
            }

            if ((r.status == 0 || r.status >= 500) && ++u->completeAttempts < kMaxCompleteAttempts) {
                // The confirmation may have been lost: ask again for the same upload
                later(backoffMs(u->completeAttempts), [this]() {
                    if (m_Upload) {
                        uploadComplete();
                    }
                });
            }
            else if (r.status == 404 && u->completeAttempts > 0) {
                uploadFail(QStringLiteral("%1 may have been sent, but the host did not confirm it").arg(u->display), false);
            }
            else if (r.status == 422) {
                uploadFail(QStringLiteral("The host rejected %1: verification failed").arg(u->display), false);
            }
            else if (r.status == 507 || r.errorCode == QLatin1String("insufficient_space")) {
                uploadFail(QStringLiteral("The host does not have enough free space for %1").arg(u->display), true);
            }
            else {
                uploadFail(QStringLiteral("Sending %1 failed (host error %2)").arg(u->display).arg(r.status), true);
            }
        });
    }

    // ---- downloads ----------------------------------------------------------------------

    void requestListing(bool manual)
    {
        if (!canRun()) {
            return;
        }
        if (!m_Cfg.filesEnabled) {
            if (manual) {
                notice(QStringLiteral("File transfer is disabled"));
            }
            return;
        }
        if (!m_HaveCaps) {
            if (manual) {
                notice(QStringLiteral("File transfer is not ready yet"));
            }
            return;
        }
        if (!m_Caps.has(kPermFileDownload)) {
            if (manual) {
                notice(QStringLiteral("The host does not allow downloading files"));
            }
            return;
        }

        if (manual) {
            // Give earlier failures another chance when the user asks explicitly
            m_FailedIds.clear();
        }

        if (m_ListingBusy) {
            m_ListingAgain = true;
            m_ListingAgainManual = m_ListingAgainManual || manual;
            return;
        }

        // Outbox events are coalesced: at most one automatic listing every 2 s
        if (!manual && m_LastListing.isValid() && m_LastListing.elapsed() < kMinListingIntervalMs) {
            if (!m_ListingTimerPending) {
                m_ListingTimerPending = true;
                later(kMinListingIntervalMs - static_cast<int>(m_LastListing.elapsed()), [this]() {
                    m_ListingTimerPending = false;
                    requestListing(false);
                });
            }
            return;
        }

        m_ListingBusy = true;
        m_LastListing.start();

        send("GET", QStringLiteral("/sds/v1/files/outbox"), QUrlQuery(), QByteArray(), QByteArray(), {},
             kJsonTimeoutMs, kMaxJsonResponse, [this, manual](const HttpResult& r) {
            m_ListingBusy = false;
            if (r.pinFailure) {
                pinFailure();
                return;
            }

            int added = 0;
            if (r.status == 200) {
                const QJsonArray files = r.json.value(QStringLiteral("files")).toArray();
                int considered = 0;
                for (const QJsonValue& v : files) {
                    if (++considered > kMaxOutboxEntries || m_DownloadQueue.size() >= kMaxQueuedDownloads) {
                        break;
                    }
                    if (m_DoneIds.size() + m_FailedIds.size() >= kMaxTrackedOutboxIds) {
                        noticeOnce(QStringLiteral("outbox_limit"), QStringLiteral("Too many host files in this session; reconnect to download more"));
                        break;
                    }
                    QJsonObject f = v.toObject();
                    OutboxEntry e;
                    e.id = f.value(QStringLiteral("id")).toString();
                    e.name = f.value(QStringLiteral("name")).toString();
                    if (!isLowerHex(e.id, 32) || e.name.isEmpty() ||
                            !jsonToInt64(f.value(QStringLiteral("size")), &e.size)) {
                        continue;
                    }
                    if (m_DoneIds.contains(e.id) || m_FailedIds.contains(e.id) || m_QueuedIds.contains(e.id) ||
                            (m_Download && m_Download->entry.id == e.id)) {
                        continue;
                    }
                    m_QueuedIds.insert(e.id);
                    m_DownloadQueue.append(e);
                    added++;
                }
                if (manual && added == 0 && !m_Download) {
                    notice(QStringLiteral("No new files in the host outbox"));
                }
            }
            else if (r.status == 403) {
                noticeOnce(QStringLiteral("file_download"), QStringLiteral("The host does not allow downloading files"));
            }
            else if (r.status == 429) {
                later(retryAfterMs(r, 2000), [this, manual]() { requestListing(manual); });
            }
            else if (r.status != 409) {
                qWarning() << "SDS: outbox listing failed:" << r.status << r.netError;
                if (manual) {
                    notice(QStringLiteral("Cannot list the host outbox"));
                }
            }

            if (m_ListingAgain) {
                bool againManual = m_ListingAgainManual;
                m_ListingAgain = false;
                m_ListingAgainManual = false;
                requestListing(againManual);
            }

            startNextDownload();
        });
    }

    void startNextDownload()
    {
        while (!m_Download && !m_DownloadQueue.isEmpty() && canRun()) {
            OutboxEntry e = m_DownloadQueue.takeFirst();
            m_QueuedIds.remove(e.id);

            const QString sanitized = sanitizeFileName(e.name);
            if (sanitized.isEmpty()) {
                m_FailedIds.insert(e.id);
                notice(QStringLiteral("Skipped a host file with an invalid name"));
                continue;
            }
            const QString display = elide(sanitized);

            if (e.size > m_Caps.limits.fileMax) {
                m_FailedIds.insert(e.id);
                notice(QStringLiteral("%1 is too large to download (limit %2)").arg(display, formatBytes(m_Caps.limits.fileMax)));
                continue;
            }

            QString error;
            if (!m_Dir) {
                m_Dir = std::make_unique<DownloadDir>(m_Cfg.downloadDir);
            }
            if (!m_Dir->open(&error)) {
                m_FailedIds.insert(e.id);
                notice(QStringLiteral("Cannot save downloads: %1").arg(error));
                m_Dir.reset();
                continue;
            }

            QStorageInfo storage(m_Dir->path());
            if (!storage.isValid() || storage.bytesAvailable() < 0 ||
                    storage.bytesAvailable() - e.size < kClientMinFreeSpace) {
                m_FailedIds.insert(e.id);
                notice(QStringLiteral("Not enough disk space to download %1").arg(display));
                continue;
            }

            QString partName;
            int fd = m_Dir->createPartFile(&partName, &error);
            if (fd < 0) {
                m_FailedIds.insert(e.id);
                notice(QStringLiteral("Cannot save %1: %2").arg(display, error));
                continue;
            }

            m_Download = std::make_unique<Download>();
            m_Download->entry = e;
            m_Download->sanitized = sanitized;
            m_Download->fd = fd;
            m_Download->partName = partName;
            emitProgress(display, false, 0);
            downloadRange();
        }
    }

    void downloadFail(const QString& message)
    {
        if (!m_Download) {
            return;
        }
        if (m_Dir) {
            m_Dir->discardPartFile(m_Download->fd, m_Download->partName);
        }
        m_FailedIds.insert(m_Download->entry.id);

        ClientEvent ev;
        ev.type = ClientEvent::TransferFinished;
        ev.upload = false;
        ev.ok = false;
        ev.name = elide(m_Download->sanitized);
        ev.message = message;
        m_Download.reset();
        emitEvent(ev);

        startNextDownload();
    }

    // Checks the response headers once they are known. Returns false (and records a failure)
    // if the host's metadata does not match the listing.
    bool checkDownloadHeaders(QNetworkReply* reply)
    {
        Download* d = m_Download.get();
        if (d->headersChecked) {
            return d->failure.isEmpty();
        }
        d->headersChecked = true;

        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200) {
            return false;
        }

        bool ok;
        const QByteArray sizeHeader = reply->rawHeader("X-SDS-File-Size").trimmed();
        qint64 size = sizeHeader.toLongLong(&ok);
        if ((d->rangeStart == 0 || !sizeHeader.isEmpty()) && (!ok || size != d->entry.size)) {
            d->failure = QStringLiteral("%1 changed on the host; try again").arg(elide(d->sanitized));
            return false;
        }

        if (d->rangeStart == 0) {
            QString sha = QString::fromLatin1(reply->rawHeader("X-SDS-File-Sha256").trimmed()).toLower();
            if (!isLowerHex(sha, 64)) {
                d->failure = QStringLiteral("The host sent %1 without a checksum").arg(elide(d->sanitized));
                return false;
            }
            d->expectedSha = sha.toLatin1();
        }
        return true;
    }

    void drainDownload(QNetworkReply* reply)
    {
        Download* d = m_Download.get();
        if (d == nullptr || !d->failure.isEmpty()) {
            return;
        }
        if (!checkDownloadHeaders(reply)) {
            if (!d->failure.isEmpty()) {
                reply->abort();
            }
            return;
        }

        while (reply->bytesAvailable() > 0) {
            QByteArray buf = reply->read(kReadBufferBytes);
            if (buf.isEmpty()) {
                break;
            }
            if (d->rangeReceived + buf.size() > d->rangeExpected) {
                d->failure = QStringLiteral("The host sent more data than requested for %1").arg(elide(d->sanitized));
                reply->abort();
                return;
            }
            d->rangeData.append(buf);
            d->rangeReceived += buf.size();
        }

        qint64 done = d->rangeStart + d->rangeReceived;
        int percent = d->entry.size > 0 ? static_cast<int>(done * 100 / d->entry.size) : 100;
        if (percent != d->lastPercent) {
            d->lastPercent = percent;
            emitProgress(elide(d->sanitized), false, percent);
        }
    }

    // Writes and hashes a completely received range.
    bool commitRange()
    {
        Download* d = m_Download.get();
#ifdef Q_OS_UNIX
        if (!writeAll(d->fd, d->rangeData.constData(), d->rangeData.size())) {
            d->failure = QStringLiteral("Cannot write %1: %2").arg(elide(d->sanitized), QString::fromLocal8Bit(strerror(errno)));
            return false;
        }
#endif
        d->hash.addData(d->rangeData);
        d->offset += d->rangeData.size();
        d->rangeData.clear();
        return true;
    }

    void downloadRange()
    {
        Download* d = m_Download.get();
        if (d == nullptr || !canRun()) {
            return;
        }

        const qint64 remaining = d->entry.size - d->offset;
        const qint64 chunk = m_Caps.limits.chunkMax;
        const qint64 length = remaining > 0 ? std::min(chunk, remaining) : chunk;

        d->rangeStart = d->offset;
        d->rangeExpected = std::max<qint64>(0, std::min(length, remaining));
        d->rangeReceived = 0;
        d->rangeData.clear();
        d->rangeData.reserve(static_cast<qsizetype>(d->rangeExpected));
        d->headersChecked = false;
        d->failure.clear();

        QUrlQuery q;
        q.addQueryItem(QStringLiteral("offset"), QString::number(d->offset));
        q.addQueryItem(QStringLiteral("length"), QString::number(length));

        QNetworkRequest req = makeRequest(QStringLiteral("/sds/v1/files/outbox/") + d->entry.id, q, kChunkIdleTimeoutMs);
        QNetworkReply* reply = m_Nam->get(req);
        reply->setReadBufferSize(kReadBufferBytes);
        track(reply, 0);

        connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
            if (m_Download && !m_Shutdown) {
                int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status == 200) {
                    drainDownload(reply);
                }
                else if (reply->bytesAvailable() > kMaxJsonResponse) {
                    reply->abort();
                }
            }
        });

        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            m_Replies.remove(reply);
            reply->deleteLater();
            if (m_Shutdown || m_Stopped || !m_Download) {
                return;
            }

            Download* d = m_Download.get();
            int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            bool pin = CertPinning::wasRejected(reply);
            if (pin) {
                pinFailure();
                return;
            }

            if (status == 200 && d->failure.isEmpty()) {
                drainDownload(reply);
            }
            if (!d->failure.isEmpty()) {
                downloadFail(d->failure);
                return;
            }

            if (status == 200 && reply->error() == QNetworkReply::NoError && d->rangeReceived == d->rangeExpected) {
                if (!commitRange()) {
                    downloadFail(d->failure);
                    return;
                }
                d->attempts = 0;
                if (d->offset >= d->entry.size) {
                    downloadFinalize();
                }
                else {
                    downloadRange();
                }
                return;
            }

            HttpResult r = collect(reply, status != 200);
            const QString display = elide(d->sanitized);
            if (status == 404) {
                downloadFail(QStringLiteral("%1 is no longer in the host outbox").arg(display));
            }
            else if (status == 403) {
                noticeOnce(QStringLiteral("file_download"), QStringLiteral("The host does not allow downloading files"));
                downloadFail(QStringLiteral("Downloading %1 is not allowed").arg(display));
            }
            else if (status == 400 || status == 413 || status == 416) {
                downloadFail(QStringLiteral("The host refused to send %1").arg(display));
            }
            else if (++d->attempts <= kMaxChunkAttempts) {
                // Transport error, short read, 429 or 5xx: drop the partial range and retry it
                d->rangeData.clear();
                int delay = status == 429 ? retryAfterMs(r, 1000) : backoffMs(d->attempts);
                later(delay, [this]() { downloadRange(); });
            }
            else {
                downloadFail(QStringLiteral("Downloading %1 failed").arg(display));
            }
        });
    }

    void downloadFinalize()
    {
        Download* d = m_Download.get();
        if (d->hash.result().toHex() != d->expectedSha) {
            downloadFail(QStringLiteral("%1 failed the integrity check and was deleted").arg(elide(d->sanitized)));
            return;
        }

        QString finalName, error;
        int fd = d->fd;
        d->fd = -1; // finalizePartFile always closes it
        if (!m_Dir->finalizePartFile(fd, d->partName, d->sanitized, &finalName, &error)) {
            d->partName.clear();
            downloadFail(QStringLiteral("Cannot save %1: %2").arg(elide(d->sanitized), error));
            return;
        }

        m_DoneIds.insert(d->entry.id);

        ClientEvent ev;
        ev.type = ClientEvent::TransferFinished;
        ev.upload = false;
        ev.ok = true;
        ev.name = elide(finalName);
        ev.path = QDir(m_Dir->path()).filePath(finalName);
        m_Download.reset();
        emitEvent(ev);

        startNextDownload();
    }

    SdsClient::Config m_Cfg;
    ClientListener* m_Listener;
    QNetworkAccessManager* m_Nam = nullptr;
    QSslConfiguration m_SslConfig;
    bool m_Shutdown = false;
    bool m_Stopped = false;
    QSet<QNetworkReply*> m_Replies;
    QSet<QString> m_NoticeKeys;

    Capabilities m_Caps;
    bool m_HaveCaps = false;
    bool m_ReadySent = false;
    bool m_InitialClipboardAnnounced = false;
    QByteArray m_FileEpoch;
    quint64 m_FileRevision = 0;
    quint64 m_FileSeq = 0;
    bool m_FileBusy = false;
    QByteArray m_PendingFiles;
    qint64 m_PendingFileGeneration = -1;
    quint64 m_PendingFileSeq = 0;
    QByteArray m_PendingFileEpoch;
    int m_CapsFailures = 0;
    bool m_CapsInFlight = false;
    bool m_CapsPending = false;

    quint64 m_LastEventId = 0;
    int m_PollFailures = 0;
    int m_FastPolls = 0;
    QNetworkReply* m_PollReply = nullptr;
    QElapsedTimer m_PollTimer;

    QList<ClipPut> m_ClipQueue;
    bool m_ClipBusy = false;
    quint64 m_ClientSeq = 0;

    HostClip m_PendingHostClip;
    bool m_HostClipPending = false;
    HostClip m_ActiveHostClip;
    bool m_HostClipBusy = false;
    int m_HostClipTypeIndex = 0;
    int m_HostClipAttempts = 0;
    ClipboardItem m_HostClipItem;

    QStringList m_UploadQueue;
    std::unique_ptr<Upload> m_Upload;

    QList<OutboxEntry> m_DownloadQueue;
    QSet<QString> m_DoneIds;
    QSet<QString> m_FailedIds;
    QSet<QString> m_QueuedIds;
    bool m_ListingBusy = false;
    bool m_ListingAgain = false;
    bool m_ListingAgainManual = false;
    bool m_ListingTimerPending = false;
    QElapsedTimer m_LastListing;
    struct DropRequest
    {
        double nx = 0.5;
        double ny = 0.5;
        QStringList paths;
    };

    struct BatchEntry
    {
        QString id;
        int index = 0;
        QString name;
        QString path;
        QFile file;
        qint64 size = 0;
        qint64 offset = 0;
        qint64 hashedOffset = 0;
        QCryptographicHash sha { QCryptographicHash::Sha256 };
        QByteArray finalSha;
        bool started = false;
        bool completedHash = false;
        int attempts = 0;
        int conflicts = 0;
    };

    struct BatchUpload
    {
        bool isClipboardOffer = false;
        double nx = 0.5;
        double ny = 0.5;
        QString remoteId;
        QString display;
        qint64 chunkMax = 4194304;
        qint64 totalBytes = 0;
        qint64 uploadedBytes = 0;
        int lastPercent = -1;
        int entryIdx = 0;
        int completeAttempts = 0;
        std::vector<std::unique_ptr<BatchEntry>> entries;
    };

    struct LazyFetch
    {
        LazyClipboardOffer offer;
        QString stageDir;
        std::shared_ptr<LazyTransferResult> result;
        std::unique_ptr<DownloadDir> dir;
        int entryIdx = 0;
        int fd = -1;
        QString partName;
        QString sanitizedName;
        qint64 offset = 0;
        qint64 totalBytes = 0;
        qint64 downloadedBytes = 0;
        int lastPercent = -1;
        int attempts = 0;
        QCryptographicHash fileSha { QCryptographicHash::Sha256 };
        QByteArray imageBuf;
        QStringList stagedPaths;
    };

    void abortLazyFetchLocked(const QString& reason)
    {
        if (!m_LazyFetch) {
            return;
        }
        if (m_LazyFetch->dir && m_LazyFetch->fd >= 0) {
            m_LazyFetch->dir->discardPartFile(m_LazyFetch->fd, m_LazyFetch->partName);
            m_LazyFetch->fd = -1;
        }
        if (m_LazyFetch->result) {
            {
                std::lock_guard<std::mutex> lock(m_LazyFetch->result->mutex);
                if (!m_LazyFetch->result->finished) {
                    m_LazyFetch->result->ok = false;
                    m_LazyFetch->result->error = reason;
                    m_LazyFetch->result->finished = true;
                }
            }
            m_LazyFetch->result->cv.notify_all();
        }
        ClientEvent ev;
        ev.type = ClientEvent::LazyClipboardFinished;
        ev.offerId = m_LazyFetch->offer.offerId;
        ev.lazyOffer = m_LazyFetch->offer;
        ev.ok = false;
        ev.message = reason;
        m_LazyFetch.reset();
        emitEvent(ev);
    }

    void startNextLazyFetchEntry()
    {
        LazyFetch* f = m_LazyFetch.get();
        if (f == nullptr || !canRun()) {
            return;
        }
        if (f->entryIdx >= f->offer.entries.size()) {
            finishLazyFetchSuccess();
            return;
        }
        const ClipboardEntryMeta& entry = f->offer.entries.at(f->entryIdx);
        f->sanitizedName = entry.name;
        f->offset = 0;
        f->attempts = 0;
        f->fileSha.reset();
        f->imageBuf.clear();
        if (f->dir) {
            QString err;
            f->fd = f->dir->createPartFile(&f->partName, &err);
            if (f->fd < 0) {
                abortLazyFetchLocked(err.isEmpty() ? QStringLiteral("Cannot stage clipboard file") : err);
                return;
            }
        }
        else {
            f->imageBuf.reserve(static_cast<qsizetype>(std::min<qint64>(entry.size, kClientImageMax)));
        }
        fetchLazyNextChunk();
    }

    void fetchLazyNextChunk()
    {
        LazyFetch* f = m_LazyFetch.get();
        if (f == nullptr || !canRun()) {
            return;
        }
        const ClipboardEntryMeta& entry = f->offer.entries.at(f->entryIdx);
        const qint64 remaining = entry.size - f->offset;
        const qint64 chunkMax = std::max<qint64>(1, m_Caps.limits.chunkMax);
        const qint64 want = remaining > 0 ? std::min(chunkMax, remaining) : 0;

        QUrlQuery q;
        q.addQueryItem(QStringLiteral("offset"), QString::number(f->offset));
        q.addQueryItem(QStringLiteral("length"), QString::number(std::max<qint64>(1, want)));

        const QString offerId = f->offer.offerId;
        const int idx = f->entryIdx;
        const qint64 offset = f->offset;
        const QString urlPath = QStringLiteral("/sds/v1/clipboard-offer/%1/entries/%2").arg(offerId, entry.id);

        send("GET", urlPath, q, QByteArray(), QByteArray(), {}, kChunkIdleTimeoutMs, chunkMax,
             [this, offerId, idx, offset, want](const HttpResult& r) {
            LazyFetch* f = m_LazyFetch.get();
            if (f == nullptr || f->offer.offerId != offerId || f->entryIdx != idx || f->offset != offset) {
                return;
            }
            if (r.pinFailure) {
                pinFailure();
                return;
            }
            if (r.status == 200 && r.netError == QNetworkReply::NoError && !r.tooLarge) {
                if (r.body.size() != want) {
                    abortLazyFetchLocked(QStringLiteral("Unexpected chunk length from host"));
                    return;
                }
                const ClipboardEntryMeta& entry = f->offer.entries.at(idx);
                bool sizeOk = false;
                const qint64 reportedSize = r.header("x-sds-file-size").trimmed().toLongLong(&sizeOk);
                if (!sizeOk || reportedSize != entry.size) {
                    abortLazyFetchLocked(QStringLiteral("Host clipboard item changed during transfer"));
                    return;
                }
                const QString expectedChunkSha = QString::fromLatin1(r.header("x-sds-chunk-sha256").trimmed()).toLower();
                const QByteArray actualChunkSha = QCryptographicHash::hash(r.body, QCryptographicHash::Sha256).toHex();
                if (!isLowerHex(expectedChunkSha, 64) || actualChunkSha != expectedChunkSha.toLatin1()) {
                    abortLazyFetchLocked(QStringLiteral("Chunk checksum mismatch"));
                    return;
                }

                if (f->dir) {
#ifdef Q_OS_UNIX
                    if (!writeAll(f->fd, r.body.constData(), r.body.size())) {
                        abortLazyFetchLocked(QStringLiteral("Cannot write staged clipboard file"));
                        return;
                    }
#endif
                }
                else {
                    f->imageBuf.append(r.body);
                }
                f->fileSha.addData(r.body);
                f->offset += r.body.size();
                f->downloadedBytes += r.body.size();
                f->attempts = 0;

                const int percent = f->totalBytes > 0 ? static_cast<int>(f->downloadedBytes * 100 / f->totalBytes) : 100;
                if (percent != f->lastPercent) {
                    f->lastPercent = percent;
                    emitProgress(elide(entry.name), false, percent);
                }

                if (f->offset >= entry.size) {
                    const QByteArray expectedFileSha = r.header("x-sds-file-sha256").trimmed().toLower();
                    if (!expectedFileSha.isEmpty()) {
                        if (!isLowerHex(QString::fromLatin1(expectedFileSha), 64) ||
                                f->fileSha.result().toHex() != expectedFileSha) {
                            abortLazyFetchLocked(QStringLiteral("Clipboard item failed SHA-256 verification"));
                            return;
                        }
                    }
                    if (f->dir) {
                        QString finalName;
                        QString err;
                        if (!f->dir->finalizePartFile(f->fd, f->partName, f->sanitizedName, &finalName, &err)) {
                            f->fd = -1;
                            abortLazyFetchLocked(err.isEmpty() ? QStringLiteral("Cannot finalize clipboard file") : err);
                            return;
                        }
                        f->fd = -1;
                        f->stagedPaths.append(QDir(f->stageDir).filePath(finalName));
                    }
                    ++f->entryIdx;
                    startNextLazyFetchEntry();
                }
                else {
                    fetchLazyNextChunk();
                }
                return;
            }

            if ((r.status == 429 || r.status >= 500 || r.status == 0) && ++f->attempts <= kMaxChunkAttempts) {
                const int delay = r.status == 429 ? retryAfterMs(r, backoffMs(f->attempts)) : backoffMs(f->attempts);
                later(delay, [this]() { fetchLazyNextChunk(); });
                return;
            }
            abortLazyFetchLocked(QStringLiteral("Host clipboard item is no longer available"));
        });
    }

    void finishLazyFetchSuccess()
    {
        std::unique_ptr<LazyFetch> f = std::move(m_LazyFetch);
        if (!f) {
            return;
        }
        QByteArray validatedPng;
        QStringList staged = f->stagedPaths;
        if (f->offer.kind == QLatin1String("image") && !f->imageBuf.isEmpty()) {
            const qint64 limit = std::min(m_Caps.limits.clipboardImageMax, kClientImageMax);
            if (validateClipboardPng(f->imageBuf, limit) != ClipboardCheck::Ok) {
                if (f->result) {
                    {
                        std::lock_guard<std::mutex> lock(f->result->mutex);
                        f->result->ok = false;
                        f->result->error = QStringLiteral("Rejected invalid host image");
                        f->result->finished = true;
                    }
                    f->result->cv.notify_all();
                }
                ClientEvent ev;
                ev.type = ClientEvent::LazyClipboardFinished;
                ev.offerId = f->offer.offerId;
                ev.lazyOffer = f->offer;
                ev.ok = false;
                ev.message = QStringLiteral("Rejected invalid host image");
                emitEvent(ev);
                return;
            }
            validatedPng = std::move(f->imageBuf);
        }

        if (f->result) {
            {
                std::lock_guard<std::mutex> lock(f->result->mutex);
                f->result->ok = true;
                f->result->stagedPaths = staged;
                f->result->png = validatedPng;
                f->result->error.clear();
                f->result->finished = true;
            }
            f->result->cv.notify_all();
        }

        ClientEvent ev;
        ev.type = ClientEvent::LazyClipboardFinished;
        ev.offerId = f->offer.offerId;
        ev.lazyOffer = f->offer;
        ev.ok = true;
        ev.stagedPaths = std::move(staged);
        ev.clipboard.png = std::move(validatedPng);
        emitEvent(ev);
    }

    void startNextDrop()
    {
        while (!m_BatchUpload && !m_DropQueue.isEmpty() && m_HaveCaps && canRun()) {
            const DropRequest req = m_DropQueue.takeFirst();
            if (req.paths.size() > kFileClipboardCountMax) {
                notice(QStringLiteral("Drop up to %1 regular files at a time").arg(kFileClipboardCountMax));
                continue;
            }
            QList<ClipboardEntryMeta> entries;
            QSet<QString> seenNames;
            bool invalid = false;
            qint64 total = 0;
            for (int i = 0; i < req.paths.size(); ++i) {
                QString name;
                qint64 size = 0;
#ifdef Q_OS_UNIX
                QString err;
                int fd = openVerifiedSourceFile(req.paths[i], &name, &size, false, &err);
                if (fd < 0) {
                    notice(err.isEmpty() ? QStringLiteral("Cannot drop non-regular file") : err);
                    invalid = true;
                    break;
                }
                ::close(fd);
#else
                const QFileInfo fi(req.paths[i]);
                if (!fi.exists() || !fi.isFile()) {
                    notice(QStringLiteral("Cannot drop non-regular file"));
                    invalid = true;
                    break;
                }
                name = sanitizeFileName(fi.fileName().normalized(QString::NormalizationForm_C));
                if (name.isEmpty()) {
                    notice(QStringLiteral("%1 has an unsupported file name").arg(fi.fileName()));
                    invalid = true;
                    break;
                }
                size = fi.size();
#endif
                QString finalName = name;
                for (int n = 1; n <= kMaxCollisionIndex &&
                        seenNames.contains(finalName.normalized(QString::NormalizationForm_C).toCaseFolded()); ++n) {
                    finalName = collisionName(name, n);
                }
                const QString key = finalName.normalized(QString::NormalizationForm_C).toCaseFolded();
                if (finalName.isEmpty() || seenNames.contains(key)) {
                    notice(QStringLiteral("Too many dropped files with the same name"));
                    invalid = true;
                    break;
                }
                seenNames.insert(key);
                if (size > m_Caps.limits.fileMax - total) {
                    notice(QStringLiteral("Dropped files exceed host size limit (%1)").arg(formatBytes(m_Caps.limits.fileMax)));
                    invalid = true;
                    break;
                }
                total += size;
                entries.append({QString::number(i), finalName, size, req.paths[i]});
            }
            if (invalid || entries.isEmpty()) {
                continue;
            }
            startBatchUpload(false, QString(), req.nx, req.ny, entries);
        }
    }

    void startBatchUpload(bool isClipboardOffer,
                          const QString& existingRemoteId,
                          double nx,
                          double ny,
                          const QList<ClipboardEntryMeta>& metas)
    {
        auto batch = std::make_unique<BatchUpload>();
        batch->isClipboardOffer = isClipboardOffer;
        batch->remoteId = existingRemoteId;
        batch->nx = nx;
        batch->ny = ny;
        batch->chunkMax = std::max<qint64>(1, m_Caps.limits.chunkMax);
        batch->display = metas.size() == 1 ? elide(metas.first().name)
                                           : QStringLiteral("%1 files").arg(metas.size());

        for (int i = 0; i < metas.size(); ++i) {
            const ClipboardEntryMeta& meta = metas.at(i);
            qint64 size = 0;
            auto entry = std::make_unique<BatchEntry>();
#ifdef Q_OS_UNIX
            QString name;
            QString err;
            int fd = openVerifiedSourceFile(meta.sourcePath, &name, &size, isClipboardOffer, &err);
            if (fd < 0) {
                notice(err.isEmpty() ? QStringLiteral("Cannot read %1").arg(elide(meta.name)) : err);
                if (isClipboardOffer && !existingRemoteId.isEmpty()) {
                    doCancelLazyClipboardOffer(existingRemoteId);
                }
                return;
            }
            if (!entry->file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
                ::close(fd);
                notice(QStringLiteral("Cannot read %1").arg(elide(meta.name)));
                if (isClipboardOffer && !existingRemoteId.isEmpty()) {
                    doCancelLazyClipboardOffer(existingRemoteId);
                }
                return;
            }
#else
            const QFileInfo fi(meta.sourcePath);
            if (!fi.isFile()) {
                notice(QStringLiteral("Cannot read %1").arg(elide(meta.name)));
                if (isClipboardOffer && !existingRemoteId.isEmpty()) {
                    doCancelLazyClipboardOffer(existingRemoteId);
                }
                return;
            }
            entry->file.setFileName(meta.sourcePath);
            if (!entry->file.open(QIODevice::ReadOnly)) {
                notice(QStringLiteral("Cannot read %1").arg(elide(meta.name)));
                if (isClipboardOffer && !existingRemoteId.isEmpty()) {
                    doCancelLazyClipboardOffer(existingRemoteId);
                }
                return;
            }
            size = entry->file.size();
#endif
            if (isClipboardOffer && size != meta.size) {
                notice(QStringLiteral("%1 changed before transfer").arg(elide(meta.name)));
                doCancelLazyClipboardOffer(existingRemoteId);
                return;
            }
            entry->id = meta.id;
            entry->index = i;
            entry->name = meta.name;
            entry->path = meta.sourcePath;
            entry->size = size;
            batch->totalBytes += size;
            batch->entries.push_back(std::move(entry));
        }

        m_BatchUpload = std::move(batch);
        emitProgress(m_BatchUpload->display, true, 0);

        if (isClipboardOffer) {
            sendBatchEntryStart();
        }
        else {
            createDropOnHost();
        }
    }

    void failBatchUpload(const QString& message, bool cancelOnHost)
    {
        std::unique_ptr<BatchUpload> batch = std::move(m_BatchUpload);
        if (!batch) {
            return;
        }
        if (cancelOnHost && !batch->remoteId.isEmpty() && canRun()) {
            if (batch->isClipboardOffer) {
                send("POST", QStringLiteral("/sds/v1/clipboard-offer/%1/cancel").arg(batch->remoteId),
                     QUrlQuery(), QByteArray(), QByteArray(), {}, kJsonTimeoutMs, kMaxJsonResponse, [](const HttpResult&) {});
            }
            else {
                send("DELETE", QStringLiteral("/sds/v1/drops/%1").arg(batch->remoteId),
                     QUrlQuery(), QByteArray(), QByteArray(), {}, kJsonTimeoutMs, kMaxJsonResponse, [](const HttpResult&) {});
            }
        }
        if (!batch->isClipboardOffer) {
            ClientEvent ev;
            ev.type = ClientEvent::TransferFinished;
            ev.upload = true;
            ev.ok = false;
            ev.name = batch->display;
            ev.message = message;
            emitEvent(ev);
            startNextDrop();
        }
        else {
            if (m_ClientOfferId == batch->remoteId) {
                m_ClientOfferId.clear();
            }
            notice(message);
        }
    }

    void createDropOnHost()
    {
        BatchUpload* b = m_BatchUpload.get();
        if (b == nullptr || !canRun()) {
            return;
        }
        QJsonArray filesArr;
        for (const auto& entry : b->entries) {
            filesArr.append(QJsonObject{
                {QStringLiteral("name"), entry->name},
                {QStringLiteral("size"), static_cast<double>(entry->size)}
            });
        }
        const QJsonObject body{
            {QStringLiteral("nx"), b->nx},
            {QStringLiteral("ny"), b->ny},
            {QStringLiteral("files"), filesArr}
        };
        send("POST", QStringLiteral("/sds/v1/drops"), QUrlQuery(),
             QJsonDocument(body).toJson(QJsonDocument::Compact),
             QByteArrayLiteral("application/json"), {}, kJsonTimeoutMs, kMaxJsonResponse,
             [this](const HttpResult& r) {
            BatchUpload* b = m_BatchUpload.get();
            if (b == nullptr || b->isClipboardOffer) {
                return;
            }
            if (r.pinFailure) {
                pinFailure();
                return;
            }
            if (r.status == 201 || r.status == 200) {
                const QString id = r.json.value(QStringLiteral("id")).toString();
                qint64 chunkMax = 0;
                if (!isSafeSdsToken(id) || !jsonToInt64(r.json.value(QStringLiteral("chunk_max")), &chunkMax) || chunkMax <= 0) {
                    failBatchUpload(QStringLiteral("Unexpected response from the host"), false);
                    return;
                }
                b->remoteId = id;
                b->chunkMax = std::min(chunkMax, m_Caps.limits.chunkMax);
                for (auto& e : b->entries) {
                    e->started = true;
                }
                sendBatchNextChunk();
                return;
            }
            if (r.status == 403) {
                failBatchUpload(QStringLiteral("The host does not allow sending files"), false);
            }
            else if (r.status == 413) {
                failBatchUpload(QStringLiteral("%1 is too large for the host").arg(b->display), false);
            }
            else if (r.status == 507) {
                failBatchUpload(QStringLiteral("The host does not have enough free space for %1").arg(b->display), false);
            }
            else if (r.status == 429 && ++b->completeAttempts <= kMaxCreateAttempts) {
                later(retryAfterMs(r, backoffMs(b->completeAttempts)), [this]() { createDropOnHost(); });
            }
            else {
                failBatchUpload(QStringLiteral("Cannot drop %1").arg(b->display), false);
            }
        });
    }

    void sendBatchEntryStart()
    {
        BatchUpload* b = m_BatchUpload.get();
        if (b == nullptr || !canRun()) {
            return;
        }
        if (b->entryIdx >= static_cast<int>(b->entries.size())) {
            sendBatchComplete();
            return;
        }
        BatchEntry* entry = b->entries[b->entryIdx].get();
        if (!b->isClipboardOffer || entry->started) {
            sendBatchNextChunk();
            return;
        }
        const QString url = QStringLiteral("/sds/v1/clipboard-offer/%1/entries/%2").arg(b->remoteId, entry->id);
        send("POST", url, QUrlQuery(), QByteArray(), QByteArray(), {}, kJsonTimeoutMs, kMaxJsonResponse,
             [this, remoteId = b->remoteId, idx = b->entryIdx](const HttpResult& r) {
            BatchUpload* b = m_BatchUpload.get();
            if (b == nullptr || b->remoteId != remoteId || b->entryIdx != idx) {
                return;
            }
            if (r.pinFailure) {
                pinFailure();
                return;
            }
            if (r.status == 201 || r.status == 200) {
                b->entries[idx]->started = true;
                b->entries[idx]->attempts = 0;
                sendBatchNextChunk();
                return;
            }
            failBatchUpload(QStringLiteral("Clipboard transfer failed (%1)").arg(r.status), true);
        });
    }

    void sendBatchNextChunk()
    {
        BatchUpload* b = m_BatchUpload.get();
        if (b == nullptr || !canRun()) {
            return;
        }
        while (b->entryIdx < static_cast<int>(b->entries.size())) {
            BatchEntry* entry = b->entries[b->entryIdx].get();
            if (entry->offset < entry->size) {
                break;
            }
            if (!entry->completedHash) {
                entry->finalSha = entry->sha.result().toHex();
                entry->completedHash = true;
            }
            ++b->entryIdx;
            if (b->isClipboardOffer && b->entryIdx < static_cast<int>(b->entries.size())) {
                sendBatchEntryStart();
                return;
            }
        }
        if (b->entryIdx >= static_cast<int>(b->entries.size())) {
            sendBatchComplete();
            return;
        }

        BatchEntry* entry = b->entries[b->entryIdx].get();
        const qint64 want = std::min(b->chunkMax, entry->size - entry->offset);
        if (!entry->file.seek(entry->offset)) {
            failBatchUpload(QStringLiteral("Cannot read %1").arg(elide(entry->name)), true);
            return;
        }
        QByteArray data = entry->file.read(want);
        if (data.size() != want) {
            failBatchUpload(QStringLiteral("Cannot read %1").arg(elide(entry->name)), true);
            return;
        }
        if (entry->offset == entry->hashedOffset) {
            entry->sha.addData(data);
            entry->hashedOffset += data.size();
        }

        QUrlQuery q;
        q.addQueryItem(QStringLiteral("offset"), QString::number(entry->offset));
        QString url;
        if (b->isClipboardOffer) {
            url = QStringLiteral("/sds/v1/clipboard-offer/%1/entries/%2").arg(b->remoteId, entry->id);
        }
        else {
            url = QStringLiteral("/sds/v1/drops/%1").arg(b->remoteId);
            q.addQueryItem(QStringLiteral("index"), QString::number(entry->index));
        }

        const qint64 sentOffset = entry->offset;
        const int idx = b->entryIdx;
        const QString remoteId = b->remoteId;
        send("PUT", url, q, data, QByteArrayLiteral("application/octet-stream"), {},
             kChunkIdleTimeoutMs, kMaxJsonResponse, [this, remoteId, idx, sentOffset, dataSize = data.size()](const HttpResult& r) {
            BatchUpload* b = m_BatchUpload.get();
            if (b == nullptr || b->remoteId != remoteId || b->entryIdx != idx) {
                return;
            }
            if (r.pinFailure) {
                pinFailure();
                return;
            }
            BatchEntry* entry = b->entries[idx].get();
            if (r.status == 200) {
                qint64 nextOff = sentOffset + dataSize;
                jsonToInt64(r.json.value(QStringLiteral("offset")), &nextOff);
                b->uploadedBytes += std::max<qint64>(0, nextOff - entry->offset);
                entry->offset = nextOff;
                entry->attempts = 0;
                const int percent = b->totalBytes > 0 ? static_cast<int>(b->uploadedBytes * 100 / b->totalBytes) : 100;
                if (percent != b->lastPercent) {
                    b->lastPercent = percent;
                    emitProgress(b->display, true, percent);
                }
                sendBatchNextChunk();
                return;
            }
            if (r.status == 409) {
                qint64 expected = -1;
                if (jsonToInt64(r.json.value(QStringLiteral("expected_offset")), &expected) &&
                        expected >= 0 && expected <= entry->size && ++entry->conflicts <= kMaxUploadConflicts) {
                    if (expected < entry->hashedOffset) {
                        entry->sha.reset();
                        entry->hashedOffset = 0;
                        entry->file.seek(0);
                        qint64 rem = expected;
                        while (rem > 0) {
                            QByteArray buf = entry->file.read(std::min<qint64>(kHashSliceBytes, rem));
                            if (buf.isEmpty()) break;
                            entry->sha.addData(buf);
                            entry->hashedOffset += buf.size();
                            rem -= buf.size();
                        }
                    }
                    entry->offset = expected;
                    sendBatchNextChunk();
                    return;
                }
            }
            if ((r.status == 429 || r.status >= 500 || r.status == 0) && ++entry->attempts <= kMaxChunkAttempts) {
                const int delay = r.status == 429 ? retryAfterMs(r, backoffMs(entry->attempts)) : backoffMs(entry->attempts);
                later(delay, [this]() { sendBatchNextChunk(); });
                return;
            }
            failBatchUpload(QStringLiteral("Sending %1 failed").arg(b->display), true);
        });
    }

    void sendBatchComplete()
    {
        BatchUpload* b = m_BatchUpload.get();
        if (b == nullptr || !canRun()) {
            return;
        }
        QJsonArray arr;
        for (auto& entry : b->entries) {
            if (!entry->completedHash) {
                entry->finalSha = entry->sha.result().toHex();
                entry->completedHash = true;
            }
            QJsonObject obj;
            if (b->isClipboardOffer) {
                obj.insert(QStringLiteral("id"), entry->id);
            }
            else {
                obj.insert(QStringLiteral("index"), entry->index);
            }
            obj.insert(QStringLiteral("sha256"), QString::fromLatin1(entry->finalSha));
            arr.append(obj);
        }
        const QJsonObject body{{QStringLiteral("entries"), arr}};
        const QString url = b->isClipboardOffer
                ? QStringLiteral("/sds/v1/clipboard-offer/%1/complete").arg(b->remoteId)
                : QStringLiteral("/sds/v1/drops/%1/complete").arg(b->remoteId);

        send("POST", url, QUrlQuery(), QJsonDocument(body).toJson(QJsonDocument::Compact),
             QByteArrayLiteral("application/json"), {}, kCompleteTimeoutMs, kMaxJsonResponse,
             [this, remoteId = b->remoteId](const HttpResult& r) {
            std::unique_ptr<BatchUpload> batch = std::move(m_BatchUpload);
            if (!batch || batch->remoteId != remoteId) {
                return;
            }
            if (r.pinFailure) {
                pinFailure();
                return;
            }
            if (r.ok()) {
                if (batch->isClipboardOffer) {
                    return;
                }
                const QString delivered = r.json.value(QStringLiteral("delivered")).toString();
                ClientEvent ev;
                ev.type = ClientEvent::TransferFinished;
                ev.upload = true;
                ev.ok = true;
                ev.name = batch->display;
                ev.message = delivered == QLatin1String("inbox")
                        ? QStringLiteral("Saved %1 to ~/Downloads/Moonlight").arg(batch->display)
                        : QStringLiteral("Dropped %1").arg(batch->display);
                emitEvent(ev);
                startNextDrop();
                return;
            }
            m_BatchUpload = std::move(batch);
            failBatchUpload(QStringLiteral("Transfer of %1 failed (%2)").arg(m_BatchUpload->display).arg(r.status), false);
        });
    }

    QString m_ClientOfferId;
    QList<DropRequest> m_DropQueue;
    std::unique_ptr<BatchUpload> m_BatchUpload;
    std::unique_ptr<LazyFetch> m_LazyFetch;

    std::unique_ptr<Download> m_Download;
    std::unique_ptr<DownloadDir> m_Dir;
};

SdsClient::SdsClient(const Config& config, ClientListener* listener) :
    m_Worker(new SdsWorker(config, listener)),
    m_Started(false),
    m_Stopped(false)
{
    m_Thread.setObjectName(QStringLiteral("SDS client"));
}

SdsClient::~SdsClient()
{
    stop();

    QMutexLocker lock(&m_Lock);
    if (!m_Started) {
        // Never moved to the worker thread
        delete m_Worker;
    }
    m_Worker = nullptr;
}

template <typename F> void SdsClient::post(F&& f)
{
    QMutexLocker lock(&m_Lock);
    if (!m_Started || m_Stopped) {
        return;
    }
    QMetaObject::invokeMethod(m_Worker, std::forward<F>(f), Qt::QueuedConnection);
}

void SdsClient::start()
{
    QMutexLocker lock(&m_Lock);
    if (m_Started || m_Stopped) {
        return;
    }
    m_Started = true;

    m_Worker->moveToThread(&m_Thread);
    // The worker is destroyed on its own thread once the thread's event loop has exited
    QObject::connect(&m_Thread, &QThread::finished, m_Worker, &QObject::deleteLater);
    m_Thread.start();

    SdsWorker* w = m_Worker;
    QMetaObject::invokeMethod(w, [w]() { w->init(); }, Qt::QueuedConnection);
}

void SdsClient::stop()
{
    {
        QMutexLocker lock(&m_Lock);
        if (m_Stopped) {
            return;
        }
        m_Stopped = true;
        if (!m_Started) {
            return;
        }
    }

    SdsWorker* w = m_Worker;
    if (QThread::currentThread() == &m_Thread) {
        // Misuse (a listener calling back into the client): never tear down the network
        // stack under the caller's feet and never wait for our own thread.
        qWarning() << "SdsClient::stop() called on the worker thread";
        QMetaObject::invokeMethod(w, [w]() { w->shutdown(); }, Qt::QueuedConnection);
        m_Thread.quit();
        return;
    }
    QMetaObject::invokeMethod(w, [w]() { w->shutdown(); }, Qt::BlockingQueuedConnection);
    m_Thread.quit();
    m_Thread.wait();
}

void SdsClient::sendClipboard(const ClipboardItem& item)
{
    SdsWorker* w = m_Worker;
    post([w, item]() { w->doSendClipboard(item); });
}

void SdsClient::sendFile(const QString& path)
{
    SdsWorker* w = m_Worker;
    post([w, path]() { w->doSendFile(path); });
}

void SdsClient::downloadOutboxNow()
{
    SdsWorker* w = m_Worker;
    post([w]() { w->doDownloadOutbox(true); });
}

void SdsClient::exchangeFileClipboard(const QByteArray& bundle, qint64 localGeneration)
{
    SdsWorker* w = m_Worker;
    post([w, bundle, localGeneration]() { w->doExchangeFileClipboard(bundle, localGeneration); });
}

void SdsClient::publishLazyClipboardOffer(const LazyClipboardOffer& offer)
{
    SdsWorker* w = m_Worker;
    post([w, offer]() { w->doPublishLazyClipboardOffer(offer); });
}

void SdsClient::cancelLazyClipboardOffer(const QString& offerId)
{
    SdsWorker* w = m_Worker;
    post([w, offerId]() { w->doCancelLazyClipboardOffer(offerId); });
}

void SdsClient::fulfillLazyClipboardPull(const LazyClipboardOffer& offer)
{
    SdsWorker* w = m_Worker;
    post([w, offer]() { w->doFulfillLazyClipboardPull(offer); });
}

void SdsClient::fetchLazyClipboardOffer(const LazyClipboardOffer& offer,
                                        const QString& stageDir,
                                        std::shared_ptr<LazyTransferResult> result)
{
    SdsWorker* w = m_Worker;
    post([w, offer, stageDir, result = std::move(result)]() mutable {
        w->doFetchLazyClipboardOffer(offer, stageDir, std::move(result));
    });
}

void SdsClient::sendDroppedFiles(double nx, double ny, const QStringList& paths)
{
    SdsWorker* w = m_Worker;
    post([w, nx, ny, paths]() { w->doSendDroppedFiles(nx, ny, paths); });
}

class SdsTileRefinementWorker : public QObject
{
public:
    SdsTileRefinementWorker(const SdsClient::Config& cfg,
                            std::shared_ptr<AdaptiveTileInbox> inbox)
        : m_Cfg(cfg),
          m_Inbox(std::move(inbox))
    {
    }

    void init()
    {
        if (m_Shutdown || !m_Inbox) {
            return;
        }
        if (!LanPolicy::isPermittedConnectionTarget(QHostAddress(m_Cfg.host))) {
            qWarning() << "Tile refinement blocked by LAN policy for host" << m_Cfg.host;
            return;
        }
        if (m_Cfg.serverCert.isNull() || m_Cfg.httpsPort == 0 || m_Cfg.host.isEmpty()) {
            qWarning() << "Tile refinement disabled: missing paired server cert/port";
            return;
        }

        m_Nam = new QNetworkAccessManager(this);
        m_Nam->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        m_Nam->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);

        m_SslConfig = QSslConfiguration::defaultConfiguration();
        m_SslConfig.setLocalCertificate(m_Cfg.clientCert);
        m_SslConfig.setPrivateKey(m_Cfg.clientKey);
        m_SslConfig = CertPinning::restrictToPinned(m_SslConfig, m_Cfg.serverCert);
        m_SslConfig.setPeerVerifyMode(QSslSocket::VerifyPeer);

        startPoll();
    }

    void shutdown()
    {
        m_Shutdown = true;
        if (m_RetryTimer) {
            m_RetryTimer->stop();
        }
        if (m_ActiveReply) {
            m_ActiveReply->abort();
            m_ActiveReply = nullptr;
        }
    }

private:
    void startPoll()
    {
        if (m_Shutdown || !m_Nam || m_ActiveReply) {
            return;
        }

        const int timeoutSec = qBound(0, m_Cfg.longPollTimeoutSec, 25);
        QUrl url;
        url.setScheme(QStringLiteral("https"));
        url.setHost(m_Cfg.host);
        url.setPort(m_Cfg.httpsPort);
        url.setPath(QStringLiteral("/sds/v1/tile-refinement"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("timeout"), QString::number(timeoutSec));
        url.setQuery(q);

        QNetworkRequest req(url);
        req.setSslConfiguration(m_SslConfig);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
        req.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
        req.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        req.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        req.setHeader(QNetworkRequest::UserAgentHeader, QByteArrayLiteral("Moonlight-SDS/1"));
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        req.setTransferTimeout((timeoutSec + 10) * 1000);
#endif

        QNetworkReply* reply = m_Nam->get(req);
        m_ActiveReply = reply;
        CertPinning::enforce(reply, m_Cfg.serverCert);

        constexpr qint64 maxBytes = AdaptiveRefinementWire::MAX_BATCH_BYTES;
        reply->setReadBufferSize(maxBytes + 1);
        connect(reply, &QIODevice::readyRead, this, [reply]() {
            if (reply->bytesAvailable() > AdaptiveRefinementWire::MAX_BATCH_BYTES) {
                reply->setProperty(kTooLargeProp, true);
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64 total) {
            if (received > AdaptiveRefinementWire::MAX_BATCH_BYTES || total > AdaptiveRefinementWire::MAX_BATCH_BYTES) {
                reply->setProperty(kTooLargeProp, true);
                reply->abort();
            }
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            handlePollFinished(reply);
        });
    }

    void scheduleRetry(int delayMs)
    {
        if (m_Shutdown) {
            return;
        }
        if (!m_RetryTimer) {
            m_RetryTimer = new QTimer(this);
            m_RetryTimer->setSingleShot(true);
            connect(m_RetryTimer, &QTimer::timeout, this, [this]() {
                startPoll();
            });
        }
        m_RetryTimer->start(qMax(0, delayMs));
    }

    void handlePollFinished(QNetworkReply* reply)
    {
        if (m_ActiveReply == reply) {
            m_ActiveReply = nullptr;
        }
        reply->deleteLater();

        if (m_Shutdown) {
            return;
        }

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool tooLarge = reply->property(kTooLargeProp).toBool();

        if (tooLarge) {
            qWarning() << "Tile refinement batch exceeded max size";
            scheduleRetry(m_Cfg.retryBaseMs);
            return;
        }

        if (status == 401 || status == 403 || status == 404 ||
            reply->error() == QNetworkReply::SslHandshakeFailedError) {
            qInfo() << "Tile refinement stopped (status" << status << ", error" << reply->error() << ")";
            return;
        }

        if (reply->error() != QNetworkReply::NoError && status != 200 && status != 204) {
            const int delay = qBound(250, m_BackoffMs, qMax(250, m_Cfg.retryMaxMs));
            m_BackoffMs = qMin(m_BackoffMs * 2, qMax(250, m_Cfg.retryMaxMs));
            scheduleRetry(delay);
            return;
        }

        if (status == 204) {
            m_BackoffMs = qMax(250, m_Cfg.retryBaseMs);
            scheduleRetry(0);
            return;
        }

        if (status == 200) {
            const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString().section(QLatin1Char(';'), 0, 0).trimmed().toLower();
            if (!contentType.isEmpty() && contentType != QLatin1String("application/octet-stream")) {
                qWarning() << "Unexpected tile refinement Content-Type:" << contentType;
                scheduleRetry(m_Cfg.retryBaseMs);
                return;
            }
            const QByteArray body = reply->readAll();
            RefinementColorspace cs = RefinementColorspace::Rec601;
            bool fullRange = true;
            m_Inbox->colorimetry(cs, fullRange);

            AdaptiveTileBatch batch;
            if (parseTileRefinementBatch(body, cs, fullRange, batch)) {
                m_Inbox->pushBatch(std::move(batch));
                m_BackoffMs = qMax(250, m_Cfg.retryBaseMs);
                scheduleRetry(0);
            } else {
                qWarning() << "Rejected malformed tile refinement batch (" << body.size() << "bytes)";
                scheduleRetry(m_Cfg.retryBaseMs);
            }
            return;
        }

        const int delay = qBound(250, m_BackoffMs, qMax(250, m_Cfg.retryMaxMs));
        m_BackoffMs = qMin(m_BackoffMs * 2, qMax(250, m_Cfg.retryMaxMs));
        scheduleRetry(delay);
    }

    SdsClient::Config m_Cfg;
    std::shared_ptr<AdaptiveTileInbox> m_Inbox;
    QNetworkAccessManager* m_Nam = nullptr;
    QSslConfiguration m_SslConfig;
    QPointer<QNetworkReply> m_ActiveReply;
    QTimer* m_RetryTimer = nullptr;
    bool m_Shutdown = false;
    int m_BackoffMs = 250;
};

SdsTileRefinementClient::SdsTileRefinementClient(const SdsClient::Config& config,
                                                 std::shared_ptr<AdaptiveTileInbox> inbox)
    : m_Worker(new SdsTileRefinementWorker(config, std::move(inbox)))
{
    m_Thread.setObjectName(QStringLiteral("SdsTileRefinement"));
}

SdsTileRefinementClient::~SdsTileRefinementClient()
{
    stop();
    if (!m_Started && m_Worker) {
        delete m_Worker;
        m_Worker = nullptr;
    }
}

void SdsTileRefinementClient::start()
{
    {
        QMutexLocker lock(&m_Lock);
        if (m_Started || m_Stopped) {
            return;
        }
        m_Started = true;
    }

    m_Worker->moveToThread(&m_Thread);
    QObject::connect(&m_Thread, &QThread::finished, m_Worker, &QObject::deleteLater);
    m_Thread.start();

    SdsTileRefinementWorker* w = m_Worker;
    QMetaObject::invokeMethod(w, [w]() { w->init(); }, Qt::QueuedConnection);
}

void SdsTileRefinementClient::stop()
{
    {
        QMutexLocker lock(&m_Lock);
        if (m_Stopped) {
            return;
        }
        m_Stopped = true;
        if (!m_Started) {
            return;
        }
    }

    SdsTileRefinementWorker* w = m_Worker;
    if (QThread::currentThread() == &m_Thread) {
        QMetaObject::invokeMethod(w, [w]() { w->shutdown(); }, Qt::QueuedConnection);
        m_Thread.quit();
        return;
    }
    QMetaObject::invokeMethod(w, [w]() { w->shutdown(); }, Qt::BlockingQueuedConnection);
    m_Thread.quit();
    m_Thread.wait();
    m_Worker = nullptr;
}
