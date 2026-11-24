#pragma once

// secure-desktop-stream: session-side controller for clipboard sync and file transfer.
//
// Runs on the SDL main thread. It polls the local clipboard on a 300 ms tick, forwards
// changes to the network client, applies host clipboard content, accepts dropped files and
// turns client events into short overlay messages. It has no SDL or AppKit dependency so it
// can be unit tested with fakes; SdsSession provides the SDL glue.

#include "clipboardsync.h"
#include "sdstypes.h"

#include <QList>
#include <QMutex>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>

class SdsController : public Sds::ClientListener
{
public:
    struct Options
    {
        bool clipboard = false;        // user setting on and host advertises "clipboard"
        bool files = false;            // user setting on and host advertises "files"
        bool fileClipboard = false;    // separate opt-in plus both settings and host feature
        bool xdndDrop = false;         // host supports coordinate-aware /sds/v1/drops
        QString unavailableReason;     // shown when the user drops a file but files == false
    };

    struct Hooks
    {
        std::function<void()> wake;                       // any thread: schedule processPendingEvents()
        std::function<void(const QString&)> showStatus;   // main thread
        std::function<void()> hideStatus;                 // main thread
    };

    static constexpr int kMessageMs = 4000;
    static constexpr int kProgressStaleMs = 15000;

    SdsController(const Options& options, std::unique_ptr<Sds::IClipboardBackend> clipboard, const Hooks& hooks);
    ~SdsController() override;

    // Takes ownership of the network client (nullptr when neither feature is usable).
    void attachClient(std::unique_ptr<Sds::IClient> client);

    void start(quint64 nowMs);
    // Stops the client (blocking, bounded) and hides our overlay message.
    void stop();

    // Main thread entry points
    void tick(quint64 nowMs);
    void processPendingEvents(quint64 nowMs);
    void fileDropped(const QString& path, quint64 nowMs);
    void filesDropped(double nx, double ny, const QStringList& paths, quint64 nowMs);
    void downloadOutboxNow(quint64 nowMs);

    // Sds::ClientListener, called on the client's worker thread
    void onSdsClientEvent(const Sds::ClientEvent& event) override;

    // For tests and diagnostics
    bool isReady() const { return m_Ready; }
    bool isStopped() const { return m_Stopped; }
    const Sds::Capabilities& capabilities() const { return m_Caps; }
    QString currentStatus() const { return m_StatusVisible ? m_StatusText : QString(); }

private:
    void handleEvent(const Sds::ClientEvent& event, quint64 nowMs);
    void pollClipboard(quint64 nowMs);
    void pushLocalClipboard(quint64 nowMs);
    void applyHostClipboard(Sds::ClipboardItem item);
    void showMessage(const QString& text, quint64 nowMs, int durationMs);
    void hideMessage();
    bool clipboardWritable() const;
    bool clipboardReadable() const;
    bool lazyClipboardActive() const;
    void onHostLazyOfferDemanded(const QString& offerId);
    void publishLocalLazyFilesIfAny(qint64 generation, quint64 nowMs);

    Options m_Options;
    std::unique_ptr<Sds::IClipboardBackend> m_Clipboard;
    Hooks m_Hooks;
    std::unique_ptr<Sds::IClient> m_Client;

    QMutex m_QueueLock;
    QList<Sds::ClientEvent> m_Queue;
    std::atomic<bool> m_WakePending { false };

    bool m_Started = false;
    bool m_Ready = false;
    bool m_Stopped = false;
    qint64 m_FileGeneration = -1;
    bool m_FileBaselineReady = false;
    quint64 m_LastFilePoll = 0;
    quint64 m_LocalOfferCounter = 0;
    Sds::LazyClipboardOffer m_LocalLazyOffer;
    Sds::LazyClipboardOffer m_HostLazyOffer;
    QString m_HostLazyStageDir;
    bool m_HostDemandStarted = false;
    std::shared_ptr<Sds::LazyTransferResult> m_ActiveHostTransfer;
    QString m_StopReason;
    Sds::Capabilities m_Caps;
    Sds::ClipboardEchoGuard m_Guard;

    bool m_StatusVisible = false;
    QString m_StatusText;
    quint64 m_StatusHideAt = 0;
};
