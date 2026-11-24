#pragma once

// secure-desktop-stream: pure helpers for host encoder / adapter / resolution overrides
// negotiated at stream launch/resume and tested by moonlight-unit-tests.

#include "nvcomputer.h"
#include "settings/streamingpreferences.h"

#include <Limelight.h>
#include <QCryptographicHash>
#include <QString>
#include <QUrl>

namespace SdsLaunch {

inline int effectiveServerCodecModeSupport(bool isNvidiaServerSoftware,
                                           bool hasHostEncoder,
                                           int serverCodecModeSupport,
                                           int sdsSoftwareCodecModeSupport,
                                           StreamingPreferences::HostEncoderConfig hostEncoder)
{
    if (!isNvidiaServerSoftware && hasHostEncoder &&
            hostEncoder == StreamingPreferences::HEC_SOFTWARE) {
        return sdsSoftwareCodecModeSupport != 0
                ? sdsSoftwareCodecModeSupport
                : (SCM_H264 | SCM_H264_HIGH8_444);
    }
    return serverCodecModeSupport;
}

inline bool isHostHevcSupported(bool isNvidiaServerSoftware,
                                bool hasHostEncoder,
                                int maxLumaPixelsHEVC,
                                int effectiveCodecModeSupport,
                                StreamingPreferences::HostEncoderConfig hostEncoder)
{
    if (!isNvidiaServerSoftware && hasHostEncoder &&
            hostEncoder == StreamingPreferences::HEC_SOFTWARE) {
        return (effectiveCodecModeSupport & SCM_MASK_HEVC) != 0;
    }
    return maxLumaPixelsHEVC != 0;
}

inline bool shouldPreferSoftwareH264Yuv444(bool isNvidiaServerSoftware,
                                           bool hasHostEncoder,
                                           StreamingPreferences::HostEncoderConfig hostEncoder,
                                           StreamingPreferences::VideoCodecConfig videoCodec,
                                           bool enableYUV444,
                                           int effectiveCodecModeSupport)
{
    return !isNvidiaServerSoftware &&
           hasHostEncoder &&
           hostEncoder == StreamingPreferences::HEC_SOFTWARE &&
           enableYUV444 &&
           videoCodec == StreamingPreferences::VCC_AUTO &&
           (effectiveCodecModeSupport & SCM_H264_HIGH8_444) != 0;
}

inline QString hostCertFingerprint(const QSslCertificate& cert)
{
    if (cert.isNull()) {
        return QString();
    }
    return QString::fromLatin1(cert.digest(QCryptographicHash::Sha256).toHex());
}

inline bool isKnownHostAdapter(const QVector<NvHostAdapter>& adapters, const QString& adapterId)
{
    if (adapterId.isEmpty()) {
        return false;
    }
    for (const NvHostAdapter& adapter : adapters) {
        if (adapter.id == adapterId) {
            return true;
        }
    }
    return false;
}

inline QString buildLaunchExtraQueryParams(bool isNvidiaServerSoftware,
                                           bool hasHostEncoder,
                                           bool hasHostResolution,
                                           StreamingPreferences::HostEncoderConfig hostEncoder,
                                           const QVector<NvHostAdapter>& adapters,
                                           const QString& selectedAdapterId,
                                           bool hevcRequested,
                                           bool matchHostResolution,
                                           bool revertHostResolution,
                                           bool hasHostCaptureAdapter = false,
                                           const QString& selectedCaptureAdapterId = QString())
{
    if (isNvidiaServerSoftware) {
        return QString();
    }

    QString extra;
    if (hasHostEncoder) {
        const QString encoder = StreamingPreferences::hostEncoderQueryValue(hostEncoder);
        extra += QStringLiteral("&sdsEncoder=") + QString::fromUtf8(QUrl::toPercentEncoding(encoder));

        if (!selectedAdapterId.isEmpty() && isKnownHostAdapter(adapters, selectedAdapterId)) {
            extra += QStringLiteral("&sdsAdapter=") + QString::fromUtf8(QUrl::toPercentEncoding(selectedAdapterId));
        }
    }

    if (hasHostCaptureAdapter && !selectedCaptureAdapterId.isEmpty() && isKnownHostAdapter(adapters, selectedCaptureAdapterId)) {
        extra += QStringLiteral("&sdsCaptureAdapter=") + QString::fromUtf8(QUrl::toPercentEncoding(selectedCaptureAdapterId));
    }

    if (hasHostEncoder) {
        extra += QStringLiteral("&sdsHevc=") + QString::number(hevcRequested ? 1 : 0);
    }

    if (hasHostResolution) {
        const bool effectiveRevert = matchHostResolution && revertHostResolution;
        extra += QStringLiteral("&sdsMatchRes=") + QString::number(matchHostResolution ? 1 : 0);
        extra += QStringLiteral("&sdsRevertRes=") + QString::number(effectiveRevert ? 1 : 0);
    }

    return extra;
}

} // namespace SdsLaunch
