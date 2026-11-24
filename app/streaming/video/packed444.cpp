#include "packed444.h"

namespace {

constexpr int kCtuAlignment = 64;
constexpr int kDefaultMaxEncW = 8192;
constexpr int kDefaultMaxEncH = 4352;
constexpr int kMaxSafeDimension = 65536;

int alignUp64(int value)
{
    return ((value + kCtuAlignment - 1) / kCtuAlignment) * kCtuAlignment;
}

QString formatResolutionLimitWarning(int logicalW, int logicalH, int maxEncW, int maxEncH)
{
    if (logicalW < kCtuAlignment || logicalH < kCtuAlignment) {
        return QStringLiteral("Selected resolution is below minimum hardware packed 4:4:4 size (min 64x64).");
    }

    if (maxEncW == kDefaultMaxEncW && maxEncH == kDefaultMaxEncH) {
        return QStringLiteral("Selected resolution exceeds host hardware packed 4:4:4 limit (max 3840x2160).");
    }

    // Derive max logical bounds supported by the advertised coded limits.
    const int maxLogicalH = (maxEncH / (2 * kCtuAlignment)) * kCtuAlignment;
    int maxLogicalW = 0;
    for (int w = kCtuAlignment; w <= maxEncW; w += kCtuAlignment) {
        const int encW = alignUp64(w) + alignUp64((w + 1) / 2);
        if (encW > maxEncW) {
            break;
        }
        maxLogicalW = w;
    }

    if (maxLogicalW >= kCtuAlignment && maxLogicalH >= kCtuAlignment) {
        return QStringLiteral("Selected resolution exceeds host hardware packed 4:4:4 limit (max %1x%2).")
            .arg(maxLogicalW)
            .arg(maxLogicalH);
    }

    return QStringLiteral("Selected resolution exceeds host hardware packed 4:4:4 limit (max coded %1x%2).")
        .arg(maxEncW)
        .arg(maxEncH);
}

} // namespace

PackedYCoCgRLayout computePackedYCoCgRA1Layout(int logicalW, int logicalH, int maxEncW, int maxEncH)
{
    PackedYCoCgRLayout layout;
    layout.logicalW = logicalW;
    layout.logicalH = logicalH;

    if (logicalW <= 0 || logicalH <= 0 ||
        logicalW > kMaxSafeDimension || logicalH > kMaxSafeDimension ||
        maxEncW < kCtuAlignment || maxEncH < kCtuAlignment) {
        return layout;
    }

    layout.tileW = alignUp64(logicalW);
    layout.splitX = (logicalW + 1) / 2;
    layout.halfW = alignUp64(layout.splitX);
    layout.tileH = alignUp64(logicalH);
    layout.encW = layout.tileW + layout.halfW;
    layout.encH = 2 * layout.tileH;
    layout.valid = (logicalW >= kCtuAlignment &&
                    logicalH >= kCtuAlignment &&
                    layout.encW <= maxEncW &&
                    layout.encH <= maxEncH);
    return layout;
}

Packed444Decision resolvePacked444Decision(const Packed444Inputs& in)
{
    Packed444Decision decision;

    if (!in.enableYUV444) {
        return decision;
    }

    if (in.hostNativeYuv444Supported) {
        decision.fallbackReason = QStringLiteral("host_native_yuv444");
        return decision;
    }

    if (in.hostEncoderIsSoftware ||
        in.hostEncoderConfig == StreamingPreferences::HEC_SOFTWARE) {
        decision.fallbackReason = QStringLiteral("host_software_encoder");
        return decision;
    }

    if (!in.hostSupportsHwYCoCgR444) {
        decision.fallbackReason = QStringLiteral("host_unsupported");
        return decision;
    }

    if (in.hostEncoderConfig == StreamingPreferences::HEC_HARDWARE_VULKAN) {
        decision.fallbackReason = QStringLiteral("host_encoder_vulkan");
        decision.userWarning = QStringLiteral(
            "Hardware packed 4:4:4 requires Automatic or VAAPI host encoder; streaming 4:2:0 with Vulkan.");
        return decision;
    }

    if (in.enableHdr) {
        decision.fallbackReason = QStringLiteral("hdr_enabled");
        decision.userWarning = QStringLiteral(
            "Hardware packed 4:4:4 cannot be combined with HDR; streaming 10-bit HDR 4:2:0.");
        return decision;
    }

    if (in.videoCodecConfig == StreamingPreferences::VCC_FORCE_H264 ||
        in.videoCodecConfig == StreamingPreferences::VCC_FORCE_AV1) {
        decision.fallbackReason = QStringLiteral("unsupported_codec");
        decision.userWarning = QStringLiteral(
            "Hardware packed 4:4:4 requires Automatic or HEVC codec.");
        return decision;
    }

    if (!in.isMacOSMetalAvailable ||
        (in.rendererSelection != StreamingPreferences::RS_AUTO &&
         in.rendererSelection != StreamingPreferences::RS_METAL)) {
        decision.fallbackReason = QStringLiteral("unsupported_renderer");
        decision.userWarning = QStringLiteral(
            "Hardware packed 4:4:4 requires Automatic or Metal renderer.");
        return decision;
    }

    const int maxW = (in.maxPackedW > 0) ? in.maxPackedW : kDefaultMaxEncW;
    const int maxH = (in.maxPackedH > 0) ? in.maxPackedH : kDefaultMaxEncH;
    const PackedYCoCgRLayout layout = computePackedYCoCgRA1Layout(in.logicalW, in.logicalH, maxW, maxH);
    if (!layout.valid) {
        decision.fallbackReason = (in.logicalW < kCtuAlignment || in.logicalH < kCtuAlignment)
            ? QStringLiteral("resolution_too_small")
            : QStringLiteral("resolution_exceeds_limit");
        decision.userWarning = formatResolutionLimitWarning(in.logicalW, in.logicalH, maxW, maxH);
        return decision;
    }

    decision.usePackedYCoCgRA1 = true;
    decision.forceHevcMain10 = true;
    decision.keep444Bitrate = true;
    return decision;
}
