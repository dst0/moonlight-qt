#include "streamingpreferences.h"
#include "utils.h"
#include "streaming/audio/miccapture.h"

#include <QSettings>
#include <QTranslator>
#include <QCoreApplication>
#include <QLocale>
#include <QReadWriteLock>
#include <QtMath>

#include <QtDebug>

#define SER_STREAMSETTINGS "streamsettings"
#define SER_WIDTH "width"
#define SER_HEIGHT "height"
#define SER_FPS "fps"
#define SER_BITRATE "bitrate"
#define SER_UNLOCK_BITRATE "unlockbitrate"
#define SER_AUTOADJUSTBITRATE "autoadjustbitrate"
#define SER_FULLSCREEN "fullscreen"
#define SER_VSYNC "vsync"
#define SER_GAMEOPTS "gameopts"
#define SER_HOSTAUDIO "hostaudio"
#define SER_MULTICONT "multicontroller"
#define SER_AUDIOCFG "audiocfg"
#define SER_VIDEOCFG "videocfg"
#define SER_HDR "hdr"
#define SER_YUV444 "yuv444"
#define SER_VIDEODEC "videodec"
#define SER_WINDOWMODE "windowmode"
#define SER_MDNS "mdns"
#define SER_QUITAPPAFTER "quitAppAfter"
#define SER_ABSMOUSEMODE "mouseacceleration"
#define SER_ABSTOUCHMODE "abstouchmode"
#define SER_STARTWINDOWED "startwindowed"
#define SER_FRAMEPACING "framepacing"
#define SER_CONNWARNINGS "connwarnings"
#define SER_CONFWARNINGS "confwarnings"
#define SER_UIDISPLAYMODE "uidisplaymode"
#define SER_GAMEPADMOUSE "gamepadmouse"
#define SER_DEFAULTVER "defaultver"
#define SER_PACKETSIZE "packetsize"
#define SER_SHOWPERFOVERLAY "showperfoverlay"
#define SER_SWAPMOUSEBUTTONS "swapmousebuttons"
#define SER_SWAPCMDCTRL "swapcmdctrl"
#define SER_MUTEONFOCUSLOSS "muteonfocusloss"
#define SER_BACKGROUNDGAMEPAD "backgroundgamepad"
#define SER_REVERSESCROLL "reversescroll"
#define SER_SWAPFACEBUTTONS "swapfacebuttons"
#define SER_CAPTURESYSKEYS "capturesyskeys"
#define SER_KEEPAWAKE "keepawake"
// secure-desktop-stream: microphone passthrough
#define SER_MICPASSTHROUGH "micpassthrough"
#define SER_MICECHOCANCEL "micechocancel"
#define SER_LANGUAGE "language"
#define SER_RENDERER "renderer"

// secure-desktop-stream: clipboard and file transfer
#define SER_SDSCLIPBOARD "sdsclipboard"
#define SER_SDSFILETRANSFER "sdsfiletransfer"
#define SER_SDSFILECLIPBOARD "sdsfileclipboard"
#define SER_SDSFILECLIPBOARDHOSTS "sdsfileclipboardhosts"
#define SER_SDSAUTODOWNLOAD "sdsautodownload"
// secure-desktop-stream: host encoder, per-host GPU adapter, and resolution matching
#define SER_SDSHOSTENCODER "sdshostencoder"
#define SER_SDSHOSTENCODEROVERRIDES "sdshostencoderoverrides"
#define SER_SDSHOSTADAPTERS "sdshostadapters"
#define SER_SDSHOSTCAPTUREADAPTERS "sdshostcaptureadapters"
#define SER_SDSHOSTADAPTIVEREFINEMENT "sdshostadaptiverefinement"
#define SER_SDSHOSTMATCHRES "sdshostmatchres"
#define SER_SDSHOSTREVERTRES "sdshostrevertres"
#define SER_HOSTGAMEOPTS "hostgameopts"
#define SER_HOSTQUITAPPAFTER "hostquitappafter"
#define SER_HOSTPLAYAUDIO "hostplayaudio"
#define SER_SDSMATCHHOSTRES "sdsmatchhostres"
#define SER_SDSREVERTHOSTRES "sdsreverthostres"

#define CURRENT_DEFAULT_VER 2

static StreamingPreferences* s_GlobalPrefs;

Q_GLOBAL_STATIC(QReadWriteLock, s_GlobalPrefsLock)

StreamingPreferences::StreamingPreferences(QQmlEngine *qmlEngine)
    : m_QmlEngine(qmlEngine)
{
    reload();
}

StreamingPreferences* StreamingPreferences::get(QQmlEngine *qmlEngine)
{
    {
        QReadLocker readGuard(s_GlobalPrefsLock);

        // If we have a preference object and it's associated with a QML engine or
        // if the caller didn't specify a QML engine, return the existing object.
        if (s_GlobalPrefs && (s_GlobalPrefs->m_QmlEngine || !qmlEngine)) {
            // The lifetime logic here relies on the QML engine also being a singleton.
            Q_ASSERT(!qmlEngine || s_GlobalPrefs->m_QmlEngine == qmlEngine);
            return s_GlobalPrefs;
        }
    }

    {
        QWriteLocker writeGuard(s_GlobalPrefsLock);

        // If we already have an preference object but the QML engine is now available,
        // associate the QML engine with the preferences.
        if (s_GlobalPrefs) {
            if (!s_GlobalPrefs->m_QmlEngine) {
                s_GlobalPrefs->m_QmlEngine = qmlEngine;
            }
            else {
                // We could reach this codepath if another thread raced with us
                // and created the object while we were outside the pref lock.
                Q_ASSERT(!qmlEngine || s_GlobalPrefs->m_QmlEngine == qmlEngine);
            }
        }
        else {
            s_GlobalPrefs = new StreamingPreferences(qmlEngine);
        }

        return s_GlobalPrefs;
    }
}

void StreamingPreferences::reload()
{
    QSettings settings;

    int defaultVer = settings.value(SER_DEFAULTVER, 0).toInt();

#ifdef Q_OS_DARWIN
    recommendedFullScreenMode = WindowMode::WM_FULLSCREEN_DESKTOP;
#else
    // Wayland doesn't support modesetting, so use fullscreen desktop mode
    // unless we have a slow GPU (which can take advantage of wp_viewporter
    // to reduce GPU load with lower resolution video streams).
    if (WMUtils::isRunningWayland() && !WMUtils::isGpuSlow()) {
        recommendedFullScreenMode = WindowMode::WM_FULLSCREEN_DESKTOP;
    }
    else {
        recommendedFullScreenMode = WindowMode::WM_FULLSCREEN;
    }
#endif

    width = settings.value(SER_WIDTH, 1920).toInt();
    height = settings.value(SER_HEIGHT, 1080).toInt();
    fps = settings.value(SER_FPS, 60).toInt();
    enableYUV444 = settings.value(SER_YUV444, false).toBool();
    bitrateKbps = settings.value(SER_BITRATE, getDefaultBitrate(width, height, fps, enableYUV444)).toInt();
    unlockBitrate = settings.value(SER_UNLOCK_BITRATE, false).toBool();
    autoAdjustBitrate = settings.value(SER_AUTOADJUSTBITRATE, true).toBool();
    enableVsync = settings.value(SER_VSYNC, true).toBool();
    gameOptimizations = settings.value(SER_GAMEOPTS, true).toBool();
    playAudioOnHost = settings.value(SER_HOSTAUDIO, false).toBool();
    multiController = settings.value(SER_MULTICONT, true).toBool();
    enableMdns = settings.value(SER_MDNS, true).toBool();
    quitAppAfter = settings.value(SER_QUITAPPAFTER, false).toBool();
    absoluteMouseMode = settings.value(SER_ABSMOUSEMODE, false).toBool();
    absoluteTouchMode = settings.value(SER_ABSTOUCHMODE, true).toBool();
    framePacing = settings.value(SER_FRAMEPACING, false).toBool();
    connectionWarnings = settings.value(SER_CONNWARNINGS, true).toBool();
    configurationWarnings = settings.value(SER_CONFWARNINGS, true).toBool();
    gamepadMouse = settings.value(SER_GAMEPADMOUSE, true).toBool();
    showPerformanceOverlay = settings.value(SER_SHOWPERFOVERLAY, false).toBool();
    packetSize = settings.value(SER_PACKETSIZE, 0).toInt();
    swapMouseButtons = settings.value(SER_SWAPMOUSEBUTTONS, false).toBool();
    swapCmdCtrl = settings.value(SER_SWAPCMDCTRL, false).toBool();
    muteOnFocusLoss = settings.value(SER_MUTEONFOCUSLOSS, false).toBool();
    backgroundGamepad = settings.value(SER_BACKGROUNDGAMEPAD, false).toBool();
    reverseScrollDirection = settings.value(SER_REVERSESCROLL, false).toBool();
    swapFaceButtons = settings.value(SER_SWAPFACEBUTTONS, false).toBool();
    keepAwake = settings.value(SER_KEEPAWAKE, true).toBool();
    // secure-desktop-stream: microphone passthrough
    micPassthrough = settings.value(SER_MICPASSTHROUGH, false).toBool();
    micEchoCancellation = settings.value(SER_MICECHOCANCEL, true).toBool();
    enableHdr = settings.value(SER_HDR, false).toBool();
    captureSysKeysMode = static_cast<CaptureSysKeysMode>(settings.value(SER_CAPTURESYSKEYS,
                                                         static_cast<int>(CaptureSysKeysMode::CSK_OFF)).toInt());
    audioConfig = static_cast<AudioConfig>(settings.value(SER_AUDIOCFG,
                                                  static_cast<int>(AudioConfig::AC_STEREO)).toInt());
    videoCodecConfig = static_cast<VideoCodecConfig>(settings.value(SER_VIDEOCFG,
                                                  static_cast<int>(VideoCodecConfig::VCC_AUTO)).toInt());
    videoDecoderSelection = static_cast<VideoDecoderSelection>(settings.value(SER_VIDEODEC,
                                                  static_cast<int>(VideoDecoderSelection::VDS_AUTO)).toInt());
    rendererSelection = static_cast<RendererSelection>(settings.value(SER_RENDERER,
                                                  static_cast<int>(RendererSelection::RS_AUTO)).toInt());
    windowMode = static_cast<WindowMode>(settings.value(SER_WINDOWMODE,
                                                        // Try to load from the old preference value too
                                                        static_cast<int>(settings.value(SER_FULLSCREEN, true).toBool() ?
                                                                             recommendedFullScreenMode : WindowMode::WM_WINDOWED)).toInt());
    uiDisplayMode = static_cast<UIDisplayMode>(settings.value(SER_UIDISPLAYMODE,
                                               static_cast<int>(settings.value(SER_STARTWINDOWED, true).toBool() ? UIDisplayMode::UI_WINDOWED
                                                                                                                 : UIDisplayMode::UI_MAXIMIZED)).toInt());
    language = static_cast<Language>(settings.value(SER_LANGUAGE,
                                                    static_cast<int>(Language::LANG_AUTO)).toInt());

    // secure-desktop-stream: clipboard and file transfer
    clipboardSync = settings.value(SER_SDSCLIPBOARD, true).toBool();
    fileTransfer = settings.value(SER_SDSFILETRANSFER, true).toBool();
    fileClipboard = settings.value(SER_SDSFILECLIPBOARD, false).toBool();
    fileClipboardHosts = settings.value(SER_SDSFILECLIPBOARDHOSTS, QStringList()).toStringList();
    autoDownloadOutbox = settings.value(SER_SDSAUTODOWNLOAD, true).toBool();

    // secure-desktop-stream: host encoder, per-host GPU adapter, and resolution matching
    int rawHostEnc = settings.value(SER_SDSHOSTENCODER, static_cast<int>(HostEncoderConfig::HEC_AUTO)).toInt();
    if (rawHostEnc < static_cast<int>(HostEncoderConfig::HEC_AUTO) || rawHostEnc > static_cast<int>(HostEncoderConfig::HEC_SOFTWARE)) {
        rawHostEnc = static_cast<int>(HostEncoderConfig::HEC_AUTO);
    }
    hostEncoderConfig = static_cast<HostEncoderConfig>(rawHostEnc);
    hostEncoderOverrides = settings.value(SER_SDSHOSTENCODEROVERRIDES, QVariantMap()).toMap();
    hostAdapterOverrides = settings.value(SER_SDSHOSTADAPTERS, QVariantMap()).toMap();
    hostCaptureAdapterOverrides = settings.value(SER_SDSHOSTCAPTUREADAPTERS, QVariantMap()).toMap();
    hostAdaptiveRefinement = settings.value(SER_SDSHOSTADAPTIVEREFINEMENT, QVariantMap()).toMap();
    hostMatchResolutionOverrides = settings.value(SER_SDSHOSTMATCHRES, QVariantMap()).toMap();
    hostRevertResolutionOverrides = settings.value(SER_SDSHOSTREVERTRES, QVariantMap()).toMap();
    hostGameOptimizationsOverrides = settings.value(SER_HOSTGAMEOPTS, QVariantMap()).toMap();
    hostQuitAppAfterOverrides = settings.value(SER_HOSTQUITAPPAFTER, QVariantMap()).toMap();
    hostPlayAudioOverrides = settings.value(SER_HOSTPLAYAUDIO, QVariantMap()).toMap();
    matchHostResolution = settings.value(SER_SDSMATCHHOSTRES, false).toBool();
    revertHostResolution = settings.value(SER_SDSREVERTHOSTRES, false).toBool();


    // Perform default settings updates as required based on last default version
    if (defaultVer < 1) {
#ifdef Q_OS_DARWIN
        // Update window mode setting on macOS from full-screen (old default) to borderless windowed (new default)
        if (windowMode == WindowMode::WM_FULLSCREEN) {
            windowMode = WindowMode::WM_FULLSCREEN_DESKTOP;
        }
#endif
    }
    if (defaultVer < 2) {
        if (windowMode == WindowMode::WM_FULLSCREEN && WMUtils::isRunningWayland()) {
            windowMode = WindowMode::WM_FULLSCREEN_DESKTOP;
        }
    }

    // Fixup VCC value to the new settings format with codec and HDR separate
    if (videoCodecConfig == VCC_FORCE_HEVC_HDR_DEPRECATED) {
        videoCodecConfig = VCC_AUTO;
        enableHdr = true;
    }
}

bool StreamingPreferences::retranslate()
{
    static QTranslator* translator = nullptr;

#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
    if (m_QmlEngine != nullptr) {
        // Dynamic retranslation is not supported until Qt 5.10
        return false;
    }
#endif

    QTranslator* newTranslator = new QTranslator();
    QString languageSuffix = getSuffixFromLanguage(language);

    // Remove the old translator, even if we can't load a new one.
    // Otherwise we'll be stuck with the old translated values instead
    // of defaulting to English.
    if (translator != nullptr) {
        QCoreApplication::removeTranslator(translator);
        delete translator;
        translator = nullptr;
    }

    if (newTranslator->load(QString(":/languages/qml_") + languageSuffix)) {
        qInfo() << "Successfully loaded translation for" << languageSuffix;

        translator = newTranslator;
        QCoreApplication::installTranslator(translator);
    }
    else {
        qInfo() << "No translation available for" << languageSuffix;
        delete newTranslator;
    }

    if (m_QmlEngine != nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        // This is a dynamic retranslation from the settings page.
        // We have to kick the QML engine into reloading our text.
        m_QmlEngine->retranslate();
#else
        // Unreachable below Qt 5.10 due to the check above
        Q_ASSERT(false);
#endif
    }
    else {
        // This is a translation from a non-QML context, which means
        // it is probably app startup. There's nothing to refresh.
    }

    return true;
}

QString StreamingPreferences::getSuffixFromLanguage(StreamingPreferences::Language lang)
{
    switch (lang)
    {
    case LANG_DE:
        return "de";
    case LANG_EN:
        return "en";
    case LANG_FR:
        return "fr";
    case LANG_ZH_CN:
        return "zh_CN";
    case LANG_NB_NO:
        return "nb_NO";
    case LANG_RU:
        return "ru";
    case LANG_ES:
        return "es";
    case LANG_JA:
        return "ja";
    case LANG_VI:
        return "vi";
    case LANG_TH:
        return "th";
    case LANG_KO:
        return "ko";
    case LANG_HU:
        return "hu";
    case LANG_NL:
        return "nl";
    case LANG_SV:
        return "sv";
    case LANG_TR:
        return "tr";
    case LANG_UK:
        return "uk";
    case LANG_ZH_TW:
        return "zh_TW";
    case LANG_PT:
        return "pt";
    case LANG_PT_BR:
        return "pt_BR";
    case LANG_EL:
        return "el";
    case LANG_IT:
        return "it";
    case LANG_HI:
        return "hi";
    case LANG_PL:
        return "pl";
    case LANG_CS:
        return "cs";
    case LANG_HE:
        return "he";
    case LANG_CKB:
        return "ckb";
    case LANG_LT:
        return "lt";
    case LANG_ET:
        return "et";
    case LANG_BG:
        return "bg";
    case LANG_EO:
        return "eo";
    case LANG_TA:
        return "ta";
    case LANG_AUTO:
    default:
        return QLocale::system().name();
    }
}

void StreamingPreferences::save()
{
    QSettings settings;

    settings.setValue(SER_WIDTH, width);
    settings.setValue(SER_HEIGHT, height);
    settings.setValue(SER_FPS, fps);
    settings.setValue(SER_BITRATE, bitrateKbps);
    settings.setValue(SER_UNLOCK_BITRATE, unlockBitrate);
    settings.setValue(SER_AUTOADJUSTBITRATE, autoAdjustBitrate);
    settings.setValue(SER_VSYNC, enableVsync);
    settings.setValue(SER_GAMEOPTS, gameOptimizations);
    settings.setValue(SER_HOSTAUDIO, playAudioOnHost);
    settings.setValue(SER_MULTICONT, multiController);
    settings.setValue(SER_MDNS, enableMdns);
    settings.setValue(SER_QUITAPPAFTER, quitAppAfter);
    settings.setValue(SER_ABSMOUSEMODE, absoluteMouseMode);
    settings.setValue(SER_ABSTOUCHMODE, absoluteTouchMode);
    settings.setValue(SER_FRAMEPACING, framePacing);
    settings.setValue(SER_CONNWARNINGS, connectionWarnings);
    settings.setValue(SER_CONFWARNINGS, configurationWarnings);
    settings.setValue(SER_GAMEPADMOUSE, gamepadMouse);
    settings.setValue(SER_PACKETSIZE, packetSize);
    settings.setValue(SER_SHOWPERFOVERLAY, showPerformanceOverlay);
    // secure-desktop-stream: microphone passthrough
    settings.setValue(SER_MICPASSTHROUGH, micPassthrough);
    settings.setValue(SER_MICECHOCANCEL, micEchoCancellation);
    settings.setValue(SER_AUDIOCFG, static_cast<int>(audioConfig));
    settings.setValue(SER_HDR, enableHdr);
    settings.setValue(SER_YUV444, enableYUV444);
    settings.setValue(SER_VIDEOCFG, static_cast<int>(videoCodecConfig));
    settings.setValue(SER_VIDEODEC, static_cast<int>(videoDecoderSelection));
    settings.setValue(SER_RENDERER, static_cast<int>(rendererSelection));
    settings.setValue(SER_WINDOWMODE, static_cast<int>(windowMode));
    settings.setValue(SER_UIDISPLAYMODE, static_cast<int>(uiDisplayMode));
    settings.setValue(SER_LANGUAGE, static_cast<int>(language));
    settings.setValue(SER_DEFAULTVER, CURRENT_DEFAULT_VER);
    settings.setValue(SER_SWAPMOUSEBUTTONS, swapMouseButtons);
    settings.setValue(SER_SWAPCMDCTRL, swapCmdCtrl);
    settings.setValue(SER_MUTEONFOCUSLOSS, muteOnFocusLoss);
    settings.setValue(SER_BACKGROUNDGAMEPAD, backgroundGamepad);
    settings.setValue(SER_REVERSESCROLL, reverseScrollDirection);
    settings.setValue(SER_SWAPFACEBUTTONS, swapFaceButtons);
    settings.setValue(SER_CAPTURESYSKEYS, captureSysKeysMode);
    settings.setValue(SER_KEEPAWAKE, keepAwake);

    // secure-desktop-stream: clipboard and file transfer
    settings.setValue(SER_SDSCLIPBOARD, clipboardSync);
    settings.setValue(SER_SDSFILETRANSFER, fileTransfer);
    settings.setValue(SER_SDSFILECLIPBOARD, fileClipboard);
    settings.setValue(SER_SDSFILECLIPBOARDHOSTS, fileClipboardHosts);
    settings.setValue(SER_SDSAUTODOWNLOAD, autoDownloadOutbox);

    // secure-desktop-stream: host encoder, per-host GPU adapter, and resolution matching
    settings.setValue(SER_SDSHOSTENCODER, static_cast<int>(hostEncoderConfig));
    settings.setValue(SER_SDSHOSTENCODEROVERRIDES, hostEncoderOverrides);
    settings.setValue(SER_SDSHOSTADAPTERS, hostAdapterOverrides);
    settings.setValue(SER_SDSHOSTCAPTUREADAPTERS, hostCaptureAdapterOverrides);
    settings.setValue(SER_SDSHOSTADAPTIVEREFINEMENT, hostAdaptiveRefinement);
    settings.setValue(SER_SDSHOSTMATCHRES, hostMatchResolutionOverrides);
    settings.setValue(SER_SDSHOSTREVERTRES, hostRevertResolutionOverrides);
    settings.setValue(SER_HOSTGAMEOPTS, hostGameOptimizationsOverrides);
    settings.setValue(SER_HOSTQUITAPPAFTER, hostQuitAppAfterOverrides);
    settings.setValue(SER_HOSTPLAYAUDIO, hostPlayAudioOverrides);
    settings.setValue(SER_SDSMATCHHOSTRES, matchHostResolution);
    settings.setValue(SER_SDSREVERTHOSTRES, revertHostResolution);
}

namespace {
QString lookupPerHostOverride(const QVariantMap& map, const QString& fingerprint)
{
    if (fingerprint.isEmpty()) {
        return QString();
    }
    return map.value(fingerprint).toString();
}

bool updatePerHostOverride(QVariantMap& map, const QString& fingerprint, const QString& adapterId)
{
    if (fingerprint.isEmpty()) {
        return false;
    }
    QString trimmed = adapterId.trimmed();
    if (trimmed.isEmpty()) {
        if (!map.contains(fingerprint)) {
            return false;
        }
        map.remove(fingerprint);
        return true;
    }
    if (map.value(fingerprint).toString() == trimmed) {
        return false;
    }
    map.insert(fingerprint, trimmed);
    return true;
}

int lookupBoolOverrideState(const QVariantMap& map, const QString& fingerprint)
{
    if (fingerprint.isEmpty() || !map.contains(fingerprint)) {
        return -1;
    }
    return map.value(fingerprint).toBool() ? 1 : 0;
}

bool resolveBoolOverride(const QVariantMap& map, const QString& fingerprint, bool globalDefault)
{
    if (!fingerprint.isEmpty() && map.contains(fingerprint)) {
        return map.value(fingerprint).toBool();
    }
    return globalDefault;
}

bool updateBoolOverrideState(QVariantMap& map, const QString& fingerprint, int state)
{
    if (fingerprint.isEmpty()) {
        return false;
    }
    if (state < 0) {
        if (!map.contains(fingerprint)) {
            return false;
        }
        map.remove(fingerprint);
        return true;
    }
    const bool value = (state > 0);
    if (map.contains(fingerprint) && map.value(fingerprint).toBool() == value) {
        return false;
    }
    map.insert(fingerprint, value);
    return true;
}
} // namespace

StreamingPreferences::HostEncoderConfig StreamingPreferences::hostEncoderForHost(const QString& fingerprint) const
{
    if (!fingerprint.isEmpty() && hostEncoderOverrides.contains(fingerprint)) {
        int raw = hostEncoderOverrides.value(fingerprint).toInt();
        if (raw >= static_cast<int>(HEC_AUTO) && raw <= static_cast<int>(HEC_SOFTWARE)) {
            return static_cast<HostEncoderConfig>(raw);
        }
        return HEC_AUTO;
    }
    return hostEncoderConfig;
}

void StreamingPreferences::setHostEncoderForHost(const QString& fingerprint, HostEncoderConfig config)
{
    if (fingerprint.isEmpty()) {
        return;
    }
    int clamped = static_cast<int>(config);
    if (clamped < static_cast<int>(HEC_AUTO) || clamped > static_cast<int>(HEC_SOFTWARE)) {
        clamped = static_cast<int>(HEC_AUTO);
    }
    // Remove key when matching default HEC_AUTO unless global legacy config differs
    if (clamped == static_cast<int>(HEC_AUTO) && hostEncoderConfig == HEC_AUTO) {
        if (!hostEncoderOverrides.contains(fingerprint)) {
            return;
        }
        hostEncoderOverrides.remove(fingerprint);
        emit hostEncoderOverridesChanged();
        return;
    }
    if (hostEncoderOverrides.contains(fingerprint) && hostEncoderOverrides.value(fingerprint).toInt() == clamped) {
        return;
    }
    hostEncoderOverrides.insert(fingerprint, clamped);
    emit hostEncoderOverridesChanged();
}

QString StreamingPreferences::hostAdapterOverride(const QString& fingerprint) const
{
    return lookupPerHostOverride(hostAdapterOverrides, fingerprint);
}

void StreamingPreferences::setHostAdapterOverride(const QString& fingerprint, const QString& adapterId)
{
    if (updatePerHostOverride(hostAdapterOverrides, fingerprint, adapterId)) {
        emit hostAdapterOverridesChanged();
    }
}

QString StreamingPreferences::hostCaptureAdapterOverride(const QString& fingerprint) const
{
    return lookupPerHostOverride(hostCaptureAdapterOverrides, fingerprint);
}

void StreamingPreferences::setHostCaptureAdapterOverride(const QString& fingerprint, const QString& adapterId)
{
    if (updatePerHostOverride(hostCaptureAdapterOverrides, fingerprint, adapterId)) {
        emit hostCaptureAdapterOverridesChanged();
    }
}

bool StreamingPreferences::adaptiveRefinementForHost(const QString& fingerprint) const
{
    if (fingerprint.isEmpty()) {
        return false;
    }
    return hostAdaptiveRefinement.value(fingerprint, false).toBool();
}

void StreamingPreferences::setAdaptiveRefinementForHost(const QString& fingerprint, bool enabled)
{
    if (fingerprint.isEmpty()) {
        return;
    }
    if (!enabled) {
        if (!hostAdaptiveRefinement.contains(fingerprint)) {
            return;
        }
        hostAdaptiveRefinement.remove(fingerprint);
        emit hostAdaptiveRefinementChanged();
        return;
    }
    if (hostAdaptiveRefinement.value(fingerprint, false).toBool()) {
        return;
    }
    hostAdaptiveRefinement.insert(fingerprint, true);
    emit hostAdaptiveRefinementChanged();
}

int StreamingPreferences::matchHostResolutionOverrideState(const QString& fingerprint) const
{
    return lookupBoolOverrideState(hostMatchResolutionOverrides, fingerprint);
}

void StreamingPreferences::setMatchHostResolutionOverride(const QString& fingerprint, int state)
{
    if (updateBoolOverrideState(hostMatchResolutionOverrides, fingerprint, state)) {
        emit hostMatchResolutionOverridesChanged();
    }
}

bool StreamingPreferences::matchHostResolutionForHost(const QString& fingerprint) const
{
    return resolveBoolOverride(hostMatchResolutionOverrides, fingerprint, matchHostResolution);
}

int StreamingPreferences::revertHostResolutionOverrideState(const QString& fingerprint) const
{
    return lookupBoolOverrideState(hostRevertResolutionOverrides, fingerprint);
}

void StreamingPreferences::setRevertHostResolutionOverride(const QString& fingerprint, int state)
{
    if (updateBoolOverrideState(hostRevertResolutionOverrides, fingerprint, state)) {
        emit hostRevertResolutionOverridesChanged();
    }
}

bool StreamingPreferences::revertHostResolutionForHost(const QString& fingerprint) const
{
    return resolveBoolOverride(hostRevertResolutionOverrides, fingerprint, revertHostResolution);
}

bool StreamingPreferences::effectiveRevertHostResolutionForHost(const QString& fingerprint) const
{
    return matchHostResolutionForHost(fingerprint) && revertHostResolutionForHost(fingerprint);
}

int StreamingPreferences::gameOptimizationsOverrideState(const QString& fingerprint) const
{
    return lookupBoolOverrideState(hostGameOptimizationsOverrides, fingerprint);
}

void StreamingPreferences::setGameOptimizationsOverride(const QString& fingerprint, int state)
{
    if (updateBoolOverrideState(hostGameOptimizationsOverrides, fingerprint, state)) {
        emit hostGameOptimizationsOverridesChanged();
    }
}

bool StreamingPreferences::gameOptimizationsForHost(const QString& fingerprint) const
{
    return resolveBoolOverride(hostGameOptimizationsOverrides, fingerprint, gameOptimizations);
}

int StreamingPreferences::quitAppAfterOverrideState(const QString& fingerprint) const
{
    return lookupBoolOverrideState(hostQuitAppAfterOverrides, fingerprint);
}

void StreamingPreferences::setQuitAppAfterOverride(const QString& fingerprint, int state)
{
    if (updateBoolOverrideState(hostQuitAppAfterOverrides, fingerprint, state)) {
        emit hostQuitAppAfterOverridesChanged();
    }
}

bool StreamingPreferences::quitAppAfterForHost(const QString& fingerprint) const
{
    return resolveBoolOverride(hostQuitAppAfterOverrides, fingerprint, quitAppAfter);
}

int StreamingPreferences::playAudioOnHostOverrideState(const QString& fingerprint) const
{
    return lookupBoolOverrideState(hostPlayAudioOverrides, fingerprint);
}

void StreamingPreferences::setPlayAudioOnHostOverride(const QString& fingerprint, int state)
{
    if (updateBoolOverrideState(hostPlayAudioOverrides, fingerprint, state)) {
        emit hostPlayAudioOverridesChanged();
    }
}

bool StreamingPreferences::playAudioOnHostForHost(const QString& fingerprint) const
{
    return resolveBoolOverride(hostPlayAudioOverrides, fingerprint, playAudioOnHost);
}

bool StreamingPreferences::clearHostOverrides(const QString& fingerprint)
{
    if (fingerprint.isEmpty()) {
        return false;
    }
    bool changed = false;
    auto removeMapEntry = [&fingerprint, &changed](QVariantMap& map, auto signalFn) {
        if (map.remove(fingerprint) > 0) {
            signalFn();
            changed = true;
        }
    };
    removeMapEntry(hostEncoderOverrides, [this]() { emit hostEncoderOverridesChanged(); });
    removeMapEntry(hostAdapterOverrides, [this]() { emit hostAdapterOverridesChanged(); });
    removeMapEntry(hostCaptureAdapterOverrides, [this]() { emit hostCaptureAdapterOverridesChanged(); });
    removeMapEntry(hostAdaptiveRefinement, [this]() { emit hostAdaptiveRefinementChanged(); });
    removeMapEntry(hostMatchResolutionOverrides, [this]() { emit hostMatchResolutionOverridesChanged(); });
    removeMapEntry(hostRevertResolutionOverrides, [this]() { emit hostRevertResolutionOverridesChanged(); });
    removeMapEntry(hostGameOptimizationsOverrides, [this]() { emit hostGameOptimizationsOverridesChanged(); });
    removeMapEntry(hostQuitAppAfterOverrides, [this]() { emit hostQuitAppAfterOverridesChanged(); });
    removeMapEntry(hostPlayAudioOverrides, [this]() { emit hostPlayAudioOverridesChanged(); });
    return changed;
}

void StreamingPreferences::requestMicrophonePermission()
{
    MicPermission::request([](bool granted) {
        qInfo() << "Microphone access" << (granted ? "granted" : "not granted");
    });
}

bool StreamingPreferences::isMicCaptureSupported() const
{
    return MicPermission::status() != MicPermission::Status::Unsupported;
}

int StreamingPreferences::getDefaultBitrate(int width, int height, int fps, bool yuv444)
{
    // Don't scale bitrate linearly beyond 60 FPS. It's definitely not a linear
    // bitrate increase for frame rate once we get to values that high.
    float frameRateFactor = (fps <= 60 ? fps : (qSqrt(fps / 60.f) * 60.f)) / 30.f;

    // TODO: Collect some empirical data to see if these defaults make sense.
    // We're just using the values that the Shield used, as we have for years.
    static const struct resTable {
        int pixels;
        int factor;
    } resTable[] {
        { 640 * 360, 1 },
        { 854 * 480, 2 },
        { 1280 * 720, 5 },
        { 1920 * 1080, 10 },
        { 2560 * 1440, 20 },
        { 3840 * 2160, 40 },
        { -1, -1 },
    };

    // Calculate the resolution factor by linear interpolation of the resolution table
    float resolutionFactor;
    int pixels = width * height;
    for (int i = 0;; i++) {
        if (pixels == resTable[i].pixels) {
            // We can bail immediately for exact matches
            resolutionFactor = resTable[i].factor;
            break;
        }
        else if (pixels < resTable[i].pixels) {
            if (i == 0) {
                // Never go below the lowest resolution entry
                resolutionFactor = resTable[i].factor;
            }
            else {
                // Interpolate between the entry greater than the chosen resolution (i) and the entry less than the chosen resolution (i-1)
                resolutionFactor = ((float)(pixels - resTable[i-1].pixels) / (resTable[i].pixels - resTable[i-1].pixels)) * (resTable[i].factor - resTable[i-1].factor) + resTable[i-1].factor;
            }
            break;
        }
        else if (resTable[i].pixels == -1) {
            // Never go above the highest resolution entry
            resolutionFactor = resTable[i-1].factor;
            break;
        }
    }

    if (yuv444) {
        // This is rough estimation based on the fact that 4:4:4 doubles the amount of raw YUV data compared to 4:2:0
        resolutionFactor *= 2;
    }

    return qRound(resolutionFactor * frameRateFactor) * 1000;
}
