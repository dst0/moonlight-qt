#include "sdscontroller.h"
#include "filesanitize.h"

#include <QDebug>
#include <QFileInfo>
#include <QMutexLocker>

using namespace Sds;

namespace {

QString displayName(const QString& path)
{
    QString name = sanitizeFileName(QFileInfo(path).fileName());
    if (name.isEmpty()) {
        name = QStringLiteral("file");
    }
constexpr int kMax = 40;
if (name.size() > kMax) {
    name = name.left(kMax / 2 - 1) + QStringLiteral("...") + name.right(kMax / 2 - 2);
}
return name;
}

constexpr qint64 kLazyImageThresholdBytes = 256 * 1024;

} // namespace

SdsController::SdsController(const Options& options, std::unique_ptr<IClipboardBackend> clipboard, const Hooks& hooks) :
    m_Options(options),
    m_Clipboard(std::move(clipboard)),
    m_Hooks(hooks)
{
}

SdsController::~SdsController()
{
    stop();
}

void SdsController::attachClient(std::unique_ptr<IClient> client)
{
    m_Client = std::move(client);
}

void SdsController::start(quint64 nowMs)
{
    Q_UNUSED(nowMs);

    if (m_Started) {
        return;
    }
    m_Started = true;

    if (m_Clipboard && m_Clipboard->isAvailable()) {
        // Whatever is on the clipboard now is handled by the start sequence (on Ready),
        // not by the change poll.
        m_Guard.reset(m_Clipboard->changeCount());
        m_FileGeneration = m_Clipboard->changeCount();
    }

    if (m_Client && (m_Options.clipboard || m_Options.files)) {
        m_Client->start();
    }
    else {
        m_Stopped = true;
        m_StopReason = m_Options.unavailableReason;
    }
}

void SdsController::stop()
{
    m_Stopped = true;
    m_Ready = false;
    if (m_ActiveHostTransfer) {
        {
            std::lock_guard<std::mutex> lock(m_ActiveHostTransfer->mutex);
            if (!m_ActiveHostTransfer->finished) {
                m_ActiveHostTransfer->ok = false;
                m_ActiveHostTransfer->error = QStringLiteral("Stopped");
                m_ActiveHostTransfer->finished = true;
            }
        }
        m_ActiveHostTransfer->cv.notify_all();
        m_ActiveHostTransfer.reset();
    }
    if (m_Client) {
        if (!m_LocalLazyOffer.offerId.isEmpty()) {
            m_Client->cancelLazyClipboardOffer(m_LocalLazyOffer.offerId);
        }
        // Blocking: no listener callback happens after this returns
        m_Client->stop();
    }
    {
        QMutexLocker lock(&m_QueueLock);
        m_Queue.clear();
    }
    if (m_Clipboard) {
        if (!m_HostLazyStageDir.isEmpty()) {
            m_Clipboard->retireStageDir(m_HostLazyStageDir);
        }
        m_Clipboard->retireFiles();
    }
    m_LocalLazyOffer = LazyClipboardOffer();
    m_HostLazyOffer = LazyClipboardOffer();
    m_HostLazyStageDir.clear();
    m_HostDemandStarted = false;
    m_FileBaselineReady = false;
    hideMessage();
}

void SdsController::onSdsClientEvent(const ClientEvent& event)
{
    {
        QMutexLocker lock(&m_QueueLock);
        m_Queue.append(event);
    }

    // Coalesce wake-ups: one pending SDL event is enough
    if (!m_WakePending.exchange(true) && m_Hooks.wake) {
        m_Hooks.wake();
    }
}

void SdsController::processPendingEvents(quint64 nowMs)
{
    m_WakePending.store(false);

    QList<ClientEvent> events;
    {
        QMutexLocker lock(&m_QueueLock);
        events.swap(m_Queue);
    }

    for (const ClientEvent& ev : std::as_const(events)) {
        handleEvent(ev, nowMs);
    }
}

void SdsController::tick(quint64 nowMs)
{
    // Picks up events whose wake-up event was lost or not yet delivered
    processPendingEvents(nowMs);

    if (lazyClipboardActive()) {
        const auto generation = m_Clipboard->changeCount();
        if (generation != m_FileGeneration) {
            publishLocalLazyFilesIfAny(generation, nowMs);
        }
    }
    else if (m_Ready && !m_Stopped && m_Options.fileClipboard && m_Options.files && m_Clipboard &&
            ((clipboardWritable() && m_Caps.has(kPermFileUpload)) ||
             (clipboardReadable() && m_Caps.has(kPermFileDownload)))) {
        const auto generation = m_Clipboard->changeCount();
        const bool changed = generation != m_FileGeneration;
        QByteArray bundle;
        if (changed) {
            m_Clipboard->retireFiles();
            if (m_FileBaselineReady && clipboardWritable() && m_Caps.has(kPermFileUpload) &&
                    m_Clipboard->hasFileSelection()) {
                QString error;
                bundle = m_Clipboard->readFiles(&error);
                if (m_Clipboard->changeCount() != generation) bundle.clear();
                if (!error.isEmpty()) showMessage(error, nowMs, kMessageMs);
            }
            m_FileGeneration = generation;
        }
        if (changed || !bundle.isEmpty() || nowMs - m_LastFilePoll >= 1000) {
            m_LastFilePoll = nowMs;
            m_Client->exchangeFileClipboard(bundle, generation);
        }
    }

    if (m_Ready && !m_Stopped && clipboardWritable()) {
        pollClipboard(nowMs);
    }

    if (m_StatusVisible && m_StatusHideAt != 0 && nowMs >= m_StatusHideAt) {
        hideMessage();
    }
}

bool SdsController::clipboardWritable() const
{
    return m_Options.clipboard && m_Clipboard && m_Clipboard->isAvailable() && m_Caps.has(kPermClipboardWrite);
}

bool SdsController::clipboardReadable() const
{
    return m_Options.clipboard && m_Clipboard && m_Clipboard->isAvailable() && m_Caps.has(kPermClipboardRead);
}

bool SdsController::lazyClipboardActive() const
{
    return m_Ready && !m_Stopped && m_Options.fileClipboard && m_Options.files &&
           m_Clipboard && m_Clipboard->isAvailable() && m_Caps.hasFeature("lazy_clipboard");
}

void SdsController::publishLocalLazyFilesIfAny(qint64 generation, quint64 nowMs)
{
    if (!m_LocalLazyOffer.offerId.isEmpty() && m_Client) {
        m_Client->cancelLazyClipboardOffer(m_LocalLazyOffer.offerId);
        m_LocalLazyOffer = LazyClipboardOffer();
    }
    m_FileGeneration = generation;
    if (!clipboardWritable() || !m_Caps.has(kPermFileUpload) || !m_Client) {
        return;
    }

    m_Clipboard->retireFiles();
    if (m_Clipboard->hasFileSelection()) {
        QList<ClipboardEntryMeta> entries;
        QString error;
        if (!m_Clipboard->readFileEntries(&entries, &error)) {
            if (!error.isEmpty()) {
                showMessage(error, nowMs, kMessageMs);
            }
            return;
        }
        if (m_Clipboard->changeCount() != generation || entries.isEmpty()) {
            return;
        }

        LazyClipboardOffer offer;
        offer.offerId = QStringLiteral("c_%1").arg(++m_LocalOfferCounter);
        offer.kind = QStringLiteral("files");
        offer.entries = std::move(entries);
        offer.localGeneration = generation;
        offer.fromHost = false;
        m_LocalLazyOffer = offer;
        m_Client->publishLazyClipboardOffer(m_LocalLazyOffer);
        return;
    }

    QString tempImgPath;
    qint64 imgSize = 0;
    QString imgError;
    if (m_Clipboard->readLazyImage(kLazyImageThresholdBytes, m_Caps.limits.fileMax,
                                   &tempImgPath, &imgSize, &imgError)) {
        if (m_Clipboard->changeCount() != generation) {
            return;
        }
        // Preserve any accompanying text representation on /sds/v1/clipboard while streaming large PNG lazily.
        IClipboardBackend::ReadResult textRes = m_Clipboard->read(m_Caps.limits.clipboardTextMax, 0);
        ClipboardCheck tc, ic;
        ClipboardItem textOnly;
        textOnly.text = textRes.item.text;
        sanitizeClipboardItem(textOnly, m_Caps.limits, &tc, &ic);
        if (!textOnly.text.isEmpty()) {
            m_Guard.noteSynced(textOnly);
            m_Client->sendClipboard(textOnly);
        }
        m_Guard.pollChanged(generation);

        LazyClipboardOffer offer;
        offer.offerId = QStringLiteral("c_%1").arg(++m_LocalOfferCounter);
        offer.kind = QStringLiteral("image");
        offer.mime = QStringLiteral("image/png");
        offer.entries.append({QStringLiteral("img0"), QStringLiteral("clipboard.png"), imgSize, tempImgPath});
        offer.localGeneration = generation;
        offer.fromHost = false;
        m_LocalLazyOffer = offer;
        m_Client->publishLazyClipboardOffer(m_LocalLazyOffer);
    }
    else if (!imgError.isEmpty()) {
        showMessage(imgError, nowMs, kMessageMs);
    }
}

void SdsController::onHostLazyOfferDemanded(const QString& offerId)
{
    if (m_Stopped || !m_Client || m_HostLazyOffer.offerId != offerId || m_HostDemandStarted) {
        return;
    }
    m_HostDemandStarted = true;

    qint64 totalBytes = 0;
    for (const ClipboardEntryMeta& entry : std::as_const(m_HostLazyOffer.entries)) {
        totalBytes += entry.size;
    }
    const qint64 maxMemImage = std::min(m_Caps.limits.clipboardImageMax, kClientImageMax);
    const bool needsStageDir = (m_HostLazyOffer.kind == QLatin1String("files")) ||
                               (m_HostLazyOffer.kind == QLatin1String("image") && totalBytes > maxMemImage);
    m_HostLazyStageDir.clear();
    if (needsStageDir) {
        QString err;
        m_HostLazyStageDir = m_Clipboard->createLazyStageDir(&err);
        if (m_HostLazyStageDir.isEmpty()) {
            if (m_ActiveHostTransfer) {
                {
                    std::lock_guard<std::mutex> lock(m_ActiveHostTransfer->mutex);
                    m_ActiveHostTransfer->ok = false;
                    m_ActiveHostTransfer->error = err.isEmpty() ? QStringLiteral("Cannot stage clipboard files") : err;
                    m_ActiveHostTransfer->finished = true;
                }
                m_ActiveHostTransfer->cv.notify_all();
            }
            return;
        }
    }

    const QString label = m_HostLazyOffer.kind == QLatin1String("image")
            ? QStringLiteral("image")
            : (m_HostLazyOffer.entries.size() == 1
                   ? displayName(m_HostLazyOffer.entries.first().name)
                   : QStringLiteral("%1 files").arg(m_HostLazyOffer.entries.size()));
    if (m_Hooks.showStatus) {
        m_Hooks.showStatus(QStringLiteral("Transferring %1...").arg(label));
    }

    m_Client->fetchLazyClipboardOffer(m_HostLazyOffer, m_HostLazyStageDir, m_ActiveHostTransfer);
}

void SdsController::pollClipboard(quint64 nowMs)
{
    if (m_Guard.pollChanged(m_Clipboard->changeCount())) {
        pushLocalClipboard(nowMs);
    }
}

void SdsController::pushLocalClipboard(quint64 nowMs)
{
    // Finder includes other representations of file selections. Never send paths as text.
    if (m_Clipboard->hasFileSelection()) return;
    IClipboardBackend::ReadResult r = m_Clipboard->read(m_Caps.limits.clipboardTextMax, m_Caps.limits.clipboardImageMax);

    ClipboardItem item = r.item;
    ClipboardCheck tc, ic;
    sanitizeClipboardItem(item, m_Caps.limits, &tc, &ic);

    QString problem;
    if (r.imageTooLarge) {
        problem = describeClipboardCheck(ClipboardCheck::TooLarge, true);
    }
    else if (r.textTooLarge) {
        problem = describeClipboardCheck(ClipboardCheck::TooLarge, false);
    }
    else if (ic != ClipboardCheck::Ok && ic != ClipboardCheck::Empty) {
        problem = describeClipboardCheck(ic, true);
    }
    else if (tc != ClipboardCheck::Ok && tc != ClipboardCheck::Empty) {
        problem = describeClipboardCheck(tc, false);
    }
    if (!problem.isEmpty()) {
        showMessage(problem, nowMs, kMessageMs);
    }

    if (item.isEmpty() || m_Guard.isDuplicateOfLastSynced(item)) {
        return;
    }

    m_Guard.noteSynced(item);
    if (m_Client) {
        m_Client->sendClipboard(item);
    }
}

void SdsController::applyHostClipboard(ClipboardItem item)
{
    if (!clipboardReadable()) {
        return;
    }

    // The client validated already; validate again at the point of use
    ClipboardCheck tc, ic;
    sanitizeClipboardItem(item, m_Caps.limits, &tc, &ic);
    if (item.isEmpty()) {
        return;
    }

    if (m_Guard.isDuplicateOfLastSynced(item)) {
        return;
    }

    qint64 cc = m_Clipboard->write(item);
    m_Guard.noteOwnWrite(cc);
    m_Guard.noteSynced(item);
}

void SdsController::handleEvent(const ClientEvent& ev, quint64 nowMs)
{
    if (m_Stopped) return;
    switch (ev.type) {
    case ClientEvent::Ready:
        m_Caps = ev.caps;
        m_Ready = true;
        if (m_Clipboard && m_Clipboard->isAvailable()) {
            // Consume changes made during start-up so the poll does not push them twice
            m_Guard.pollChanged(m_Clipboard->changeCount());
            m_FileGeneration = m_Clipboard->changeCount();
        }
        // DESIGN 5.4: host wins. Push the local clipboard only if the host did not announce
        // content of its own.
        if (clipboardWritable() && !ev.hostProvidedClipboard) {
            pushLocalClipboard(nowMs);
        }
        break;

    case ClientEvent::CapabilitiesChanged:
        m_Caps = ev.caps;
        m_FileBaselineReady = false;
        m_LocalLazyOffer = LazyClipboardOffer();
        m_HostLazyOffer = LazyClipboardOffer();
        m_HostDemandStarted = false;
        if (m_ActiveHostTransfer) {
            {
                std::lock_guard<std::mutex> lock(m_ActiveHostTransfer->mutex);
                if (!m_ActiveHostTransfer->finished) {
                    m_ActiveHostTransfer->ok = false;
                    m_ActiveHostTransfer->error = QStringLiteral("Capabilities changed");
                    m_ActiveHostTransfer->finished = true;
                }
            }
            m_ActiveHostTransfer->cv.notify_all();
            m_ActiveHostTransfer.reset();
        }
        if (m_Clipboard) {
            if (!m_HostLazyStageDir.isEmpty()) {
                m_Clipboard->retireStageDir(m_HostLazyStageDir);
                m_HostLazyStageDir.clear();
            }
            m_Clipboard->retireFiles();
            m_FileGeneration = m_Clipboard->changeCount();
        }
        break;

    case ClientEvent::LazyClipboardOfferReceived:
        if (!lazyClipboardActive() || !clipboardReadable() || !m_Caps.has(kPermFileDownload)) {
            break;
        }
        if (m_ActiveHostTransfer) {
            {
                std::lock_guard<std::mutex> lock(m_ActiveHostTransfer->mutex);
                if (!m_ActiveHostTransfer->finished) {
                    m_ActiveHostTransfer->ok = false;
                    m_ActiveHostTransfer->error = QStringLiteral("Superseded");
                    m_ActiveHostTransfer->finished = true;
                }
            }
            m_ActiveHostTransfer->cv.notify_all();
        }
        if (!m_HostLazyStageDir.isEmpty()) {
            m_Clipboard->retireStageDir(m_HostLazyStageDir);
            m_HostLazyStageDir.clear();
        }
        m_HostLazyOffer = ev.lazyOffer;
        m_HostDemandStarted = false;
        m_ActiveHostTransfer = std::make_shared<LazyTransferResult>();
        {
            QString error;
            const QString oid = ev.lazyOffer.offerId;
            const qint64 cc = m_Clipboard->writeLazyOffer(
                        ev.lazyOffer,
                        m_ActiveHostTransfer,
                        [this, oid](const QString& demandedId) {
                            onHostLazyOfferDemanded(demandedId);
                        },
                        &error);
            if (cc >= 0) {
                m_FileGeneration = cc;
                m_Guard.noteOwnWrite(cc);
            }
            else if (!error.isEmpty()) {
                showMessage(error, nowMs, kMessageMs);
            }
        }
        break;

    case ClientEvent::LazyClipboardPullRequested:
        if (!lazyClipboardActive() || m_LocalLazyOffer.offerId.isEmpty() ||
                ev.offerId != m_LocalLazyOffer.offerId || !m_Client) {
            break;
        }
        m_Client->fulfillLazyClipboardPull(m_LocalLazyOffer);
        break;

    case ClientEvent::LazyClipboardFinished:
        if (ev.offerId != m_HostLazyOffer.offerId) {
            break;
        }
        if (m_Clipboard) {
            qint64 newCc = -1;
            bool needRepaste = false;
            m_Clipboard->completeLazyOffer(ev.offerId, ev.ok, ev.stagedPaths, ev.clipboard.png,
                                           ev.message, &newCc, &needRepaste);
            if (newCc >= 0) {
                m_FileGeneration = newCc;
                m_Guard.noteOwnWrite(newCc);
            }
            if (!ev.ok) {
                if (!m_HostLazyStageDir.isEmpty()) {
                    m_Clipboard->retireStageDir(m_HostLazyStageDir);
                    m_HostLazyStageDir.clear();
                }
                showMessage(ev.message.isEmpty() ? QStringLiteral("Clipboard transfer failed") : ev.message,
                            nowMs, kMessageMs);
            }
            else if (needRepaste) {
                showMessage(ev.lazyOffer.kind == QLatin1String("image")
                                ? QStringLiteral("Image ready — press Cmd+V")
                                : QStringLiteral("Files ready — press Cmd+V"),
                            nowMs, kMessageMs);
            }
            else {
                hideMessage();
            }
        }
        m_HostLazyStageDir.clear();
        break;

    case ClientEvent::FileClipboardObserved:
        if (!m_Options.fileClipboard || !m_Options.files || !m_Clipboard || m_Stopped) break;
        if (ev.localGeneration == -2) {
            m_FileBaselineReady = false;
            m_Clipboard->retireFiles();
            m_FileGeneration = m_Clipboard->changeCount();
            break;
        }
        if (!m_FileBaselineReady) {
            m_FileBaselineReady = true;
            m_FileGeneration = m_Clipboard->changeCount();
        }
        if (!ev.fileBundle.isEmpty() && clipboardReadable() && m_Caps.has(kPermFileDownload) &&
                ev.localGeneration == m_Clipboard->changeCount()) {
            QString error;
            const auto generation = m_Clipboard->writeFiles(ev.fileBundle, ev.localGeneration, &error);
            if (generation >= 0) {
                m_FileGeneration = generation;
                m_Guard.noteOwnWrite(generation);
                showMessage(QStringLiteral("Files ready to paste"), nowMs, kMessageMs);
            }
            else if (!error.isEmpty()) showMessage(error, nowMs, kMessageMs);
        }
        break;

    case ClientEvent::ClipboardReceived:
        applyHostClipboard(ev.clipboard);
        break;

    case ClientEvent::TransferProgress:
        showMessage(QStringLiteral("%1 %2 %3%")
                    .arg(ev.upload ? QStringLiteral("Sending") : QStringLiteral("Receiving"), ev.name)
                    .arg(ev.percent),
                    nowMs, kProgressStaleMs);
        break;

    case ClientEvent::TransferFinished:
        if (ev.ok) {
            showMessage(!ev.message.isEmpty() ? ev.message :
                                                (ev.upload ? QStringLiteral("Sent %1").arg(ev.name) :
                                                             QStringLiteral("Saved %1 to Downloads/Moonlight").arg(ev.name)),
                        nowMs, kMessageMs);
        }
        else {
            showMessage(ev.message.isEmpty() ? QStringLiteral("Transfer of %1 failed").arg(ev.name) : ev.message,
                        nowMs, kMessageMs);
        }
        break;

    case ClientEvent::Notice:
        showMessage(ev.message, nowMs, kMessageMs);
        break;

    case ClientEvent::Stopped:
        if (m_ActiveHostTransfer) {
            {
                std::lock_guard<std::mutex> lock(m_ActiveHostTransfer->mutex);
                if (!m_ActiveHostTransfer->finished) {
                    m_ActiveHostTransfer->ok = false;
                    m_ActiveHostTransfer->error = ev.message;
                    m_ActiveHostTransfer->finished = true;
                }
            }
            m_ActiveHostTransfer->cv.notify_all();
            m_ActiveHostTransfer.reset();
        }
        if (m_Clipboard) {
            if (!m_HostLazyStageDir.isEmpty()) {
                m_Clipboard->retireStageDir(m_HostLazyStageDir);
                m_HostLazyStageDir.clear();
            }
            m_Clipboard->retireFiles();
        }
        m_LocalLazyOffer = LazyClipboardOffer();
        m_HostLazyOffer = LazyClipboardOffer();
        m_HostDemandStarted = false;
        m_FileBaselineReady = false;
        m_Stopped = true;
        m_Ready = false;
        m_StopReason = ev.message;
        break;
    }
}

void SdsController::fileDropped(const QString& path, quint64 nowMs)
{
    filesDropped(0.5, 0.5, {path}, nowMs);
}

void SdsController::filesDropped(double nx, double ny, const QStringList& paths, quint64 nowMs)
{
    if (paths.isEmpty()) {
        return;
    }
    if (!m_Options.files) {
        showMessage(m_Options.unavailableReason.isEmpty() ? QStringLiteral("File transfer is not available") :
                                                            m_Options.unavailableReason,
                    nowMs, kMessageMs);
        return;
    }
    if (m_Stopped || !m_Client) {
        showMessage(m_StopReason.isEmpty() ? QStringLiteral("File transfer is not available") : m_StopReason,
                    nowMs, kMessageMs);
        return;
    }
    if (m_Ready && !m_Caps.has(kPermFileUpload)) {
        showMessage(QStringLiteral("The host does not allow sending files"), nowMs, kMessageMs);
        return;
    }

    for (const QString& path : paths) {
        QFileInfo fi(path);
        if (fi.isDir()) {
            showMessage(QStringLiteral("Folders cannot be sent; drop files instead"), nowMs, kMessageMs);
            return;
        }
        if (!fi.exists()) {
            showMessage(QStringLiteral("Cannot read %1").arg(displayName(path)), nowMs, kMessageMs);
            return;
        }
        if (!fi.isFile()) {
            showMessage(QStringLiteral("Only regular files can be sent"), nowMs, kMessageMs);
            return;
        }
    }

    if (paths.size() == 1 && !m_Options.xdndDrop && !m_Caps.hasFeature("xdnd_drop")) {
        m_Client->sendFile(paths.first());
        showMessage(QStringLiteral("Sending %1").arg(displayName(paths.first())), nowMs, kProgressStaleMs);
        return;
    }

    m_Client->sendDroppedFiles(nx, ny, paths);
    const QString label = paths.size() == 1
            ? displayName(paths.first())
            : QStringLiteral("%1 files").arg(paths.size());
    showMessage(QStringLiteral("Sending %1").arg(label), nowMs, kProgressStaleMs);
}

void SdsController::downloadOutboxNow(quint64 nowMs)
{
    if (!m_Options.files) {
        showMessage(m_Options.unavailableReason.isEmpty() ? QStringLiteral("File transfer is not available") :
                                                            m_Options.unavailableReason,
                    nowMs, kMessageMs);
        return;
    }
    if (m_Stopped || !m_Client) {
        showMessage(m_StopReason.isEmpty() ? QStringLiteral("File transfer is not available") : m_StopReason,
                    nowMs, kMessageMs);
        return;
    }
    if (m_Ready && !m_Caps.has(kPermFileDownload)) {
        showMessage(QStringLiteral("The host does not allow downloading files"), nowMs, kMessageMs);
        return;
    }

    m_Client->downloadOutboxNow();
    showMessage(QStringLiteral("Checking the host outbox"), nowMs, kMessageMs);
}

void SdsController::showMessage(const QString& text, quint64 nowMs, int durationMs)
{
    if (text.isEmpty()) {
        return;
    }
    m_StatusText = text;
    m_StatusVisible = true;
    m_StatusHideAt = nowMs + static_cast<quint64>(durationMs);
    if (m_Hooks.showStatus) {
        m_Hooks.showStatus(text);
    }
}

void SdsController::hideMessage()
{
    if (!m_StatusVisible) {
        return;
    }
    m_StatusVisible = false;
    m_StatusText.clear();
    m_StatusHideAt = 0;
    if (m_Hooks.hideStatus) {
        m_Hooks.hideStatus();
    }
}
