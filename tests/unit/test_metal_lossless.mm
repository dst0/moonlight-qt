#include <QtTest>

#include "streaming/video/lossless_codec.h"
#include "streaming/video/lossless_hash.h"
#include "test_metal_lossless.h"

#define SDS_LOSSLESS_NO_BOOST_LOG 1
#include "../../../sunshine/src/sds/lossless_gpu_metal.mm"

#import <Metal/Metal.h>

#include <QFile>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace {

struct alignas(16) TestCopyRectParam {
    std::uint32_t src_x, src_y, dst_x, dst_y, w, h, pad0, pad1;
};

struct alignas(16) TestDecodeParams {
    std::uint32_t width, height, stride, copy_rect_count, dirty_tile_count, pad0, pad1, pad2;
    TestCopyRectParam copy_rects[sds::lossless::MAX_COPY_RECTS];
};

struct alignas(16) TestDirtyTileDesc {
    std::uint32_t tile_x, tile_y, tile_w, tile_h, mode, payload_offset, payload_len, pad0;
};

class HeadlessMetalDecoder
{
public:
    bool init(const QString& shaderPath)
    {
        @autoreleasepool {
            m_Device = MTLCreateSystemDefaultDevice();
            if (!m_Device) return false;
            m_Queue = [m_Device newCommandQueue];
            if (!m_Queue) return false;

            QFile f(shaderPath);
            if (!f.open(QIODevice::ReadOnly)) return false;
            QByteArray src = f.readAll();
            NSString* srcNs = [[NSString alloc] initWithBytes:src.constData()
                                                       length:src.size()
                                                     encoding:NSUTF8StringEncoding];
            NSError* err = nil;
            id<MTLLibrary> lib = [m_Device newLibraryWithSource:srcNs options:nil error:&err];
            [srcNs release];
            if (!lib) {
                qWarning("Metal decode shader compile error: %s",
                         err ? err.localizedDescription.UTF8String : "unknown");
                return false;
            }
            auto makePso = [&](NSString* name) -> id<MTLComputePipelineState> {
                id<MTLFunction> fn = [lib newFunctionWithName:name];
                if (!fn) return nil;
                id<MTLComputePipelineState> pso = [m_Device newComputePipelineStateWithFunction:fn error:nil];
                [fn release];
                return pso;
            };
            m_Copy = makePso(@"cs_copy_rects");
            m_Decode = makePso(@"cs_decode_tiles");
            m_Crc = makePso(@"cs_crc32c_rows");
            [lib release];
            if (!m_Copy || !m_Decode || !m_Crc) return false;

            std::array<std::uint32_t, 256> tbl {};
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1u) ? ((c >> 1) ^ 0x82F63B78u) : (c >> 1);
                tbl[i] = c;
            }
            m_CrcTable = [m_Device newBufferWithBytes:tbl.data() length:sizeof(tbl) options:MTLResourceStorageModeShared];
            m_Params = [m_Device newBufferWithLength:sizeof(TestDecodeParams) options:MTLResourceStorageModeShared];
            return m_CrcTable && m_Params;
        }
    }

    ~HeadlessMetalDecoder()
    {
        [m_Prev release];
        [m_Curr release];
        [m_Pkt release];
        [m_Desc release];
        [m_RowCrcs release];
        [m_CrcTable release];
        [m_Params release];
        [m_Copy release];
        [m_Decode release];
        [m_Crc release];
        [m_Queue release];
        [m_Device release];
    }

    bool decode(const std::vector<std::uint8_t>& packet,
                std::vector<std::uint8_t>& outBgra,
                sds::lossless::frame_header_t& outHdr,
                bool& outHashOk)
    {
        sds::lossless::parsed_frame_t parsed;
        if (!sds::lossless::parse_frame_packet(packet.data(), packet.size(), parsed)) return false;
        outHdr = parsed.header;

        const bool isKey = (outHdr.flags & sds::lossless::FLAG_KEYFRAME) != 0;
        const int w = outHdr.width, h = outHdr.height, stride = w * 4;
        if (!isKey && (!m_HasPrev || w != m_W || h != m_H)) return false;

        std::vector<sds::lossless::gpu_tile_desc_t> descs;
        if (!sds::lossless::build_dirty_tile_descs(parsed, packet.data(), descs)) return false;

        const std::size_t frameBytes = static_cast<std::size_t>(stride) * h;
        if (frameBytes > m_CapFrame) {
            [m_Prev release]; [m_Curr release];
            m_Prev = [m_Device newBufferWithLength:frameBytes options:MTLResourceStorageModeShared];
            m_Curr = [m_Device newBufferWithLength:frameBytes options:MTLResourceStorageModeShared];
            m_CapFrame = frameBytes;
        }
        if (packet.size() > m_CapPkt) {
            [m_Pkt release];
            m_Pkt = [m_Device newBufferWithLength:packet.size() options:MTLResourceStorageModeShared];
            m_CapPkt = packet.size();
        }
        const std::size_t descBytes = std::max<std::size_t>(descs.size(), 1) * sizeof(sds::lossless::gpu_tile_desc_t);
        if (descBytes > m_CapDesc) {
            [m_Desc release];
            m_Desc = [m_Device newBufferWithLength:descBytes options:MTLResourceStorageModeShared];
            m_CapDesc = descBytes;
        }
        const std::size_t rowBytes = static_cast<std::size_t>(h) * sizeof(std::uint32_t);
        if (rowBytes > m_CapRow) {
            [m_RowCrcs release];
            m_RowCrcs = [m_Device newBufferWithLength:rowBytes options:MTLResourceStorageModeShared];
            m_CapRow = rowBytes;
        }

        std::memcpy([m_Pkt contents], packet.data(), packet.size());
        if (!descs.empty()) {
            std::memcpy([m_Desc contents], descs.data(), descs.size() * sizeof(sds::lossless::gpu_tile_desc_t));
        }

        auto* params = static_cast<TestDecodeParams*>([m_Params contents]);
        std::memset(params, 0, sizeof(*params));
        params->width = outHdr.width; params->height = outHdr.height; params->stride = static_cast<std::uint32_t>(stride);
        params->copy_rect_count = static_cast<std::uint32_t>(std::min<std::size_t>(parsed.copy_rects.size(), sds::lossless::MAX_COPY_RECTS));
        params->dirty_tile_count = static_cast<std::uint32_t>(descs.size());
        for (std::uint32_t i = 0; i < params->copy_rect_count; ++i) {
            params->copy_rects[i] = { parsed.copy_rects[i].src_x, parsed.copy_rects[i].src_y,
                                      parsed.copy_rects[i].dst_x, parsed.copy_rects[i].dst_y,
                                      parsed.copy_rects[i].w, parsed.copy_rects[i].h, 0, 0 };
        }

        @autoreleasepool {
            id<MTLCommandBuffer> cmd = [m_Queue commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cmd computeCommandEncoder];
            if (!isKey) {
                [ce setComputePipelineState:m_Copy];
                [ce setBuffer:m_Prev offset:0 atIndex:0];
                [ce setBuffer:m_Curr offset:0 atIndex:1];
                [ce setBuffer:m_Params offset:0 atIndex:2];
                [ce dispatchThreads:MTLSizeMake(outHdr.width, outHdr.height, 1)
               threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
            }
            if (!descs.empty()) {
                [ce setComputePipelineState:m_Decode];
                [ce setBuffer:m_Pkt offset:0 atIndex:0];
                [ce setBuffer:m_Desc offset:0 atIndex:1];
                [ce setBuffer:m_Curr offset:0 atIndex:2];
                [ce setBuffer:m_Params offset:0 atIndex:3];
                [ce dispatchThreadgroups:MTLSizeMake(descs.size(), 1, 1)
                   threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
            }
            if (outHdr.hash_algo == sds::lossless::hash_algo_e::crc32c) {
                [ce setComputePipelineState:m_Crc];
                [ce setBuffer:m_Curr offset:0 atIndex:0];
                [ce setBuffer:m_RowCrcs offset:0 atIndex:1];
                [ce setBuffer:m_CrcTable offset:0 atIndex:2];
                [ce setBuffer:m_Params offset:0 atIndex:3];
                [ce dispatchThreads:MTLSizeMake(outHdr.height, 1, 1)
               threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            }
            [ce endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];
            if ([cmd status] != MTLCommandBufferStatusCompleted) return false;
        }

        outBgra.resize(frameBytes);
        std::memcpy(outBgra.data(), [m_Curr contents], frameBytes);

        outHashOk = true;
        if (outHdr.hash_algo == sds::lossless::hash_algo_e::crc32c) {
            const auto* rowCrcs = static_cast<const std::uint32_t*>([m_RowCrcs contents]);
            std::uint32_t combined = rowCrcs[0];
            for (int y = 1; y < h; ++y) {
                combined = sds::lossless::crc32c_combine(combined, rowCrcs[y], static_cast<std::uint64_t>(w) * 3ULL);
            }
            const std::uint32_t expected =
                static_cast<std::uint32_t>(outHdr.frame_hash[0]) |
                (static_cast<std::uint32_t>(outHdr.frame_hash[1]) << 8) |
                (static_cast<std::uint32_t>(outHdr.frame_hash[2]) << 16) |
                (static_cast<std::uint32_t>(outHdr.frame_hash[3]) << 24);
            outHashOk = (combined == expected);
        } else if (outHdr.hash_algo == sds::lossless::hash_algo_e::blake3) {
            const auto b3 = sds::lossless::blake3_rgb24_from_bgra(outBgra.data(), stride, w, h);
            outHashOk = (b3 == outHdr.frame_hash);
        }

        m_W = w; m_H = h; m_HasPrev = true;
        std::swap(m_Prev, m_Curr);
        return true;
    }

private:
    id<MTLDevice> m_Device = nil;
    id<MTLCommandQueue> m_Queue = nil;
    id<MTLComputePipelineState> m_Copy = nil, m_Decode = nil, m_Crc = nil;
    id<MTLBuffer> m_Prev = nil, m_Curr = nil, m_Pkt = nil, m_Desc = nil, m_RowCrcs = nil, m_CrcTable = nil, m_Params = nil;
    std::size_t m_CapFrame = 0, m_CapPkt = 0, m_CapDesc = 0, m_CapRow = 0;
    int m_W = 0, m_H = 0;
    bool m_HasPrev = false;
};

std::vector<std::uint8_t> makeRichDesktopBgra(int w, int h, int seed)
{
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(w) * h * 4u);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t idx = (static_cast<std::size_t>(y) * w + x) * 4u;
            std::uint8_t r = 0, g = 0, b = 0;
            if (y < h / 4) {
                // Solid quadrant
                r = 24; g = 42; b = 70;
            } else if (y < h / 2) {
                // 8-color palette UI stripes
                const int col = ((x / 7) ^ (y / 5) ^ seed) & 7;
                r = static_cast<std::uint8_t>((col & 1) ? 230 : 30);
                g = static_cast<std::uint8_t>((col & 2) ? 220 : 40);
                b = static_cast<std::uint8_t>((col & 4) ? 245 : 35);
            } else if (y < (h * 3) / 4) {
                // Smooth gradient
                r = static_cast<std::uint8_t>((x * 255) / std::max(w - 1, 1));
                g = static_cast<std::uint8_t>((y * 255) / std::max(h - 1, 1));
                b = static_cast<std::uint8_t>(((x + y + seed * 13) * 3) & 0xFF);
            } else {
                // High-entropy noise triggering RAW_RGB24 or Rice escape
                std::uint32_t v = static_cast<std::uint32_t>(x * 73856093u) ^
                                  static_cast<std::uint32_t>(y * 19349663u) ^
                                  static_cast<std::uint32_t>(seed * 83492791u);
                v ^= (v >> 13); v *= 0x5bd1e995u;
                r = static_cast<std::uint8_t>(v & 0xFFu);
                g = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
                b = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
            }
            buf[idx + 0] = b;
            buf[idx + 1] = g;
            buf[idx + 2] = r;
            buf[idx + 3] = static_cast<std::uint8_t>((x + y + seed) & 0xFF);
        }
    }
    return buf;
}

void assertRgb24Match(const std::vector<std::uint8_t>& actual, int aStride,
                      const std::vector<std::uint8_t>& expected, int eStride, int w, int h)
{
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* ra = actual.data() + static_cast<std::size_t>(y) * aStride;
        const std::uint8_t* re = expected.data() + static_cast<std::size_t>(y) * eStride;
        for (int x = 0; x < w; ++x) {
            if (ra[x * 4 + 0] != re[x * 4 + 0] ||
                ra[x * 4 + 1] != re[x * 4 + 1] ||
                ra[x * 4 + 2] != re[x * 4 + 2]) {
                QFAIL(qPrintable(QString("RGB24 mismatch at (%1,%2)").arg(x).arg(y)));
            }
        }
    }
}

} // namespace

class MetalLosslessTest : public QObject
{
    Q_OBJECT
private slots:
    void metalGpuComputeEncodeAndDecodeCrossParity()
    {
        auto gpuEnc = sds::lossless::make_gpu_encoder_backend();
        if (!gpuEnc) {
            QSKIP("Metal GPU device not available on this runner");
        }
        QVERIFY(gpuEnc->is_gpu());

        const QString metalPath = QFINDTESTDATA("../../app/shaders/sds_lossless.metal");
        QVERIFY2(!metalPath.isEmpty(), "Could not locate app/shaders/sds_lossless.metal");

        HeadlessMetalDecoder gpuDecFromGpu;
        HeadlessMetalDecoder gpuDecFromCpu;
        QVERIFY(gpuDecFromGpu.init(metalPath));
        QVERIFY(gpuDecFromCpu.init(metalPath));

        sds::lossless::cpu_encoder_t cpuEnc(2);
        sds::lossless::cpu_decoder_t cpuDecFromGpu(2);

        const int w = 1366, h = 768;
        const int stride = w * 4;

        std::vector<std::uint8_t> f1 = makeRichDesktopBgra(w, h, 1);
        // f2: identical RGB with scrambled alpha -> 0 dirty tiles
        std::vector<std::uint8_t> f2 = f1;
        for (std::size_t i = 3; i < f2.size(); i += 4) f2[i] ^= 0x5Au;
        // f3: scroll up by 64 rows + new bottom strip -> CopyRect + Temporal/MED/Palette/Solid
        std::vector<std::uint8_t> f3 = f1;
        for (int y = 0; y < h - 64; ++y) {
            std::memcpy(f3.data() + static_cast<std::size_t>(y) * stride,
                        f1.data() + static_cast<std::size_t>(y + 64) * stride, stride);
        }
        for (int y = h - 64; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::size_t idx = (static_cast<std::size_t>(y) * w + x) * 4u;
                f3[idx + 0] = static_cast<std::uint8_t>(x & 0xFF);
                f3[idx + 1] = static_cast<std::uint8_t>((y * 3) & 0xFF);
                f3[idx + 2] = static_cast<std::uint8_t>(((x ^ y) + 42) & 0xFF);
                f3[idx + 3] = 255;
            }
        }
        // f4: scroll down by 48 rows + BLAKE3 hash
        std::vector<std::uint8_t> f4 = f3;
        for (int y = h - 1; y >= 48; --y) {
            std::memcpy(f4.data() + static_cast<std::size_t>(y) * stride,
                        f3.data() + static_cast<std::size_t>(y - 48) * stride, stride);
        }
        std::vector<std::uint8_t> extra = makeRichDesktopBgra(w, 48, 9);
        std::memcpy(f4.data(), extra.data(), extra.size());

        const std::vector<std::vector<std::uint8_t>*> frames = { &f1, &f2, &f3, &f4 };
        const std::array<sds::lossless::hash_algo_e, 4> algos = {
            sds::lossless::hash_algo_e::crc32c,
            sds::lossless::hash_algo_e::crc32c,
            sds::lossless::hash_algo_e::crc32c,
            sds::lossless::hash_algo_e::blake3
        };

        for (std::size_t fi = 0; fi < frames.size(); ++fi) {
            const auto& frame = *frames[fi];
            const bool key = (fi == 0);
            std::vector<std::uint8_t> gpuPkt, cpuPkt;
            sds::lossless::encode_stats_t gStats {}, cStats {};

            QVERIFY(gpuEnc->encode_frame(frame.data(), stride, w, h, fi + 1, (fi + 1) * 16666,
                                         algos[fi], key, gpuPkt, &gStats));
            QVERIFY(cpuEnc.encode_frame(frame.data(), stride, w, h, fi + 1, (fi + 1) * 16666,
                                        algos[fi], key, cpuPkt, &cStats));

            if (fi == 1) {
                QCOMPARE(gStats.dirty_tiles, 0u);
            } else if (fi == 2 || fi == 3) {
                QVERIFY(gStats.copy_rects >= 1u);
            }

            // Verify GPU and CPU emitted the exact same canonical frame hash!
            sds::lossless::frame_header_t gh {}, ch {};
            QVERIFY(sds::lossless::parse_frame_header(gpuPkt.data(), gpuPkt.size(), gh));
            QVERIFY(sds::lossless::parse_frame_header(cpuPkt.data(), cpuPkt.size(), ch));
            QVERIFY((gh.flags & sds::lossless::FLAG_GPU_ENCODED) != 0);
            QCOMPARE(gh.frame_hash, ch.frame_hash);

            // 1) Metal GPU Encode -> CPU SIMD Decode
            sds::lossless::decode_stats_t dstats {};
            QVERIFY(cpuDecFromGpu.decode_frame(gpuPkt.data(), gpuPkt.size(), true, nullptr, &dstats));
            QVERIFY(dstats.hash_verified);
            QVERIFY(dstats.hash_matched);
            std::vector<std::uint8_t> cpuOut(cpuDecFromGpu.current_bgra(),
                                             cpuDecFromGpu.current_bgra() + static_cast<std::size_t>(cpuDecFromGpu.stride()) * h);
            assertRgb24Match(cpuOut, cpuDecFromGpu.stride(), frame, stride, w, h);

            // 2) Metal GPU Encode -> Metal GPU Decode
            std::vector<std::uint8_t> gpuOut1;
            sds::lossless::frame_header_t dh1 {};
            bool hOk1 = false;
            QVERIFY(gpuDecFromGpu.decode(gpuPkt, gpuOut1, dh1, hOk1));
            QVERIFY(hOk1);
            assertRgb24Match(gpuOut1, stride, frame, stride, w, h);

            // 3) CPU SIMD Encode -> Metal GPU Decode
            std::vector<std::uint8_t> gpuOut2;
            sds::lossless::frame_header_t dh2 {};
            bool hOk2 = false;
            QVERIFY(gpuDecFromCpu.decode(cpuPkt, gpuOut2, dh2, hOk2));
            QVERIFY(hOk2);
            assertRgb24Match(gpuOut2, stride, frame, stride, w, h);
        }
    }
};

int runMetalLosslessTests(int argc, char** argv)
{
    MetalLosslessTest t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_metal_lossless.moc"
