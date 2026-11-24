// Nasty hack to avoid conflict between AVFoundation and
// libavutil both defining AVMediaType
#define AVMediaType AVMediaType_FFmpeg
#include "vt.h"
#include "pacer/pacer.h"
#undef AVMediaType

#include <SDL_syswm.h>
#include <Limelight.h>
#include "streaming/session.h"
#include "streaming/streamutils.h"
#include "streaming/video/adaptiverefinement_codec.h"
#include "streaming/video/packed444.h"
#include "path.h"

#import <Cocoa/Cocoa.h>
#import <VideoToolbox/VideoToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <dispatch/dispatch.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

extern "C" {
    #include <libavutil/pixdesc.h>
}

struct CscParams
{
    simd_half3x3 matrix;
    simd_half3 offsets;
};

struct ParamBuffer
{
    CscParams cscParams;
    simd_half2 chromaOffset;
    simd_half1 bitnessScaleFactor;
};

struct Packed444Params
{
    simd_uint2 logicalSize;
    simd_uint2 codedSize;
    uint32_t tileW;
    uint32_t tileH;
    uint32_t splitX;
    uint32_t isScaled;
    float lumaUnormScale;
};

struct RefinementParams
{
    float yTol;
    uint32_t enabled;
    simd_uint2 logicalSize;
};

struct Vertex
{
    simd_float4 position;
    simd_float2 texCoord;
};

#define MAX_VIDEO_PLANES 3

class VTMetalRenderer;

@interface DisplayLinkDelegate : NSObject <CAMetalDisplayLinkDelegate>

- (id)initWithRenderer:(VTMetalRenderer *)renderer;

@end

class VTMetalRenderer : public VTBaseRenderer
{
public:
    VTMetalRenderer(bool hwAccel)
        : VTBaseRenderer(RendererType::VTMetal),
          m_HwAccel(hwAccel),
          m_Window(nullptr),
          m_HwContext(nullptr),
          m_MetalLayer(nullptr),
          m_MetalDisplayLink(nullptr),
          m_LatestUnrenderedFrame(nullptr),
          m_FrameLock(SDL_CreateMutex()),
          m_FrameReady(SDL_CreateCond()),
          m_TextureCache(nullptr),
          m_CscParamsBuffer(nullptr),
          m_VideoVertexBuffer(nullptr),
          m_OverlayTextures{},
          m_OverlayLock(0),
          m_VideoPipelineState(nullptr),
          m_OverlayPipelineState(nullptr),
          m_ShaderLibrary(nullptr),
          m_CommandQueue(nullptr),
          m_SwMappingTextures{},
          m_MetalView(nullptr),
          m_LastFrameWidth(-1),
          m_LastFrameHeight(-1),
          m_LastDrawableWidth(-1),
          m_LastDrawableHeight(-1),
          m_LastRenderWidth(-1),
          m_LastRenderHeight(-1),
          m_PackedYCoCgRMode(0),
          m_LogicalWidth(0),
          m_LogicalHeight(0),
          m_LastPackedYCoCgRMode(-1),
          m_PackedLayout{},
          m_AdaptiveTileRefinement(false),
          m_RefinementTexture(nullptr),
          m_RefinementMaskTexture(nullptr),
          m_FallbackRefTex(nullptr),
          m_FallbackMaskTex(nullptr),
          m_RefTexWidth(0),
          m_RefTexHeight(0),
          m_LastRefColorSpace(RefinementColorspace::Rec601),
          m_LastRefFullRange(true),
          m_HasRefColorimetry(false)
    {
    }

    virtual ~VTMetalRenderer() override
    { @autoreleasepool {
        // Stop the display link and free associated state
        stopDisplayLink();
        av_frame_free(&m_LatestUnrenderedFrame);
        SDL_DestroyCond(m_FrameReady);
        SDL_DestroyMutex(m_FrameLock);

        if (m_HwContext != nullptr) {
            av_buffer_unref(&m_HwContext);
        }

        if (m_CscParamsBuffer != nullptr) {
            [m_CscParamsBuffer release];
        }

        if (m_VideoVertexBuffer != nullptr) {
            [m_VideoVertexBuffer release];
        }

        if (m_VideoPipelineState != nullptr) {
            [m_VideoPipelineState release];
        }

        for (int i = 0; i < Overlay::OverlayMax; i++) {
            if (m_OverlayTextures[i] != nullptr) {
                [m_OverlayTextures[i] release];
            }
        }

        for (int i = 0; i < MAX_VIDEO_PLANES; i++) {
            if (m_SwMappingTextures[i] != nullptr) {
                [m_SwMappingTextures[i] release];
            }
        }

        if (m_OverlayPipelineState != nullptr) {
            [m_OverlayPipelineState release];
        }

        if (m_ShaderLibrary != nullptr) {
            [m_ShaderLibrary release];
        }

        if (m_CommandQueue != nullptr) {
            [m_CommandQueue release];
        }

        if (m_TextureCache != nullptr) {
            CFRelease(m_TextureCache);
        }

        if (m_RefinementTexture != nullptr) {
            [m_RefinementTexture release];
            m_RefinementTexture = nullptr;
        }

        if (m_RefinementMaskTexture != nullptr) {
            [m_RefinementMaskTexture release];
            m_RefinementMaskTexture = nullptr;
        }

        if (m_FallbackRefTex != nullptr) {
            [m_FallbackRefTex release];
            m_FallbackRefTex = nullptr;
        }

        if (m_FallbackMaskTex != nullptr) {
            [m_FallbackMaskTex release];
            m_FallbackMaskTex = nullptr;
        }

        if (m_MetalView != nullptr) {
            SDL_Metal_DestroyView(m_MetalView);
        }
    }}

    bool updateVideoRegionSizeForFrame(AVFrame* frame)
    {
        int drawableWidth, drawableHeight;
        SDL_Metal_GetDrawableSize(m_Window, &drawableWidth, &drawableHeight);

        const int videoWidth = (m_PackedYCoCgRMode == 1 && m_LogicalWidth > 0) ? m_LogicalWidth : frame->width;
        const int videoHeight = (m_PackedYCoCgRMode == 1 && m_LogicalHeight > 0) ? m_LogicalHeight : frame->height;

        // Check if anything has changed since the last vertex buffer upload
        if (m_VideoVertexBuffer &&
                videoWidth == m_LastFrameWidth && videoHeight == m_LastFrameHeight &&
                drawableWidth == m_LastDrawableWidth && drawableHeight == m_LastDrawableHeight) {
            // Nothing to do
            return true;
        }

        // Determine the correct scaled size for the video region
        SDL_Rect src, dst;
        src.x = src.y = 0;
        src.w = videoWidth;
        src.h = videoHeight;
        dst.x = dst.y = 0;
        dst.w = drawableWidth;
        dst.h = drawableHeight;
        StreamUtils::scaleSourceToDestinationSurface(&src, &dst);

        // Convert screen space to normalized device coordinates
        SDL_FRect renderRect;
        StreamUtils::screenSpaceToNormalizedDeviceCoords(&dst, &renderRect, drawableWidth, drawableHeight);

        Vertex verts[] =
        {
            { { renderRect.x, renderRect.y, 0.0f, 1.0f }, { 0.0f, 1.0f } },
            { { renderRect.x, renderRect.y+renderRect.h, 0.0f, 1.0f }, { 0.0f, 0} },
            { { renderRect.x+renderRect.w, renderRect.y, 0.0f, 1.0f }, { 1.0f, 1.0f} },
            { { renderRect.x+renderRect.w, renderRect.y+renderRect.h, 0.0f, 1.0f }, { 1.0f, 0} },
        };

        [m_VideoVertexBuffer release];
        auto bufferOptions = MTLCPUCacheModeWriteCombined | MTLResourceStorageModeManaged;
        m_VideoVertexBuffer = [m_MetalLayer.device newBufferWithBytes:verts length:sizeof(verts) options:bufferOptions];
        if (!m_VideoVertexBuffer) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to create video vertex buffer");
            return false;
        }

        m_LastFrameWidth = videoWidth;
        m_LastFrameHeight = videoHeight;
        m_LastDrawableWidth = drawableWidth;
        m_LastDrawableHeight = drawableHeight;
        m_LastRenderWidth = dst.w;
        m_LastRenderHeight = dst.h;

        return true;
    }

    int getFramePlaneCount(AVFrame* frame)
    {
        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            return CVPixelBufferGetPlaneCount((CVPixelBufferRef)frame->data[3]);
        }
        else {
            return av_pix_fmt_count_planes((AVPixelFormat)frame->format);
        }
    }

    int getBitnessScaleFactor(AVFrame* frame)
    {
        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            // VideoToolbox frames never require scaling
            return 1;
        }
        else {
            const AVPixFmtDescriptor* formatDesc = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
            if (!formatDesc) {
                // This shouldn't be possible but handle it anyway
                SDL_assert(formatDesc);
                return 1;
            }

            // This assumes plane 0 is exclusively the Y component
            SDL_assert(formatDesc->comp[0].step == 1 || formatDesc->comp[0].step == 2);
            return pow(2, (formatDesc->comp[0].step * 8) - formatDesc->comp[0].depth);
        }
    }

    bool createOverlayPipelineState()
    {
        MTLRenderPipelineDescriptor *pipelineDesc = [[MTLRenderPipelineDescriptor new] autorelease];
        pipelineDesc.vertexFunction = [[m_ShaderLibrary newFunctionWithName:@"vs_draw"] autorelease];
        pipelineDesc.fragmentFunction = [[m_ShaderLibrary newFunctionWithName:@"ps_draw_rgb"] autorelease];
        pipelineDesc.colorAttachments[0].pixelFormat = m_MetalLayer.pixelFormat;
        pipelineDesc.colorAttachments[0].blendingEnabled = YES;
        pipelineDesc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        pipelineDesc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        pipelineDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        pipelineDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
        pipelineDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pipelineDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        [m_OverlayPipelineState release];
        m_OverlayPipelineState = [m_MetalLayer.device newRenderPipelineStateWithDescriptor:pipelineDesc error:nullptr];
        if (!m_OverlayPipelineState) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to create overlay pipeline state");
            return false;
        }

        return true;
    }

    float getPackedLumaUnormScale(const AVFrame* frame) const
    {
        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            // VideoToolbox P010 (xf20/x420) places 10-bit samples in bits [15:6]
            return 65535.0f / 64.0f;
        }

        const AVPixFmtDescriptor* formatDesc = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
        if (formatDesc && formatDesc->comp[0].shift == 6) {
            return 65535.0f / 64.0f;
        }

        // Software AV_PIX_FMT_YUV420P10LE stores 10-bit samples in LSB [9:0]
        // and mapPlaneForSoftwareFrame() uploads directly as MTLPixelFormatR16Unorm
        return 65535.0f;
    }

    bool updateColorSpaceForFrame(AVFrame* frame)
    {
        if (!hasFrameFormatChanged(frame) && !m_HdrMetadataChanged &&
            m_VideoPipelineState != nullptr && m_LastPackedYCoCgRMode == m_PackedYCoCgRMode) {
            return true;
        }

        // Stop the display link before changing the Metal layer
        stopDisplayLink();

        if (m_PackedYCoCgRMode == 1) {
            CGColorSpaceRef newColorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            m_MetalLayer.colorspace = newColorSpace;
            m_MetalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            m_MetalLayer.EDRMetadata = nullptr;
            CGColorSpaceRelease(newColorSpace);

            [m_CscParamsBuffer release];
            m_CscParamsBuffer = nullptr;

            MTLRenderPipelineDescriptor *pipelineDesc = [[MTLRenderPipelineDescriptor new] autorelease];
            pipelineDesc.vertexFunction = [[m_ShaderLibrary newFunctionWithName:@"vs_draw"] autorelease];
            pipelineDesc.fragmentFunction = [[m_ShaderLibrary newFunctionWithName:@"ps_draw_ycocgr_a1"] autorelease];
            pipelineDesc.colorAttachments[0].pixelFormat = m_MetalLayer.pixelFormat;
            [m_VideoPipelineState release];
            m_VideoPipelineState = [m_MetalLayer.device newRenderPipelineStateWithDescriptor:pipelineDesc error:nullptr];
            if (!m_VideoPipelineState) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Failed to create packed YCoCg-R video pipeline state");
                return false;
            }

            if (!createOverlayPipelineState()) {
                return false;
            }

            m_HdrMetadataChanged = false;
            m_LastPackedYCoCgRMode = m_PackedYCoCgRMode;
            return true;
        }

        int colorspace = getFrameColorspace(frame);
        CGColorSpaceRef newColorSpace;
        ParamBuffer paramBuffer;

        switch (colorspace) {
        case COLORSPACE_REC_709:
            m_MetalLayer.colorspace = newColorSpace = CGColorSpaceCreateWithName(kCGColorSpaceITUR_709);
            m_MetalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            break;
        case COLORSPACE_REC_2020:
            m_MetalLayer.pixelFormat = MTLPixelFormatBGR10A2Unorm;
            if (frame->color_trc == AVCOL_TRC_SMPTE2084) {
                // https://developer.apple.com/documentation/metal/hdr_content/using_color_spaces_to_display_hdr_content
                m_MetalLayer.colorspace = newColorSpace = CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_PQ);
            }
            else {
                m_MetalLayer.colorspace = newColorSpace = CGColorSpaceCreateWithName(kCGColorSpaceITUR_2020);
            }
            break;
        default:
        case COLORSPACE_REC_601:
            m_MetalLayer.colorspace = newColorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            m_MetalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            break;
        }

        std::array<float, 9> cscMatrix;
        std::array<float, 3> yuvOffsets;
        std::array<float, 2> chromaOffset;
        getFramePremultipliedCscConstants(frame, cscMatrix, yuvOffsets);
        getFrameChromaCositingOffsets(frame, chromaOffset);

        paramBuffer.cscParams.matrix = simd_matrix(simd_make_half3(cscMatrix[0], cscMatrix[3], cscMatrix[6]),
                                                   simd_make_half3(cscMatrix[1], cscMatrix[4], cscMatrix[7]),
                                                   simd_make_half3(cscMatrix[2], cscMatrix[5], cscMatrix[8]));
        paramBuffer.cscParams.offsets = simd_make_half3(yuvOffsets[0],
                                                        yuvOffsets[1],
                                                        yuvOffsets[2]);
        paramBuffer.chromaOffset = simd_make_half2(chromaOffset[0],
                                                   chromaOffset[1]);

        // Set the EDR metadata for HDR10 to enable OS tonemapping
        if (frame->color_trc == AVCOL_TRC_SMPTE2084 && m_MasteringDisplayColorVolume != nullptr) {
            m_MetalLayer.EDRMetadata = [CAEDRMetadata HDR10MetadataWithDisplayInfo:(__bridge NSData*)m_MasteringDisplayColorVolume
                                                                       contentInfo:(__bridge NSData*)m_ContentLightLevelInfo
                                                                opticalOutputScale:203.0];
        }
        else {
            m_MetalLayer.EDRMetadata = nullptr;
        }

        paramBuffer.bitnessScaleFactor = getBitnessScaleFactor(frame);

        // The CAMetalLayer retains the CGColorSpace
        CGColorSpaceRelease(newColorSpace);

        // Create the new colorspace parameter buffer for our fragment shader
        [m_CscParamsBuffer release];
        auto bufferOptions = MTLCPUCacheModeWriteCombined | MTLResourceStorageModeManaged;
        m_CscParamsBuffer = [m_MetalLayer.device newBufferWithBytes:(void*)&paramBuffer length:sizeof(paramBuffer) options:bufferOptions];
        if (!m_CscParamsBuffer) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to create CSC parameters buffer");
            return false;
        }

        int planes = getFramePlaneCount(frame);
        SDL_assert(planes == 2 || planes == 3);

        NSString *fragName = nil;
        if (m_AdaptiveTileRefinement) {
            fragName = (planes == 2) ? @"ps_draw_biplanar_refined" : @"ps_draw_triplanar_refined";
        } else {
            fragName = (planes == 2) ? @"ps_draw_biplanar" : @"ps_draw_triplanar";
        }

        MTLRenderPipelineDescriptor *pipelineDesc = [[MTLRenderPipelineDescriptor new] autorelease];
        pipelineDesc.vertexFunction = [[m_ShaderLibrary newFunctionWithName:@"vs_draw"] autorelease];
        pipelineDesc.fragmentFunction = [[m_ShaderLibrary newFunctionWithName:fragName] autorelease];
        pipelineDesc.colorAttachments[0].pixelFormat = m_MetalLayer.pixelFormat;
        [m_VideoPipelineState release];
        m_VideoPipelineState = [m_MetalLayer.device newRenderPipelineStateWithDescriptor:pipelineDesc error:nullptr];
        if (!m_VideoPipelineState) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to create video pipeline state");
            return false;
        }

        if (!createOverlayPipelineState()) {
            return false;
        }

        m_HdrMetadataChanged = false;
        m_LastPackedYCoCgRMode = m_PackedYCoCgRMode;
        return true;
    }

    id<MTLTexture> mapPlaneForSoftwareFrame(AVFrame* frame, int planeIndex)
    {
        const AVPixFmtDescriptor* formatDesc = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
        if (!formatDesc) {
            // This shouldn't be possible but handle it anyway
            SDL_assert(formatDesc);
            return nil;
        }

        SDL_assert(planeIndex < MAX_VIDEO_PLANES);

        NSUInteger planeWidth = planeIndex ? AV_CEIL_RSHIFT(frame->width, formatDesc->log2_chroma_w) : frame->width;
        NSUInteger planeHeight = planeIndex ? AV_CEIL_RSHIFT(frame->height, formatDesc->log2_chroma_h) : frame->height;

        // Recreate the texture if the plane size changes
        if (m_SwMappingTextures[planeIndex] && (m_SwMappingTextures[planeIndex].width != planeWidth ||
                                                m_SwMappingTextures[planeIndex].height != planeHeight)) {
            [m_SwMappingTextures[planeIndex] release];
            m_SwMappingTextures[planeIndex] = nil;
        }

        if (!m_SwMappingTextures[planeIndex]) {
            MTLPixelFormat metalFormat;

            switch (formatDesc->comp[planeIndex].step) {
            case 1:
                metalFormat = MTLPixelFormatR8Unorm;
                break;
            case 2:
                metalFormat = MTLPixelFormatR16Unorm;
                break;
            default:
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Unhandled plane step: %d (plane: %d)",
                             formatDesc->comp[planeIndex].step,
                             planeIndex);
                SDL_assert(false);
                return nil;
            }

            auto texDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:metalFormat
                                                                              width:planeWidth
                                                                             height:planeHeight
                                                                          mipmapped:NO];
            texDesc.cpuCacheMode = MTLCPUCacheModeWriteCombined;
            texDesc.storageMode = MTLStorageModeManaged;
            texDesc.usage = MTLTextureUsageShaderRead;

            m_SwMappingTextures[planeIndex] = [m_MetalLayer.device newTextureWithDescriptor:texDesc];
            if (!m_SwMappingTextures[planeIndex]) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Failed to allocate software frame texture");
                return nil;
            }
        }

        [m_SwMappingTextures[planeIndex] replaceRegion:MTLRegionMake2D(0, 0, planeWidth, planeHeight)
                                           mipmapLevel:0
                                             withBytes:frame->data[planeIndex]
                                           bytesPerRow:frame->linesize[planeIndex]];

        return m_SwMappingTextures[planeIndex];
    }

    bool createTexturesFromFrame(AVFrame* frame, std::array<CVMetalTextureRef, MAX_VIDEO_PLANES>& cvMetalTextures)
    {
        SDL_assert(frame->format == AV_PIX_FMT_VIDEOTOOLBOX);

        CVPixelBufferRef pixBuf = reinterpret_cast<CVPixelBufferRef>(frame->data[3]);
        size_t planes = getFramePlaneCount(frame);

        // Create Metal textures for the planes of the CVPixelBuffer
        for (size_t i = 0; i < planes; i++) {
            MTLPixelFormat fmt;

            switch (CVPixelBufferGetPixelFormatType(pixBuf)) {
            case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
            case kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange:
            case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
            case kCVPixelFormatType_444YpCbCr8BiPlanarFullRange:
                fmt = (i == 0) ? MTLPixelFormatR8Unorm : MTLPixelFormatRG8Unorm;
                break;

            case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
            case kCVPixelFormatType_444YpCbCr10BiPlanarFullRange:
            case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
            case kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange:
                fmt = (i == 0) ? MTLPixelFormatR16Unorm : MTLPixelFormatRG16Unorm;
                break;

            default:
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Unknown pixel format: %x",
                             CVPixelBufferGetPixelFormatType(pixBuf));
                return false;
            }

            CVReturn err = CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault, m_TextureCache, pixBuf, nullptr, fmt,
                                                                     CVPixelBufferGetWidthOfPlane(pixBuf, i),
                                                                     CVPixelBufferGetHeightOfPlane(pixBuf, i),
                                                                     i,
                                                                     &cvMetalTextures[i]);
            if (err != kCVReturnSuccess) {
                for (size_t j = 0; j < i; j++) {
                    CFRelease(cvMetalTextures[j]);
                }
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "CVMetalTextureCacheCreateTextureFromImage() failed: %d",
                             err);
                return false;
            }
        }

        return true;
    }

    bool testRenderFrame(AVFrame *frame) override
    { @autoreleasepool {
        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            std::array<CVMetalTextureRef, MAX_VIDEO_PLANES> cvMetalTextures;
            size_t planes = getFramePlaneCount(frame);
            SDL_assert(planes <= MAX_VIDEO_PLANES);

            // Test that we can actually create Metal textures from the CVPixelBufferRef
            if (!createTexturesFromFrame(frame, cvMetalTextures)) {
                return false;
            }

            for (size_t i = 0; i < planes; i++) {
                CFRelease(cvMetalTextures[i]);
            }
        }
        else {
            // Mapping software frames should always work
        }

        return true;
    }}

    void clearRefinementMask()
    {
        if (!m_RefinementMaskTexture) {
            return;
        }
        const NSUInteger maskW = m_RefinementMaskTexture.width;
        const NSUInteger maskH = m_RefinementMaskTexture.height;
        if (maskW == 0 || maskH == 0) {
            return;
        }
        std::vector<uint8_t> zeros(maskW * maskH, 0);
        [m_RefinementMaskTexture replaceRegion:MTLRegionMake2D(0, 0, maskW, maskH)
                                   mipmapLevel:0
                                     withBytes:zeros.data()
                                   bytesPerRow:maskW];
    }

    bool ensureFallbackRefinementTextures()
    {
        if (m_FallbackRefTex && m_FallbackMaskTex) {
            return true;
        }
        if (!m_MetalLayer || !m_MetalLayer.device) {
            return false;
        }
        const MTLStorageMode storageMode =
            m_MetalLayer.device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;

        if (!m_FallbackRefTex) {
            auto refDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                              width:1
                                                                             height:1
                                                                          mipmapped:NO];
            refDesc.cpuCacheMode = MTLCPUCacheModeWriteCombined;
            refDesc.storageMode = storageMode;
            refDesc.usage = MTLTextureUsageShaderRead;
            m_FallbackRefTex = [m_MetalLayer.device newTextureWithDescriptor:refDesc];
            if (!m_FallbackRefTex) {
                return false;
            }
            const uint8_t zeroRef[4] = {0, 0, 0, 0};
            [m_FallbackRefTex replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                                mipmapLevel:0
                                  withBytes:zeroRef
                                bytesPerRow:4];
        }

        if (!m_FallbackMaskTex) {
            auto maskDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                               width:1
                                                                              height:1
                                                                           mipmapped:NO];
            maskDesc.cpuCacheMode = MTLCPUCacheModeWriteCombined;
            maskDesc.storageMode = storageMode;
            maskDesc.usage = MTLTextureUsageShaderRead;
            m_FallbackMaskTex = [m_MetalLayer.device newTextureWithDescriptor:maskDesc];
            if (!m_FallbackMaskTex) {
                return false;
            }
            const uint8_t zeroMask = 0;
            [m_FallbackMaskTex replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                                 mipmapLevel:0
                                   withBytes:&zeroMask
                                 bytesPerRow:1];
        }
        return true;
    }

    bool ensureRefinementTextures(int width, int height)
    {
        if (width <= 0 || height <= 0 || !m_MetalLayer || !m_MetalLayer.device) {
            return false;
        }
        if (m_RefinementTexture && m_RefinementMaskTexture &&
            m_RefTexWidth == width && m_RefTexHeight == height) {
            return true;
        }

        [m_RefinementTexture release];
        m_RefinementTexture = nullptr;
        [m_RefinementMaskTexture release];
        m_RefinementMaskTexture = nullptr;
        m_RefTexWidth = 0;
        m_RefTexHeight = 0;

        const MTLStorageMode storageMode =
            m_MetalLayer.device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;

        auto refDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                          width:(NSUInteger)width
                                                                         height:(NSUInteger)height
                                                                      mipmapped:NO];
        refDesc.cpuCacheMode = MTLCPUCacheModeWriteCombined;
        refDesc.storageMode = storageMode;
        refDesc.usage = MTLTextureUsageShaderRead;
        m_RefinementTexture = [m_MetalLayer.device newTextureWithDescriptor:refDesc];
        if (!m_RefinementTexture) {
            return false;
        }

        const int tilesX = (width + AdaptiveRefinementWire::TILE_SIZE - 1) / AdaptiveRefinementWire::TILE_SIZE;
        const int tilesY = (height + AdaptiveRefinementWire::TILE_SIZE - 1) / AdaptiveRefinementWire::TILE_SIZE;
        auto maskDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                                           width:(NSUInteger)tilesX
                                                                          height:(NSUInteger)tilesY
                                                                       mipmapped:NO];
        maskDesc.cpuCacheMode = MTLCPUCacheModeWriteCombined;
        maskDesc.storageMode = storageMode;
        maskDesc.usage = MTLTextureUsageShaderRead;
        m_RefinementMaskTexture = [m_MetalLayer.device newTextureWithDescriptor:maskDesc];
        if (!m_RefinementMaskTexture) {
            [m_RefinementTexture release];
            m_RefinementTexture = nullptr;
            return false;
        }

        m_RefTexWidth = width;
        m_RefTexHeight = height;
        clearRefinementMask();
        return true;
    }

    bool syncRefinementForFrame(AVFrame* frame)
    {
        if (!m_AdaptiveTileRefinement || !frame || frame->width <= 0 || frame->height <= 0) {
            return false;
        }
        if (!ensureRefinementTextures(frame->width, frame->height)) {
            return false;
        }

        RefinementColorspace refCs = RefinementColorspace::Rec601;
        switch (getFrameColorspace(frame)) {
        case COLORSPACE_REC_709:
            refCs = RefinementColorspace::Rec709;
            break;
        case COLORSPACE_REC_2020:
            refCs = RefinementColorspace::Rec2020;
            break;
        default:
        case COLORSPACE_REC_601:
            refCs = RefinementColorspace::Rec601;
            break;
        }
        const bool refFullRange = isFrameFullRange(frame);

        if (m_HasRefColorimetry && (m_LastRefColorSpace != refCs || m_LastRefFullRange != refFullRange)) {
            clearRefinementMask();
        }
        m_LastRefColorSpace = refCs;
        m_LastRefFullRange = refFullRange;
        m_HasRefColorimetry = true;

        if (!m_RefinementInbox) {
            return true;
        }

        m_RefinementInbox->setColorimetry(refCs, refFullRange);

        bool resetAll = false;
        uint16_t batchW = 0;
        uint16_t batchH = 0;
        std::vector<AdaptiveDecodedTile> tiles;
        if (m_RefinementInbox->takePending(resetAll, batchW, batchH, tiles, refCs, refFullRange)) {
            if (resetAll) {
                clearRefinementMask();
            }
            const bool dimsMatch = (batchW == 0 && batchH == 0) ||
                                   (batchW == (uint16_t)frame->width && batchH == (uint16_t)frame->height);
            if (dimsMatch) {
                const NSUInteger tilesX = m_RefinementMaskTexture.width;
                const NSUInteger tilesY = m_RefinementMaskTexture.height;
                for (const AdaptiveDecodedTile& tile : tiles) {
                    if (tile.w == 0 || tile.h == 0 ||
                        tile.w > AdaptiveRefinementWire::TILE_SIZE ||
                        tile.h > AdaptiveRefinementWire::TILE_SIZE ||
                        (tile.x % AdaptiveRefinementWire::TILE_SIZE) != 0 ||
                        (tile.y % AdaptiveRefinementWire::TILE_SIZE) != 0 ||
                        (int)tile.x + (int)tile.w > frame->width ||
                        (int)tile.y + (int)tile.h > frame->height ||
                        tile.rgba.size() != (int)tile.w * (int)tile.h * 4) {
                        continue;
                    }
                    [m_RefinementTexture replaceRegion:MTLRegionMake2D(tile.x, tile.y, tile.w, tile.h)
                                           mipmapLevel:0
                                             withBytes:tile.rgba.constData()
                                           bytesPerRow:(NSUInteger)tile.w * 4u];
                    const NSUInteger tx = tile.x / AdaptiveRefinementWire::TILE_SIZE;
                    const NSUInteger ty = tile.y / AdaptiveRefinementWire::TILE_SIZE;
                    if (tx < tilesX && ty < tilesY) {
                        const uint8_t maskOn = 255;
                        [m_RefinementMaskTexture replaceRegion:MTLRegionMake2D(tx, ty, 1, 1)
                                                   mipmapLevel:0
                                                     withBytes:&maskOn
                                                   bytesPerRow:1];
                    }
                }
            }
        }
        return true;
    }

    // Caller frees frame after we return
    virtual void renderFrameIntoDrawable(AVFrame* frame, id<CAMetalDrawable> drawable)
    { @autoreleasepool {
        if (m_PackedYCoCgRMode == 1 &&
            (!m_PackedLayout.valid || frame->width < m_PackedLayout.encW || frame->height < m_PackedLayout.encH)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Packed YCoCg-R frame (%dx%d) smaller than expected coded layout (%dx%d)",
                        frame->width, frame->height, m_PackedLayout.encW, m_PackedLayout.encH);
            return;
        }

        std::array<CVMetalTextureRef, MAX_VIDEO_PLANES> cvMetalTextures;
        size_t planes = getFramePlaneCount(frame);
        SDL_assert(planes <= MAX_VIDEO_PLANES);

        id<MTLTexture> swLumaTex = nil;
        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            if (!createTexturesFromFrame(frame, cvMetalTextures)) {
                return;
            }
        }
        else if (m_PackedYCoCgRMode == 1) {
            swLumaTex = mapPlaneForSoftwareFrame(frame, 0);
            if (!swLumaTex) {
                return;
            }
        }

        // Prepare a render pass to render into the next drawable
        const bool refReady = (m_PackedYCoCgRMode == 0 && m_AdaptiveTileRefinement)
            ? syncRefinementForFrame(frame)
            : false;

        auto renderPassDescriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        renderPassDescriptor.colorAttachments[0].texture = drawable.texture;
        renderPassDescriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
        renderPassDescriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
        renderPassDescriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
        auto commandBuffer = [m_CommandQueue commandBuffer];
        auto renderEncoder = [commandBuffer renderCommandEncoderWithDescriptor:renderPassDescriptor];

        // Bind textures and buffers then draw the video region
        [renderEncoder setRenderPipelineState:m_VideoPipelineState];
        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
            if (m_PackedYCoCgRMode == 1) {
                [renderEncoder setFragmentTexture:CVMetalTextureGetTexture(cvMetalTextures[0]) atIndex:0];
            }
            else {
                for (size_t i = 0; i < planes; i++) {
                    [renderEncoder setFragmentTexture:CVMetalTextureGetTexture(cvMetalTextures[i]) atIndex:i];
                }
            }
            [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                // Free textures after completion of rendering per CVMetalTextureCache requirements
                for (size_t i = 0; i < planes; i++) {
                    CFRelease(cvMetalTextures[i]);
                }
            }];
        }
        else if (m_PackedYCoCgRMode == 1) {
            [renderEncoder setFragmentTexture:swLumaTex atIndex:0];
        }
        else {
            for (size_t i = 0; i < planes; i++) {
                [renderEncoder setFragmentTexture:mapPlaneForSoftwareFrame(frame, i) atIndex:i];
            }
        }

        if (m_PackedYCoCgRMode == 1) {
            Packed444Params packedParams = {};
            packedParams.logicalSize = simd_make_uint2((uint32_t)m_LogicalWidth, (uint32_t)m_LogicalHeight);
            packedParams.codedSize = simd_make_uint2((uint32_t)frame->width, (uint32_t)frame->height);
            packedParams.tileW = (uint32_t)m_PackedLayout.tileW;
            packedParams.tileH = (uint32_t)m_PackedLayout.tileH;
            packedParams.splitX = (uint32_t)m_PackedLayout.splitX;
            packedParams.isScaled = (m_LastRenderWidth != m_LogicalWidth || m_LastRenderHeight != m_LogicalHeight) ? 1u : 0u;
            packedParams.lumaUnormScale = getPackedLumaUnormScale(frame);
            [renderEncoder setFragmentBytes:&packedParams length:sizeof(packedParams) atIndex:0];
        }
        else {
            [renderEncoder setFragmentBuffer:m_CscParamsBuffer offset:0 atIndex:0];
            if (m_AdaptiveTileRefinement && (planes == 2 || planes == 3)) {
                RefinementParams refParams = {};
                refParams.yTol = 8.5f / 255.0f;
                refParams.logicalSize = simd_make_uint2((uint32_t)frame->width, (uint32_t)frame->height);
                const NSUInteger refIdx = (planes == 2) ? 2 : 3;
                const NSUInteger maskIdx = (planes == 2) ? 3 : 4;
                id<MTLTexture> bindRef = nil;
                id<MTLTexture> bindMask = nil;
                if (refReady && m_RefinementTexture && m_RefinementMaskTexture) {
                    refParams.enabled = 1u;
                    bindRef = m_RefinementTexture;
                    bindMask = m_RefinementMaskTexture;
                } else if (ensureFallbackRefinementTextures()) {
                    refParams.enabled = 0u;
                    bindRef = m_FallbackRefTex;
                    bindMask = m_FallbackMaskTex;
                }
                if (!bindRef || !bindMask) {
                    [renderEncoder endEncoding];
                    return;
                }
                [renderEncoder setFragmentTexture:bindRef atIndex:refIdx];
                [renderEncoder setFragmentTexture:bindMask atIndex:maskIdx];
                [renderEncoder setFragmentBytes:&refParams length:sizeof(refParams) atIndex:1];
            }
        }
        [renderEncoder setVertexBuffer:m_VideoVertexBuffer offset:0 atIndex:0];
        [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];

        // Now draw any overlays that are enabled
        for (int i = 0; i < Overlay::OverlayMax; i++) {
            id<MTLTexture> overlayTexture = nullptr;

            // Try to acquire a reference on the overlay texture
            SDL_AtomicLock(&m_OverlayLock);
            overlayTexture = [m_OverlayTextures[i] retain];
            SDL_AtomicUnlock(&m_OverlayLock);

            if (overlayTexture) {
                SDL_FRect renderRect = {};
                if (i == Overlay::OverlayStatusUpdate) {
                    // Bottom Left
                    renderRect.x = 0;
                    renderRect.y = 0;
                }
                else if (i == Overlay::OverlayDebug) {
                    // Top left
                    renderRect.x = 0;
                    renderRect.y = m_LastDrawableHeight - overlayTexture.height;
                }

                renderRect.w = overlayTexture.width;
                renderRect.h = overlayTexture.height;

                // Convert screen space to normalized device coordinates
                StreamUtils::screenSpaceToNormalizedDeviceCoords(&renderRect, m_LastDrawableWidth, m_LastDrawableHeight);

                Vertex verts[] =
                {
                    { { renderRect.x, renderRect.y, 0.0f, 1.0f }, { 0.0f, 1.0f } },
                    { { renderRect.x, renderRect.y+renderRect.h, 0.0f, 1.0f }, { 0.0f, 0} },
                    { { renderRect.x+renderRect.w, renderRect.y, 0.0f, 1.0f }, { 1.0f, 1.0f} },
                    { { renderRect.x+renderRect.w, renderRect.y+renderRect.h, 0.0f, 1.0f }, { 1.0f, 0} },
                };

                [renderEncoder setRenderPipelineState:m_OverlayPipelineState];
                [renderEncoder setFragmentTexture:overlayTexture atIndex:0];
                [renderEncoder setVertexBytes:verts length:sizeof(verts) atIndex:0];
                [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:SDL_arraysize(verts)];

                [overlayTexture release];
            }
        }

        [renderEncoder endEncoding];

        // Flip to the newly rendered buffer
        [commandBuffer presentDrawable:drawable];
        [commandBuffer commit];

        // Wait for the command buffer to complete and free our CVMetalTextureCache references
        [commandBuffer waitUntilCompleted];
    }}

    // Caller frees frame after we return
    virtual void renderFrame(AVFrame* frame) override
    { @autoreleasepool {
        // Handle changes to the frame's colorspace from last time we rendered
        if (!updateColorSpaceForFrame(frame)) {
            // Trigger the main thread to recreate the decoder
            SDL_Event event;
            event.type = SDL_RENDER_DEVICE_RESET;
            SDL_PushEvent(&event);
            return;
        }

        // Handle changes to the video size or drawable size
        if (!updateVideoRegionSizeForFrame(frame)) {
            // Trigger the main thread to recreate the decoder
            SDL_Event event;
            event.type = SDL_RENDER_DEVICE_RESET;
            SDL_PushEvent(&event);
            return;
        }

        // Start the display link if necessary
        startDisplayLink();

        if (hasDisplayLink()) {
            // Move the buffers into a new AVFrame
            AVFrame* newFrame = av_frame_alloc();
            av_frame_move_ref(newFrame, frame);

            // Replace any existing unrendered frame with this new one
            // and signal the CAMetalDisplayLink callback
            AVFrame* oldFrame = nullptr;
            SDL_LockMutex(m_FrameLock);
            if (m_LatestUnrenderedFrame != nullptr) {
                oldFrame = m_LatestUnrenderedFrame;
            }
            m_LatestUnrenderedFrame = newFrame;
            SDL_UnlockMutex(m_FrameLock);
            SDL_CondSignal(m_FrameReady);

            av_frame_free(&oldFrame);
        }
        else {
            // Render to the next drawable right now when CAMetalDisplayLink is not in use
            id<CAMetalDrawable> drawable = [m_MetalLayer nextDrawable];
            if (drawable == nullptr) {
                return;
            }

            renderFrameIntoDrawable(frame, drawable);
        }
    }}

    id<MTLDevice> getMetalDevice() {
        if (qgetenv("VT_FORCE_METAL") == "0") {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Avoiding Metal renderer due to VT_FORCE_METAL=0 override.");
            return nullptr;
        }

        NSArray<id<MTLDevice>> *devices = [MTLCopyAllDevices() autorelease];
        if (devices.count == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "No Metal device found!");
            return nullptr;
        }

        // First, try to find a low power (Intel) or unified memory (Apple Silicon) GPU
        for (id<MTLDevice> device in devices) {
            if (device.isLowPower || device.hasUnifiedMemory) {
                return device;
            }
        }

        // Next, we'll just try to pick something internal to the system
        for (id<MTLDevice> device in devices) {
            if (!device.isRemovable) {
                return device;
            }
        }

        // Use the system-default device
        return [MTLCreateSystemDefaultDevice() autorelease];
    }

    virtual bool initialize(PDECODER_PARAMETERS params) override
    { @autoreleasepool {
        int err;

        m_Window = params->window;
        m_FrameRateRange = CAFrameRateRangeMake(params->frameRate, params->frameRate, params->frameRate);
        m_PackedYCoCgRMode = params->packedYCoCgRMode;
        m_LogicalWidth = params->logicalWidth;
        m_LogicalHeight = params->logicalHeight;
        m_LastPackedYCoCgRMode = -1;
        m_AdaptiveTileRefinement = params->adaptiveTileRefinement && (m_PackedYCoCgRMode == 0);
        m_RefinementInbox = params->refinementInbox;
        m_HasRefColorimetry = false;
        if (m_AdaptiveTileRefinement && m_RefinementInbox) {
            m_RefinementInbox->setColorimetry(RefinementColorspace::Rec601, true);
        }

        if (m_PackedYCoCgRMode == 1 && !params->testOnly) {
            m_PackedLayout = computePackedYCoCgRA1Layout(m_LogicalWidth,
                                                         m_LogicalHeight,
                                                         qMax(params->width, 8192),
                                                         qMax(params->height, 4352));
            if (!m_PackedLayout.valid) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "Invalid packed YCoCg-R logical dimensions: %dx%d",
                             m_LogicalWidth, m_LogicalHeight);
                return false;
            }
        }

        id<MTLDevice> device = getMetalDevice();
        if (!device) {
            m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
            return false;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Selected Metal device: %s",
                    device.name.UTF8String);

        if (m_HwAccel && !checkDecoderCapabilities(device, params)) {
            return false;
        }

        err = av_hwdevice_ctx_create(&m_HwContext,
                                     AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                     nullptr,
                                     nullptr,
                                     0);
        if (err < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "av_hwdevice_ctx_create() failed for VT decoder: %d",
                        err);
            m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
            return false;
        }

        // Create the Metal texture cache for our CVPixelBuffers
        NSDictionary *textureAttributes = @{
            // NB: kCVMetalTextureUsage is documented as if it's a key used in the
            // cacheAttributes dictionary, but it is actually for textureAttributes
            // (as confirmed by Metal Validation failures for incorrect usages).
            (__bridge NSString *)kCVMetalTextureUsage: @(MTLTextureUsageShaderRead)
        };

        err = CVMetalTextureCacheCreate(kCFAllocatorDefault,
                                        nullptr,
                                        device,
                                        (__bridge CFDictionaryRef)textureAttributes,
                                        &m_TextureCache);

        if (err != kCVReturnSuccess) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "CVMetalTextureCacheCreate() failed: %d",
                         err);
            return false;
        }

        // Compile our shaders
        QString shaderSource = QString::fromUtf8(Path::readDataFile("vt_renderer.metal"));
        NSError *shaderError = nil;
        m_ShaderLibrary = [device newLibraryWithSource:shaderSource.toNSString() options:nullptr error:&shaderError];
        if (!m_ShaderLibrary) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Failed to compile shaders: %s",
                         shaderError ? shaderError.localizedDescription.UTF8String : "unknown");
            return false;
        }

        // Create a command queue for submission
        m_CommandQueue = [device newCommandQueue];

        // Add the Metal view to the window if we're not in test-only mode
        //
        // NB: Test-only renderers may be created on a non-main thread, so
        // we don't want to touch the view hierarchy in that context.
        if (!params->testOnly) {
            m_MetalView = SDL_Metal_CreateView(m_Window);
            if (!m_MetalView) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "SDL_Metal_CreateView() failed: %s",
                             SDL_GetError());
                return false;
            }

            m_MetalLayer = (CAMetalLayer*)SDL_Metal_GetLayer(m_MetalView);

            // Choose a device
            m_MetalLayer.device = device;

            // Allow EDR content if we're streaming in a 10-bit HDR format (never for SDR packed 4:4:4)
            m_MetalLayer.wantsExtendedDynamicRangeContent =
                (m_PackedYCoCgRMode == 0) && !!(params->videoFormat & VIDEO_FORMAT_MASK_10BIT);

            // Allow tearing if V-Sync is off (also requires direct display path)
            m_MetalLayer.displaySyncEnabled = params->enableVsync;
        }

        return true;
    }}

    virtual void notifyOverlayUpdated(Overlay::OverlayType type) override
    { @autoreleasepool {
        SDL_Surface* newSurface = Session::get()->getOverlayManager().getUpdatedOverlaySurface(type);
        bool overlayEnabled = Session::get()->getOverlayManager().isOverlayEnabled(type);
        if (newSurface == nullptr && overlayEnabled) {
            // The overlay is enabled and there is no new surface. Leave the old texture alone.
            return;
        }

        SDL_AtomicLock(&m_OverlayLock);
        auto oldTexture = m_OverlayTextures[type];
        m_OverlayTextures[type] = nullptr;
        SDL_AtomicUnlock(&m_OverlayLock);

        [oldTexture release];

        // If the overlay is disabled, we're done
        if (!overlayEnabled) {
            SDL_FreeSurface(newSurface);
            return;
        }

        // Create a texture to hold our pixel data
        SDL_assert(!SDL_MUSTLOCK(newSurface));
        SDL_assert(newSurface->format->format == SDL_PIXELFORMAT_ARGB8888);
        auto texDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                          width:newSurface->w
                                                                         height:newSurface->h
                                                                      mipmapped:NO];
        texDesc.cpuCacheMode = MTLCPUCacheModeWriteCombined;
        texDesc.storageMode = MTLStorageModeManaged;
        texDesc.usage = MTLTextureUsageShaderRead;
        auto newTexture = [m_MetalLayer.device newTextureWithDescriptor:texDesc];

        // Load the pixel data into the new texture
        [newTexture replaceRegion:MTLRegionMake2D(0, 0, newSurface->w, newSurface->h)
                      mipmapLevel:0
                        withBytes:newSurface->pixels
                      bytesPerRow:newSurface->pitch];

        // The surface is no longer required
        SDL_FreeSurface(newSurface);
        newSurface = nullptr;

        SDL_AtomicLock(&m_OverlayLock);
        m_OverlayTextures[type] = newTexture;
        SDL_AtomicUnlock(&m_OverlayLock);
    }}

    virtual bool prepareDecoderContext(AVCodecContext* context, AVDictionary**) override
    {
        if (m_HwAccel) {
            context->hw_device_ctx = av_buffer_ref(m_HwContext);
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Using Metal renderer with %s decoding",
                    m_HwAccel ? "hardware" : "software");

        return true;
    }

    void startDisplayLink()
    {
        if (@available(macOS 14, *)) {
            if (m_MetalDisplayLink != nullptr || !m_MetalLayer.displaySyncEnabled || !isAppleSilicon()) {
                return;
            }

            m_MetalDisplayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:m_MetalLayer];
            m_MetalDisplayLink.preferredFrameLatency = 1.0f;
            m_MetalDisplayLink.preferredFrameRateRange = m_FrameRateRange;
            m_MetalDisplayLink.delegate = [[DisplayLinkDelegate alloc] initWithRenderer:this];
            [m_MetalDisplayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
        }
    }

    void stopDisplayLink()
    {
        if (@available(macOS 14, *)) {
            if (m_MetalDisplayLink == nullptr) {
                return;
            }

            [m_MetalDisplayLink invalidate];
            m_MetalDisplayLink = nullptr;
        }
    }

    bool hasDisplayLink()
    {
        if (@available(macOS 14, *)) {
            if (m_MetalDisplayLink != nullptr) {
                return true;
            }
        }

        return false;
    }

    int getDecoderColorspace() override
    {
        // macOS seems to handle Rec 601 best
        return COLORSPACE_REC_601;
    }

    int getDecoderCapabilities() override
    {
        return CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC |
               CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1;
    }

    int getRendererAttributes() override
    {
        // Metal supports HDR output
        return RENDERER_ATTRIBUTE_HDR_SUPPORT;
    }

    bool isPixelFormatSupported(int videoFormat, AVPixelFormat pixelFormat) override
    {
        if (m_HwAccel) {
            return pixelFormat == AV_PIX_FMT_VIDEOTOOLBOX;
        }
        else {
            if (pixelFormat == AV_PIX_FMT_VIDEOTOOLBOX) {
                // VideoToolbox frames are always supported
                return true;
            }
            else {
                // Otherwise it's supported if we can map it
                const int expectedPixelDepth = (videoFormat & VIDEO_FORMAT_MASK_10BIT) ? 10 : 8;
                const int expectedLog2ChromaW = (videoFormat & VIDEO_FORMAT_MASK_YUV444) ? 0 : 1;
                const int expectedLog2ChromaH = (videoFormat & VIDEO_FORMAT_MASK_YUV444) ? 0 : 1;

                const AVPixFmtDescriptor* formatDesc = av_pix_fmt_desc_get(pixelFormat);
                if (!formatDesc) {
                    // This shouldn't be possible but handle it anyway
                    SDL_assert(formatDesc);
                    return false;
                }

                int planes = av_pix_fmt_count_planes(pixelFormat);
                return (planes == 2 || planes == 3) &&
                       formatDesc->comp[0].depth == expectedPixelDepth &&
                       formatDesc->log2_chroma_w == expectedLog2ChromaW &&
                       formatDesc->log2_chroma_h == expectedLog2ChromaH;
            }
        }
    }

    bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) override
    {
        auto unhandledStateFlags = info->stateChangeFlags;

        // We can always handle size changes
        unhandledStateFlags &= ~WINDOW_STATE_CHANGE_SIZE;

        // We can handle monitor changes
        unhandledStateFlags &= ~WINDOW_STATE_CHANGE_DISPLAY;

        // If nothing is left, we handled everything
        return unhandledStateFlags == 0;
    }

    void renderLatestFrameOnDrawable(id<CAMetalDrawable> drawable, CFTimeInterval targetTimestamp)
    {
        AVFrame* frame = nullptr;

        // Determine how long we can wait depending on how long our CAMetalDisplayLink
        // says we have until the next frame needs to be rendered. We will wait up to
        // half the per-frame interval for a new frame to become available.
        int waitTimeMs = ((targetTimestamp - CACurrentMediaTime()) * 1000) / 2;
        if (waitTimeMs < 0) {
            return;
        }

        // Wait for a new frame to be ready
        SDL_LockMutex(m_FrameLock);
        if (m_LatestUnrenderedFrame != nullptr || SDL_CondWaitTimeout(m_FrameReady, m_FrameLock, waitTimeMs) == 0) {
            frame = m_LatestUnrenderedFrame;
            m_LatestUnrenderedFrame = nullptr;
        }
        SDL_UnlockMutex(m_FrameLock);

        // Render a frame if we got one in time
        if (frame != nullptr) {
            renderFrameIntoDrawable(frame, drawable);
            av_frame_free(&frame);
        }
    }

private:
    bool m_HwAccel;
    SDL_Window* m_Window;
    AVBufferRef* m_HwContext;
    CAMetalLayer* m_MetalLayer;
    CAMetalDisplayLink* m_MetalDisplayLink API_AVAILABLE(macos(14.0));
    CAFrameRateRange m_FrameRateRange;
    AVFrame* m_LatestUnrenderedFrame;
    SDL_mutex* m_FrameLock;
    SDL_cond* m_FrameReady;
    CVMetalTextureCacheRef m_TextureCache;
    id<MTLBuffer> m_CscParamsBuffer;
    id<MTLBuffer> m_VideoVertexBuffer;
    id<MTLTexture> m_OverlayTextures[Overlay::OverlayMax];
    SDL_SpinLock m_OverlayLock;
    id<MTLRenderPipelineState> m_VideoPipelineState;
    id<MTLRenderPipelineState> m_OverlayPipelineState;
    id<MTLLibrary> m_ShaderLibrary;
    id<MTLCommandQueue> m_CommandQueue;
    id<MTLTexture> m_SwMappingTextures[MAX_VIDEO_PLANES];
    SDL_MetalView m_MetalView;
    int m_LastFrameWidth;
    int m_LastFrameHeight;
    int m_LastDrawableWidth;
    int m_LastDrawableHeight;
    int m_LastRenderWidth;
    int m_LastRenderHeight;
    int m_PackedYCoCgRMode;
    int m_LogicalWidth;
    int m_LogicalHeight;
    int m_LastPackedYCoCgRMode;
    PackedYCoCgRLayout m_PackedLayout;
    bool m_AdaptiveTileRefinement;
    std::shared_ptr<AdaptiveTileInbox> m_RefinementInbox;
    id<MTLTexture> m_RefinementTexture;
    id<MTLTexture> m_RefinementMaskTexture;
    id<MTLTexture> m_FallbackRefTex;
    id<MTLTexture> m_FallbackMaskTex;
    int m_RefTexWidth;
    int m_RefTexHeight;
    RefinementColorspace m_LastRefColorSpace;
    bool m_LastRefFullRange;
    bool m_HasRefColorimetry;
};

@implementation DisplayLinkDelegate {
    VTMetalRenderer* _renderer;
}

- (id)initWithRenderer:(VTMetalRenderer *)renderer {
    _renderer = renderer;
    return self;
}

- (void)metalDisplayLink:(CAMetalDisplayLink *)link
             needsUpdate:(CAMetalDisplayLinkUpdate *)update API_AVAILABLE(macos(14.0)) {
    _renderer->renderLatestFrameOnDrawable(update.drawable, update.targetTimestamp);
}

@end

IFFmpegRenderer* VTMetalRendererFactory::createRenderer(bool hwAccel) {
    return new VTMetalRenderer(hwAccel);
}

bool VTMetalRendererFactory::isMetalAvailable() {
@autoreleasepool {
    if (qgetenv("VT_FORCE_METAL") == "0") {
        return false;
    }

    NSArray<id<MTLDevice>> *devices = [MTLCopyAllDevices() autorelease];
    if (devices.count > 0) {
        return true;
    }

    return [MTLCreateSystemDefaultDevice() autorelease] != nil;
}
}
