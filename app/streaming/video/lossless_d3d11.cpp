#include "lossless_d3d11.h"

#ifdef Q_OS_WIN32

#include "lossless_hash.h"
#include "streaming/session.h"
#include "streaming/streamutils.h"

#include <QElapsedTimer>
#include <QMutexLocker>

#include <SDL_syswm.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

struct D3DCopyRectParam {
    std::uint32_t src_x, src_y, dst_x, dst_y, w, h, pad0, pad1;
};

struct alignas(16) D3DDecodeParams {
    std::uint32_t width, height, copy_rect_count, dirty_tile_count;
    D3DCopyRectParam copy_rects[sds::lossless::MAX_COPY_RECTS];
};

struct D3DDirtyTileDesc {
    std::uint32_t tile_x, tile_y, tile_w, tile_h;
    std::uint32_t mode, payload_offset, payload_len, pad0;
};

constexpr const char* kHlslDecodeSource = R"HLSL(
struct CopyRectParam {
    uint src_x, src_y, dst_x, dst_y, w, h, pad0, pad1;
};

struct DirtyTileDesc {
    uint tile_x, tile_y, tile_w, tile_h;
    uint mode, payload_offset, payload_len, pad0;
};

cbuffer DecodeParams : register(b0) {
    uint width, height, copy_rect_count, dirty_tile_count;
    CopyRectParam copy_rects[4];
};

ByteAddressBuffer packet_bytes : register(t0);
StructuredBuffer<DirtyTileDesc> tile_descs : register(t1);
StructuredBuffer<uint> prev_bgra : register(t2);
StructuredBuffer<uint> crc_table : register(t3);

RWStructuredBuffer<uint> curr_bgra : register(u0);
RWStructuredBuffer<uint> row_crcs : register(u1);
RWTexture2D<unorm float4> out_tex : register(u2);

static const int RICE_ESCAPE_Q = 16;

int floor_div2_i(int v) { return (v >= 0) ? (v >> 1) : -((-v + 1) >> 1); }

void rgb8_to_ycocgr_i(uint r, uint g, uint b, out int out_y, out int out_co, out int out_cg) {
    int ri = (int)r, gi = (int)g, bi = (int)b;
    out_co = ri - bi;
    int t = bi + floor_div2_i(out_co);
    out_cg = gi - t;
    out_y = t + floor_div2_i(out_cg);
}

void ycocgr_to_rgb8_i(int y, int co, int cg, out uint out_r, out uint out_g, out uint out_b) {
    int t = y - floor_div2_i(cg);
    int g = cg + t;
    int b = t - floor_div2_i(co);
    int r = b + co;
    out_r = (uint)(r & 0xFF);
    out_g = (uint)(g & 0xFF);
    out_b = (uint)(b & 0xFF);
}

int med_predict_i(int a, int b, int c) {
    int mn = min(a, b), mx = max(a, b);
    if (c >= mx) return mn;
    if (c <= mn) return mx;
    return a + b - c;
}

int zigzag_decode_i(uint u) {
    return ((u & 1u) != 0u) ? -(int)((u >> 1) + 1u) : (int)(u >> 1);
}

uint pkt_byte(uint addr) {
    uint w = packet_bytes.Load(addr & ~3u);
    return (w >> ((addr & 3u) * 8u)) & 0xFFu;
}

groupshared uint gs_sy[1024];
groupshared uint gs_sco[1024];
groupshared uint gs_scg[1024];
groupshared int gs_y[1024];
groupshared int gs_co[1024];
groupshared int gs_cg[1024];
groupshared uint gs_ok;

bool read_bit_pkt(uint base, uint len, inout uint bit_pos, out uint out_bit) {
    if (bit_pos >= len * 8u) { out_bit = 0u; return false; }
    uint b = pkt_byte(base + (bit_pos >> 3));
    out_bit = (b >> (7u - (bit_pos & 7u))) & 1u;
    ++bit_pos;
    return true;
}

bool read_bits_pkt(uint base, uint len, inout uint bit_pos, int count, out uint out_val) {
    out_val = 0u;
    if (count == 0) return true;
    if (bit_pos + (uint)count > len * 8u) return false;
    uint v = 0u;
    for (int i = 0; i < count; ++i) {
        uint b = pkt_byte(base + (bit_pos >> 3));
        v = (v << 1) | ((b >> (7u - (bit_pos & 7u))) & 1u);
        ++bit_pos;
    }
    out_val = v;
    return true;
}

bool decode_rice_chan(uint base, uint len, uint tw, uint th, int raw_bits, uint ch) {
    uint bit_pos = 0u;
    for (uint y = 0u; y < th; ++y) {
        uint ro = y * tw;
        uint nz = 0u;
        if (!read_bit_pkt(base, len, bit_pos, nz)) return false;
        if (nz == 0u) {
            for (uint x = 0u; x < tw; ++x) {
                if (ch == 0u) gs_sy[ro + x] = 0u;
                else if (ch == 1u) gs_sco[ro + x] = 0u;
                else gs_scg[ro + x] = 0u;
            }
            continue;
        }
        uint k = 0u;
        if (!read_bits_pkt(base, len, bit_pos, 3, k)) return false;
        for (uint x = 0u; x < tw; ++x) {
            int q = 0;
            while (q < RICE_ESCAPE_Q) {
                uint b = 0u;
                if (!read_bit_pkt(base, len, bit_pos, b)) return false;
                if (b != 0u) break;
                ++q;
            }
            uint val = 0u;
            if (q < RICE_ESCAPE_Q) {
                uint rem = 0u;
                if (!read_bits_pkt(base, len, bit_pos, (int)k, rem)) return false;
                val = ((uint)q << k) | rem;
            } else {
                if (!read_bits_pkt(base, len, bit_pos, raw_bits, val)) return false;
            }
            if (ch == 0u) gs_sy[ro + x] = val;
            else if (ch == 1u) gs_sco[ro + x] = val;
            else gs_scg[ro + x] = val;
        }
    }
    return true;
}

[numthreads(16, 16, 1)]
void cs_copy_rects(uint3 gid : SV_DispatchThreadID) {
    if (gid.x >= width || gid.y >= height) return;
    uint sx = gid.x, sy = gid.y;
    for (uint i = 0u; i < copy_rect_count && i < 4u; ++i) {
        CopyRectParam r = copy_rects[i];
        if (gid.x >= r.dst_x && gid.x < r.dst_x + r.w && gid.y >= r.dst_y && gid.y < r.dst_y + r.h) {
            sx = r.src_x + (gid.x - r.dst_x);
            sy = r.src_y + (gid.y - r.dst_y);
        }
    }
    curr_bgra[gid.y * width + gid.x] = prev_bgra[sy * width + sx] | 0xFF000000u;
}

[numthreads(32, 1, 1)]
void cs_decode_tiles(uint3 tgid : SV_GroupID, uint lid : SV_GroupIndex) {
    if (tgid.x >= dirty_tile_count) return;
    DirtyTileDesc d = tile_descs[tgid.x];
    uint pixels = d.tile_w * d.tile_h;
    uint base = d.payload_offset;

    if (d.mode == 1u) {
        if (d.payload_len != 3u) return;
        uint px = (pkt_byte(base + 2u)) | (pkt_byte(base + 1u) << 8) | (pkt_byte(base) << 16) | 0xFF000000u;
        for (uint i = lid; i < pixels; i += 32u) {
            uint x = i % d.tile_w, y = i / d.tile_w;
            curr_bgra[(d.tile_y + y) * width + (d.tile_x + x)] = px;
        }
        return;
    }
    if (d.mode == 5u) {
        if (d.payload_len != pixels * 3u) return;
        for (uint i = lid; i < pixels; i += 32u) {
            uint x = i % d.tile_w, y = i / d.tile_w;
            uint r = pkt_byte(base + i * 3u), g = pkt_byte(base + i * 3u + 1u), b = pkt_byte(base + i * 3u + 2u);
            curr_bgra[(d.tile_y + y) * width + (d.tile_x + x)] = b | (g << 8) | (r << 16) | 0xFF000000u;
        }
        return;
    }
    if (d.mode == 2u) {
        if (d.payload_len < 1u) return;
        uint count = pkt_byte(base);
        if (count < 2u || count > 16u) return;
        int bpp = (count <= 2u) ? 1 : (count <= 4u ? 2 : 4);
        uint pal_end = 1u + count * 3u;
        if (d.payload_len < pal_end) return;
        for (uint i = lid; i < pixels; i += 32u) {
            uint bi = i * (uint)bpp;
            uint by = pal_end + (bi >> 3);
            if (by >= d.payload_len) break;
            uint sh = 8u - (uint)bpp - (bi & 7u);
            uint idx = (pkt_byte(base + by) >> sh) & ((1u << (uint)bpp) - 1u);
            if (idx >= count) idx = 0u;
            uint x = i % d.tile_w, y = i / d.tile_w;
            uint r = pkt_byte(base + 1u + idx * 3u), g = pkt_byte(base + 2u + idx * 3u), b = pkt_byte(base + 3u + idx * 3u);
            curr_bgra[(d.tile_y + y) * width + (d.tile_x + x)] = b | (g << 8) | (r << 16) | 0xFF000000u;
        }
        return;
    }
    if (d.mode != 3u && d.mode != 4u) return;

    if (lid == 0u) {
        gs_ok = 0u;
        if (d.payload_len >= 6u) {
            uint sy = pkt_byte(base) | (pkt_byte(base + 1u) << 8);
            uint sco = pkt_byte(base + 2u) | (pkt_byte(base + 3u) << 8);
            uint scg = pkt_byte(base + 4u) | (pkt_byte(base + 5u) << 8);
            if (6u + sy + sco + scg <= d.payload_len &&
                decode_rice_chan(base + 6u, sy, d.tile_w, d.tile_h, 8, 0u) &&
                decode_rice_chan(base + 6u + sy, sco, d.tile_w, d.tile_h, 10, 1u) &&
                decode_rice_chan(base + 6u + sy + sco, scg, d.tile_w, d.tile_h, 10, 2u)) {
                gs_ok = 1u;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gs_ok == 0u) return;

    if (d.mode == 3u) {
        if (lid == 0u) {
            for (uint y = 0u; y < d.tile_h; ++y) {
                for (uint x = 0u; x < d.tile_w; ++x) {
                    uint i = y * d.tile_w + x;
                    int py = med_predict_i((x > 0u) ? gs_y[i - 1u] : 0, (y > 0u) ? gs_y[i - d.tile_w] : 0,
                                           (x > 0u && y > 0u) ? gs_y[i - d.tile_w - 1u] : 0);
                    gs_y[i] = (py + zigzag_decode_i(gs_sy[i])) & 0xFF;
                    int pco = med_predict_i((x > 0u) ? gs_co[i - 1u] : 0, (y > 0u) ? gs_co[i - d.tile_w] : 0,
                                            (x > 0u && y > 0u) ? gs_co[i - d.tile_w - 1u] : 0);
                    gs_co[i] = pco + zigzag_decode_i(gs_sco[i]);
                    int pcg = med_predict_i((x > 0u) ? gs_cg[i - 1u] : 0, (y > 0u) ? gs_cg[i - d.tile_w] : 0,
                                            (x > 0u && y > 0u) ? gs_cg[i - d.tile_w - 1u] : 0);
                    gs_cg[i] = pcg + zigzag_decode_i(gs_scg[i]);
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();
        for (uint i = lid; i < pixels; i += 32u) {
            uint x = i % d.tile_w, y = i / d.tile_w;
            uint r, g, b;
            ycocgr_to_rgb8_i(gs_y[i], gs_co[i], gs_cg[i], r, g, b);
            curr_bgra[(d.tile_y + y) * width + (d.tile_x + x)] = b | (g << 8) | (r << 16) | 0xFF000000u;
        }
    } else {
        for (uint i = lid; i < pixels; i += 32u) {
            uint x = i % d.tile_w, y = i / d.tile_w;
            uint addr = (d.tile_y + y) * width + (d.tile_x + x);
            uint px = curr_bgra[addr];
            int py, pco, pcg;
            rgb8_to_ycocgr_i((px >> 16) & 0xFFu, (px >> 8) & 0xFFu, px & 0xFFu, py, pco, pcg);
            int yv = (py + zigzag_decode_i(gs_sy[i])) & 0xFF;
            int cov = pco + zigzag_decode_i(gs_sco[i]);
            int cgv = pcg + zigzag_decode_i(gs_scg[i]);
            uint r, g, b;
            ycocgr_to_rgb8_i(yv, cov, cgv, r, g, b);
            curr_bgra[addr] = b | (g << 8) | (r << 16) | 0xFF000000u;
        }
    }
}

[numthreads(64, 1, 1)]
void cs_crc32c_rows(uint3 gid : SV_DispatchThreadID) {
    uint y = gid.x;
    if (y >= height) return;
    uint crc = 0xFFFFFFFFu;
    uint base = y * width;
    for (uint x = 0u; x < width; ++x) {
        uint px = curr_bgra[base + x];
        uint b = px & 0xFFu, g = (px >> 8) & 0xFFu, r = (px >> 16) & 0xFFu;
        crc = (crc >> 8) ^ crc_table[(crc ^ r) & 0xFFu];
        crc = (crc >> 8) ^ crc_table[(crc ^ g) & 0xFFu];
        crc = (crc >> 8) ^ crc_table[(crc ^ b) & 0xFFu];
    }
    row_crcs[y] = crc ^ 0xFFFFFFFFu;
}

[numthreads(16, 16, 1)]
void cs_buffer_to_tex(uint3 gid : SV_DispatchThreadID) {
    if (gid.x >= width || gid.y >= height) return;
    uint px = curr_bgra[gid.y * width + gid.x];
    float b = (float)(px & 0xFFu) / 255.0;
    float g = (float)((px >> 8) & 0xFFu) / 255.0;
    float r = (float)((px >> 16) & 0xFFu) / 255.0;
    out_tex[gid.xy] = float4(r, g, b, 1.0);
}
)HLSL";

constexpr const char* kHlslPresentSource = R"HLSL(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};
cbuffer QuadCB : register(b0) {
    float4 rect; // left, top, right, bottom in NDC
};
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

VSOut vs_quad(uint id : SV_VertexID) {
    float2 uvs[4] = { float2(0, 1), float2(1, 1), float2(0, 0), float2(1, 0) };
    float2 pos[4] = {
        float2(rect.x, rect.w),
        float2(rect.z, rect.w),
        float2(rect.x, rect.y),
        float2(rect.z, rect.y)
    };
    VSOut o;
    o.pos = float4(pos[id], 0.0, 1.0);
    o.uv = uvs[id];
    return o;
}

float4 ps_video(VSOut i) : SV_TARGET {
    float4 c = tex0.Sample(samp0, i.uv);
    return float4(c.rgb, 1.0);
}

float4 ps_overlay(VSOut i) : SV_TARGET {
    return tex0.Sample(samp0, i.uv);
}
)HLSL";

} // namespace

struct LosslessD3D11Renderer::Impl
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11RenderTargetView> rtv;

    ComPtr<ID3D11ComputeShader> csCopyRects;
    ComPtr<ID3D11ComputeShader> csDecodeTiles;
    ComPtr<ID3D11ComputeShader> csCrc32c;
    ComPtr<ID3D11ComputeShader> csBufToTex;

    ComPtr<ID3D11VertexShader> vsQuad;
    ComPtr<ID3D11PixelShader> psVideo;
    ComPtr<ID3D11PixelShader> psOverlay;
    ComPtr<ID3D11SamplerState> sampNearest;
    ComPtr<ID3D11SamplerState> sampLinear;
    ComPtr<ID3D11BlendState> blendOverlay;

    ComPtr<ID3D11Buffer> cbParams;
    ComPtr<ID3D11Buffer> cbQuad;
    ComPtr<ID3D11Buffer> crcTableBuf;
    ComPtr<ID3D11ShaderResourceView> crcTableSrv;

    ComPtr<ID3D11Buffer> prevBuf;
    ComPtr<ID3D11ShaderResourceView> prevSrv;
    ComPtr<ID3D11Buffer> currBuf;
    ComPtr<ID3D11UnorderedAccessView> currUav;
    ComPtr<ID3D11Buffer> packetBuf;
    ComPtr<ID3D11ShaderResourceView> packetSrv;
    ComPtr<ID3D11Buffer> tileDescBuf;
    ComPtr<ID3D11ShaderResourceView> tileDescSrv;
    ComPtr<ID3D11Buffer> rowCrcBuf;
    ComPtr<ID3D11UnorderedAccessView> rowCrcUav;
    ComPtr<ID3D11Buffer> rowCrcStaging;
    ComPtr<ID3D11Buffer> blake3Staging;

    ComPtr<ID3D11Texture2D> videoTex;
    ComPtr<ID3D11UnorderedAccessView> videoTexUav;
    ComPtr<ID3D11ShaderResourceView> videoTexSrv;
    ComPtr<ID3D11Texture2D> overlayTex[Overlay::OverlayMax];
    ComPtr<ID3D11ShaderResourceView> overlaySrv[Overlay::OverlayMax];
    SDL_Rect overlayRect[Overlay::OverlayMax] {};

    std::size_t capFrameBytes = 0;
    std::size_t capPacketBytes = 0;
    std::size_t capDescBytes = 0;
    std::size_t capRowBytes = 0;
    int texWidth = 0;
    int texHeight = 0;
    int backWidth = 0;
    int backHeight = 0;
    bool hasPrev = false;
    bool hasFrame = false;
    bool enableVsync = true;
};

LosslessD3D11Renderer::~LosslessD3D11Renderer()
{
    delete m_Impl;
    m_Impl = nullptr;
}

bool LosslessD3D11Renderer::initialize(SDL_Window* window,
                                       int width,
                                       int height,
                                       bool enableVsync,
                                       Overlay::OverlayManager* overlayManager,
                                       bool preferGpuCompute)
{
    m_Window = window;
    m_OverlayManager = overlayManager;

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS) {
        return false;
    }

    m_Impl = new Impl();
    m_Impl->enableVsync = enableVsync;

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL achieved {};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 levels, 1, D3D11_SDK_VERSION,
                                 &m_Impl->device, &achieved, &m_Impl->ctx))) {
        delete m_Impl;
        m_Impl = nullptr;
        return false;
    }

    // Check D3D11 runtime thread protection
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m_Impl->device.As(&mt))) {
        mt->SetMultithreadProtected(TRUE);
    }

    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(m_Impl->device.As(&dxgiDev)) ||
        FAILED(dxgiDev->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        delete m_Impl;
        m_Impl = nullptr;
        return false;
    }

    int outW = 0, outH = 0;
    SDL_GetWindowSizeInPixels(window, &outW, &outH);
    DXGI_SWAP_CHAIN_DESC1 scd {};
    scd.Width = std::max(outW, 1);
    scd.Height = std::max(outH, 1);
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(m_Impl->device.Get(), info.info.win.window, &scd, nullptr, nullptr, &m_Impl->swapChain))) {
        delete m_Impl;
        m_Impl = nullptr;
        return false;
    }
    m_Impl->backWidth = static_cast<int>(scd.Width);
    m_Impl->backHeight = static_cast<int>(scd.Height);

    ComPtr<ID3D11Texture2D> backBuf;
    if (FAILED(m_Impl->swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf))) ||
        FAILED(m_Impl->device->CreateRenderTargetView(backBuf.Get(), nullptr, &m_Impl->rtv))) {
        delete m_Impl;
        m_Impl = nullptr;
        return false;
    }

    auto compileCS = [&](const char* entry, ComPtr<ID3D11ComputeShader>& out) -> bool {
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3DCompile(kHlslDecodeSource, std::strlen(kHlslDecodeSource), "sds_lossless_decode.hlsl", nullptr, nullptr,
                              entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "LosslessD3D11Renderer: D3DCompile(%s) failed: %s",
                         entry,
                         err ? static_cast<const char*>(err->GetBufferPointer()) : "unknown");
            return false;
        }
        return SUCCEEDED(m_Impl->device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out));
    };

    if (!compileCS("cs_copy_rects", m_Impl->csCopyRects) ||
        !compileCS("cs_decode_tiles", m_Impl->csDecodeTiles) ||
        !compileCS("cs_crc32c_rows", m_Impl->csCrc32c) ||
        !compileCS("cs_buffer_to_tex", m_Impl->csBufToTex)) {
        delete m_Impl;
        m_Impl = nullptr;
        return false;
    }

    ComPtr<ID3DBlob> vsb, psb, psob;
    if (FAILED(D3DCompile(kHlslPresentSource, std::strlen(kHlslPresentSource), "present.hlsl", nullptr, nullptr, "vs_quad", "vs_5_0", 0, 0, &vsb, nullptr)) ||
        FAILED(D3DCompile(kHlslPresentSource, std::strlen(kHlslPresentSource), "present.hlsl", nullptr, nullptr, "ps_video", "ps_5_0", 0, 0, &psb, nullptr)) ||
        FAILED(D3DCompile(kHlslPresentSource, std::strlen(kHlslPresentSource), "present.hlsl", nullptr, nullptr, "ps_overlay", "ps_5_0", 0, 0, &psob, nullptr)) ||
        FAILED(m_Impl->device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_Impl->vsQuad)) ||
        FAILED(m_Impl->device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_Impl->psVideo)) ||
        FAILED(m_Impl->device->CreatePixelShader(psob->GetBufferPointer(), psob->GetBufferSize(), nullptr, &m_Impl->psOverlay))) {
        delete m_Impl;
        m_Impl = nullptr;
        return false;
    }

    D3D11_SAMPLER_DESC sd {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    m_Impl->device->CreateSamplerState(&sd, &m_Impl->sampNearest);
    sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    m_Impl->device->CreateSamplerState(&sd, &m_Impl->sampLinear);

    D3D11_BLEND_DESC bld {};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_Impl->device->CreateBlendState(&bld, &m_Impl->blendOverlay);

    D3D11_BUFFER_DESC cbd {};
    cbd.ByteWidth = sizeof(D3DDecodeParams);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_Impl->device->CreateBuffer(&cbd, nullptr, &m_Impl->cbParams);
    cbd.ByteWidth = 16;
    m_Impl->device->CreateBuffer(&cbd, nullptr, &m_Impl->cbQuad);

    std::array<std::uint32_t, 256> tbl {};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? ((c >> 1) ^ 0x82F63B78u) : (c >> 1);
        tbl[i] = c;
    }
    D3D11_BUFFER_DESC tbd {};
    tbd.ByteWidth = sizeof(tbl);
    tbd.Usage = D3D11_USAGE_IMMUTABLE;
    tbd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    tbd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    tbd.StructureByteStride = sizeof(std::uint32_t);
    D3D11_SUBRESOURCE_DATA tid {};
    tid.pSysMem = tbl.data();
    m_Impl->device->CreateBuffer(&tbd, &tid, &m_Impl->crcTableBuf);
    D3D11_SHADER_RESOURCE_VIEW_DESC tsrvd {};
    tsrvd.Format = DXGI_FORMAT_UNKNOWN;
    tsrvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    tsrvd.Buffer.NumElements = 256;
    m_Impl->device->CreateShaderResourceView(m_Impl->crcTableBuf.Get(), &tsrvd, &m_Impl->crcTableSrv);

    (void)width;
    (void)height;
    m_GpuReady = true;
    m_UsingGpuCompute = preferGpuCompute;
    return true;
}

bool LosslessD3D11Renderer::presentPacket(const std::uint8_t* packet,
                                          std::size_t len,
                                          const sds::lossless::frame_header_t& hdr,
                                          bool useGpuCompute,
                                          bool verifyHash,
                                          bool& outHashOk,
                                          std::uint64_t& outDecodeUs,
                                          std::uint64_t& outHashUs)
{
    if (!m_Impl) return false;
    QElapsedTimer timer;
    timer.start();
    QMutexLocker lk(&m_FrameMutex);

    const int w = static_cast<int>(hdr.width);
    const int h = static_cast<int>(hdr.height);
    const std::size_t frameBytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;

    auto ensureTex = [&]() -> bool {
        if (m_Impl->texWidth == w && m_Impl->texHeight == h && m_Impl->videoTex) return true;
        D3D11_TEXTURE2D_DESC td {};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(m_Impl->device->CreateTexture2D(&td, nullptr, &m_Impl->videoTex))) return false;
        if (FAILED(m_Impl->device->CreateShaderResourceView(m_Impl->videoTex.Get(), nullptr, &m_Impl->videoTexSrv))) return false;
        if (FAILED(m_Impl->device->CreateUnorderedAccessView(m_Impl->videoTex.Get(), nullptr, &m_Impl->videoTexUav))) return false;
        m_Impl->texWidth = w;
        m_Impl->texHeight = h;
        return true;
    };

    if (!useGpuCompute) {
        m_UsingGpuCompute = false;
        m_Impl->hasPrev = false;
        sds::lossless::decode_stats_t stats {};
        if (!m_CpuDecoder.decode_frame(packet, len, verifyHash, nullptr, &stats)) {
            outHashOk = false;
            outDecodeUs = static_cast<std::uint64_t>(timer.nsecsElapsed() / 1000);
            outHashUs = 0;
            return false;
        }
        outHashOk = (!verifyHash || hdr.hash_algo == sds::lossless::hash_algo_e::none) ? true : stats.hash_matched;
        outDecodeUs = static_cast<std::uint64_t>(timer.nsecsElapsed() / 1000);
        outHashUs = 0;
        if (!ensureTex()) return false;
        m_Impl->ctx->UpdateSubresource(m_Impl->videoTex.Get(), 0, nullptr, m_CpuDecoder.current_bgra(),
                                       static_cast<UINT>(m_CpuDecoder.stride()), 0);
        m_Impl->hasFrame = true;
        return true;
    }

    m_UsingGpuCompute = true;
    sds::lossless::parsed_frame_t parsed;
    if (!sds::lossless::parse_frame_packet(packet, len, parsed)) {
        outHashOk = false;
        return false;
    }
    const bool isKeyframe = (hdr.flags & sds::lossless::FLAG_KEYFRAME) != 0;
    const bool dimsChanged = (w != m_Impl->texWidth || h != m_Impl->texHeight);
    if (!isKeyframe && (!m_Impl->hasPrev || dimsChanged)) {
        outHashOk = false;
        return false;
    }
    std::vector<sds::lossless::gpu_tile_desc_t> descs;
    if (!sds::lossless::build_dirty_tile_descs(parsed, packet, descs)) {
        outHashOk = false;
        return false;
    }
    if (!ensureTex()) return false;

    // Ensure GPU structured/raw buffers
    if (frameBytes > m_Impl->capFrameBytes) {
        m_Impl->capFrameBytes = 0;
        m_Impl->hasPrev = false;
        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth = static_cast<UINT>(frameBytes);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 4;
        if (FAILED(m_Impl->device->CreateBuffer(&bd, nullptr, &m_Impl->prevBuf)) ||
            FAILED(m_Impl->device->CreateBuffer(&bd, nullptr, &m_Impl->currBuf)) ||
            FAILED(m_Impl->device->CreateShaderResourceView(m_Impl->prevBuf.Get(), nullptr, &m_Impl->prevSrv)) ||
            FAILED(m_Impl->device->CreateUnorderedAccessView(m_Impl->currBuf.Get(), nullptr, &m_Impl->currUav))) {
            return false;
        }
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.MiscFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_Impl->device->CreateBuffer(&bd, nullptr, &m_Impl->blake3Staging))) {
            return false;
        }
        m_Impl->capFrameBytes = frameBytes;
    }
    const std::size_t alignedPkt = (len + 15u) & ~std::size_t(15u);
    if (alignedPkt > m_Impl->capPacketBytes) {
        m_Impl->capPacketBytes = 0;
        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth = static_cast<UINT>(alignedPkt);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(m_Impl->device->CreateBuffer(&bd, nullptr, &m_Impl->packetBuf))) {
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd {};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.NumElements = static_cast<UINT>(alignedPkt / 4u);
        sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        if (FAILED(m_Impl->device->CreateShaderResourceView(m_Impl->packetBuf.Get(), &sd, &m_Impl->packetSrv))) {
            return false;
        }
        m_Impl->capPacketBytes = alignedPkt;
    }
    const std::size_t descBytes = std::max<std::size_t>(descs.size(), 1) * sizeof(D3DDirtyTileDesc);
    if (descBytes > m_Impl->capDescBytes) {
        m_Impl->capDescBytes = 0;
        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth = static_cast<UINT>(descBytes);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(D3DDirtyTileDesc);
        if (FAILED(m_Impl->device->CreateBuffer(&bd, nullptr, &m_Impl->tileDescBuf)) ||
            FAILED(m_Impl->device->CreateShaderResourceView(m_Impl->tileDescBuf.Get(), nullptr, &m_Impl->tileDescSrv))) {
            return false;
        }
        m_Impl->capDescBytes = descBytes;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(h) * sizeof(std::uint32_t);
    if (rowBytes > m_Impl->capRowBytes) {
        m_Impl->capRowBytes = 0;
        D3D11_BUFFER_DESC bd {};
        bd.ByteWidth = static_cast<UINT>(rowBytes);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = sizeof(std::uint32_t);
        if (FAILED(m_Impl->device->CreateBuffer(&bd, nullptr, &m_Impl->rowCrcBuf)) ||
            FAILED(m_Impl->device->CreateUnorderedAccessView(m_Impl->rowCrcBuf.Get(), nullptr, &m_Impl->rowCrcUav))) {
            return false;
        }
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.MiscFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(m_Impl->device->CreateBuffer(&bd, nullptr, &m_Impl->rowCrcStaging))) {
            return false;
        }
        m_Impl->capRowBytes = rowBytes;
    }

    D3D11_MAPPED_SUBRESOURCE ms {};
    if (FAILED(m_Impl->ctx->Map(m_Impl->packetBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        return false;
    }
    std::memcpy(ms.pData, packet, len);
    m_Impl->ctx->Unmap(m_Impl->packetBuf.Get(), 0);

    if (!descs.empty()) {
        if (FAILED(m_Impl->ctx->Map(m_Impl->tileDescBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            return false;
        }
        std::memcpy(ms.pData, descs.data(), descs.size() * sizeof(sds::lossless::gpu_tile_desc_t));
        m_Impl->ctx->Unmap(m_Impl->tileDescBuf.Get(), 0);
    }

    if (FAILED(m_Impl->ctx->Map(m_Impl->cbParams.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        return false;
    }
    auto* cp = static_cast<D3DDecodeParams*>(ms.pData);
    std::memset(cp, 0, sizeof(*cp));
    cp->width = hdr.width;
    cp->height = hdr.height;
    cp->copy_rect_count = static_cast<std::uint32_t>(std::min<std::size_t>(parsed.copy_rects.size(), sds::lossless::MAX_COPY_RECTS));
    cp->dirty_tile_count = static_cast<std::uint32_t>(descs.size());
    for (std::uint32_t i = 0; i < cp->copy_rect_count; ++i) {
        cp->copy_rects[i] = { parsed.copy_rects[i].src_x, parsed.copy_rects[i].src_y,
                              parsed.copy_rects[i].dst_x, parsed.copy_rects[i].dst_y,
                              parsed.copy_rects[i].w, parsed.copy_rects[i].h, 0, 0 };
    }
    m_Impl->ctx->Unmap(m_Impl->cbParams.Get(), 0);

    ID3D11ShaderResourceView* srvs[4] = { m_Impl->packetSrv.Get(), m_Impl->tileDescSrv.Get(), m_Impl->prevSrv.Get(), m_Impl->crcTableSrv.Get() };
    ID3D11UnorderedAccessView* uavs[3] = { m_Impl->currUav.Get(), m_Impl->rowCrcUav.Get(), m_Impl->videoTexUav.Get() };
    m_Impl->ctx->CSSetConstantBuffers(0, 1, m_Impl->cbParams.GetAddressOf());
    m_Impl->ctx->CSSetShaderResources(0, 4, srvs);
    m_Impl->ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);

    if (!isKeyframe) {
        m_Impl->ctx->CSSetShader(m_Impl->csCopyRects.Get(), nullptr, 0);
        m_Impl->ctx->Dispatch((static_cast<UINT>(w) + 15u) / 16u, (static_cast<UINT>(h) + 15u) / 16u, 1u);
    }
    if (!descs.empty()) {
        m_Impl->ctx->CSSetShader(m_Impl->csDecodeTiles.Get(), nullptr, 0);
        m_Impl->ctx->Dispatch(static_cast<UINT>(descs.size()), 1u, 1u);
    }
    if (verifyHash && hdr.hash_algo == sds::lossless::hash_algo_e::crc32c) {
        m_Impl->ctx->CSSetShader(m_Impl->csCrc32c.Get(), nullptr, 0);
        m_Impl->ctx->Dispatch((static_cast<UINT>(h) + 63u) / 64u, 1u, 1u);
    }
    m_Impl->ctx->CSSetShader(m_Impl->csBufToTex.Get(), nullptr, 0);
    m_Impl->ctx->Dispatch((static_cast<UINT>(w) + 15u) / 16u, (static_cast<UINT>(h) + 15u) / 16u, 1u);

    ID3D11UnorderedAccessView* nullU[3] = { nullptr, nullptr, nullptr };
    ID3D11ShaderResourceView* nullS[4] = { nullptr, nullptr, nullptr, nullptr };
    m_Impl->ctx->CSSetUnorderedAccessViews(0, 3, nullU, nullptr);
    m_Impl->ctx->CSSetShaderResources(0, 4, nullS);

    m_Impl->ctx->CopyResource(m_Impl->prevBuf.Get(), m_Impl->currBuf.Get());

    QElapsedTimer htimer;
    htimer.start();
    outHashOk = true;
    if (verifyHash && hdr.hash_algo == sds::lossless::hash_algo_e::crc32c) {
        m_Impl->ctx->CopyResource(m_Impl->rowCrcStaging.Get(), m_Impl->rowCrcBuf.Get());
        D3D11_MAPPED_SUBRESOURCE rms {};
        if (SUCCEEDED(m_Impl->ctx->Map(m_Impl->rowCrcStaging.Get(), 0, D3D11_MAP_READ, 0, &rms))) {
            const auto* rowCrcs = static_cast<const std::uint32_t*>(rms.pData);
            std::uint32_t combined = rowCrcs[0];
            for (int y = 1; y < h; ++y) {
                combined = sds::lossless::crc32c_combine(combined, rowCrcs[y], static_cast<std::uint64_t>(w) * 3ULL);
            }
            m_Impl->ctx->Unmap(m_Impl->rowCrcStaging.Get(), 0);
            const std::uint32_t expected =
                static_cast<std::uint32_t>(hdr.frame_hash[0]) |
                (static_cast<std::uint32_t>(hdr.frame_hash[1]) << 8) |
                (static_cast<std::uint32_t>(hdr.frame_hash[2]) << 16) |
                (static_cast<std::uint32_t>(hdr.frame_hash[3]) << 24);
            outHashOk = (combined == expected);
        } else {
            outHashOk = false;
        }
    } else if (verifyHash && hdr.hash_algo == sds::lossless::hash_algo_e::blake3) {
        m_Impl->ctx->CopyResource(m_Impl->blake3Staging.Get(), m_Impl->currBuf.Get());
        D3D11_MAPPED_SUBRESOURCE bms {};
        if (SUCCEEDED(m_Impl->ctx->Map(m_Impl->blake3Staging.Get(), 0, D3D11_MAP_READ, 0, &bms))) {
            const auto b3 = sds::lossless::blake3_rgb24_from_bgra(
                static_cast<const std::uint8_t*>(bms.pData), w * 4, w, h);
            m_Impl->ctx->Unmap(m_Impl->blake3Staging.Get(), 0);
            outHashOk = (b3 == hdr.frame_hash);
        } else {
            outHashOk = false;
        }
    }
    outHashUs = static_cast<std::uint64_t>(htimer.nsecsElapsed() / 1000);
    outDecodeUs = static_cast<std::uint64_t>(timer.nsecsElapsed() / 1000);
    m_Impl->hasPrev = true;
    m_Impl->hasFrame = true;
    return true;
}

void LosslessD3D11Renderer::renderOnMainThread()
{
    if (!m_Impl || !m_Impl->swapChain) return;

    int outW = 0, outH = 0;
    SDL_GetWindowSizeInPixels(m_Window, &outW, &outH);

    const int overlayRefW = (outW > 0) ? outW : m_Impl->backWidth;
    const int overlayRefH = (outH > 0) ? outH : m_Impl->backHeight;

    for (int i = 0; i < Overlay::OverlayMax; ++i) {
        const auto type = static_cast<Overlay::OverlayType>(i);
        if (!m_OverlayDirty[i]) continue;
        m_OverlayDirty[i] = false;
        m_Impl->overlayTex[i].Reset();
        m_Impl->overlaySrv[i].Reset();
        if (!m_OverlayManager || !m_OverlayManager->isOverlayEnabled(type)) continue;
        SDL_Surface* surf = m_OverlayManager->getUpdatedOverlaySurface(type);
        if (!surf) continue;
        D3D11_TEXTURE2D_DESC td {};
        td.Width = static_cast<UINT>(surf->w);
        td.Height = static_cast<UINT>(surf->h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init { surf->pixels, static_cast<UINT>(surf->pitch), 0 };
        if (SUCCEEDED(m_Impl->device->CreateTexture2D(&td, &init, &m_Impl->overlayTex[i]))) {
            m_Impl->device->CreateShaderResourceView(m_Impl->overlayTex[i].Get(), nullptr, &m_Impl->overlaySrv[i]);
        }
        m_Impl->overlayRect[i] = (type == Overlay::OverlayStatusUpdate)
            ? SDL_Rect { overlayRefW - surf->w - 10, overlayRefH - surf->h - 10, surf->w, surf->h }
            : SDL_Rect { 10, 10, surf->w, surf->h };
        SDL_FreeSurface(surf);
    }

    QMutexLocker lk(&m_FrameMutex);

    if (outW > 0 && outH > 0 && (outW != m_Impl->backWidth || outH != m_Impl->backHeight)) {
        m_Impl->rtv.Reset();
        m_Impl->swapChain->ResizeBuffers(0, static_cast<UINT>(outW), static_cast<UINT>(outH), DXGI_FORMAT_UNKNOWN, 0);
        ComPtr<ID3D11Texture2D> backBuf;
        if (SUCCEEDED(m_Impl->swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf)))) {
            m_Impl->device->CreateRenderTargetView(backBuf.Get(), nullptr, &m_Impl->rtv);
        }
        m_Impl->backWidth = outW;
        m_Impl->backHeight = outH;
    }

    const float clear[4] = { 0.f, 0.f, 0.f, 1.f };
    m_Impl->ctx->OMSetRenderTargets(1, m_Impl->rtv.GetAddressOf(), nullptr);
    m_Impl->ctx->ClearRenderTargetView(m_Impl->rtv.Get(), clear);

    D3D11_VIEWPORT vp { 0.f, 0.f, static_cast<float>(m_Impl->backWidth), static_cast<float>(m_Impl->backHeight), 0.f, 1.f };
    m_Impl->ctx->RSSetViewports(1, &vp);
    m_Impl->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_Impl->ctx->IASetInputLayout(nullptr);
    m_Impl->ctx->VSSetShader(m_Impl->vsQuad.Get(), nullptr, 0);

    auto drawQuad = [&](const SDL_Rect& r, ID3D11ShaderResourceView* srv, ID3D11PixelShader* ps, ID3D11SamplerState* samp, bool blend) {
        D3D11_MAPPED_SUBRESOURCE qms {};
        if (SUCCEEDED(m_Impl->ctx->Map(m_Impl->cbQuad.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &qms))) {
            auto* f = static_cast<float*>(qms.pData);
            f[0] = (2.f * r.x) / m_Impl->backWidth - 1.f;
            f[1] = 1.f - (2.f * r.y) / m_Impl->backHeight;
            f[2] = (2.f * (r.x + r.w)) / m_Impl->backWidth - 1.f;
            f[3] = 1.f - (2.f * (r.y + r.h)) / m_Impl->backHeight;
            m_Impl->ctx->Unmap(m_Impl->cbQuad.Get(), 0);
        }
        m_Impl->ctx->VSSetConstantBuffers(0, 1, m_Impl->cbQuad.GetAddressOf());
        m_Impl->ctx->PSSetShader(ps, nullptr, 0);
        m_Impl->ctx->PSSetShaderResources(0, 1, &srv);
        m_Impl->ctx->PSSetSamplers(0, 1, &samp);
        m_Impl->ctx->OMSetBlendState(blend ? m_Impl->blendOverlay.Get() : nullptr, nullptr, 0xFFFFFFFFu);
        m_Impl->ctx->Draw(4, 0);
    };

    if (m_Impl->hasFrame && m_Impl->videoTexSrv && m_Impl->backWidth > 0 && m_Impl->backHeight > 0) {
        SDL_Rect src { 0, 0, m_Impl->texWidth, m_Impl->texHeight };
        SDL_Rect dst { 0, 0, m_Impl->backWidth, m_Impl->backHeight };
        StreamUtils::scaleSourceToDestinationSurface(&src, &dst);
        const bool exact1to1 = (dst.w == m_Impl->texWidth && dst.h == m_Impl->texHeight);
        drawQuad(dst, m_Impl->videoTexSrv.Get(), m_Impl->psVideo.Get(),
                 exact1to1 ? m_Impl->sampNearest.Get() : m_Impl->sampLinear.Get(), false);
    }

    for (int i = 0; i < Overlay::OverlayMax; ++i) {
        const auto type = static_cast<Overlay::OverlayType>(i);
        if (m_OverlayManager && m_OverlayManager->isOverlayEnabled(type) && m_Impl->overlaySrv[i]) {
            drawQuad(m_Impl->overlayRect[i], m_Impl->overlaySrv[i].Get(), m_Impl->psOverlay.Get(), m_Impl->sampNearest.Get(), true);
        }
    }

    ID3D11ShaderResourceView* nullS[1] = { nullptr };
    m_Impl->ctx->PSSetShaderResources(0, 1, nullS);
    m_Impl->swapChain->Present(m_Impl->enableVsync ? 1u : 0u, 0u);
    lk.unlock();
}

bool LosslessD3D11Renderer::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info)
{
    auto unhandled = info->stateChangeFlags;
    unhandled &= ~WINDOW_STATE_CHANGE_SIZE;
    unhandled &= ~WINDOW_STATE_CHANGE_DISPLAY;
    return unhandled == 0;
}

void LosslessD3D11Renderer::notifyOverlayUpdated(Overlay::OverlayType type)
{
    m_OverlayDirty[type] = true;
}

#endif
