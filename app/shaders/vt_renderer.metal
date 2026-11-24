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

fragment half4 ps_draw_biplanar(Vertex v [[ stage_in ]],
                                constant CscParams &cscParams [[ buffer(0) ]],
                                texture2d<half> luminancePlane [[ texture(0) ]],
                                texture2d<half> chrominancePlane [[ texture(1) ]])
{
    float2 chromaOffset = float2(cscParams.chromaOffset) / float2(luminancePlane.get_width(),
                                                                  luminancePlane.get_height());
    half3 yuv = half3(luminancePlane.sample(s, v.texCoords).r,
                      chrominancePlane.sample(s, v.texCoords + chromaOffset).rg);
    yuv *= cscParams.bitnessScaleFactor;
    yuv -= cscParams.offsets;

    return half4(yuv * cscParams.matrix, 1.0h);
}

fragment half4 ps_draw_triplanar(Vertex v [[ stage_in ]],
                                 constant CscParams &cscParams [[ buffer(0) ]],
                                 texture2d<half> luminancePlane [[ texture(0) ]],
                                 texture2d<half> chrominancePlaneU [[ texture(1) ]],
                                 texture2d<half> chrominancePlaneV [[ texture(2) ]])
{
    float2 chromaOffset = float2(cscParams.chromaOffset) / float2(luminancePlane.get_width(),
                                                                  luminancePlane.get_height());
    half3 yuv = half3(luminancePlane.sample(s, v.texCoords).r,
                      chrominancePlaneU.sample(s, v.texCoords + chromaOffset).r,
                      chrominancePlaneV.sample(s, v.texCoords + chromaOffset).r);
    yuv *= cscParams.bitnessScaleFactor;
    yuv -= cscParams.offsets;

    return half4(yuv * cscParams.matrix, 1.0h);
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
