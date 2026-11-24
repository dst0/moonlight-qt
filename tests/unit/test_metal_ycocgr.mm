#include "test_metal_ycocgr.h"

#include "streaming/video/packed444.h"
#include "tests.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <simd/simd.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

struct MetalVertex
{
    simd_float4 position;
    simd_float2 texCoord;
};

struct Packed444ParamsGpu
{
    simd_uint2 logicalSize;
    simd_uint2 codedSize;
    uint32_t tileW;
    uint32_t tileH;
    uint32_t splitX;
    uint32_t isScaled;
    float lumaUnormScale;
};

inline int floorDiv2(int v)
{
    return (v >= 0) ? (v >> 1) : -((-v + 1) >> 1);
}

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

// Builds a synthetic RGB test chart with 1px lines, synthetic glyphs, gradient,
// border corners, and seam-adjacent strokes at CTU-64 and splitX boundaries.
std::vector<uint8_t> buildTestChartRGBA(int width, int height)
{
    std::vector<uint8_t> img(static_cast<size_t>(width) * height * 4);
    auto setPixel = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || x >= width || y < 0 || y >= height) {
            return;
        }
        const size_t idx = (static_cast<size_t>(y) * width + x) * 4;
        img[idx + 0] = r;
        img[idx + 1] = g;
        img[idx + 2] = b;
        img[idx + 3] = 255;
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            setPixel(x, y, 0x1E, 0x1E, 0x1E);
        }
    }

    // Smooth horizontal color gradient strip (rows 200..260)
    for (int y = 200; y < 260 && y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t r = static_cast<uint8_t>((x * 255) / std::max(1, width - 1));
            const uint8_t g = static_cast<uint8_t>(((width - 1 - x) * 255) / std::max(1, width - 1));
            const uint8_t b = static_cast<uint8_t>(((x + y) * 255) / std::max(1, width + height - 2));
            setPixel(x, y, r, g, b);
        }
    }

    // 1px colored lines across full width/height (including around splitX and CTU-64 edges)
    for (int x = 16; x < width - 16; ++x) {
        setPixel(x, 64, 0xFF, 0x20, 0x20);
        setPixel(x, 67, 0x20, 0xFF, 0x20);
        setPixel(x, 128, 0x30, 0x90, 0xFF);
        setPixel(x, 131, 0xFF, 0x00, 0xFF);
    }
    const int splitX = (width + 1) / 2;
    for (int y = 16; y < height - 16; ++y) {
        setPixel(splitX - 2, y, 0xFF, 0x20, 0x20);
        setPixel(splitX - 1, y, 0x20, 0xFF, 0x20);
        setPixel(splitX, y, 0x30, 0x90, 0xFF);
        setPixel(splitX + 1, y, 0xFF, 0x00, 0xFF);
        setPixel(64, y, 0xFF, 0x20, 0x20);
    }

    // Synthetic 1px glyph strokes ("A", "H", "M") near top-left and center
    auto drawGlyphBlock = [&](int gx, int gy, uint8_t r, uint8_t g, uint8_t b) {
        for (int i = 0; i < 16; ++i) {
            setPixel(gx + i, gy, r, g, b);
            setPixel(gx, gy + i, r, g, b);
            setPixel(gx + 15, gy + i, r, g, b);
            setPixel(gx + i, gy + 8, r, g, b);
        }
        for (int i = 0; i < 16; ++i) {
            setPixel(gx + 20 + i, gy + i, r, g, b);
            setPixel(gx + 35 - i, gy + i, r, g, b);
        }
    };
    drawGlyphBlock(32, 32, 0xFF, 0x20, 0x20);
    drawGlyphBlock(80, 32, 0x20, 0xFF, 0x20);
    drawGlyphBlock(128, 32, 0x30, 0x90, 0xFF);
    drawGlyphBlock(splitX - 20, 320, 0xFF, 0x00, 0xFF);

    // Distinct border corners
    setPixel(0, 0, 0xFF, 0x20, 0x20);
    setPixel(width - 1, 0, 0x20, 0xFF, 0x20);
    setPixel(0, height - 1, 0x30, 0x90, 0xFF);
    setPixel(width - 1, height - 1, 0xFF, 0x00, 0xFF);

    return img;
}

std::vector<uint16_t> packChartLayoutA1(const std::vector<uint8_t>& rgba,
                                        const PackedYCoCgRLayout& layout,
                                        bool msbShift6)
{
    std::vector<uint16_t> luma(static_cast<size_t>(layout.encW) * layout.encH);

    // Fill all padding/sentinel regions with extreme alternating values to trap any tile-seam bleed
    for (int y = 0; y < layout.encH; ++y) {
        for (int x = 0; x < layout.encW; ++x) {
            const uint16_t trap10 = ((x + y) & 1) ? 1023 : 0;
            luma[static_cast<size_t>(y) * layout.encW + x] = msbShift6 ? static_cast<uint16_t>(trap10 << 6) : trap10;
        }
    }

    auto putSample = [&](int dstX, int dstY, uint16_t val10) {
        luma[static_cast<size_t>(dstY) * layout.encW + dstX] =
            msbShift6 ? static_cast<uint16_t>(val10 << 6) : val10;
    };

    for (int y = 0; y < layout.logicalH; ++y) {
        for (int x = 0; x < layout.logicalW; ++x) {
            const size_t idx = (static_cast<size_t>(y) * layout.logicalW + x) * 4;
            const int r = rgba[idx + 0];
            const int g = rgba[idx + 1];
            const int b = rgba[idx + 2];

            const int co = r - b;
            const int t = b + floorDiv2(co);
            const int cg = g - t;
            const int yVal = t + floorDiv2(cg);

            const uint16_t y10 = static_cast<uint16_t>(yVal * 4);
            const uint16_t co10 = static_cast<uint16_t>(512 + co * 2);
            const uint16_t cg10 = static_cast<uint16_t>(512 + cg * 2);

            putSample(x, y, y10);
            putSample(x, layout.tileH + y, co10);
            if (x < layout.splitX) {
                putSample(layout.tileW + x, y, cg10);
            }
            else {
                putSample(layout.tileW + (x - layout.splitX), layout.tileH + y, cg10);
            }
        }
    }

    return luma;
}

bool renderPackedShaderOnGpu(const std::vector<uint16_t>& lumaPlane,
                             const PackedYCoCgRLayout& layout,
                             int outW,
                             int outH,
                             uint32_t isScaled,
                             float lumaUnormScale,
                             std::vector<uint8_t>& outBGRA,
                             QString& outError)
{ @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        outError = QStringLiteral("MTLCreateSystemDefaultDevice() returned nil");
        return false;
    }

    const QString shaderPath = locateMetalShaderPath();
    QFile shaderFile(shaderPath);
    if (!shaderFile.open(QIODevice::ReadOnly)) {
        outError = QStringLiteral("Failed to open shader: ") + shaderPath;
        return false;
    }
    const QString shaderSource = QString::fromUtf8(shaderFile.readAll());

    NSError* nsErr = nil;
    id<MTLLibrary> lib = [[device newLibraryWithSource:shaderSource.toNSString()
                                              options:nil
                                                error:&nsErr] autorelease];
    if (!lib) {
        outError = QStringLiteral("Metal compile failed: ") +
            (nsErr ? QString::fromNSString(nsErr.localizedDescription) : QStringLiteral("unknown"));
        return false;
    }

    id<MTLFunction> vsFunc = [[lib newFunctionWithName:@"vs_draw"] autorelease];
    id<MTLFunction> fsFunc = [[lib newFunctionWithName:@"ps_draw_ycocgr_a1"] autorelease];
    if (!vsFunc || !fsFunc) {
        outError = QStringLiteral("Missing vs_draw or ps_draw_ycocgr_a1 in vt_renderer.metal");
        return false;
    }

    MTLRenderPipelineDescriptor* psoDesc = [[MTLRenderPipelineDescriptor new] autorelease];
    psoDesc.vertexFunction = vsFunc;
    psoDesc.fragmentFunction = fsFunc;
    psoDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    id<MTLRenderPipelineState> pso = [[device newRenderPipelineStateWithDescriptor:psoDesc error:&nsErr] autorelease];
    if (!pso) {
        outError = QStringLiteral("Pipeline creation failed: ") +
            (nsErr ? QString::fromNSString(nsErr.localizedDescription) : QStringLiteral("unknown"));
        return false;
    }

    const MTLStorageMode storageMode = device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;

    MTLTextureDescriptor* lumDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Unorm
                                                                                       width:layout.encW
                                                                                      height:layout.encH
                                                                                   mipmapped:NO];
    lumDesc.storageMode = storageMode;
    lumDesc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> lumTex = [[device newTextureWithDescriptor:lumDesc] autorelease];
    if (!lumTex) {
        outError = QStringLiteral("Failed to allocate input R16Unorm texture");
        return false;
    }
    [lumTex replaceRegion:MTLRegionMake2D(0, 0, layout.encW, layout.encH)
              mipmapLevel:0
                withBytes:lumaPlane.data()
              bytesPerRow:static_cast<NSUInteger>(layout.encW) * sizeof(uint16_t)];

    MTLTextureDescriptor* rtDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:outW
                                                                                     height:outH
                                                                                  mipmapped:NO];
    rtDesc.storageMode = storageMode;
    rtDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> rtTex = [[device newTextureWithDescriptor:rtDesc] autorelease];
    if (!rtTex) {
        outError = QStringLiteral("Failed to allocate output BGRA8Unorm texture");
        return false;
    }

    const MetalVertex verts[] = {
        { { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
        { { -1.0f,  1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
        { {  1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
        { {  1.0f,  1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
    };
    const MTLResourceOptions bufOpts = device.hasUnifiedMemory
        ? MTLResourceStorageModeShared
        : MTLResourceStorageModeManaged;
    id<MTLBuffer> vbuf = [[device newBufferWithBytes:verts length:sizeof(verts) options:bufOpts] autorelease];

    Packed444ParamsGpu params{};
    params.logicalSize = simd_make_uint2(static_cast<uint32_t>(layout.logicalW), static_cast<uint32_t>(layout.logicalH));
    params.codedSize = simd_make_uint2(static_cast<uint32_t>(layout.encW), static_cast<uint32_t>(layout.encH));
    params.tileW = static_cast<uint32_t>(layout.tileW);
    params.tileH = static_cast<uint32_t>(layout.tileH);
    params.splitX = static_cast<uint32_t>(layout.splitX);
    params.isScaled = isScaled;
    params.lumaUnormScale = lumaUnormScale;

    id<MTLCommandQueue> queue = [[device newCommandQueue] autorelease];
    id<MTLCommandBuffer> cmd = [queue commandBuffer];

    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rtTex;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;

    id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:pso];
    [enc setFragmentTexture:lumTex atIndex:0];
    [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
    [enc setVertexBuffer:vbuf offset:0 atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [enc endEncoding];

    if (storageMode == MTLStorageModeManaged) {
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        [blit synchronizeResource:rtTex];
        [blit endEncoding];
    }

    [cmd commit];
    [cmd waitUntilCompleted];
    if (cmd.status != MTLCommandBufferStatusCompleted || cmd.error != nil) {
        outError = QStringLiteral("Metal command buffer failed");
        return false;
    }

    outBGRA.resize(static_cast<size_t>(outW) * outH * 4);
    [rtTex getBytes:outBGRA.data()
        bytesPerRow:static_cast<NSUInteger>(outW) * 4
         fromRegion:MTLRegionMake2D(0, 0, outW, outH)
        mipmapLevel:0];
    return true;
}}

int maxAbsDiffBGRAvsRGBA(const std::vector<uint8_t>& bgra,
                         const std::vector<uint8_t>& expectedRGBA,
                         int width,
                         int height,
                         int& badX,
                         int& badY)
{
    int maxDiff = 0;
    badX = -1;
    badY = -1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            const int dB = std::abs(static_cast<int>(bgra[i + 0]) - static_cast<int>(expectedRGBA[i + 2]));
            const int dG = std::abs(static_cast<int>(bgra[i + 1]) - static_cast<int>(expectedRGBA[i + 1]));
            const int dR = std::abs(static_cast<int>(bgra[i + 2]) - static_cast<int>(expectedRGBA[i + 0]));
            const int d = std::max({dR, dG, dB});
            if (d > maxDiff) {
                maxDiff = d;
                badX = x;
                badY = y;
            }
        }
    }
    return maxDiff;
}

} // namespace

void TestMetalYCoCgR::unpacks1080pExactAt1To1_VTMsbMode()
{
    const PackedYCoCgRLayout layout = computePackedYCoCgRA1Layout(1920, 1080);
    QVERIFY(layout.valid);
    QCOMPARE(layout.encW, 2880);
    QCOMPARE(layout.encH, 2176);

    const std::vector<uint8_t> chartRGBA = buildTestChartRGBA(1920, 1080);
    const std::vector<uint16_t> luma = packChartLayoutA1(chartRGBA, layout, true /* VT xf20 MSB */);

    std::vector<uint8_t> actualBGRA;
    QString err;
    QVERIFY2(renderPackedShaderOnGpu(luma, layout, 1920, 1080, 0u, 65535.0f / 64.0f, actualBGRA, err),
             qPrintable(err));

    int badX = -1;
    int badY = -1;
    const int maxDiff = maxAbsDiffBGRAvsRGBA(actualBGRA, chartRGBA, 1920, 1080, badX, badY);
    QVERIFY2(maxDiff == 0,
             qPrintable(QStringLiteral("Expected 0 diff in VT MSB 1:1 mode, got %1 at (%2,%3)")
                            .arg(maxDiff).arg(badX).arg(badY)));
}

void TestMetalYCoCgR::unpacks1080pExactAt1To1_SwLsbMode()
{
    const PackedYCoCgRLayout layout = computePackedYCoCgRA1Layout(1920, 1080);
    QVERIFY(layout.valid);

    const std::vector<uint8_t> chartRGBA = buildTestChartRGBA(1920, 1080);
    const std::vector<uint16_t> luma = packChartLayoutA1(chartRGBA, layout, false /* SW YUV420P10LE LSB */);

    std::vector<uint8_t> actualBGRA;
    QString err;
    QVERIFY2(renderPackedShaderOnGpu(luma, layout, 1920, 1080, 0u, 65535.0f, actualBGRA, err),
             qPrintable(err));

    int badX = -1;
    int badY = -1;
    const int maxDiff = maxAbsDiffBGRAvsRGBA(actualBGRA, chartRGBA, 1920, 1080, badX, badY);
    QVERIFY2(maxDiff == 0,
             qPrintable(QStringLiteral("Expected 0 diff in SW LSB 1:1 mode, got %1 at (%2,%3)")
                            .arg(maxDiff).arg(badX).arg(badY)));
}

void TestMetalYCoCgR::unpacksOddResolutionExactAt1To1()
{
    const PackedYCoCgRLayout layout = computePackedYCoCgRA1Layout(1511, 981);
    QVERIFY(layout.valid);
    QCOMPARE(layout.encW, 2304);
    QCOMPARE(layout.encH, 2048);

    const std::vector<uint8_t> chartRGBA = buildTestChartRGBA(1511, 981);
    const std::vector<uint16_t> luma = packChartLayoutA1(chartRGBA, layout, true);

    std::vector<uint8_t> actualBGRA;
    QString err;
    QVERIFY2(renderPackedShaderOnGpu(luma, layout, 1511, 981, 0u, 65535.0f / 64.0f, actualBGRA, err),
             qPrintable(err));

    int badX = -1;
    int badY = -1;
    const int maxDiff = maxAbsDiffBGRAvsRGBA(actualBGRA, chartRGBA, 1511, 981, badX, badY);
    QVERIFY2(maxDiff == 0,
             qPrintable(QStringLiteral("Expected 0 diff in odd 1511x981 1:1 mode, got %1 at (%2,%3)")
                            .arg(maxDiff).arg(badX).arg(badY)));
}

void TestMetalYCoCgR::unpacks1080pScaledTo720pWithoutSeamBleed()
{
    const int srcW = 1920;
    const int srcH = 1080;
    const int dstW = 1280;
    const int dstH = 720;

    const PackedYCoCgRLayout layout = computePackedYCoCgRA1Layout(srcW, srcH);
    QVERIFY(layout.valid);

    const std::vector<uint8_t> chartRGBA = buildTestChartRGBA(srcW, srcH);
    const std::vector<uint16_t> luma = packChartLayoutA1(chartRGBA, layout, true);

    std::vector<uint8_t> actualBGRA;
    QString err;
    QVERIFY2(renderPackedShaderOnGpu(luma, layout, dstW, dstH, 1u, 65535.0f / 64.0f, actualBGRA, err),
             qPrintable(err));

    // CPU reference 4-tap RGB bilinear downsample matching ps_draw_ycocgr_a1
    std::vector<uint8_t> refRGBA(static_cast<size_t>(dstW) * dstH * 4);
    for (int dy = 0; dy < dstH; ++dy) {
        const float v = (static_cast<float>(dy) + 0.5f) / static_cast<float>(dstH);
        const float posY = v * static_cast<float>(srcH) - 0.5f;
        const float f0y = std::floor(posY);
        const float wy = posY - f0y;
        const int y0 = qBound(0, static_cast<int>(f0y), srcH - 1);
        const int y1 = qBound(0, static_cast<int>(f0y) + 1, srcH - 1);

        for (int dx = 0; dx < dstW; ++dx) {
            const float u = (static_cast<float>(dx) + 0.5f) / static_cast<float>(dstW);
            const float posX = u * static_cast<float>(srcW) - 0.5f;
            const float f0x = std::floor(posX);
            const float wx = posX - f0x;
            const int x0 = qBound(0, static_cast<int>(f0x), srcW - 1);
            const int x1 = qBound(0, static_cast<int>(f0x) + 1, srcW - 1);

            for (int c = 0; c < 3; ++c) {
                const float c00 = chartRGBA[(static_cast<size_t>(y0) * srcW + x0) * 4 + c];
                const float c10 = chartRGBA[(static_cast<size_t>(y0) * srcW + x1) * 4 + c];
                const float c01 = chartRGBA[(static_cast<size_t>(y1) * srcW + x0) * 4 + c];
                const float c11 = chartRGBA[(static_cast<size_t>(y1) * srcW + x1) * 4 + c];
                const float top = c00 + (c10 - c00) * wx;
                const float bot = c01 + (c11 - c01) * wx;
                const float val = top + (bot - top) * wy;
                refRGBA[(static_cast<size_t>(dy) * dstW + dx) * 4 + c] =
                    static_cast<uint8_t>(qBound(0, static_cast<int>(std::lround(val)), 255));
            }
            refRGBA[(static_cast<size_t>(dy) * dstW + dx) * 4 + 3] = 255;
        }
    }

    int badX = -1;
    int badY = -1;
    const int maxDiff = maxAbsDiffBGRAvsRGBA(actualBGRA, refRGBA, dstW, dstH, badX, badY);
    QVERIFY2(maxDiff <= 2,
             qPrintable(QStringLiteral("Expected smooth bilinear scaled output (diff <= 2), got %1 at (%2,%3)")
                            .arg(maxDiff).arg(badX).arg(badY)));

    // Explicitly verify bottom background row (adjacent to tileH sentinel trap) has zero black/green bleed
    for (int x = 16; x < dstW - 16; ++x) {
        if (std::abs(x - dstW / 2) <= 4 || std::abs(x - 42) <= 4) {
            continue;
        }
        const size_t i = (static_cast<size_t>(dstH - 1) * dstW + x) * 4;
        QCOMPARE(actualBGRA[i + 0], uint8_t(0x1E));
        QCOMPARE(actualBGRA[i + 1], uint8_t(0x1E));
        QCOMPARE(actualBGRA[i + 2], uint8_t(0x1E));
    }
}

int runMetalYCoCgRTests(int argc, char** argv)
{
    TestMetalYCoCgR tests;
    return QTest::qExec(&tests, argc, argv);
}
