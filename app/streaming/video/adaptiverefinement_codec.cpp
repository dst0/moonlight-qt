#include "adaptiverefinement_codec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace
{
    constexpr std::array<std::uint8_t, 8> kQoiEndMarker{0, 0, 0, 0, 0, 0, 0, 1};
    constexpr std::uint8_t QOI_OP_INDEX = 0x00;
    constexpr std::uint8_t QOI_OP_DIFF  = 0x40;
    constexpr std::uint8_t QOI_OP_LUMA  = 0x80;
    constexpr std::uint8_t QOI_OP_RUN   = 0xC0;
    constexpr std::uint8_t QOI_OP_RGB   = 0xFE;
    constexpr std::uint8_t QOI_OP_RGBA  = 0xFF;
    constexpr std::uint8_t QOI_MASK_2   = 0xC0;
    constexpr std::size_t QOI_HEADER_SIZE = 14;

    inline std::uint32_t qoiColorHash(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
    {
        return (static_cast<std::uint32_t>(r) * 3u +
                static_cast<std::uint32_t>(g) * 5u +
                static_cast<std::uint32_t>(b) * 7u +
                static_cast<std::uint32_t>(a) * 11u) & 63u;
    }

    inline std::uint32_t readU32BE(const std::uint8_t* p)
    {
        return (static_cast<std::uint32_t>(p[0]) << 24) |
               (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8) |
               static_cast<std::uint32_t>(p[3]);
    }

    inline std::uint16_t readU16LE(const std::uint8_t* p)
    {
        return static_cast<std::uint16_t>(p[0]) |
               (static_cast<std::uint16_t>(p[1]) << 8);
    }

    inline std::uint32_t readU32LE(const std::uint8_t* p)
    {
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[3]) << 24);
    }

    inline void appendU32BE(std::vector<std::uint8_t>& out, std::uint32_t v)
    {
        out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    }

    void fillTileRgbaFromRgb(const std::vector<std::uint8_t>& rgb,
                             int width,
                             int height,
                             RefinementColorspace cs,
                             bool fullRange,
                             QByteArray& outRgba)
    {
        const int pixelCount = width * height;
        outRgba.resize(pixelCount * 4);
        auto* dst = reinterpret_cast<std::uint8_t*>(outRgba.data());
        for (int i = 0; i < pixelCount; ++i) {
            const std::uint8_t r = rgb[static_cast<std::size_t>(i) * 3u + 0];
            const std::uint8_t g = rgb[static_cast<std::size_t>(i) * 3u + 1];
            const std::uint8_t b = rgb[static_cast<std::size_t>(i) * 3u + 2];
            dst[i * 4 + 0] = r;
            dst[i * 4 + 1] = g;
            dst[i * 4 + 2] = b;
            dst[i * 4 + 3] = computeRefinementYRef(r, g, b, cs, fullRange);
        }
    }
}

std::uint8_t computeRefinementYRef(std::uint8_t r,
                                    std::uint8_t g,
                                    std::uint8_t b,
                                    RefinementColorspace colorSpace,
                                    bool fullRange)
{
    double kr = 0.299;
    double kg = 0.587;
    double kb = 0.114;
    switch (colorSpace) {
    case RefinementColorspace::Rec709:
        kr = 0.2126;
        kg = 0.7152;
        kb = 0.0722;
        break;
    case RefinementColorspace::Rec2020:
        kr = 0.2627;
        kg = 0.6780;
        kb = 0.0593;
        break;
    case RefinementColorspace::Rec601:
    default:
        break;
    }

    const double yNorm = (kr * static_cast<double>(r) +
                          kg * static_cast<double>(g) +
                          kb * static_cast<double>(b)) / 255.0;
    const double scaled = fullRange ? (255.0 * yNorm) : (16.0 + 219.0 * yNorm);
    const int rounded = static_cast<int>(std::llround(scaled));
    const int minVal = fullRange ? 0 : 16;
    const int maxVal = fullRange ? 255 : 235;
    return static_cast<std::uint8_t>(std::clamp(rounded, minVal, maxVal));
}

void recomputeTileYRef(AdaptiveDecodedTile& tile,
                       RefinementColorspace colorSpace,
                       bool fullRange)
{
    const int expectedBytes = static_cast<int>(tile.w) * static_cast<int>(tile.h) * 4;
    if (expectedBytes <= 0 || tile.rgba.size() != expectedBytes) {
        return;
    }
    auto* data = reinterpret_cast<std::uint8_t*>(tile.rgba.data());
    const int pixels = static_cast<int>(tile.w) * static_cast<int>(tile.h);
    for (int i = 0; i < pixels; ++i) {
        const std::uint8_t r = data[i * 4 + 0];
        const std::uint8_t g = data[i * 4 + 1];
        const std::uint8_t b = data[i * 4 + 2];
        data[i * 4 + 3] = computeRefinementYRef(r, g, b, colorSpace, fullRange);
    }
    tile.colorSpace = colorSpace;
    tile.fullRange = fullRange;
}

bool encodeQoiRgb24(const std::uint8_t* rgb,
                    int strideBytes,
                    int width,
                    int height,
                    std::vector<std::uint8_t>& outQoi)
{
    outQoi.clear();
    if (!rgb || width < 1 || width > AdaptiveRefinementWire::TILE_SIZE ||
        height < 1 || height > AdaptiveRefinementWire::TILE_SIZE ||
        strideBytes < width * 3) {
        return false;
    }

    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    outQoi.reserve(QOI_HEADER_SIZE + pixelCount * 4u + kQoiEndMarker.size());
    outQoi.push_back('q');
    outQoi.push_back('o');
    outQoi.push_back('i');
    outQoi.push_back('f');
    appendU32BE(outQoi, static_cast<std::uint32_t>(width));
    appendU32BE(outQoi, static_cast<std::uint32_t>(height));
    outQoi.push_back(3);
    outQoi.push_back(0);

    std::array<std::array<std::uint8_t, 4>, 64> index{};
    std::uint8_t prevR = 0;
    std::uint8_t prevG = 0;
    std::uint8_t prevB = 0;
    constexpr std::uint8_t kAlpha = 255;
    int run = 0;
    std::size_t pxPos = 0;

    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = rgb + static_cast<std::size_t>(y) * static_cast<std::size_t>(strideBytes);
        for (int x = 0; x < width; ++x, ++pxPos) {
            const std::uint8_t r = row[x * 3 + 0];
            const std::uint8_t g = row[x * 3 + 1];
            const std::uint8_t b = row[x * 3 + 2];

            if (r == prevR && g == prevG && b == prevB) {
                ++run;
                if (run == 62 || pxPos + 1 == pixelCount) {
                    outQoi.push_back(static_cast<std::uint8_t>(QOI_OP_RUN | (run - 1)));
                    run = 0;
                }
                continue;
            }

            if (run > 0) {
                outQoi.push_back(static_cast<std::uint8_t>(QOI_OP_RUN | (run - 1)));
                run = 0;
            }

            const std::uint32_t idx = qoiColorHash(r, g, b, kAlpha);
            auto& entry = index[idx];
            if (entry[0] == r && entry[1] == g && entry[2] == b && entry[3] == kAlpha) {
                outQoi.push_back(static_cast<std::uint8_t>(QOI_OP_INDEX | idx));
            } else {
                entry = {r, g, b, kAlpha};
                const int dr = static_cast<int>(static_cast<std::int8_t>(r - prevR));
                const int dg = static_cast<int>(static_cast<std::int8_t>(g - prevG));
                const int db = static_cast<int>(static_cast<std::int8_t>(b - prevB));
                const int drDg = dr - dg;
                const int dbDg = db - dg;

                if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
                    outQoi.push_back(static_cast<std::uint8_t>(
                        QOI_OP_DIFF | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2)));
                } else if (dg >= -32 && dg <= 31 && drDg >= -8 && drDg <= 7 && dbDg >= -8 && dbDg <= 7) {
                    outQoi.push_back(static_cast<std::uint8_t>(QOI_OP_LUMA | (dg + 32)));
                    outQoi.push_back(static_cast<std::uint8_t>(((drDg + 8) << 4) | (dbDg + 8)));
                } else {
                    outQoi.push_back(QOI_OP_RGB);
                    outQoi.push_back(r);
                    outQoi.push_back(g);
                    outQoi.push_back(b);
                }
            }

            prevR = r;
            prevG = g;
            prevB = b;
            if (outQoi.size() > static_cast<std::size_t>(AdaptiveRefinementWire::MAX_TILE_PAYLOAD)) {
                outQoi.clear();
                return false;
            }
        }
    }

    outQoi.insert(outQoi.end(), kQoiEndMarker.begin(), kQoiEndMarker.end());
    if (outQoi.size() > static_cast<std::size_t>(AdaptiveRefinementWire::MAX_TILE_PAYLOAD)) {
        outQoi.clear();
        return false;
    }
    return true;
}

bool decodeQoiRgb24(const std::uint8_t* data,
                    std::size_t size,
                    int expectedWidth,
                    int expectedHeight,
                    std::vector<std::uint8_t>& outRgb)
{
    outRgb.clear();
    if (!data ||
        expectedWidth < 1 || expectedWidth > AdaptiveRefinementWire::TILE_SIZE ||
        expectedHeight < 1 || expectedHeight > AdaptiveRefinementWire::TILE_SIZE ||
        size < QOI_HEADER_SIZE + kQoiEndMarker.size() ||
        size > static_cast<std::size_t>(AdaptiveRefinementWire::MAX_TILE_PAYLOAD)) {
        return false;
    }

    if (data[0] != 'q' || data[1] != 'o' || data[2] != 'i' || data[3] != 'f') {
        return false;
    }
    const std::uint32_t width = readU32BE(data + 4);
    const std::uint32_t height = readU32BE(data + 8);
    const std::uint8_t channels = data[12];
    const std::uint8_t colorspace = data[13];
    if (width != static_cast<std::uint32_t>(expectedWidth) ||
        height != static_cast<std::uint32_t>(expectedHeight) ||
        channels != 3 || colorspace > 1) {
        return false;
    }

    const std::size_t endOffset = size - kQoiEndMarker.size();
    if (std::memcmp(data + endOffset, kQoiEndMarker.data(), kQoiEndMarker.size()) != 0) {
        return false;
    }

    const std::size_t totalPixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    outRgb.resize(totalPixels * 3u);

    std::array<std::array<std::uint8_t, 4>, 64> index{};
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    constexpr std::uint8_t kAlpha = 255;

    std::size_t pos = QOI_HEADER_SIZE;
    std::size_t pxIdx = 0;

    while (pxIdx < totalPixels) {
        if (pos >= endOffset) {
            outRgb.clear();
            return false;
        }
        const std::uint8_t b1 = data[pos++];
        if (b1 == QOI_OP_RGBA) {
            outRgb.clear();
            return false;
        }
        if (b1 == QOI_OP_RGB) {
            if (pos + 3u > endOffset) {
                outRgb.clear();
                return false;
            }
            r = data[pos++];
            g = data[pos++];
            b = data[pos++];
            index[qoiColorHash(r, g, b, kAlpha)] = {r, g, b, kAlpha};
            outRgb[pxIdx * 3u + 0] = r;
            outRgb[pxIdx * 3u + 1] = g;
            outRgb[pxIdx * 3u + 2] = b;
            ++pxIdx;
            continue;
        }

        const std::uint8_t tag = b1 & QOI_MASK_2;
        if (tag == QOI_OP_INDEX) {
            const auto& entry = index[b1 & 0x3Fu];
            r = entry[0];
            g = entry[1];
            b = entry[2];
            outRgb[pxIdx * 3u + 0] = r;
            outRgb[pxIdx * 3u + 1] = g;
            outRgb[pxIdx * 3u + 2] = b;
            ++pxIdx;
        } else if (tag == QOI_OP_DIFF) {
            r = static_cast<std::uint8_t>(r + ((b1 >> 4) & 0x03u) - 2);
            g = static_cast<std::uint8_t>(g + ((b1 >> 2) & 0x03u) - 2);
            b = static_cast<std::uint8_t>(b + (b1 & 0x03u) - 2);
            index[qoiColorHash(r, g, b, kAlpha)] = {r, g, b, kAlpha};
            outRgb[pxIdx * 3u + 0] = r;
            outRgb[pxIdx * 3u + 1] = g;
            outRgb[pxIdx * 3u + 2] = b;
            ++pxIdx;
        } else if (tag == QOI_OP_LUMA) {
            if (pos >= endOffset) {
                outRgb.clear();
                return false;
            }
            const std::uint8_t b2 = data[pos++];
            const int vg = static_cast<int>(b1 & 0x3Fu) - 32;
            r = static_cast<std::uint8_t>(r + vg + static_cast<int>((b2 >> 4) & 0x0Fu) - 8);
            g = static_cast<std::uint8_t>(g + vg);
            b = static_cast<std::uint8_t>(b + vg + static_cast<int>(b2 & 0x0Fu) - 8);
            index[qoiColorHash(r, g, b, kAlpha)] = {r, g, b, kAlpha};
            outRgb[pxIdx * 3u + 0] = r;
            outRgb[pxIdx * 3u + 1] = g;
            outRgb[pxIdx * 3u + 2] = b;
            ++pxIdx;
        } else { // QOI_OP_RUN
            const std::size_t run = static_cast<std::size_t>(b1 & 0x3Fu) + 1u;
            if (run == 0 || run > 62u || pxIdx + run > totalPixels) {
                outRgb.clear();
                return false;
            }
            for (std::size_t k = 0; k < run; ++k) {
                outRgb[(pxIdx + k) * 3u + 0] = r;
                outRgb[(pxIdx + k) * 3u + 1] = g;
                outRgb[(pxIdx + k) * 3u + 2] = b;
            }
            pxIdx += run;
        }
    }

    if (pos != endOffset) {
        outRgb.clear();
        return false;
    }
    return true;
}

bool parseTileRefinementBatch(const QByteArray& binary,
                              RefinementColorspace colorSpace,
                              bool fullRange,
                              AdaptiveTileBatch& outBatch)
{
    outBatch = AdaptiveTileBatch{};
    if (binary.size() < AdaptiveRefinementWire::HEADER_SIZE ||
        binary.size() > AdaptiveRefinementWire::MAX_BATCH_BYTES) {
        return false;
    }

    const auto* p = reinterpret_cast<const std::uint8_t*>(binary.constData());
    if (p[0] != 'S' || p[1] != 'D' || p[2] != 'T' || p[3] != 'R') {
        return false;
    }

    const std::uint16_t version = readU16LE(p + 4);
    const std::uint16_t flags = readU16LE(p + 6);
    const std::uint16_t frameW = readU16LE(p + 8);
    const std::uint16_t frameH = readU16LE(p + 10);
    const std::uint16_t tileCount = readU16LE(p + 12);

    if (version != AdaptiveRefinementWire::PROTOCOL_VERSION ||
        frameW > AdaptiveRefinementWire::MAX_FRAME_DIM ||
        frameH > AdaptiveRefinementWire::MAX_FRAME_DIM ||
        tileCount > AdaptiveRefinementWire::MAX_TILES_PER_BATCH) {
        return false;
    }
    if (tileCount > 0 && (frameW == 0 || frameH == 0)) {
        return false;
    }

    outBatch.resetAll = (flags & AdaptiveRefinementWire::FLAG_RESET_ALL) != 0;
    outBatch.frameW = frameW;
    outBatch.frameH = frameH;
    outBatch.tiles.reserve(tileCount);

    std::size_t offset = AdaptiveRefinementWire::HEADER_SIZE;
    const std::size_t totalSize = static_cast<std::size_t>(binary.size());
    std::vector<std::uint8_t> scratchRgb;

    for (std::uint16_t i = 0; i < tileCount; ++i) {
        if (offset + AdaptiveRefinementWire::TILE_HEADER_SIZE > totalSize) {
            outBatch = AdaptiveTileBatch{};
            return false;
        }
        AdaptiveDecodedTile tile;
        tile.seq = readU32LE(p + offset);
        tile.x = readU16LE(p + offset + 4);
        tile.y = readU16LE(p + offset + 6);
        tile.w = readU16LE(p + offset + 8);
        tile.h = readU16LE(p + offset + 10);
        const std::uint32_t payloadLen = readU32LE(p + offset + 12);
        offset += AdaptiveRefinementWire::TILE_HEADER_SIZE;

        if (tile.w == 0 || tile.w > AdaptiveRefinementWire::TILE_SIZE ||
            tile.h == 0 || tile.h > AdaptiveRefinementWire::TILE_SIZE ||
            (tile.x % AdaptiveRefinementWire::TILE_SIZE) != 0 ||
            (tile.y % AdaptiveRefinementWire::TILE_SIZE) != 0 ||
            static_cast<std::uint32_t>(tile.x) >= static_cast<std::uint32_t>(frameW) ||
            static_cast<std::uint32_t>(tile.y) >= static_cast<std::uint32_t>(frameH) ||
            static_cast<std::uint32_t>(tile.x) + static_cast<std::uint32_t>(tile.w) > static_cast<std::uint32_t>(frameW) ||
            static_cast<std::uint32_t>(tile.y) + static_cast<std::uint32_t>(tile.h) > static_cast<std::uint32_t>(frameH) ||
            payloadLen > static_cast<std::uint32_t>(AdaptiveRefinementWire::MAX_TILE_PAYLOAD) ||
            offset + static_cast<std::size_t>(payloadLen) > totalSize) {
            outBatch = AdaptiveTileBatch{};
            return false;
        }
        const std::uint16_t expectedW = static_cast<std::uint16_t>(
            std::min<std::uint32_t>(AdaptiveRefinementWire::TILE_SIZE,
                                    static_cast<std::uint32_t>(frameW - tile.x)));
        const std::uint16_t expectedH = static_cast<std::uint16_t>(
            std::min<std::uint32_t>(AdaptiveRefinementWire::TILE_SIZE,
                                    static_cast<std::uint32_t>(frameH - tile.y)));
        if (tile.w != expectedW || tile.h != expectedH) {
            outBatch = AdaptiveTileBatch{};
            return false;
        }

        if (!decodeQoiRgb24(p + offset, payloadLen, tile.w, tile.h, scratchRgb)) {
            outBatch = AdaptiveTileBatch{};
            return false;
        }
        offset += static_cast<std::size_t>(payloadLen);

        fillTileRgbaFromRgb(scratchRgb, tile.w, tile.h, colorSpace, fullRange, tile.rgba);
        tile.colorSpace = colorSpace;
        tile.fullRange = fullRange;
        outBatch.tiles.push_back(std::move(tile));
    }

    if (offset != totalSize) {
        outBatch = AdaptiveTileBatch{};
        return false;
    }

    outBatch.valid = true;
    return true;
}

void AdaptiveTileInbox::setColorimetry(RefinementColorspace colorSpace, bool fullRange)
{
    m_Colorspace.store(static_cast<int>(colorSpace), std::memory_order_release);
    m_FullRange.store(fullRange, std::memory_order_release);
}

void AdaptiveTileInbox::colorimetry(RefinementColorspace& outColorSpace, bool& outFullRange) const
{
    outColorSpace = static_cast<RefinementColorspace>(m_Colorspace.load(std::memory_order_acquire));
    outFullRange = m_FullRange.load(std::memory_order_acquire);
}

void AdaptiveTileInbox::pushBatch(AdaptiveTileBatch&& batch)
{
    if (!batch.valid) {
        return;
    }

    QMutexLocker locker(&m_Mutex);
    const bool dimsProvided = (batch.frameW > 0 && batch.frameH > 0);
    const bool dimsChanged = dimsProvided && m_ResetFrameW != 0 && m_ResetFrameH != 0 &&
                             (batch.frameW != m_ResetFrameW || batch.frameH != m_ResetFrameH);
    if (batch.resetAll || dimsChanged) {
        m_PendingReset = true;
        if (dimsProvided || batch.resetAll) {
            m_ResetFrameW = batch.frameW;
            m_ResetFrameH = batch.frameH;
        }
        m_Order.clear();
        m_Tiles.clear();
    } else if (dimsProvided && m_ResetFrameW == 0 && m_ResetFrameH == 0) {
        m_ResetFrameW = batch.frameW;
        m_ResetFrameH = batch.frameH;
    }

    for (auto& tile : batch.tiles) {
        const std::uint64_t key = tileKey(tile.x, tile.y);
        auto it = m_Tiles.find(key);
        if (it != m_Tiles.end()) {
            it->second = std::move(tile);
            continue;
        }

        while (m_Tiles.size() >= static_cast<std::size_t>(AdaptiveRefinementWire::MAX_INBOX_TILES) && !m_Order.empty()) {
            const std::uint64_t oldest = m_Order.front();
            m_Order.pop_front();
            m_Tiles.erase(oldest);
        }

        m_Tiles.emplace(key, std::move(tile));
        m_Order.push_back(key);
    }
}

void AdaptiveTileInbox::requestReset(std::uint16_t frameW, std::uint16_t frameH)
{
    QMutexLocker locker(&m_Mutex);
    m_PendingReset = true;
    m_ResetFrameW = frameW;
    m_ResetFrameH = frameH;
    m_Order.clear();
    m_Tiles.clear();
}

void AdaptiveTileInbox::clear()
{
    QMutexLocker locker(&m_Mutex);
    m_PendingReset = false;
    m_ResetFrameW = 0;
    m_ResetFrameH = 0;
    m_Order.clear();
    m_Tiles.clear();
}

bool AdaptiveTileInbox::takePending(bool& outResetAll,
                                    std::uint16_t& outFrameW,
                                    std::uint16_t& outFrameH,
                                    std::vector<AdaptiveDecodedTile>& outTiles,
                                    RefinementColorspace targetColorSpace,
                                    bool targetFullRange)
{
    outTiles.clear();
    QMutexLocker locker(&m_Mutex);
    outResetAll = m_PendingReset;
    outFrameW = m_ResetFrameW;
    outFrameH = m_ResetFrameH;
    m_PendingReset = false;

    const bool hadWork = outResetAll || !m_Tiles.empty();
    if (!hadWork) {
        return false;
    }

    outTiles.reserve(m_Tiles.size());
    for (const std::uint64_t key : m_Order) {
        auto it = m_Tiles.find(key);
        if (it == m_Tiles.end()) {
            continue;
        }
        auto tile = std::move(it->second);
        if (tile.colorSpace != targetColorSpace || tile.fullRange != targetFullRange) {
            recomputeTileYRef(tile, targetColorSpace, targetFullRange);
        }
        outTiles.push_back(std::move(tile));
    }
    m_Order.clear();
    m_Tiles.clear();
    return true;
}

std::size_t AdaptiveTileInbox::queuedTileCount() const
{
    QMutexLocker locker(&m_Mutex);
    return m_Tiles.size();
}
