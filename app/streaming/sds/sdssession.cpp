#include "sdssession.h"

#include "backend/identitymanager.h"
#include "backend/nvcomputer.h"
#include "backend/sdsclient.h"
#include "clipboard_platform.h"
#include "sdscontroller.h"
#include "settings/streamingpreferences.h"
#include "streaming/video/overlaymanager.h"

#include <QDir>
#include <QCryptographicHash>
#include <QStandardPaths>

namespace {

constexpr Uint32 kTickMs = 300;

QString downloadFolder()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (base.isEmpty()) {
        base = QDir::home().filePath(QStringLiteral("Downloads"));
    }
    return QDir(base).filePath(QStringLiteral("Moonlight"));
}

void pushUserEvent(int code)
{
    SDL_Event event = {};
    event.type = SDL_USEREVENT;
    event.user.code = code;
    SDL_PushEvent(&event);
}

} // namespace

std::unique_ptr<SdsSession> SdsSession::create(NvComputer* computer,
                                               StreamingPreferences* prefs,
                                               Overlay::OverlayManager& overlay,
                                               std::function<bool()> otherOverlayActive)
{
    if (!prefs->clipboardSync && !prefs->fileTransfer) {
        return nullptr;
    }

    int sdsVersion;
    QStringList sdsFeatures;
    SdsClient::Config cfg;
    {
        QReadLocker lock(&computer->lock);
        sdsVersion = computer->sdsVersion;
        sdsFeatures = computer->sdsFeatures;
        cfg.host = computer->activeAddress.address();
        cfg.httpsPort = computer->activeHttpsPort != 0 ? computer->activeHttpsPort : DEFAULT_HTTPS_PORT;
        cfg.serverCert = computer->serverCert;
    }

    bool hostClipboard = sdsVersion >= 1 && sdsFeatures.contains(QStringLiteral("clipboard"));
    bool hostFiles = sdsVersion >= 1 && sdsFeatures.contains(QStringLiteral("files"));

    SdsController::Options options;
    options.clipboard = prefs->clipboardSync && hostClipboard;
    options.files = prefs->fileTransfer && hostFiles;
    const auto hostFingerprint = QString::fromLatin1(cfg.serverCert.digest(QCryptographicHash::Sha256).toHex());
    options.fileClipboard = options.clipboard && options.files && prefs->fileClipboard &&
            prefs->fileClipboardHosts.contains(hostFingerprint) &&
            sdsFeatures.contains(QStringLiteral("file_clipboard"));
    options.xdndDrop = options.files && sdsFeatures.contains(QStringLiteral("xdnd_drop"));
    if (!prefs->fileTransfer) {
        options.unavailableReason = QStringLiteral("File transfer is disabled in the settings");
    }
    else if (!hostFiles) {
        options.unavailableReason = QStringLiteral("This host does not support file transfer");
    }

    if (cfg.serverCert.isNull() || cfg.host.isEmpty()) {
        // Without a pinned certificate there is no authenticated channel
        options.clipboard = false;
        options.files = false;
        options.fileClipboard = false;
        options.unavailableReason = QStringLiteral("File transfer needs a paired host");
    }

    std::unique_ptr<SdsSession> session(new SdsSession(overlay, std::move(otherOverlayActive)));
    SdsSession* self = session.get();

    SdsController::Hooks hooks;
    hooks.wake = []() { pushUserEvent(SDL_CODE_SDS_WAKE); };
    hooks.showStatus = [self](const QString& text) { self->showStatus(text); };
    hooks.hideStatus = [self]() { self->hideStatus(); };

    session->m_Controller = std::make_unique<SdsController>(
                options,
                options.clipboard ? Sds::createPlatformClipboard() : std::unique_ptr<Sds::IClipboardBackend>(),
                hooks);

    if (options.clipboard || options.files) {
        QSslConfiguration identity = IdentityManager::get()->getSslConfig();
        cfg.clientCert = identity.localCertificate();
        cfg.clientKey = identity.privateKey();
        cfg.downloadDir = downloadFolder();
        cfg.clipboardEnabled = options.clipboard;
        cfg.filesEnabled = options.files;
        cfg.fileClipboardEnabled = options.fileClipboard;
        cfg.xdndDropEnabled = options.xdndDrop;
        cfg.autoDownload = prefs->autoDownloadOutbox;

        session->m_Controller->attachClient(std::make_unique<SdsClient>(cfg, session->m_Controller.get()));
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "SDS: clipboard %s, file transfer %s",
                    options.clipboard ? "on" : "off",
                    options.files ? "on" : "off");
    }

    session->m_Controller->start(nowMs());
    session->m_Timer = SDL_AddTimer(kTickMs, timerCallback, nullptr);
    if (session->m_Timer == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDS: SDL_AddTimer() failed: %s", SDL_GetError());
    }

    return session;
}

SdsSession::SdsSession(Overlay::OverlayManager& overlay, std::function<bool()> otherOverlayActive) :
    m_Timer(0),
    m_Overlay(overlay),
    m_OtherOverlayActive(std::move(otherOverlayActive))
{
}

SdsSession::~SdsSession()
{
    if (m_Timer != 0) {
        SDL_RemoveTimer(m_Timer);
    }

    // Stops the network client before the connection is torn down
    m_Controller.reset();
    hideStatus();
}

Uint32 SdsSession::timerCallback(Uint32 interval, void*)
{
    // SDL timer thread: only post an event
    pushUserEvent(SDL_CODE_SDS_TICK);
    return interval;
}

quint64 SdsSession::nowMs()
{
#if SDL_VERSION_ATLEAST(2, 0, 18)
    return SDL_GetTicks64();
#else
    return SDL_GetTicks();
#endif
}

void SdsSession::handleUserEvent(int code)
{
    if (code == SDL_CODE_SDS_TICK) {
        m_Controller->tick(nowMs());
    }
    else if (code == SDL_CODE_SDS_WAKE) {
        m_Controller->processPendingEvents(nowMs());
    }
}

void SdsSession::handleDropFile(const char* path)
{
    if (path != nullptr) {
        m_Controller->fileDropped(QString::fromUtf8(path), nowMs());
    }
}

void SdsSession::handleDropFiles(double nx, double ny, const QStringList& paths)
{
    m_Controller->filesDropped(nx, ny, paths, nowMs());
}

void SdsSession::downloadOutboxNow()
{
    m_Controller->downloadOutboxNow(nowMs());
}

bool SdsSession::ownsOverlay() const
{
    return !m_ShownText.isEmpty() &&
            qstrcmp(m_Overlay.getOverlayText(Overlay::OverlayStatusUpdate), m_ShownText.constData()) == 0;
}

void SdsSession::showStatus(const QString& text)
{
    // The status overlay is shared with gamepad mouse mode and connection warnings: never
    // replace text that another feature put there.
    if (m_OtherOverlayActive && m_OtherOverlayActive()) {
        return;
    }
    if (m_Overlay.isOverlayEnabled(Overlay::OverlayStatusUpdate) && !ownsOverlay()) {
        return;
    }

    m_ShownText = text.toUtf8().left(m_Overlay.getOverlayMaxTextLength() - 1);
    // Keep the copy in sync with what the overlay stores (it truncates on a UTF-8 boundary)
    m_Overlay.updateOverlayText(Overlay::OverlayStatusUpdate, m_ShownText.constData());
    m_ShownText = QByteArray(m_Overlay.getOverlayText(Overlay::OverlayStatusUpdate));
    m_Overlay.setOverlayState(Overlay::OverlayStatusUpdate, true);
}

void SdsSession::hideStatus()
{
    if (m_Overlay.isOverlayEnabled(Overlay::OverlayStatusUpdate) && ownsOverlay()) {
        m_Overlay.setOverlayState(Overlay::OverlayStatusUpdate, false);
    }
    m_ShownText.clear();
}
