#include "test_metal_refinement.h"

#include "streaming/video/adaptiverefinement_codec.h"
#include "tests.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <simd/simd.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct MetalVertex
{
    simd_float4 position;
    simd_float2 texCoord;
};

struct CscParamsGpu
{
    simd_half3x3 matrix;
    simd_half3 offsets;
    simd_half2 chromaOffset;
    simd_half1 bitnessScaleFactor;
};

struct RefinementParamsGpu
{
    float yTol;
    uint32_t enabled;
    simd_uint2 logicalSize;
};

QString locateMetalShaderPath()
{
    const QString envRoot = qEnvironmentVariable("SDS_SOURCE_ROOT");
    if (!envRoot.isEmpty()) {
        const QString p = QDir(envRoot).filePath(QStringLiteral("app/shaders/vt_renderer.metal"));
        if (QFile::exists(p)) {
            return p;
        }
    }

    const QString testData = QFINDTESTDATA("../../app/shaders/vt_renderer.metal");
    if (!testData.isEmpty() && QFile::exists(testData)) {
        return testData;
    }

    return QDir::cleanPath(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() +
                           QStringLiteral("/../../app/shaders/vt_renderer.metal"));
}

inline int clampU8(int v)
{
    return std::max(0, std::min(255, v));
}

// Converts full-range RGB888 to degraded 4:2:0 BT.601 Y + 2x2 subsampled UV planes
void encodeDegraded420Bt601Full(const std::vector<uint8_t>& rgb,
                                int width,
                                int height,
                                std::vector<uint8_t>& outY,
                                std::vector<uint8_t>& outU,
                                std::vector<uint8_t>& outV)
{
    outY.resize(static_cast<size_t>(width) * height);
    const int uvW = (width + 1) / 2;
    const int uvH = (height + 1) / 2;
    outU.resize(static_cast<size_t>(uvW) * uvH);
    outV.resize(static_cast<size_t>(uvW) * uvH);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = (static_cast<size_t>(y) * width + x) * 3;
            outY[static_cast<size_t>(y) * width + x] =
                computeRefinementYRef(rgb[i], rgb[i + 1], rgb[i + 2], RefinementColorspace::Rec601, true);
        }
    }

    for (int uy = 0; uy < uvH; ++uy) {
        for (int ux = 0; ux < uvW; ++ux) {
            int sumR = 0, sumG = 0, sumB = 0, cnt = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const int sy = uy * 2 + dy;
                if (sy >= height) continue;
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = ux * 2 + dx;
                    if (sx >= width) continue;
                    const size_t i = (static_cast<size_t>(sy) * width + sx) * 3;
                    sumR += rgb[i + 0];
                    sumG += rgb[i + 1];
                    sumB += rgb[i + 2];
                    ++cnt;
                }
            }
            const double r = static_cast<double>(sumR) / cnt;
            const double g = static_cast<double>(sumG) / cnt;
            const double b = static_cast<double>(sumB) / cnt;
            const int cb = static_cast<int>(std::llround(-0.168736 * r - 0.331264 * g + 0.5 * b + 128.0));
            const int cr = static_cast<int>(std::llround(0.5 * r - 0.418688 * g - 0.081312 * b + 128.0));
            outU[static_cast<size_t>(uy) * uvW + ux] = static_cast<uint8_t>(clampU8(cb));
            outV[static_cast<size_t>(uy) * uvW + ux] = static_cast<uint8_t>(clampU8(cr));
        }
    }
}

void drawColored1PxTextPattern(std::vector<uint8_t>& rgb, int width, int tileX, int tileY, int tileW, int tileH)
{
    auto put = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < tileX || x >= tileX + tileW || y < tileY || y >= tileY + tileH) return;
        const size_t i = (static_cast<size_t>(y) * width + x) * 3;
        rgb[i + 0] = r;
        rgb[i + 1] = g;
        rgb[i + 2] = b;
    };

    for (int y = 4; y < tileH - 4 && y < 32; ++y) {
        put(tileX + 6, tileY + y, 255, 24, 24);
        put(tileX + 8, tileY + y, 24, 240, 255);
        put(tileX + 10, tileY + y, 255, 224, 0);
    }
    for (int x = 14; x < tileW - 4 && x < 58; ++x) {
        put(tileX + x, tileY + 12, 255, 48, 200);
        put(tileX + x, tileY + tileH - 3, 64, 255, 96);
    }
    // Corner seam stress pixels inside tile boundaries
    put(tileX, tileY, 255, 0, 128);
    put(tileX + tileW - 1, tileY + tileH - 1, 0, 255, 192);
}

void appendU16(QByteArray& dst, uint16_t v)
{
    dst.append(static_cast<char>(v & 0xFFu));
    dst.append(static_cast<char>((v >> 8) & 0xFFu));
}

void appendU32(QByteArray& dst, uint32_t v)
{
    dst.append(static_cast<char>(v & 0xFFu));
    dst.append(static_cast<char>((v >> 8) & 0xFFu));
    dst.append(static_cast<char>((v >> 16) & 0xFFu));
    dst.append(static_cast<char>((v >> 24) & 0xFFu));
}

bool renderRefinedShaderOnGpu(bool biplanar,
                              const std::vector<uint8_t>& yPlane,
                              const std::vector<uint8_t>& uPlane,
                              const std::vector<uint8_t>& vPlane,
                              int width,
                              int height,
                              const std::vector<AdaptiveDecodedTile>& tiles,
                              const std::vector<uint8_t>* explicitMask,
                              float yTol,
                              bool enabled,
                              std::vector<uint8_t>& outBGRA,
                              QString& errorOut)
{
@autoreleasepool {
    id<MTLDevice> device = [MTLCreateSystemDefaultDevice() autorelease];
    if (!device) {
        errorOut = QStringLiteral("No Metal device available");
        return false;
    }

    QFile shaderFile(locateMetalShaderPath());
    if (!shaderFile.open(QIODevice::ReadOnly)) {
        errorOut = QStringLiteral("Failed to read vt_renderer.metal");
        return false;
    }
    QString shaderSource = QString::fromUtf8(shaderFile.readAll());

    NSError* err = nil;
    id<MTLLibrary> lib = [[device newLibraryWithSource:shaderSource.toNSString() options:nil error:&err] autorelease];
    if (!lib) {
        errorOut = QStringLiteral("Shader compile error: %1")
            .arg(err ? QString::fromUtf8(err.localizedDescription.UTF8String) : QStringLiteral("unknown"));
        return false;
    }

    MTLRenderPipelineDescriptor* pDesc = [[MTLRenderPipelineDescriptor new] autorelease];
    pDesc.vertexFunction = [[lib newFunctionWithName:@"vs_draw"] autorelease];
    pDesc.fragmentFunction = [[lib newFunctionWithName:(biplanar ? @"ps_draw_biplanar_refined" : @"ps_draw_triplanar_refined")] autorelease];
    pDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> pipeline = [[device newRenderPipelineStateWithDescriptor:pDesc error:&err] autorelease];
    if (!pipeline) {
        errorOut = QStringLiteral("Pipeline creation failed");
        return false;
    }

    auto makeTex = [&](MTLPixelFormat fmt, int w, int h, MTLTextureUsage usage) -> id<MTLTexture> {
        auto* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
        d.storageMode = MTLStorageModeManaged;
        d.usage = usage;
        return [[device newTextureWithDescriptor:d] autorelease];
    };

    const int uvW = (width + 1) / 2;
    const int uvH = (height + 1) / 2;
    const int tilesX = (width + 63) / 64;
    const int tilesY = (height + 63) / 64;

    id<MTLTexture> yTex = makeTex(MTLPixelFormatR8Unorm, width, height, MTLTextureUsageShaderRead);
    [yTex replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:yPlane.data() bytesPerRow:width];

    id<MTLTexture> uvTex = nil;
    id<MTLTexture> uTex = nil;
    id<MTLTexture> vTex = nil;
    if (biplanar) {
        std::vector<uint8_t> uvInterleaved(static_cast<size_t>(uvW) * uvH * 2);
        for (int i = 0; i < uvW * uvH; ++i) {
            uvInterleaved[i * 2 + 0] = uPlane[i];
            uvInterleaved[i * 2 + 1] = vPlane[i];
        }
        uvTex = makeTex(MTLPixelFormatRG8Unorm, uvW, uvH, MTLTextureUsageShaderRead);
        [uvTex replaceRegion:MTLRegionMake2D(0, 0, uvW, uvH) mipmapLevel:0 withBytes:uvInterleaved.data() bytesPerRow:uvW * 2];
    } else {
        uTex = makeTex(MTLPixelFormatR8Unorm, uvW, uvH, MTLTextureUsageShaderRead);
        vTex = makeTex(MTLPixelFormatR8Unorm, uvW, uvH, MTLTextureUsageShaderRead);
        [uTex replaceRegion:MTLRegionMake2D(0, 0, uvW, uvH) mipmapLevel:0 withBytes:uPlane.data() bytesPerRow:uvW];
        [vTex replaceRegion:MTLRegionMake2D(0, 0, uvW, uvH) mipmapLevel:0 withBytes:vPlane.data() bytesPerRow:uvW];
    }

    id<MTLTexture> refTex = makeTex(MTLPixelFormatRGBA8Unorm, width, height, MTLTextureUsageShaderRead);
    std::vector<uint8_t> refZero(static_cast<size_t>(width) * height * 4, 0);
    [refTex replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:refZero.data() bytesPerRow:width * 4];

    id<MTLTexture> maskTex = makeTex(MTLPixelFormatR8Unorm, tilesX, tilesY, MTLTextureUsageShaderRead);
    std::vector<uint8_t> maskData(static_cast<size_t>(tilesX) * tilesY, 0);
    for (const AdaptiveDecodedTile& t : tiles) {
        [refTex replaceRegion:MTLRegionMake2D(t.x, t.y, t.w, t.h)
                  mipmapLevel:0
                    withBytes:t.rgba.constData()
                  bytesPerRow:t.w * 4];
        maskData[static_cast<size_t>(t.y / 64) * tilesX + (t.x / 64)] = 255;
    }
    if (explicitMask && explicitMask->size() == maskData.size()) {
        maskData = *explicitMask;
    }
    [maskTex replaceRegion:MTLRegionMake2D(0, 0, tilesX, tilesY) mipmapLevel:0 withBytes:maskData.data() bytesPerRow:tilesX];

    id<MTLTexture> outTex = makeTex(MTLPixelFormatBGRA8Unorm, width, height, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);

    const MetalVertex verts[4] = {
        { { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
        { { -1.0f,  1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
        { {  1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
        { {  1.0f,  1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
    };

    CscParamsGpu csc = {};
    // BT.601 full-range matrix matching renderer.h k_CscMatrix_Bt601
    csc.matrix = simd_matrix(simd_make_half3(1.0f, 0.0f, 1.4020f),
                             simd_make_half3(1.0f, -0.3441f, -0.7141f),
                             simd_make_half3(1.0f, 1.7720f, 0.0f));
    csc.offsets = simd_make_half3(0.0f, 128.0f / 255.0f, 128.0f / 255.0f);
    csc.chromaOffset = simd_make_half2(0.0f, 0.0f);
    csc.bitnessScaleFactor = 1.0f;

    RefinementParamsGpu refP = {};
    refP.yTol = yTol;
    refP.enabled = enabled ? 1u : 0u;
    refP.logicalSize = simd_make_uint2((uint32_t)width, (uint32_t)height);

    id<MTLCommandQueue> queue = [[device newCommandQueue] autorelease];
    id<MTLCommandBuffer> cmd = [queue commandBuffer];
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = outTex;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pipeline];
    [enc setVertexBytes:verts length:sizeof(verts) atIndex:0];
    [enc setFragmentTexture:yTex atIndex:0];
    if (biplanar) {
        [enc setFragmentTexture:uvTex atIndex:1];
        [enc setFragmentTexture:refTex atIndex:2];
        [enc setFragmentTexture:maskTex atIndex:3];
    } else {
        [enc setFragmentTexture:uTex atIndex:1];
        [enc setFragmentTexture:vTex atIndex:2];
        [enc setFragmentTexture:refTex atIndex:3];
        [enc setFragmentTexture:maskTex atIndex:4];
    }
    [enc setFragmentBytes:&csc length:sizeof(csc) atIndex:0];
    [enc setFragmentBytes:&refP length:sizeof(refP) atIndex:1];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [enc endEncoding];

    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit synchronizeResource:outTex];
    [blit endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];

    outBGRA.resize(static_cast<size_t>(width) * height * 4);
    [outTex getBytes:outBGRA.data()
         bytesPerRow:(NSUInteger)width * 4u
          fromRegion:MTLRegionMake2D(0, 0, width, height)
         mipmapLevel:0];
    return true;
}
}

AdaptiveTileBatch buildQoiWireBatchForRegion(const std::vector<uint8_t>& rgb,
                                             int frameW,
                                             int frameH,
                                             const std::vector<std::array<int, 4>>& rects)
{
    QByteArray wire;
    wire.append("SDTR", 4);
    appendU16(wire, AdaptiveRefinementWire::PROTOCOL_VERSION);
    appendU16(wire, 0);
    appendU16(wire, static_cast<uint16_t>(frameW));
    appendU16(wire, static_cast<uint16_t>(frameH));
    appendU16(wire, static_cast<uint16_t>(rects.size()));
    appendU16(wire, 0);

    for (size_t i = 0; i < rects.size(); ++i) {
        const int x = rects[i][0];
        const int y = rects[i][1];
        const int w = rects[i][2];
        const int h = rects[i][3];
        std::vector<uint8_t> tileRgb(static_cast<size_t>(w) * h * 3);
        for (int row = 0; row < h; ++row) {
            std::memcpy(tileRgb.data() + static_cast<size_t>(row) * w * 3,
                        rgb.data() + (static_cast<size_t>(y + row) * frameW + x) * 3,
                        static_cast<size_t>(w) * 3);
        }
        std::vector<uint8_t> qoi;
        encodeQoiRgb24(tileRgb.data(), w * 3, w, h, qoi);
        appendU32(wire, static_cast<uint32_t>(i + 1));
        appendU16(wire, static_cast<uint16_t>(x));
        appendU16(wire, static_cast<uint16_t>(y));
        appendU16(wire, static_cast<uint16_t>(w));
        appendU16(wire, static_cast<uint16_t>(h));
        appendU32(wire, static_cast<uint32_t>(qoi.size()));
        wire.append(reinterpret_cast<const char*>(qoi.data()), static_cast<qsizetype>(qoi.size()));
    }

    AdaptiveTileBatch batch;
    parseTileRefinementBatch(wire, RefinementColorspace::Rec601, true, batch);
    return batch;
}

} // namespace

void TestMetalRefinement::refinesStaticColoredTextTileBitExactOnBiplanarNv12()
{
    constexpr int W = 1920;
    constexpr int H = 1080; // Non-multiple-of-64 height exercises partial edge tile math

    std::vector<uint8_t> rgb(static_cast<size_t>(W) * H * 3);
    for (int i = 0; i < W * H; ++i) {
        rgb[i * 3 + 0] = 28;
        rgb[i * 3 + 1] = 30;
        rgb[i * 3 + 2] = 34;
    }
    drawColored1PxTextPattern(rgb, W, 64, 64, 64, 64);
    drawColored1PxTextPattern(rgb, W, 1856, 1024, 64, 56);

    std::vector<uint8_t> yPlane, uPlane, vPlane;
    encodeDegraded420Bt601Full(rgb, W, H, yPlane, uPlane, vPlane);

    const std::vector<std::array<int, 4>> tileRects{{64, 64, 64, 64}, {1856, 1024, 64, 56}};
    const AdaptiveTileBatch batch = buildQoiWireBatchForRegion(rgb, W, H, tileRects);
    QVERIFY(batch.valid);
    QCOMPARE(batch.tiles.size(), size_t(2));

    std::vector<uint8_t> actualBGRA;
    QString err;
    QVERIFY2(renderRefinedShaderOnGpu(true, yPlane, uPlane, vPlane, W, H, batch.tiles, nullptr,
                                      8.5f / 255.0f, true, actualBGRA, err),
             qPrintable(err));

    // Every pixel in refined tiles (64,64) and partial edge tile (1856,1024) must match bit-exact RGB888
    for (const auto& r : tileRects) {
        for (int py = r[1]; py < r[1] + r[3]; ++py) {
            for (int px = r[0]; px < r[0] + r[2]; ++px) {
                const size_t outIdx = (static_cast<size_t>(py) * W + px) * 4;
                const size_t srcIdx = (static_cast<size_t>(py) * W + px) * 3;
                const uint8_t actualR = actualBGRA[outIdx + 2];
                const uint8_t actualG = actualBGRA[outIdx + 1];
                const uint8_t actualB = actualBGRA[outIdx + 0];
                const int diffR = std::abs(int(actualR) - int(rgb[srcIdx + 0]));
                const int diffG = std::abs(int(actualG) - int(rgb[srcIdx + 1]));
                const int diffB = std::abs(int(actualB) - int(rgb[srcIdx + 2]));
                QVERIFY2(diffR == 0 && diffG == 0 && diffB == 0,
                         qPrintable(QStringLiteral("Expected bit-exact refined RGB at (%1,%2), diff=(%3,%4,%5)")
                                        .arg(px).arg(py).arg(diffR).arg(diffG).arg(diffB)));
            }
        }
    }
}

void TestMetalRefinement::zeroGhostingOnLumaShiftWithStaleRefinementTexture()
{
    constexpr int W = 256;
    constexpr int H = 256;

    // Frame A (stale refinement cache inside tile (64, 64))
    std::vector<uint8_t> staleRgb(static_cast<size_t>(W) * H * 3, 28);
    drawColored1PxTextPattern(staleRgb, W, 64, 64, 64, 64);
    const AdaptiveTileBatch staleBatch = buildQoiWireBatchForRegion(staleRgb, W, H, {{64, 64, 64, 64}});
    QVERIFY(staleBatch.valid);

    // Frame B: simulate 1px/luminance motion across tile (64, 64) by shifting pattern & brightening
    // Previously bright strokes turn dark (Y ~ 28 vs stale Y_ref ~ 85..210) and previously dark
    // background pixels receive bright white text (Y ~ 230 vs stale Y_ref ~ 28).
    std::vector<uint8_t> liveRgb(static_cast<size_t>(W) * H * 3, 28);
    for (int y = 4; y < 32; ++y) {
        const size_t i = (static_cast<size_t>(64 + y) * W + (64 + 7)) * 3; // 1px shifted right
        liveRgb[i + 0] = 240;
        liveRgb[i + 1] = 240;
        liveRgb[i + 2] = 240;
    }

    std::vector<uint8_t> yLive, uLive, vLive;
    encodeDegraded420Bt601Full(liveRgb, W, H, yLive, uLive, vLive);

    // Render pure unrefined NV12 baseline vs stale-cache refined NV12
    std::vector<uint8_t> baselineBGRA;
    std::vector<uint8_t> ghostCheckBGRA;
    QString err;
    QVERIFY2(renderRefinedShaderOnGpu(true, yLive, uLive, vLive, W, H, {}, nullptr,
                                      8.5f / 255.0f, false, baselineBGRA, err),
             qPrintable(err));
    QVERIFY2(renderRefinedShaderOnGpu(true, yLive, uLive, vLive, W, H, staleBatch.tiles, nullptr,
                                      8.5f / 255.0f, true, ghostCheckBGRA, err),
             qPrintable(err));

    // All moved pixels whose Y shifted from frame A -> frame B must have ZERO ghosting (exact match to baseline video)
    for (int y = 64; y < 128; ++y) {
        for (int x = 64; x < 128; ++x) {
            const size_t srcIdx = (static_cast<size_t>(y) * W + x) * 3;
            const int yOld = computeRefinementYRef(staleRgb[srcIdx], staleRgb[srcIdx + 1], staleRgb[srcIdx + 2],
                                                   RefinementColorspace::Rec601, true);
            const int yNew = yLive[static_cast<size_t>(y) * W + x];
            if (std::abs(yNew - yOld) > 10) {
                const size_t pxIdx = (static_cast<size_t>(y) * W + x) * 4;
                QCOMPARE(ghostCheckBGRA[pxIdx + 0], baselineBGRA[pxIdx + 0]);
                QCOMPARE(ghostCheckBGRA[pxIdx + 1], baselineBGRA[pxIdx + 1]);
                QCOMPARE(ghostCheckBGRA[pxIdx + 2], baselineBGRA[pxIdx + 2]);
            }
        }
    }
}

void TestMetalRefinement::resetAllClearingRestoresVideoOnTriplanarI420()
{
    constexpr int W = 192;
    constexpr int H = 192;

    std::vector<uint8_t> videoRgb(static_cast<size_t>(W) * H * 3, 28);
    std::vector<uint8_t> staleOverlayRgb(static_cast<size_t>(W) * H * 3, 28);
    // Deliberately identical Y=150 but wildly wrong color in refTex vs live video
    for (int y = 64; y < 128; ++y) {
        for (int x = 64; x < 128; ++x) {
            const size_t i = (static_cast<size_t>(y) * W + x) * 3;
            videoRgb[i + 0] = 150;
            videoRgb[i + 1] = 150;
            videoRgb[i + 2] = 150;
            staleOverlayRgb[i + 0] = 255;
            staleOverlayRgb[i + 1] = 110;
            staleOverlayRgb[i + 2] = 75;
        }
    }

    std::vector<uint8_t> yPlane, uPlane, vPlane;
    encodeDegraded420Bt601Full(videoRgb, W, H, yPlane, uPlane, vPlane);
    const AdaptiveTileBatch overlayBatch = buildQoiWireBatchForRegion(staleOverlayRgb, W, H, {{64, 64, 64, 64}});
    QVERIFY(overlayBatch.valid);

    // 1. With mask=255 -> refined overlay takes effect cleanly on triplanar I420 shader
    std::vector<uint8_t> refinedBGRA;
    QString err;
    QVERIFY2(renderRefinedShaderOnGpu(false, yPlane, uPlane, vPlane, W, H, overlayBatch.tiles, nullptr,
                                      8.5f / 255.0f, true, refinedBGRA, err),
             qPrintable(err));
    const size_t centerOut = (static_cast<size_t>(96) * W + 96) * 4;
    QCOMPARE(refinedBGRA[centerOut + 2], uint8_t(255));
    QCOMPARE(refinedBGRA[centerOut + 1], uint8_t(110));
    QCOMPARE(refinedBGRA[centerOut + 0], uint8_t(75));

    // 2. After reset_all clears mask to 0 (even keeping refTex dirty!), video color returns identically
    const std::vector<uint8_t> zeroMask(static_cast<size_t>(3 * 3), 0);
    std::vector<uint8_t> clearedBGRA;
    QVERIFY2(renderRefinedShaderOnGpu(false, yPlane, uPlane, vPlane, W, H, overlayBatch.tiles, &zeroMask,
                                      8.5f / 255.0f, true, clearedBGRA, err),
             qPrintable(err));
    QVERIFY(std::abs(int(clearedBGRA[centerOut + 2]) - 150) <= 1);
    QVERIFY(std::abs(int(clearedBGRA[centerOut + 1]) - 150) <= 1);
    QVERIFY(std::abs(int(clearedBGRA[centerOut + 0]) - 150) <= 1);
}

int runMetalRefinementTests(int argc, char** argv)
{
    TestMetalRefinement tests;
    return QTest::qExec(&tests, argc, argv);
}
