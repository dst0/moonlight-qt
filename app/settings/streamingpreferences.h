#pragma once

#include <QObject>
#include <QRect>
#include <QQmlEngine>
#include <QStringList>
#include <QVariantMap>

class StreamingPreferences : public QObject
{
    Q_OBJECT

public:
    static StreamingPreferences* get(QQmlEngine *qmlEngine = nullptr);

    Q_INVOKABLE static int
    getDefaultBitrate(int width, int height, int fps, bool yuv444);

    Q_INVOKABLE void save();

    void reload();

    enum AudioConfig
    {
        AC_STEREO,
        AC_51_SURROUND,
        AC_71_SURROUND
    };
    Q_ENUM(AudioConfig)

    enum VideoCodecConfig
    {
        VCC_AUTO,
        VCC_FORCE_H264,
        VCC_FORCE_HEVC,
        VCC_FORCE_HEVC_HDR_DEPRECATED, // Kept for backwards compatibility
        VCC_FORCE_AV1
    };
    Q_ENUM(VideoCodecConfig)

    enum VideoDecoderSelection
    {
        VDS_AUTO,
        VDS_FORCE_HARDWARE,
        VDS_FORCE_SOFTWARE
    };
    Q_ENUM(VideoDecoderSelection)

    // secure-desktop-stream: per-session host encoder override
    enum HostEncoderConfig
    {
        HEC_AUTO,
        HEC_HARDWARE_VAAPI,
        HEC_HARDWARE_VULKAN,
        HEC_SOFTWARE
    };
    Q_ENUM(HostEncoderConfig)

    // secure-desktop-stream: true bit-exact SDLL v1 lossless video mode
    enum LosslessDecoderMode
    {
        LDM_AUTO,
        LDM_GPU,
        LDM_CPU
    };
    Q_ENUM(LosslessDecoderMode)

    enum LosslessHashMode
    {
        LHM_CRC32C,
        LHM_BLAKE3,
        LHM_OFF
    };
    Q_ENUM(LosslessHashMode)

    enum LosslessHostEncoderMode
    {
        LHEM_AUTO,
        LHEM_GPU,
        LHEM_CPU
    };
    Q_ENUM(LosslessHostEncoderMode)

    // Mac only (for now)
    enum RendererSelection
    {
        RS_PROBE_ONLY = -1, // Only valid for probing decoder properties
        RS_AUTO,
        RS_VULKAN,
        RS_METAL,
        RS_AVSBDL
    };
    Q_ENUM(RendererSelection)

    enum WindowMode
    {
        WM_FULLSCREEN,
        WM_FULLSCREEN_DESKTOP,
        WM_WINDOWED
    };
    Q_ENUM(WindowMode)

    enum UIDisplayMode
    {
        UI_WINDOWED,
        UI_MAXIMIZED,
        UI_FULLSCREEN
    };
    Q_ENUM(UIDisplayMode)

    // New entries must go at the end of the enum
    // to avoid renumbering existing entries (which
    // would affect existing user preferences).
    enum Language
    {
        LANG_AUTO,
        LANG_EN,
        LANG_FR,
        LANG_ZH_CN,
        LANG_DE,
        LANG_NB_NO,
        LANG_RU,
        LANG_ES,
        LANG_JA,
        LANG_VI,
        LANG_TH,
        LANG_KO,
        LANG_HU,
        LANG_NL,
        LANG_SV,
        LANG_TR,
        LANG_UK,
        LANG_ZH_TW,
        LANG_PT,
        LANG_PT_BR,
        LANG_EL,
        LANG_IT,
        LANG_HI,
        LANG_PL,
        LANG_CS,
        LANG_HE,
        LANG_CKB,
        LANG_LT,
        LANG_ET,
        LANG_BG,
        LANG_EO,
        LANG_TA,
    };
    Q_ENUM(Language);

    enum CaptureSysKeysMode
    {
        CSK_OFF,
        CSK_FULLSCREEN,
        CSK_ALWAYS,
    };
    Q_ENUM(CaptureSysKeysMode);

    Q_PROPERTY(int width MEMBER width NOTIFY displayModeChanged)
    Q_PROPERTY(int height MEMBER height NOTIFY displayModeChanged)
    Q_PROPERTY(int fps MEMBER fps NOTIFY displayModeChanged)
    Q_PROPERTY(int bitrateKbps MEMBER bitrateKbps NOTIFY bitrateChanged)
    Q_PROPERTY(bool unlockBitrate MEMBER unlockBitrate NOTIFY unlockBitrateChanged)
    Q_PROPERTY(bool autoAdjustBitrate MEMBER autoAdjustBitrate NOTIFY autoAdjustBitrateChanged)
    Q_PROPERTY(bool enableVsync MEMBER enableVsync NOTIFY enableVsyncChanged)
    Q_PROPERTY(bool gameOptimizations MEMBER gameOptimizations NOTIFY gameOptimizationsChanged)
    Q_PROPERTY(bool playAudioOnHost MEMBER playAudioOnHost NOTIFY playAudioOnHostChanged)
    Q_PROPERTY(bool multiController MEMBER multiController NOTIFY multiControllerChanged)
    Q_PROPERTY(bool enableMdns MEMBER enableMdns NOTIFY enableMdnsChanged)
    Q_PROPERTY(bool quitAppAfter MEMBER quitAppAfter NOTIFY quitAppAfterChanged)
    Q_PROPERTY(bool absoluteMouseMode MEMBER absoluteMouseMode NOTIFY absoluteMouseModeChanged)
    Q_PROPERTY(bool absoluteTouchMode MEMBER absoluteTouchMode NOTIFY absoluteTouchModeChanged)
    Q_PROPERTY(bool framePacing MEMBER framePacing NOTIFY framePacingChanged)
    Q_PROPERTY(bool connectionWarnings MEMBER connectionWarnings NOTIFY connectionWarningsChanged)
    Q_PROPERTY(bool configurationWarnings MEMBER configurationWarnings NOTIFY configurationWarningsChanged)
    Q_PROPERTY(bool gamepadMouse MEMBER gamepadMouse NOTIFY gamepadMouseChanged)
    Q_PROPERTY(bool showPerformanceOverlay MEMBER showPerformanceOverlay NOTIFY showPerformanceOverlayChanged)
    Q_PROPERTY(AudioConfig audioConfig MEMBER audioConfig NOTIFY audioConfigChanged)
    Q_PROPERTY(VideoCodecConfig videoCodecConfig MEMBER videoCodecConfig NOTIFY videoCodecConfigChanged)
    Q_PROPERTY(bool enableHdr MEMBER enableHdr NOTIFY enableHdrChanged)
    Q_PROPERTY(bool enableYUV444 MEMBER enableYUV444 NOTIFY enableYUV444Changed)
    Q_PROPERTY(VideoDecoderSelection videoDecoderSelection MEMBER videoDecoderSelection NOTIFY videoDecoderSelectionChanged)
    Q_PROPERTY(RendererSelection rendererSelection MEMBER rendererSelection NOTIFY rendererSelectionChanged)
    Q_PROPERTY(WindowMode windowMode MEMBER windowMode NOTIFY windowModeChanged)
    Q_PROPERTY(WindowMode recommendedFullScreenMode MEMBER recommendedFullScreenMode CONSTANT)
    Q_PROPERTY(UIDisplayMode uiDisplayMode MEMBER uiDisplayMode NOTIFY uiDisplayModeChanged)
    Q_PROPERTY(bool swapMouseButtons MEMBER swapMouseButtons NOTIFY mouseButtonsChanged)
    Q_PROPERTY(bool swapCmdCtrl MEMBER swapCmdCtrl NOTIFY swapCmdCtrlChanged)
    Q_PROPERTY(bool muteOnFocusLoss MEMBER muteOnFocusLoss NOTIFY muteOnFocusLossChanged)
    Q_PROPERTY(bool backgroundGamepad MEMBER backgroundGamepad NOTIFY backgroundGamepadChanged)
    Q_PROPERTY(bool reverseScrollDirection MEMBER reverseScrollDirection NOTIFY reverseScrollDirectionChanged)
    Q_PROPERTY(bool swapFaceButtons MEMBER swapFaceButtons NOTIFY swapFaceButtonsChanged)
    Q_PROPERTY(bool keepAwake MEMBER keepAwake NOTIFY keepAwakeChanged)
    // secure-desktop-stream: microphone passthrough
    Q_PROPERTY(bool micPassthrough MEMBER micPassthrough NOTIFY micPassthroughChanged)
    Q_PROPERTY(bool micEchoCancellation MEMBER micEchoCancellation NOTIFY micEchoCancellationChanged)
    Q_PROPERTY(bool micCaptureSupported READ isMicCaptureSupported CONSTANT)
    Q_PROPERTY(CaptureSysKeysMode captureSysKeysMode MEMBER captureSysKeysMode NOTIFY captureSysKeysModeChanged)
    Q_PROPERTY(Language language MEMBER language NOTIFY languageChanged);

    // secure-desktop-stream: clipboard and file transfer
    Q_PROPERTY(bool clipboardSync MEMBER clipboardSync NOTIFY clipboardSyncChanged)
    Q_PROPERTY(bool fileTransfer MEMBER fileTransfer NOTIFY fileTransferChanged)
    Q_PROPERTY(bool fileClipboard MEMBER fileClipboard NOTIFY fileClipboardChanged)
    Q_PROPERTY(bool autoDownloadOutbox MEMBER autoDownloadOutbox NOTIFY autoDownloadOutboxChanged)

    // secure-desktop-stream: host encoder, per-host GPU adapter, and resolution matching
    Q_PROPERTY(HostEncoderConfig hostEncoderConfig MEMBER hostEncoderConfig NOTIFY hostEncoderConfigChanged)
    Q_PROPERTY(QVariantMap hostEncoderOverrides MEMBER hostEncoderOverrides NOTIFY hostEncoderOverridesChanged)
    Q_PROPERTY(QVariantMap hostAdapterOverrides MEMBER hostAdapterOverrides NOTIFY hostAdapterOverridesChanged)
    Q_PROPERTY(QVariantMap hostCaptureAdapterOverrides MEMBER hostCaptureAdapterOverrides NOTIFY hostCaptureAdapterOverridesChanged)
    Q_PROPERTY(QVariantMap hostAdaptiveRefinement MEMBER hostAdaptiveRefinement NOTIFY hostAdaptiveRefinementChanged)
    Q_PROPERTY(QVariantMap hostMatchResolutionOverrides MEMBER hostMatchResolutionOverrides NOTIFY hostMatchResolutionOverridesChanged)
    Q_PROPERTY(QVariantMap hostRevertResolutionOverrides MEMBER hostRevertResolutionOverrides NOTIFY hostRevertResolutionOverridesChanged)
    Q_PROPERTY(QVariantMap hostGameOptimizationsOverrides MEMBER hostGameOptimizationsOverrides NOTIFY hostGameOptimizationsOverridesChanged)
    Q_PROPERTY(QVariantMap hostQuitAppAfterOverrides MEMBER hostQuitAppAfterOverrides NOTIFY hostQuitAppAfterOverridesChanged)
    Q_PROPERTY(QVariantMap hostPlayAudioOverrides MEMBER hostPlayAudioOverrides NOTIFY hostPlayAudioOverridesChanged)
    Q_PROPERTY(bool matchHostResolution MEMBER matchHostResolution NOTIFY matchHostResolutionChanged)
    Q_PROPERTY(bool revertHostResolution MEMBER revertHostResolution NOTIFY revertHostResolutionChanged)

    // secure-desktop-stream: true bit-exact SDLL v1 lossless video mode
    Q_PROPERTY(bool enableLosslessMode MEMBER enableLosslessMode NOTIFY enableLosslessModeChanged)
    Q_PROPERTY(LosslessDecoderMode losslessDecoderMode MEMBER losslessDecoderMode NOTIFY losslessDecoderModeChanged)
    Q_PROPERTY(LosslessHashMode losslessHashMode MEMBER losslessHashMode NOTIFY losslessHashModeChanged)
    Q_PROPERTY(LosslessHostEncoderMode losslessHostEncoderMode MEMBER losslessHostEncoderMode NOTIFY losslessHostEncoderModeChanged)
    Q_PROPERTY(QVariantMap hostLosslessModeOverrides MEMBER hostLosslessModeOverrides NOTIFY hostLosslessModeOverridesChanged)

    Q_INVOKABLE bool retranslate();
    Q_INVOKABLE HostEncoderConfig hostEncoderForHost(const QString& fingerprint) const;
    Q_INVOKABLE void setHostEncoderForHost(const QString& fingerprint, HostEncoderConfig config);
    Q_INVOKABLE QString hostAdapterOverride(const QString& fingerprint) const;
    Q_INVOKABLE void setHostAdapterOverride(const QString& fingerprint, const QString& adapterId);
    Q_INVOKABLE QString hostCaptureAdapterOverride(const QString& fingerprint) const;
    Q_INVOKABLE void setHostCaptureAdapterOverride(const QString& fingerprint, const QString& adapterId);
    Q_INVOKABLE bool adaptiveRefinementForHost(const QString& fingerprint) const;
    Q_INVOKABLE void setAdaptiveRefinementForHost(const QString& fingerprint, bool enabled);
    // Per-PC bool override states: -1 = Global Default, 0 = Disabled, 1 = Enabled
    Q_INVOKABLE int matchHostResolutionOverrideState(const QString& fingerprint) const;
    Q_INVOKABLE void setMatchHostResolutionOverride(const QString& fingerprint, int state);
    Q_INVOKABLE bool matchHostResolutionForHost(const QString& fingerprint) const;
    Q_INVOKABLE int revertHostResolutionOverrideState(const QString& fingerprint) const;
    Q_INVOKABLE void setRevertHostResolutionOverride(const QString& fingerprint, int state);
    Q_INVOKABLE bool revertHostResolutionForHost(const QString& fingerprint) const;
    Q_INVOKABLE bool effectiveRevertHostResolutionForHost(const QString& fingerprint) const;
    Q_INVOKABLE int gameOptimizationsOverrideState(const QString& fingerprint) const;
    Q_INVOKABLE void setGameOptimizationsOverride(const QString& fingerprint, int state);
    Q_INVOKABLE bool gameOptimizationsForHost(const QString& fingerprint) const;
    Q_INVOKABLE int quitAppAfterOverrideState(const QString& fingerprint) const;
    Q_INVOKABLE void setQuitAppAfterOverride(const QString& fingerprint, int state);
    Q_INVOKABLE bool quitAppAfterForHost(const QString& fingerprint) const;
    Q_INVOKABLE int playAudioOnHostOverrideState(const QString& fingerprint) const;
    Q_INVOKABLE void setPlayAudioOnHostOverride(const QString& fingerprint, int state);
    Q_INVOKABLE bool playAudioOnHostForHost(const QString& fingerprint) const;
    Q_INVOKABLE int losslessModeOverrideState(const QString& fingerprint) const;
    Q_INVOKABLE void setLosslessModeOverride(const QString& fingerprint, int state);
    Q_INVOKABLE bool losslessModeForHost(const QString& fingerprint) const;
    Q_INVOKABLE static QString losslessHashQueryValue(LosslessHashMode mode)
    {
        switch (mode) {
        case LHM_BLAKE3:
            return QStringLiteral("blake3");
        case LHM_OFF:
            return QStringLiteral("off");
        case LHM_CRC32C:
        default:
            return QStringLiteral("crc32c");
        }
    }
    Q_INVOKABLE static QString losslessHostEncoderQueryValue(LosslessHostEncoderMode mode)
    {
        switch (mode) {
        case LHEM_GPU:
            return QStringLiteral("gpu");
        case LHEM_CPU:
            return QStringLiteral("cpu");
        case LHEM_AUTO:
        default:
            return QStringLiteral("auto");
        }
    }
    Q_INVOKABLE bool clearHostOverrides(const QString& fingerprint);
    Q_INVOKABLE static QString hostEncoderQueryValue(HostEncoderConfig config)
    {
        switch (config) {
        case HEC_HARDWARE_VAAPI:
            return QStringLiteral("vaapi");
        case HEC_HARDWARE_VULKAN:
            return QStringLiteral("vulkan");
        case HEC_SOFTWARE:
            return QStringLiteral("software");
        case HEC_AUTO:
        default:
            return QStringLiteral("auto");
        }
    }
    Q_INVOKABLE bool effectiveRevertHostResolution() const { return matchHostResolution && revertHostResolution; }

    // secure-desktop-stream: asks for macOS microphone access (no-op if already decided)
    Q_INVOKABLE void requestMicrophonePermission();
    bool isMicCaptureSupported() const;

    // Directly accessible members for preferences
    int width;
    int height;
    int fps;
    int bitrateKbps;
    bool unlockBitrate;
    bool autoAdjustBitrate;
    bool enableVsync;
    bool gameOptimizations;
    bool playAudioOnHost;
    bool multiController;
    bool enableMdns;
    bool quitAppAfter;
    bool absoluteMouseMode;
    bool absoluteTouchMode;
    bool framePacing;
    bool connectionWarnings;
    bool configurationWarnings;
    bool gamepadMouse;
    bool showPerformanceOverlay;
    bool swapMouseButtons;
    bool swapCmdCtrl;
    bool muteOnFocusLoss;
    bool backgroundGamepad;
    bool reverseScrollDirection;
    bool swapFaceButtons;
    bool keepAwake;
    // secure-desktop-stream: microphone passthrough
    bool micPassthrough;
    bool micEchoCancellation;
    int packetSize;
    AudioConfig audioConfig;
    VideoCodecConfig videoCodecConfig;
    bool enableHdr;
    bool enableYUV444;
    VideoDecoderSelection videoDecoderSelection;
    WindowMode windowMode;
    WindowMode recommendedFullScreenMode;
    UIDisplayMode uiDisplayMode;
    Language language;
    CaptureSysKeysMode captureSysKeysMode;
    RendererSelection rendererSelection;

    // secure-desktop-stream: clipboard and file transfer
    bool clipboardSync;
    bool fileTransfer;
    bool fileClipboard;
    // Explicitly approved SHA-256 pinned-certificate fingerprints; empty by default.
    QStringList fileClipboardHosts;
    bool autoDownloadOutbox;

    // secure-desktop-stream: host encoder, per-host GPU adapter, and resolution matching
    HostEncoderConfig hostEncoderConfig;
    QVariantMap hostEncoderOverrides;
    QVariantMap hostAdapterOverrides;
    QVariantMap hostCaptureAdapterOverrides;
    QVariantMap hostAdaptiveRefinement;
    QVariantMap hostMatchResolutionOverrides;
    QVariantMap hostRevertResolutionOverrides;
    QVariantMap hostGameOptimizationsOverrides;
    QVariantMap hostQuitAppAfterOverrides;
    QVariantMap hostPlayAudioOverrides;
    bool matchHostResolution;
    bool revertHostResolution;

    // secure-desktop-stream: true bit-exact SDLL v1 lossless video mode
    bool enableLosslessMode;
    LosslessDecoderMode losslessDecoderMode;
    LosslessHashMode losslessHashMode;
    LosslessHostEncoderMode losslessHostEncoderMode;
    QVariantMap hostLosslessModeOverrides;

signals:
    void displayModeChanged();
    void bitrateChanged();
    void unlockBitrateChanged();
    void autoAdjustBitrateChanged();
    void enableVsyncChanged();
    void gameOptimizationsChanged();
    void playAudioOnHostChanged();
    void multiControllerChanged();
    void unsupportedFpsChanged();
    void enableMdnsChanged();
    void quitAppAfterChanged();
    void absoluteMouseModeChanged();
    void absoluteTouchModeChanged();
    void audioConfigChanged();
    void videoCodecConfigChanged();
    void enableHdrChanged();
    void enableYUV444Changed();
    void videoDecoderSelectionChanged();
    void uiDisplayModeChanged();
    void windowModeChanged();
    void framePacingChanged();
    void connectionWarningsChanged();
    void configurationWarningsChanged();
    void gamepadMouseChanged();
    void showPerformanceOverlayChanged();
    void mouseButtonsChanged();
    void swapCmdCtrlChanged();
    void muteOnFocusLossChanged();
    void backgroundGamepadChanged();
    void reverseScrollDirectionChanged();
    void swapFaceButtonsChanged();
    void captureSysKeysModeChanged();
    void keepAwakeChanged();
    // secure-desktop-stream: microphone passthrough
    void micPassthroughChanged();
    void micEchoCancellationChanged();
    void languageChanged();
    void rendererSelectionChanged();

    // secure-desktop-stream: clipboard and file transfer
    void clipboardSyncChanged();
    void fileTransferChanged();
    void fileClipboardChanged();
    void autoDownloadOutboxChanged();

    // secure-desktop-stream: host encoder, per-host GPU adapter, and resolution matching
    void hostEncoderConfigChanged();
    void hostEncoderOverridesChanged();
    void hostAdapterOverridesChanged();
    void hostCaptureAdapterOverridesChanged();
    void hostAdaptiveRefinementChanged();
    void hostMatchResolutionOverridesChanged();
    void hostRevertResolutionOverridesChanged();
    void hostGameOptimizationsOverridesChanged();
    void hostQuitAppAfterOverridesChanged();
    void hostPlayAudioOverridesChanged();
    void matchHostResolutionChanged();
    void revertHostResolutionChanged();
    void enableLosslessModeChanged();
    void losslessDecoderModeChanged();
    void losslessHashModeChanged();
    void losslessHostEncoderModeChanged();
    void hostLosslessModeOverridesChanged();

private:
    explicit StreamingPreferences(QQmlEngine *qmlEngine);

    QString getSuffixFromLanguage(Language lang);

    QQmlEngine* m_QmlEngine;
};

