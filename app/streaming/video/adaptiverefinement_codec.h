#pragma once

#include <QByteArray>
#include <QMutex>

#include "streaming/bandwidth.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace AdaptiveRefinementWire
{
    inline constexpr int TILE_SIZE = 64;
    inline constexpr std::uint16_t PROTOCOL_VERSION = 1;
    inline constexpr std::uint16_t FLAG_RESET_ALL = 0x0001;
    inline constexpr int HEADER_SIZE = 16;
    inline constexpr int TILE_HEADER_SIZE = 16;
    inline constexpr int MAX_TILES_PER_BATCH = 64;
    inline constexpr int MAX_BATCH_BYTES = 256 * 1024;
    inline constexpr int MAX_TILE_PAYLOAD = 24 * 1024;
    inline constexpr int MAX_FRAME_DIM = 16384;
    inline constexpr int MAX_INBOX_TILES = 4096;
}

enum class RefinementColorspace : int
{
    Rec601 = 0,
    Rec709 = 1,
    Rec2020 = 2,
};

struct AdaptiveDecodedTile
{
    std::uint32_t seq = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t w = 0;
    std::uint16_t h = 0;
    bool isClear = false;
    QByteArray rgba; // w * h * 4 bytes: (R, G, B, Y_ref) when !isClear
    RefinementColorspace colorSpace = RefinementColorspace::Rec601;
    bool fullRange = true;
};

struct AdaptiveTileBatch
{
    bool valid = false;
    bool resetAll = false;
    std::uint16_t frameW = 0;
    std::uint16_t frameH = 0;
    std::size_t wireBytes = 0;
    std::vector<AdaptiveDecodedTile> tiles;
};

std::uint8_t computeRefinementYRef(std::uint8_t r,
                                    std::uint8_t g,
                                    std::uint8_t b,
                                    RefinementColorspace colorSpace,
                                    bool fullRange);

void recomputeTileYRef(AdaptiveDecodedTile& tile,
                       RefinementColorspace colorSpace,
                       bool fullRange);

bool encodeQoiRgb24(const std::uint8_t* rgb,
                    int strideBytes,
                    int width,
                    int height,
                    std::vector<std::uint8_t>& outQoi);

bool decodeQoiRgb24(const std::uint8_t* data,
                    std::size_t size,
                    int expectedWidth,
                    int expectedHeight,
                    std::vector<std::uint8_t>& outRgb);

bool parseTileRefinementBatch(const QByteArray& binary,
                              RefinementColorspace colorSpace,
                              bool fullRange,
                              AdaptiveTileBatch& outBatch);

class AdaptiveTileInbox
{
public:
    AdaptiveTileInbox() = default;

    void setColorimetry(RefinementColorspace colorSpace, bool fullRange);
    void colorimetry(RefinementColorspace& outColorSpace, bool& outFullRange) const;

    void pushBatch(AdaptiveTileBatch&& batch);
    void requestReset(std::uint16_t frameW = 0, std::uint16_t frameH = 0);
    void clear();

    bool takePending(bool& outResetAll,
                     std::uint16_t& outFrameW,
                     std::uint16_t& outFrameH,
                     std::vector<AdaptiveDecodedTile>& outTiles,
                     RefinementColorspace targetColorSpace,
                     bool targetFullRange);

    std::size_t queuedTileCount() const;

    /**
     * @brief Returns the recent average bitrate in Mbps for received adaptive tile batches.
     */
    double averageBitrateMbps();

    /**
     * @brief Returns the peak bucket bitrate in Mbps within the sliding window.
     */
    double peakBitrateMbps();

    /**
     * @brief Returns the sliding window duration in seconds for adaptive tile bitrate tracking.
     */
    unsigned int bitrateWindowSeconds();

private:
    static std::uint64_t tileKey(std::uint16_t x, std::uint16_t y)
    {
        return (static_cast<std::uint64_t>(x) << 16) | static_cast<std::uint64_t>(y);
    }

    mutable QMutex m_Mutex;
    bool m_PendingReset = false;
    std::uint16_t m_ResetFrameW = 0;
    std::uint16_t m_ResetFrameH = 0;
    std::deque<std::uint64_t> m_Order;
    std::unordered_map<std::uint64_t, AdaptiveDecodedTile> m_Tiles;
    BandwidthTracker m_BwTracker{10, 250};

    std::atomic<int> m_Colorspace{static_cast<int>(RefinementColorspace::Rec601)};
    std::atomic<bool> m_FullRange{true};
};
