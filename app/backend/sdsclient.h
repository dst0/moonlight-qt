#pragma once

// secure-desktop-stream: client for the host's /sds/v1 endpoints (clipboard and file
// transfer, DESIGN sections 4-6).
//
// All network I/O runs on a dedicated worker thread that owns its own
// QNetworkAccessManager, because Session::exec() suspends the Qt event loop of the main
// thread while streaming. Results are delivered to a Sds::ClientListener on the worker
// thread; the listener hands them to the SDL loop.

#include "streaming/sds/sdstypes.h"

#include <QMutex>
#include <QSslCertificate>
#include <QSslKey>
#include <QString>
#include <QThread>

#include <memory>

class SdsWorker;
class SdsTileRefinementWorker;
class AdaptiveTileInbox;

class SdsClient : public Sds::IClient
{
public:
    struct Config
    {
        QString host;                 // literal private/link-local or loopback IP; no DNS
        quint16 httpsPort = 0;        // paired mTLS HTTPS port (47984 by default)
        QSslCertificate serverCert;   // pinned host certificate (from pairing)
        QSslCertificate clientCert;   // our identity
        QSslKey clientKey;
        QString downloadDir;          // absolute path, normally ~/Downloads/Moonlight
        bool clipboardEnabled = true; // user setting && host advertises "clipboard"
        bool filesEnabled = true;     // user setting && host advertises "files"
        bool fileClipboardEnabled = false;
        bool xdndDropEnabled = false;
        bool autoDownload = true;     // download outbox files on "outbox" events
        int longPollTimeoutSec = 25;
        int retryBaseMs = 1000;       // exponential backoff base
        int retryMaxMs = 10000;       // backoff cap
        int retryAfterMaxSec = 30;    // cap for a host-provided Retry-After
    };

    SdsClient(const Config& config, Sds::ClientListener* listener);
    ~SdsClient() override;

    SdsClient(const SdsClient&) = delete;
    SdsClient& operator=(const SdsClient&) = delete;

    // Thread-safe. start() fetches the capabilities and begins the event long-poll.
    void start() override;
    // Thread-safe and blocking: aborts all requests and joins the worker thread. No listener
    // call happens after stop() returns.
    void stop() override;

    void sendClipboard(const Sds::ClipboardItem& item) override;
    void sendFile(const QString& path) override;
    void downloadOutboxNow() override;
    void exchangeFileClipboard(const QByteArray& bundle, qint64 localGeneration) override;
    void publishLazyClipboardOffer(const Sds::LazyClipboardOffer& offer) override;
    void cancelLazyClipboardOffer(const QString& offerId) override;
    void fulfillLazyClipboardPull(const Sds::LazyClipboardOffer& offer) override;
    void fetchLazyClipboardOffer(const Sds::LazyClipboardOffer& offer,
                                 const QString& stageDir,
                                 std::shared_ptr<Sds::LazyTransferResult> result) override;
    void sendDroppedFiles(double nx, double ny, const QStringList& paths) override;

private:
    template <typename F> void post(F&& f);

    QThread m_Thread;
    SdsWorker* m_Worker;
    QMutex m_Lock;
    bool m_Started;
    bool m_Stopped;
};

class SdsTileRefinementClient
{
public:
    SdsTileRefinementClient(const SdsClient::Config& config,
                            std::shared_ptr<AdaptiveTileInbox> inbox);
    ~SdsTileRefinementClient();

    SdsTileRefinementClient(const SdsTileRefinementClient&) = delete;
    SdsTileRefinementClient& operator=(const SdsTileRefinementClient&) = delete;

    void start();
    void stop();

private:
    QThread m_Thread;
    SdsTileRefinementWorker* m_Worker;
    QMutex m_Lock;
    bool m_Started = false;
    bool m_Stopped = false;
};
