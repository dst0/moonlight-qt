#include "lossless_metal.h"

#include "lossless_hash.h"
#include "path.h"
#include "streaming/session.h"
#include "streaming/streamutils.h"

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <QElapsedTimer>
#include <QFile>
#include <QMutexLocker>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace {

struct alignas(16) MetalCopyRectParam
{
    std::uint32_t src_x;
    std::uint32_t src_y;
    std::uint32_t dst_x;
    std::uint32_t dst_y;
    std::uint32_t w;
    std::uint32_t h;
    std::uint32_t pad0;
    std::uint32_t pad1;
};

struct alignas(16) MetalDecodeParams
{
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stride;
    std::uint32_t copy_rect_count;
    std::uint32_t dirty_tile_count;
    std::uint32_t pad0;
    std::uint32_t pad1;
    std::uint32_t pad2;
    MetalCopyRectParam copy_rects[sds::lossless::MAX_COPY_RECTS];
};

struct alignas(16) MetalDirtyTileDesc
{
    std::uint32_t tile_x;
    std::uint32_t tile_y;
    std::uint32_t tile_w;
    std::uint32_t tile_h;
    std::uint32_t mode;
    std::uint32_t payload_offset;
    std::uint32_t payload_len;
    std::uint32_t pad0;
};

QByteArray loadMetalShaderSource()
{
    QByteArray src = Path::readDataFile("sds_lossless.metal");
    if (!src.isEmpty()) {
        return src;
    }
    QFile resFile(QStringLiteral(":/data/sds_lossless.metal"));
    if (resFile.open(QIODevice::ReadOnly)) {
        return resFile.readAll();
    }
#ifdef QT_TESTCASE_BUILDDIR
    QFile diskFile(QStringLiteral(QT_TESTCASE_BUILDDIR) + QStringLiteral("/../../moonlight-qt/app/shaders/sds_lossless.metal"));
    if (diskFile.open(QIODevice::ReadOnly)) {
        return diskFile.readAll();
    }
#endif
    return {};
}

} // namespace

struct LosslessMetalRenderer::Impl
{
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLLibrary> library = nil;
    SDL_MetalView metalView = nullptr;
    CAMetalLayer* metalLayer = nil;

    id<MTLComputePipelineState> psoCopyRects = nil;
    id<MTLComputePipelineState> psoDecodeTiles = nil;
    id<MTLComputePipelineState> psoCrc32c = nil;

    id<MTLRenderPipelineState> psoVideoNearest = nil;
    id<MTLRenderPipelineState> psoVideoLinear = nil;
    id<MTLRenderPipelineState> psoOverlay = nil;

    id<MTLBuffer> prevBgra = nil;
    id<MTLBuffer> currBgra = nil;
    id<MTLBuffer> packetBuf = nil;
    id<MTLBuffer> tileDescBuf = nil;
    id<MTLBuffer> rowCrcsBuf = nil;
    id<MTLBuffer> crcTableBuf = nil;
    id<MTLBuffer> paramsBuf = nil;
    id<MTLBuffer> errFlagBuf = nil;
    id<MTLBuffer> quadVertsBuf = nil;

    id<MTLTexture> videoTex[2] {};
    id<MTLTexture> overlayTex[Overlay::OverlayMax] {};
    SDL_Rect overlayRect[Overlay::OverlayMax] {};

    std::size_t capFrameBytes = 0;
    std::size_t capPacketBytes = 0;
    std::size_t capTileDescBytes = 0;
    std::size_t capRowCrcBytes = 0;
    int texWidth[2] {};
    int texHeight[2] {};
    int renderTexIdx = 0;
    int curWidth = 0;
    int curHeight = 0;
    bool hasPrev = false;
    bool hasRenderTex = false;

    ~Impl()
    {
        for (int i = 0; i < Overlay::OverlayMax; ++i) {
            [overlayTex[i] release];
            overlayTex[i] = nil;
        }
        for (int i = 0; i < 2; ++i) {
            [videoTex[i] release];
            videoTex[i] = nil;
        }
        [prevBgra release];
        [currBgra release];
        [packetBuf release];
        [tileDescBuf release];
        [rowCrcsBuf release];
        [crcTableBuf release];
        [paramsBuf release];
        [errFlagBuf release];
        [quadVertsBuf release];
        [psoOverlay release];
        [psoVideoLinear release];
        [psoVideoNearest release];
        [psoCrc32c release];
        [psoDecodeTiles release];
        [psoCopyRects release];
        [library release];
        [queue release];
        if (metalView) {
            SDL_Metal_DestroyView(metalView);
            metalView = nullptr;
        }
        [device release];
    }

    bool init(bool headless, SDL_Window* window, bool enableVsync)
    {
        @autoreleasepool {
            device = MTLCreateSystemDefaultDevice();
            if (!device) {
                return false;
            }
            queue = [device newCommandQueue];
            if (!queue) {
                return false;
            }
            const QByteArray shaderSrc = loadMetalShaderSource();
            if (shaderSrc.isEmpty()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LosslessMetalRenderer: failed to load sds_lossless.metal");
                return false;
            }
            NSError* err = nil;
            NSString* srcNs = [[NSString alloc] initWithBytes:shaderSrc.constData()
                                                       length:shaderSrc.size()
                                                     encoding:NSUTF8StringEncoding];
            library = [device newLibraryWithSource:srcNs options:nil error:&err];
            [srcNs release];
            if (!library) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "LosslessMetalRenderer: Metal compile error: %s",
                             err ? err.localizedDescription.UTF8String : "unknown");
                return false;
            }

            auto makeCompute = [&](NSString* name) -> id<MTLComputePipelineState> {
                id<MTLFunction> fn = [library newFunctionWithName:name];
                if (!fn) return nil;
                NSError* perr = nil;
                id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:fn error:&perr];
                [fn release];
                return pso;
            };
            psoCopyRects = makeCompute(@"cs_copy_rects");
            psoDecodeTiles = makeCompute(@"cs_decode_tiles");
            psoCrc32c = makeCompute(@"cs_crc32c_rows");
            if (!psoCopyRects || !psoDecodeTiles || !psoCrc32c) {
                return false;
            }

            std::array<std::uint32_t, 256> tbl {};
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t c = i;
                for (int k = 0; k < 8; ++k) {
                    c = (c & 1u) ? ((c >> 1) ^ 0x82F63B78u) : (c >> 1);
                }
                tbl[i] = c;
            }
            crcTableBuf = [device newBufferWithBytes:tbl.data()
                                              length:sizeof(tbl)
                                             options:MTLResourceStorageModeShared];
            paramsBuf = [device newBufferWithLength:sizeof(MetalDecodeParams)
                                            options:MTLResourceStorageModeShared];
            errFlagBuf = [device newBufferWithLength:sizeof(std::uint32_t)
                                             options:MTLResourceStorageModeShared];
            if (!crcTableBuf || !paramsBuf || !errFlagBuf) {
                return false;
            }

            if (headless) {
                return true;
            }

            metalView = SDL_Metal_CreateView(window);
            if (!metalView) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "LosslessMetalRenderer: SDL_Metal_CreateView failed: %s", SDL_GetError());
                return false;
            }
            metalLayer = (CAMetalLayer*)SDL_Metal_GetLayer(metalView);
            metalLayer.device = device;
            metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            metalLayer.framebufferOnly = YES;
            metalLayer.displaySyncEnabled = enableVsync;
            if (CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB)) {
                metalLayer.colorspace = srgb;
                CGColorSpaceRelease(srgb);
            }

            auto makeRender = [&](NSString* fragName, bool blend) -> id<MTLRenderPipelineState> {
                id<MTLFunction> vfn = [library newFunctionWithName:@"vs_quad"];
                id<MTLFunction> ffn = [library newFunctionWithName:fragName];
                if (!vfn || !ffn) {
                    [vfn release];
                    [ffn release];
                    return nil;
                }
                MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
                desc.vertexFunction = vfn;
                desc.fragmentFunction = ffn;
                desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                if (blend) {
                    desc.colorAttachments[0].blendingEnabled = YES;
                    desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
                    desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
                    desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                    desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
                    desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                }
                NSError* rerr = nil;
                id<MTLRenderPipelineState> rpso = [device newRenderPipelineStateWithDescriptor:desc error:&rerr];
                [desc release];
                [vfn release];
                [ffn release];
                return rpso;
            };
            psoVideoNearest = makeRender(@"fs_video_nearest", false);
            psoVideoLinear = makeRender(@"fs_video_linear", false);
            psoOverlay = makeRender(@"fs_overlay", true);
            quadVertsBuf = [device newBufferWithLength:sizeof(float) * 16
                                               options:MTLResourceStorageModeShared];
            return psoVideoNearest && psoVideoLinear && psoOverlay && quadVertsBuf;
        }
    }

    bool ensureBuffers(int width, int height, std::size_t packetBytes, std::size_t dirtyTileCount, bool createTex, int texIdx)
    {
        const std::size_t frameBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
        if (frameBytes > capFrameBytes) {
            [prevBgra release];
            [currBgra release];
            prevBgra = [device newBufferWithLength:frameBytes options:MTLResourceStorageModeShared];
            currBgra = [device newBufferWithLength:frameBytes options:MTLResourceStorageModeShared];
            if (!prevBgra || !currBgra) {
                capFrameBytes = 0;
                hasPrev = false;
                return false;
            }
            capFrameBytes = frameBytes;
        }
        if (packetBytes > capPacketBytes) {
            [packetBuf release];
            packetBuf = [device newBufferWithLength:packetBytes options:MTLResourceStorageModeShared];
            if (!packetBuf) {
                capPacketBytes = 0;
                return false;
            }
            capPacketBytes = packetBytes;
        }
        const std::size_t neededDescs = std::max<std::size_t>(dirtyTileCount, 1) * sizeof(MetalDirtyTileDesc);
        if (neededDescs > capTileDescBytes) {
            [tileDescBuf release];
            tileDescBuf = [device newBufferWithLength:neededDescs options:MTLResourceStorageModeShared];
            if (!tileDescBuf) {
                capTileDescBytes = 0;
                return false;
            }
            capTileDescBytes = neededDescs;
        }
        const std::size_t rowBytes = static_cast<std::size_t>(height) * sizeof(std::uint32_t);
        if (rowBytes > capRowCrcBytes) {
            [rowCrcsBuf release];
            rowCrcsBuf = [device newBufferWithLength:rowBytes options:MTLResourceStorageModeShared];
            if (!rowCrcsBuf) {
                capRowCrcBytes = 0;
                return false;
            }
            capRowCrcBytes = rowBytes;
        }
        if (createTex && (texWidth[texIdx] != width || texHeight[texIdx] != height || !videoTex[texIdx])) {
            [videoTex[texIdx] release];
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                            width:static_cast<NSUInteger>(width)
                                                                                           height:static_cast<NSUInteger>(height)
                                                                                        mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeManaged;
            videoTex[texIdx] = [device newTextureWithDescriptor:td];
            if (!videoTex[texIdx]) {
                texWidth[texIdx] = 0;
                texHeight[texIdx] = 0;
                return false;
            }
            texWidth[texIdx] = width;
            texHeight[texIdx] = height;
        }
        return true;
    }

    bool gpuDecodeFrame(const std::uint8_t* packet,
                        std::size_t len,
                        const sds::lossless::frame_header_t& hdr,
                        bool verifyHash,
                        bool blitTexture,
                        bool& outHashOk,
                        std::uint32_t* outCombinedCrc,
                        std::uint64_t& outHashUs)
    {
        @autoreleasepool {
            sds::lossless::parsed_frame_t parsed;
            if (!sds::lossless::parse_frame_packet(packet, len, parsed)) {
                outHashOk = false;
                return false;
            }
            const bool dimsChanged = (static_cast<int>(hdr.width) != curWidth || static_cast<int>(hdr.height) != curHeight);
            const bool isKeyframe = (hdr.flags & sds::lossless::FLAG_KEYFRAME) != 0;
            if (!isKeyframe && (!hasPrev || dimsChanged)) {
                outHashOk = false;
                return false;
            }

            const int w = static_cast<int>(hdr.width);
            const int h = static_cast<int>(hdr.height);
            const int stride = w * 4;
            const std::size_t frameBytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(h);
            const int tilesX = (w + sds::lossless::TILE_SIZE - 1) / sds::lossless::TILE_SIZE;
            const int tilesY = (h + sds::lossless::TILE_SIZE - 1) / sds::lossless::TILE_SIZE;
            const std::size_t totalTiles = static_cast<std::size_t>(tilesX) * static_cast<std::size_t>(tilesY);
            if (isKeyframe && (parsed.header.copy_rect_count != 0 || parsed.header.dirty_tile_count != totalTiles)) {
                outHashOk = false;
                return false;
            }

            std::vector<sds::lossless::gpu_tile_desc_t> descs;
            if (!sds::lossless::build_dirty_tile_descs(parsed, packet, descs)) {
                outHashOk = false;
                return false;
            }
            if (isKeyframe) {
                for (const auto& d : descs) {
                    if (d.mode == static_cast<std::uint32_t>(sds::lossless::tile_mode_e::ycocgr_temporal_rice)) {
                        outHashOk = false;
                        return false;
                    }
                }
            }
            const int writeIdx = blitTexture ? (1 - renderTexIdx) : 0;
            if (!ensureBuffers(w, h, len, descs.size(), blitTexture, writeIdx)) {
                outHashOk = false;
                hasPrev = false;
                return false;
            }

            std::memcpy([packetBuf contents], packet, len);
            if (!descs.empty()) {
                std::memcpy([tileDescBuf contents], descs.data(), descs.size() * sizeof(sds::lossless::gpu_tile_desc_t));
            }
            *static_cast<std::uint32_t*>([errFlagBuf contents]) = 0u;

            auto* params = static_cast<MetalDecodeParams*>([paramsBuf contents]);
            std::memset(params, 0, sizeof(*params));
            params->width = hdr.width;
            params->height = hdr.height;
            params->stride = static_cast<std::uint32_t>(stride);
            params->copy_rect_count = static_cast<std::uint32_t>(std::min<std::size_t>(parsed.copy_rects.size(), sds::lossless::MAX_COPY_RECTS));
            params->dirty_tile_count = static_cast<std::uint32_t>(descs.size());
            for (std::uint32_t i = 0; i < params->copy_rect_count; ++i) {
                params->copy_rects[i].src_x = parsed.copy_rects[i].src_x;
                params->copy_rects[i].src_y = parsed.copy_rects[i].src_y;
                params->copy_rects[i].dst_x = parsed.copy_rects[i].dst_x;
                params->copy_rects[i].dst_y = parsed.copy_rects[i].dst_y;
                params->copy_rects[i].w = parsed.copy_rects[i].w;
                params->copy_rects[i].h = parsed.copy_rects[i].h;
            }

            id<MTLCommandBuffer> cmd = [queue commandBuffer];
            if (!cmd) {
                hasPrev = false;
                return false;
            }
            id<MTLComputeCommandEncoder> ce = [cmd computeCommandEncoder];
            if (!isKeyframe) {
                [ce setComputePipelineState:psoCopyRects];
                [ce setBuffer:prevBgra offset:0 atIndex:0];
                [ce setBuffer:currBgra offset:0 atIndex:1];
                [ce setBuffer:paramsBuf offset:0 atIndex:2];
                [ce dispatchThreadgroups:MTLSizeMake((hdr.width + 15u) / 16u, (hdr.height + 15u) / 16u, 1)
                   threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
            }
            if (!descs.empty()) {
                const NSUInteger gx = std::min<NSUInteger>(descs.size(), 256u);
                const NSUInteger gy = (descs.size() + 255u) / 256u;
                [ce setComputePipelineState:psoDecodeTiles];
                [ce setBuffer:packetBuf offset:0 atIndex:0];
                [ce setBuffer:tileDescBuf offset:0 atIndex:1];
                [ce setBuffer:currBgra offset:0 atIndex:2];
                [ce setBuffer:paramsBuf offset:0 atIndex:3];
                [ce setBuffer:errFlagBuf offset:0 atIndex:4];
                [ce dispatchThreadgroups:MTLSizeMake(gx, gy, 1)
                   threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
            }
            if (verifyHash && hdr.hash_algo == sds::lossless::hash_algo_e::crc32c) {
                [ce setComputePipelineState:psoCrc32c];
                [ce setBuffer:currBgra offset:0 atIndex:0];
                [ce setBuffer:rowCrcsBuf offset:0 atIndex:1];
                [ce setBuffer:crcTableBuf offset:0 atIndex:2];
                [ce setBuffer:paramsBuf offset:0 atIndex:3];
                [ce dispatchThreadgroups:MTLSizeMake((hdr.height + 63u) / 64u, 1, 1)
                   threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            }
            [ce endEncoding];

            if (blitTexture && videoTex[writeIdx]) {
                id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
                [blit copyFromBuffer:currBgra
                        sourceOffset:0
                   sourceBytesPerRow:static_cast<NSUInteger>(stride)
                 sourceBytesPerImage:static_cast<NSUInteger>(frameBytes)
                          sourceSize:MTLSizeMake(hdr.width, hdr.height, 1)
                           toTexture:videoTex[writeIdx]
                    destinationSlice:0
                    destinationLevel:0
                   destinationOrigin:MTLOriginMake(0, 0, 0)];
                [blit endEncoding];
            }

            [cmd commit];
            [cmd waitUntilCompleted];
            if ([cmd status] != MTLCommandBufferStatusCompleted ||
                *static_cast<const std::uint32_t*>([errFlagBuf contents]) != 0u) {
                outHashOk = false;
                hasPrev = false;
                return false;
            }

            QElapsedTimer htimer;
            htimer.start();
            outHashOk = true;
            if (verifyHash && hdr.hash_algo == sds::lossless::hash_algo_e::crc32c) {
                const auto* rowCrcs = static_cast<const std::uint32_t*>([rowCrcsBuf contents]);
                const std::uint64_t rowBytesRgb = static_cast<std::uint64_t>(w) * 3ULL;
                std::uint32_t combined = rowCrcs[0];
                for (int y = 1; y < h; ++y) {
                    combined = sds::lossless::crc32c_combine(combined, rowCrcs[y], rowBytesRgb);
                }
                if (outCombinedCrc) {
                    *outCombinedCrc = combined;
                }
                const std::uint32_t expected =
                    static_cast<std::uint32_t>(hdr.frame_hash[0]) |
                    (static_cast<std::uint32_t>(hdr.frame_hash[1]) << 8) |
                    (static_cast<std::uint32_t>(hdr.frame_hash[2]) << 16) |
                    (static_cast<std::uint32_t>(hdr.frame_hash[3]) << 24);
                outHashOk = (combined == expected);
            } else if (verifyHash && hdr.hash_algo == sds::lossless::hash_algo_e::blake3) {
                const auto* currPtr = static_cast<const std::uint8_t*>([currBgra contents]);
                const auto b3 = sds::lossless::blake3_rgb24_from_bgra(currPtr, stride, w, h);
                outHashOk = (b3 == hdr.frame_hash);
            }
            outHashUs = static_cast<std::uint64_t>(htimer.nsecsElapsed() / 1000);

            curWidth = w;
            curHeight = h;
            hasPrev = true;
            if (blitTexture) {
                renderTexIdx = writeIdx;
                hasRenderTex = true;
            }
            std::swap(prevBgra, currBgra);
            return true;
        }
    }
};

LosslessMetalRenderer::~LosslessMetalRenderer()
{
    delete m_Impl;
    m_Impl = nullptr;
}

bool LosslessMetalRenderer::initialize(SDL_Window* window,
                                       int width,
                                       int height,
                                       bool enableVsync,
                                       Overlay::OverlayManager* overlayManager,
                                       bool preferGpuCompute)
{
    m_Window = window;
    m_StreamWidth = width;
    m_StreamHeight = height;
    m_EnableVsync = enableVsync;
    m_OverlayManager = overlayManager;

    m_Impl = new Impl();
    if (!m_Impl->init(false, window, enableVsync)) {
        delete m_Impl;
        m_Impl = nullptr;
        return false;
    }
    m_GpuPipelineReady = true;
    m_UsingGpuCompute = preferGpuCompute;
    return true;
}

bool LosslessMetalRenderer::presentPacket(const std::uint8_t* packet,
                                          std::size_t len,
                                          const sds::lossless::frame_header_t& hdr,
                                          bool useGpuCompute,
                                          bool verifyHash,
                                          bool& outHashOk,
                                          std::uint64_t& outDecodeUs,
                                          std::uint64_t& outHashUs)
{
    if (!m_Impl) {
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    QMutexLocker lk(&m_FrameMutex);

    if (useGpuCompute && m_GpuPipelineReady) {
        m_UsingGpuCompute = true;
        const bool ok = m_Impl->gpuDecodeFrame(packet, len, hdr, verifyHash, true, outHashOk, nullptr, outHashUs);
        outDecodeUs = static_cast<std::uint64_t>(timer.nsecsElapsed() / 1000);
        return ok;
    }

    lk.unlock();
    sds::lossless::decode_stats_t stats {};
    if (!m_CpuDecoder.decode_frame(packet, len, verifyHash, nullptr, &stats)) {
        QMutexLocker lk2(&m_FrameMutex);
        m_UsingGpuCompute = false;
        m_Impl->hasPrev = false;
        outHashOk = false;
        outDecodeUs = static_cast<std::uint64_t>(timer.nsecsElapsed() / 1000);
        outHashUs = 0;
        return false;
    }
    outHashOk = (!verifyHash || hdr.hash_algo == sds::lossless::hash_algo_e::none) ? true : stats.hash_matched;
    outDecodeUs = static_cast<std::uint64_t>(timer.nsecsElapsed() / 1000);
    outHashUs = stats.hash_us;

    const int w = m_CpuDecoder.width();
    const int h = m_CpuDecoder.height();
    QMutexLocker lk2(&m_FrameMutex);
    m_UsingGpuCompute = false;
    m_Impl->hasPrev = false;
    const int writeIdx = 1 - m_Impl->renderTexIdx;
    if (!m_Impl->ensureBuffers(w, h, 0, 0, true, writeIdx)) {
        return false;
    }
    @autoreleasepool {
        [m_Impl->videoTex[writeIdx] replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(w), static_cast<NSUInteger>(h))
                                      mipmapLevel:0
                                        withBytes:m_CpuDecoder.current_bgra()
                                      bytesPerRow:static_cast<NSUInteger>(m_CpuDecoder.stride())];
        m_Impl->renderTexIdx = writeIdx;
        m_Impl->hasRenderTex = true;
    }
    return true;
}

void LosslessMetalRenderer::renderOnMainThread()
{
    if (!m_Impl || !m_Impl->metalLayer) {
        return;
    }

    @autoreleasepool {
        int outW = 0, outH = 0;
        SDL_Metal_GetDrawableSize(m_Window, &outW, &outH);
        if (outW > 0 && outH > 0) {
            m_Impl->metalLayer.drawableSize = CGSizeMake(outW, outH);
        }

        id<CAMetalDrawable> drawable = [m_Impl->metalLayer nextDrawable];
        if (!drawable) {
            return;
        }

        // Update any dirty overlays on main thread
        for (int i = 0; i < Overlay::OverlayMax; ++i) {
            const auto type = static_cast<Overlay::OverlayType>(i);
            if (!m_OverlayDirty[i]) {
                continue;
            }
            m_OverlayDirty[i] = false;
            [m_Impl->overlayTex[i] release];
            m_Impl->overlayTex[i] = nil;
            if (!m_OverlayManager || !m_OverlayManager->isOverlayEnabled(type)) {
                continue;
            }
            SDL_Surface* surf = m_OverlayManager->getUpdatedOverlaySurface(type);
            if (!surf) {
                continue;
            }
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                            width:static_cast<NSUInteger>(surf->w)
                                                                                           height:static_cast<NSUInteger>(surf->h)
                                                                                        mipmapped:NO];
            td.storageMode = MTLStorageModeManaged;
            td.usage = MTLTextureUsageShaderRead;
            m_Impl->overlayTex[i] = [m_Impl->device newTextureWithDescriptor:td];
            if (m_Impl->overlayTex[i]) {
                [m_Impl->overlayTex[i] replaceRegion:MTLRegionMake2D(0, 0, static_cast<NSUInteger>(surf->w), static_cast<NSUInteger>(surf->h))
                                         mipmapLevel:0
                                           withBytes:surf->pixels
                                         bytesPerRow:static_cast<NSUInteger>(surf->pitch)];
            }
            if (type == Overlay::OverlayStatusUpdate) {
                m_Impl->overlayRect[i] = {outW - surf->w - 10, outH - surf->h - 10, surf->w, surf->h};
            } else {
                m_Impl->overlayRect[i] = {10, 10, surf->w, surf->h};
            }
            SDL_FreeSurface(surf);
        }

        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = drawable.texture;
        rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        rpd.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

        id<MTLCommandBuffer> cmd = [m_Impl->queue commandBuffer];
        id<MTLRenderCommandEncoder> re = [cmd renderCommandEncoderWithDescriptor:rpd];

        QMutexLocker lk(&m_FrameMutex);
        const int rIdx = m_Impl->renderTexIdx;
        id<MTLTexture> activeTex = m_Impl->videoTex[rIdx];
        const int vw = m_Impl->texWidth[rIdx];
        const int vh = m_Impl->texHeight[rIdx];
        if (m_Impl->hasRenderTex && activeTex && vw > 0 && vh > 0 && outW > 0 && outH > 0) {
            SDL_Rect src {0, 0, vw, vh};
            SDL_Rect dst {0, 0, outW, outH};
            StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

            const float left = (2.0f * dst.x) / outW - 1.0f;
            const float right = (2.0f * (dst.x + dst.w)) / outW - 1.0f;
            const float top = 1.0f - (2.0f * dst.y) / outH;
            const float bottom = 1.0f - (2.0f * (dst.y + dst.h)) / outH;
            const float verts[16] = {
                left, bottom, 0.0f, 1.0f,
                right, bottom, 1.0f, 1.0f,
                left, top, 0.0f, 0.0f,
                right, top, 1.0f, 0.0f,
            };
            const bool exact1to1 = (dst.w == vw && dst.h == vh);
            [re setRenderPipelineState:exact1to1 ? m_Impl->psoVideoNearest : m_Impl->psoVideoLinear];
            [re setVertexBytes:verts length:sizeof(verts) atIndex:0];
            [re setFragmentTexture:activeTex atIndex:0];
            [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        }

        for (int i = 0; i < Overlay::OverlayMax; ++i) {
            const auto type = static_cast<Overlay::OverlayType>(i);
            if (m_OverlayManager && m_OverlayManager->isOverlayEnabled(type) && m_Impl->overlayTex[i] && outW > 0 && outH > 0) {
                const int ow = m_Impl->overlayRect[i].w;
                const int oh = m_Impl->overlayRect[i].h;
                const SDL_Rect r = (type == Overlay::OverlayStatusUpdate)
                    ? SDL_Rect {outW - ow - 10, outH - oh - 10, ow, oh}
                    : SDL_Rect {10, 10, ow, oh};
                const float left = (2.0f * r.x) / outW - 1.0f;
                const float right = (2.0f * (r.x + r.w)) / outW - 1.0f;
                const float top = 1.0f - (2.0f * r.y) / outH;
                const float bottom = 1.0f - (2.0f * (r.y + r.h)) / outH;
                const float verts[16] = {
                    left, bottom, 0.0f, 1.0f,
                    right, bottom, 1.0f, 1.0f,
                    left, top, 0.0f, 0.0f,
                    right, top, 1.0f, 0.0f,
                };
                [re setRenderPipelineState:m_Impl->psoOverlay];
                [re setVertexBytes:verts length:sizeof(verts) atIndex:0];
                [re setFragmentTexture:m_Impl->overlayTex[i] atIndex:0];
                [re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            }
        }

        [re endEncoding];
        [cmd presentDrawable:drawable];
        [cmd commit];
        lk.unlock();
    }
}

bool LosslessMetalRenderer::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info)
{
    auto unhandled = info->stateChangeFlags;
    unhandled &= ~WINDOW_STATE_CHANGE_SIZE;
    unhandled &= ~WINDOW_STATE_CHANGE_DISPLAY;
    return unhandled == 0;
}

void LosslessMetalRenderer::notifyOverlayUpdated(Overlay::OverlayType type)
{
    m_OverlayDirty[type] = true;
}

bool LosslessMetalRenderer::runHeadlessGpuDecodeSelfTest(const std::vector<const std::vector<std::uint8_t>*>& packets,
                                                         std::vector<std::uint8_t>& outFinalBgra,
                                                         int& outWidth,
                                                         int& outHeight,
                                                         std::vector<std::uint32_t>& outCrc32cPerFrame)
{
    Impl impl;
    if (!impl.init(true, nullptr, false)) {
        return false;
    }
    outCrc32cPerFrame.clear();
    for (const auto* pkt : packets) {
        if (!pkt || pkt->size() < sds::lossless::HEADER_SIZE) {
            return false;
        }
        sds::lossless::frame_header_t hdr {};
        if (!sds::lossless::parse_frame_header(pkt->data(), pkt->size(), hdr)) {
            return false;
        }
        bool hashOk = false;
        std::uint32_t crc = 0;
        std::uint64_t hashUs = 0;
        if (!impl.gpuDecodeFrame(pkt->data(), pkt->size(), hdr, true, false, hashOk, &crc, hashUs)) {
            return false;
        }
        if (hdr.hash_algo != sds::lossless::hash_algo_e::none && !hashOk) {
            return false;
        }
        outCrc32cPerFrame.push_back(crc);
    }
    if (!impl.hasPrev || !impl.prevBgra || impl.curWidth <= 0 || impl.curHeight <= 0) {
        return false;
    }
    outWidth = impl.curWidth;
    outHeight = impl.curHeight;
    const std::size_t bytes = static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight) * 4u;
    outFinalBgra.resize(bytes);
    // Note: gpuDecodeFrame swapped currBgra into prevBgra at the end of the last frame!
    std::memcpy(outFinalBgra.data(), [impl.prevBgra contents], bytes);
    return true;
}
