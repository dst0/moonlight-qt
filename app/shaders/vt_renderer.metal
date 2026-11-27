#include <metal_stdlib>
using namespace metal;

struct Vertex
{
    float4 position [[ position ]];
    float2 texCoords;
};

struct CscParams
{
    half3x3 matrix;
    half3 offsets;
    half2 chromaOffset;
    half bitnessScaleFactor;
};

struct Packed444Params
{
    uint2 logicalSize;
    uint2 codedSize;
    uint tileW;
    uint tileH;
    uint splitX;
    uint isScaled;
    float lumaUnormScale;
};

constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
constexpr sampler s_ref(coord::normalized, address::clamp_to_edge, filter::nearest);

struct RefinementParams
{
    float yTol;
    uint enabled;
    uint2 logicalSize;
    uint isScaled;
    uint tileValidated;
};

kernel void cs_validate_tiles(constant CscParams &cscParams [[ buffer(0) ]],
                              constant RefinementParams &refParams [[ buffer(1) ]],
                              texture2d<half> luminancePlane [[ texture(0) ]],
                              texture2d<half> refTex [[ texture(1) ]],
                              texture2d<half> maskTex [[ texture(2) ]],
                              texture2d<half, access::write> effectiveMaskTex [[ texture(3) ]],
                              uint2 tgPos [[ threadgroup_position_in_grid ]],
                              uint2 tid [[ thread_position_in_threadgroup ]])
{
    if (tgPos.x >= effectiveMaskTex.get_width() || tgPos.y >= effectiveMaskTex.get_height()) {
        return;
    }

    const uint lane = tid.y * 8u + tid.x;
    const uint2 tileOrigin = tgPos * 64u;
    const uint2 tileEnd = min(tileOrigin + 64u, refParams.logicalSize);
    bool tileActive = (refParams.enabled != 0u) &&
                      (refParams.logicalSize.x > 0u) &&
                      (refParams.logicalSize.y > 0u) &&
                      (tgPos.x < maskTex.get_width()) &&
                      (tgPos.y < maskTex.get_height()) &&
                      (tileEnd.x > tileOrigin.x) &&
                      (tileEnd.y > tileOrigin.y);
    if (tileActive && maskTex.read(tgPos).r < 0.5h) {
        tileActive = false;
    }

    if (!tileActive) {
        if (lane == 0u) {
            effectiveMaskTex.write(half4(0.0h), tgPos);
        }
        return;
    }

    const uint2 tileSize = tileEnd - tileOrigin;
    const uint tilePixels = tileSize.x * tileSize.y;
    const uint2 maxTilePx = tileEnd - 1u;
    const float2 invLogical = 1.0f / float2(refParams.logicalSize);

    uint localMismatch = 0u;
    float localDiffSum = 0.0f;

    for (uint dy = tid.y; dy < tileSize.y; dy += 8u) {
        for (uint dx = tid.x; dx < tileSize.x; dx += 8u) {
            const uint2 px = tileOrigin + uint2(dx, dy);
            const float2 normCoord = (float2(px) + 0.5f) * invLogical;
            const float yVideo = float(luminancePlane.sample(s_ref, normCoord).r) * float(cscParams.bitnessScaleFactor);
            const float yRef = float(refTex.read(px).a);
            const float diff = abs(yVideo - yRef);
            localDiffSum += diff;

            if (diff > (24.0f / 255.0f)) {
                localMismatch += 1u;
            } else if (diff > (12.0f / 255.0f)) {
                float minY = yRef;
                float maxY = yRef;
                for (int oy = -1; oy <= 1; ++oy) {
                    const uint sy = clamp(uint(int(px.y) + oy), tileOrigin.y, maxTilePx.y);
                    for (int ox = -1; ox <= 1; ++ox) {
                        const uint sx = clamp(uint(int(px.x) + ox), tileOrigin.x, maxTilePx.x);
                        const float ny = float(refTex.read(uint2(sx, sy)).a);
                        minY = min(minY, ny);
                        maxY = max(maxY, ny);
                    }
                }
                if (yVideo < (minY - 8.0f / 255.0f) || yVideo > (maxY + 8.0f / 255.0f)) {
                    localMismatch += 1u;
                }
            }
        }
    }

    threadgroup uint tgMismatch[64];
    threadgroup float tgDiffSum[64];
    tgMismatch[lane] = localMismatch;
    tgDiffSum[lane] = localDiffSum;
    threadgroup_barrier(mem_flags::mem_threadgroup);

    for (uint stride = 32u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            tgMismatch[lane] += tgMismatch[lane + stride];
            tgDiffSum[lane] += tgDiffSum[lane + stride];
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }

    if (lane == 0u) {
        const float meanDiff = tgDiffSum[0] / float(tilePixels);
        const bool valid = (tgMismatch[0] <= 2u) && (meanDiff <= (6.0f / 255.0f));
        effectiveMaskTex.write(half4(valid ? 1.0h : 0.0h), tgPos);
    }
}

inline half3 sampleRefTap(texture2d<half> refTex,
                          texture2d<half> maskTex,
                          uint2 p,
                          uint2 maxMask,
                          half3 fallback)
{
    uint2 tc = min(p / 64u, maxMask);
    if (maskTex.read(tc).r < 0.5h) {
        return fallback;
    }
    return refTex.read(p).rgb;
}

inline half4 applyTileRefinement(Vertex v,
                                 half4 videoColor,
                                 constant CscParams &cscParams,
                                 constant RefinementParams &refParams,
                                 texture2d<half> luminancePlane,
                                 texture2d<half> refTex,
                                 texture2d<half> maskTex)
{
    if (refParams.enabled == 0u ||
        refParams.logicalSize.x == 0u ||
        refParams.logicalSize.y == 0u ||
        maskTex.get_width() == 0u ||
        maskTex.get_height() == 0u) {
        return videoColor;
    }

    float2 normCoord = clamp(v.texCoords, float2(0.0f), float2(0.999999f));
    uint2 maxLogical = refParams.logicalSize - 1u;
    uint2 maxMask = uint2(maskTex.get_width() - 1u, maskTex.get_height() - 1u);
    uint2 px = min(uint2(normCoord * float2(refParams.logicalSize)), maxLogical);
    uint2 tileCoord = min(px / 64u, maxMask);
    if (maskTex.read(tileCoord).r < 0.5h) {
        return videoColor;
    }

    half4 refSample = refTex.read(px);
    if (refParams.tileValidated == 0u) {
        float yVideo = float(luminancePlane.sample(s_ref, normCoord).r) * float(cscParams.bitnessScaleFactor);
        float yRef = float(refSample.a);
        if (abs(yVideo - yRef) > refParams.yTol) {
            return videoColor;
        }
    }

    if (refParams.isScaled == 0u) {
        return half4(refSample.rgb, 1.0h);
    }

    float2 pos = normCoord * float2(refParams.logicalSize) - 0.5f;
    float2 f0 = floor(pos);
    float2 frac = pos - f0;
    int2 maxCoord = int2(maxLogical);
    uint2 p0 = uint2(clamp(int2(f0), int2(0), maxCoord));
    uint2 p1 = uint2(clamp(int2(f0) + 1, int2(0), maxCoord));

    half3 c00 = sampleRefTap(refTex, maskTex, uint2(p0.x, p0.y), maxMask, refSample.rgb);
    half3 c10 = sampleRefTap(refTex, maskTex, uint2(p1.x, p0.y), maxMask, refSample.rgb);
    half3 c01 = sampleRefTap(refTex, maskTex, uint2(p0.x, p1.y), maxMask, refSample.rgb);
    half3 c11 = sampleRefTap(refTex, maskTex, uint2(p1.x, p1.y), maxMask, refSample.rgb);

    half3 top = mix(c00, c10, half(frac.x));
    half3 bot = mix(c01, c11, half(frac.x));
    return half4(mix(top, bot, half(frac.y)), 1.0h);
}

inline int floorDiv2(int v)
{
    return (v >= 0) ? (v >> 1) : -((-v + 1) >> 1);
}

inline half3 decodeYCoCgRA1(texture2d<float> lum,
                            constant Packed444Params &cfg,
                            uint2 p)
{
    float yVal = lum.read(uint2(p.x, p.y)).r;
    float coVal = lum.read(uint2(p.x, cfg.tileH + p.y)).r;
    uint2 cgCoord = (p.x < cfg.splitX)
        ? uint2(cfg.tileW + p.x, p.y)
        : uint2(cfg.tileW + (p.x - cfg.splitX), cfg.tileH + p.y);
    float cgVal = lum.read(cgCoord).r;

    int y10 = int(rint(yVal * cfg.lumaUnormScale));
    int co10 = int(rint(coVal * cfg.lumaUnormScale));
    int cg10 = int(rint(cgVal * cfg.lumaUnormScale));

    int y = clamp(int(rint(float(y10) * 0.25f)), 0, 255);
    int co = clamp(int(rint(float(co10 - 512) * 0.5f)), -255, 255);
    int cg = clamp(int(rint(float(cg10 - 512) * 0.5f)), -255, 255);

    int t = y - floorDiv2(cg);
    int g = clamp(cg + t, 0, 255);
    int b = clamp(t - floorDiv2(co), 0, 255);
    int r = clamp(b + co, 0, 255);
    return half3(r, g, b) / 255.0h;
}

vertex Vertex vs_draw(constant Vertex *vertices [[ buffer(0) ]], uint id [[ vertex_id ]])
{
    return vertices[id];
}

inline half4 sampleBiplanarColor(float2 texCoords,
                                 constant CscParams &cscParams,
                                 texture2d<half> luminancePlane,
                                 texture2d<half> chrominancePlane)
{
    float2 chromaOffset = float2(cscParams.chromaOffset) / float2(luminancePlane.get_width(),
                                                                  luminancePlane.get_height());
    half3 yuv = half3(luminancePlane.sample(s, texCoords).r,
                      chrominancePlane.sample(s, texCoords + chromaOffset).rg);
    yuv *= cscParams.bitnessScaleFactor;
    yuv -= cscParams.offsets;

    return half4(yuv * cscParams.matrix, 1.0h);
}

inline half4 sampleTriplanarColor(float2 texCoords,
                                  constant CscParams &cscParams,
                                  texture2d<half> luminancePlane,
                                  texture2d<half> chrominancePlaneU,
                                  texture2d<half> chrominancePlaneV)
{
    float2 chromaOffset = float2(cscParams.chromaOffset) / float2(luminancePlane.get_width(),
                                                                  luminancePlane.get_height());
    half3 yuv = half3(luminancePlane.sample(s, texCoords).r,
                      chrominancePlaneU.sample(s, texCoords + chromaOffset).r,
                      chrominancePlaneV.sample(s, texCoords + chromaOffset).r);
    yuv *= cscParams.bitnessScaleFactor;
    yuv -= cscParams.offsets;

    return half4(yuv * cscParams.matrix, 1.0h);
}

fragment half4 ps_draw_biplanar(Vertex v [[ stage_in ]],
                                constant CscParams &cscParams [[ buffer(0) ]],
                                texture2d<half> luminancePlane [[ texture(0) ]],
                                texture2d<half> chrominancePlane [[ texture(1) ]])
{
    return sampleBiplanarColor(v.texCoords, cscParams, luminancePlane, chrominancePlane);
}

fragment half4 ps_draw_biplanar_refined(Vertex v [[ stage_in ]],
                                        constant CscParams &cscParams [[ buffer(0) ]],
                                        constant RefinementParams &refParams [[ buffer(1) ]],
                                        texture2d<half> luminancePlane [[ texture(0) ]],
                                        texture2d<half> chrominancePlane [[ texture(1) ]],
                                        texture2d<half> refTex [[ texture(2) ]],
                                        texture2d<half> maskTex [[ texture(3) ]])
{
    half4 baseColor = sampleBiplanarColor(v.texCoords, cscParams, luminancePlane, chrominancePlane);
    return applyTileRefinement(v, baseColor, cscParams, refParams, luminancePlane, refTex, maskTex);
}

fragment half4 ps_draw_triplanar(Vertex v [[ stage_in ]],
                                 constant CscParams &cscParams [[ buffer(0) ]],
                                 texture2d<half> luminancePlane [[ texture(0) ]],
                                 texture2d<half> chrominancePlaneU [[ texture(1) ]],
                                 texture2d<half> chrominancePlaneV [[ texture(2) ]])
{
    return sampleTriplanarColor(v.texCoords, cscParams, luminancePlane, chrominancePlaneU, chrominancePlaneV);
}

fragment half4 ps_draw_triplanar_refined(Vertex v [[ stage_in ]],
                                         constant CscParams &cscParams [[ buffer(0) ]],
                                         constant RefinementParams &refParams [[ buffer(1) ]],
                                         texture2d<half> luminancePlane [[ texture(0) ]],
                                         texture2d<half> chrominancePlaneU [[ texture(1) ]],
                                         texture2d<half> chrominancePlaneV [[ texture(2) ]],
                                         texture2d<half> refTex [[ texture(3) ]],
                                         texture2d<half> maskTex [[ texture(4) ]])
{
    half4 baseColor = sampleTriplanarColor(v.texCoords, cscParams, luminancePlane, chrominancePlaneU, chrominancePlaneV);
    return applyTileRefinement(v, baseColor, cscParams, refParams, luminancePlane, refTex, maskTex);
}

fragment half4 ps_draw_rgb(Vertex v [[ stage_in ]],
                           texture2d<half> rgbTexture [[ texture(0) ]])
{
    return rgbTexture.sample(s, v.texCoords);
}

fragment half4 ps_draw_ycocgr_a1(Vertex v [[ stage_in ]],
                                 constant Packed444Params &cfg [[ buffer(0) ]],
                                 texture2d<float> luminancePlane [[ texture(0) ]])
{
    float2 pos = v.texCoords * float2(cfg.logicalSize) - 0.5f;
    int2 maxCoord = int2(cfg.logicalSize) - 1;

    if (cfg.isScaled == 0u) {
        int2 p = clamp(int2(rint(pos)), int2(0), maxCoord);
        return half4(decodeYCoCgRA1(luminancePlane, cfg, uint2(p)), 1.0h);
    }

    float2 f0 = floor(pos);
    float2 frac = pos - f0;
    int2 p0 = clamp(int2(f0), int2(0), maxCoord);
    int2 p1 = clamp(int2(f0) + 1, int2(0), maxCoord);

    half3 c00 = decodeYCoCgRA1(luminancePlane, cfg, uint2(p0.x, p0.y));
    half3 c10 = decodeYCoCgRA1(luminancePlane, cfg, uint2(p1.x, p0.y));
    half3 c01 = decodeYCoCgRA1(luminancePlane, cfg, uint2(p0.x, p1.y));
    half3 c11 = decodeYCoCgRA1(luminancePlane, cfg, uint2(p1.x, p1.y));

    half3 top = mix(c00, c10, half(frac.x));
    half3 bot = mix(c01, c11, half(frac.x));
    return half4(mix(top, bot, half(frac.y)), 1.0h);
}
