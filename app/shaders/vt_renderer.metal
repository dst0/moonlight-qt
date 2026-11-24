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
};

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
    uint2 px = min(uint2(normCoord * float2(refParams.logicalSize)), refParams.logicalSize - 1u);
    uint2 tileCoord = min(px / 64u, uint2(maskTex.get_width() - 1u, maskTex.get_height() - 1u));
    if (maskTex.read(tileCoord).r < 0.5h) {
        return videoColor;
    }

    half4 refSample = refTex.read(px);
    float yVideo = float(luminancePlane.sample(s_ref, normCoord).r) * float(cscParams.bitnessScaleFactor);
    float yRef = float(refSample.a);
    if (abs(yVideo - yRef) <= refParams.yTol) {
        return half4(refSample.rgb, 1.0h);
    }
    return videoColor;
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
