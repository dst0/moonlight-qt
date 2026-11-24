#pragma once

#include <QString>

#include "settings/streamingpreferences.h"

/**
 * @brief Coded frame geometry for Layout A1 packed 10-bit YCoCg-R 4:4:4 inside P010.
 *
 * Regions are aligned on 64-pixel HEVC CTU boundaries so Y, Co, and Cg planes
 * never share a CTU across tile seams:
 * - Region 0 (Y):  (0, 0) .. (logicalW - 1, logicalH - 1)
 * - Region 1 (Co): (0, tileH) .. (logicalW - 1, tileH + logicalH - 1)
 * - Region 2 (Cg): left half [0, splitX) at (tileW + x, y),
 *                  right half [splitX, logicalW) at (tileW + x - splitX, tileH + y)
 */
struct PackedYCoCgRLayout
{
    int logicalW = 0;
    int logicalH = 0;
    int tileW = 0;
    int tileH = 0;
    int splitX = 0;
    int halfW = 0;
    int encW = 0;
    int encH = 0;
    bool valid = false;
};

/**
 * @brief Computes Layout A1 packed YCoCg-R P010 dimensions for a logical stream size.
 */
PackedYCoCgRLayout computePackedYCoCgRA1Layout(int logicalW,
                                               int logicalH,
                                               int maxEncW = 8192,
                                               int maxEncH = 4352);

/**
 * @brief Inputs controlling packed YCoCg-R 4:4:4 negotiation and edge-case resolution.
 */
struct Packed444Inputs
{
    bool enableYUV444 = false;
    bool enableHdr = false;
    StreamingPreferences::VideoCodecConfig videoCodecConfig = StreamingPreferences::VCC_AUTO;
    StreamingPreferences::VideoDecoderSelection videoDecoderSelection = StreamingPreferences::VDS_AUTO;
    StreamingPreferences::RendererSelection rendererSelection = StreamingPreferences::RS_AUTO;
    StreamingPreferences::HostEncoderConfig hostEncoderConfig = StreamingPreferences::HEC_AUTO;
    bool hostEncoderIsSoftware = false;
    bool hostSupportsHwYCoCgR444 = false;
    bool hostNativeYuv444Supported = false;
    int maxPackedW = 8192;
    int maxPackedH = 4352;
    int logicalW = 0;
    int logicalH = 0;
    bool isMacOSMetalAvailable = false;
};

/**
 * @brief Resolved packed YCoCg-R 4:4:4 decision, codec/bitrate overrides, and user-facing warning.
 */
struct Packed444Decision
{
    bool usePackedYCoCgRA1 = false;
    bool forceHevcMain10 = false;
    bool keep444Bitrate = false;
    QString userWarning;
    QString fallbackReason;
};

/**
 * @brief Resolves whether hardware packed YCoCg-R P010 4:4:4 streaming should be used
 *        for the given host capabilities and client settings.
 */
Packed444Decision resolvePacked444Decision(const Packed444Inputs& in);
