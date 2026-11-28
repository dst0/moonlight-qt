#include <metal_stdlib>
using namespace metal;

constant int RICE_ESCAPE_Q = 16;

struct CopyRectParam {
    uint src_x;
    uint src_y;
    uint dst_x;
    uint dst_y;
    uint w;
    uint h;
    uint pad0;
    uint pad1;
};

struct DecodeParams {
    uint width;
    uint height;
    uint stride;
    uint copy_rect_count;
    uint dirty_tile_count;
    uint pad0;
    uint pad1;
    uint pad2;
    CopyRectParam copy_rects[4];
};

struct DirtyTileDesc {
    uint tile_x;
    uint tile_y;
    uint tile_w;
    uint tile_h;
    uint mode;
    uint payload_offset;
    uint payload_len;
    uint pad0;
};

struct VideoVertex {
    float4 position [[position]];
    float2 texCoord;
};

inline int floor_div2_i(int v) {
    return (v >= 0) ? (v >> 1) : -((-v + 1) >> 1);
}

inline void rgb8_to_ycocgr_i(uint r, uint g, uint b, thread int& out_y, thread int& out_co, thread int& out_cg) {
    int ri = int(r);
    int gi = int(g);
    int bi = int(b);
    out_co = ri - bi;
    int t = bi + floor_div2_i(out_co);
    out_cg = gi - t;
    out_y = t + floor_div2_i(out_cg);
}

inline void ycocgr_to_rgb8_i(int y, int co, int cg, thread uchar& out_r, thread uchar& out_g, thread uchar& out_b) {
    int t = y - floor_div2_i(cg);
    int g = cg + t;
    int b = t - floor_div2_i(co);
    int r = b + co;
    out_r = uchar(r & 0xFF);
    out_g = uchar(g & 0xFF);
    out_b = uchar(b & 0xFF);
}

inline int med_predict_i(int a, int b, int c) {
    int mn = min(a, b);
    int mx = max(a, b);
    if (c >= mx) return mn;
    if (c <= mn) return mx;
    return a + b - c;
}

inline int zigzag_decode_i(uint u) {
    return ((u & 1u) != 0u) ? -int((u >> 1) + 1u) : int(u >> 1);
}

struct BitReader {
    device const uchar* data;
    uint len;
    uint bit_pos;

    bool read_bit(thread uint& out_bit) {
        if (bit_pos >= len * 8u) return false;
        uint byte_idx = bit_pos >> 3;
        int shift = 7 - int(bit_pos & 7u);
        out_bit = (data[byte_idx] >> shift) & 1u;
        ++bit_pos;
        return true;
    }

    bool read_bits(int count, thread uint& out_val) {
        if (count == 0) { out_val = 0u; return true; }
        if (bit_pos + uint(count) > len * 8u) return false;
        uint v = 0u;
        for (int i = 0; i < count; ++i) {
            uint byte_idx = bit_pos >> 3;
            int shift = 7 - int(bit_pos & 7u);
            v = (v << 1) | ((data[byte_idx] >> shift) & 1u);
            ++bit_pos;
        }
        out_val = v;
        return true;
    }
};

inline bool decode_rice_chan(
    device const uchar* data,
    uint len,
    uint tile_w,
    uint tile_h,
    int raw_bits,
    threadgroup ushort* out_syms)
{
    BitReader br;
    br.data = data;
    br.len = len;
    br.bit_pos = 0u;

    for (uint y = 0u; y < tile_h; ++y) {
        uint row_off = y * tile_w;
        uint non_zero = 0u;
        if (!br.read_bit(non_zero)) return false;
        if (non_zero == 0u) {
            for (uint x = 0u; x < tile_w; ++x) out_syms[row_off + x] = 0u;
            continue;
        }
        uint k = 0u;
        if (!br.read_bits(3, k)) return false;
        for (uint x = 0u; x < tile_w; ++x) {
            int q = 0;
            while (q < RICE_ESCAPE_Q) {
                uint b = 0u;
                if (!br.read_bit(b)) return false;
                if (b != 0u) break;
                ++q;
            }
            if (q < RICE_ESCAPE_Q) {
                uint rem = 0u;
                if (!br.read_bits(int(k), rem)) return false;
                out_syms[row_off + x] = ushort((uint(q) << k) | rem);
            } else {
                uint raw = 0u;
                if (!br.read_bits(raw_bits, raw)) return false;
                out_syms[row_off + x] = ushort(raw);
            }
        }
    }
    return true;
}

kernel void cs_copy_rects(
    device const uchar* prev_bgra [[buffer(0)]],
    device uchar* curr_bgra [[buffer(1)]],
    constant DecodeParams& params [[buffer(2)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= params.width || gid.y >= params.height) return;
    uint sx = gid.x;
    uint sy = gid.y;
    for (uint i = 0u; i < params.copy_rect_count && i < 4u; ++i) {
        CopyRectParam r = params.copy_rects[i];
        if (gid.x >= r.dst_x && gid.x < r.dst_x + r.w &&
            gid.y >= r.dst_y && gid.y < r.dst_y + r.h) {
            sx = r.src_x + (gid.x - r.dst_x);
            sy = r.src_y + (gid.y - r.dst_y);
        }
    }
    device const uchar* sp = prev_bgra + ulong(sy) * ulong(params.stride) + ulong(sx) * 4ul;
    device uchar* dp = curr_bgra + ulong(gid.y) * ulong(params.stride) + ulong(gid.x) * 4ul;
    dp[0] = sp[0];
    dp[1] = sp[1];
    dp[2] = sp[2];
    dp[3] = 255u;
}

kernel void cs_decode_tiles(
    device const uchar* packet_bytes [[buffer(0)]],
    device const DirtyTileDesc* tiles [[buffer(1)]],
    device uchar* curr_bgra [[buffer(2)]],
    constant DecodeParams& params [[buffer(3)]],
    device atomic_uint* err_flag [[buffer(4)]],
    uint2 tgid2 [[threadgroup_position_in_grid]],
    uint lid [[thread_index_in_threadgroup]])
{
    uint tgid = tgid2.y * 256u + tgid2.x;
    if (tgid >= params.dirty_tile_count) return;
    DirtyTileDesc d = tiles[tgid];
    uint pixels = d.tile_w * d.tile_h;
    device const uchar* payload = packet_bytes + d.payload_offset;

    if (d.mode == 1u) {
        // SOLID
        if (d.payload_len != 3u) {
            if (lid == 0u) atomic_store_explicit(err_flag, 1u, memory_order_relaxed);
            return;
        }
        uchar r = payload[0], g = payload[1], b = payload[2];
        for (uint i = lid; i < pixels; i += 32u) {
            uint x = i % d.tile_w;
            uint y = i / d.tile_w;
            device uchar* dst = curr_bgra + ulong(d.tile_y + y) * ulong(params.stride) + ulong(d.tile_x + x) * 4ul;
            dst[0] = b; dst[1] = g; dst[2] = r; dst[3] = 255u;
        }
        return;
    }

    if (d.mode == 5u) {
        // RAW_RGB24
        if (d.payload_len != pixels * 3u) {
            if (lid == 0u) atomic_store_explicit(err_flag, 1u, memory_order_relaxed);
            return;
        }
        for (uint i = lid; i < pixels; i += 32u) {
            uint x = i % d.tile_w;
            uint y = i / d.tile_w;
            device uchar* dst = curr_bgra + ulong(d.tile_y + y) * ulong(params.stride) + ulong(d.tile_x + x) * 4ul;
            dst[0] = payload[i * 3u + 2u];
            dst[1] = payload[i * 3u + 1u];
            dst[2] = payload[i * 3u + 0u];
            dst[3] = 255u;
        }
        return;
    }

    if (d.mode == 2u) {
        // PALETTE
        if (d.payload_len < 1u) {
            if (lid == 0u) atomic_store_explicit(err_flag, 1u, memory_order_relaxed);
            return;
        }
        uint count = uint(payload[0]);
        if (count < 2u || count > 16u) {
            if (lid == 0u) atomic_store_explicit(err_flag, 1u, memory_order_relaxed);
            return;
        }
        int bpp = (count <= 2u) ? 1 : (count <= 4u ? 2 : 4);
        uint pal_end = 1u + count * 3u;
        if (d.payload_len < pal_end) {
            if (lid == 0u) atomic_store_explicit(err_flag, 1u, memory_order_relaxed);
            return;
        }
        for (uint i = lid; i < pixels; i += 32u) {
            uint bit_idx = i * uint(bpp);
            uint byte_idx = pal_end + (bit_idx >> 3);
            if (byte_idx >= d.payload_len) break;
            uint shift = 8u - uint(bpp) - (bit_idx & 7u);
            uint idx = (uint(payload[byte_idx]) >> shift) & ((1u << uint(bpp)) - 1u);
            if (idx >= count) idx = 0u;
            uint x = i % d.tile_w;
            uint y = i / d.tile_w;
            device uchar* dst = curr_bgra + ulong(d.tile_y + y) * ulong(params.stride) + ulong(d.tile_x + x) * 4ul;
            dst[0] = payload[1u + idx * 3u + 2u];
            dst[1] = payload[1u + idx * 3u + 1u];
            dst[2] = payload[1u + idx * 3u + 0u];
            dst[3] = 255u;
        }
        return;
    }

    if (d.mode != 3u && d.mode != 4u) {
        if (lid == 0u) atomic_store_explicit(err_flag, 1u, memory_order_relaxed);
        return;
    }

    threadgroup ushort tg_sy[1024];
    threadgroup ushort tg_sco[1024];
    threadgroup ushort tg_scg[1024];
    threadgroup int tg_y[1024];
    threadgroup int tg_co[1024];
    threadgroup int tg_cg[1024];
    threadgroup bool tg_ok;

    if (lid == 0u) {
        tg_ok = false;
        if (d.payload_len >= 6u) {
            uint sy_len = uint(payload[0]) | (uint(payload[1]) << 8);
            uint sco_len = uint(payload[2]) | (uint(payload[3]) << 8);
            uint scg_len = uint(payload[4]) | (uint(payload[5]) << 8);
            if (6u + sy_len + sco_len + scg_len <= d.payload_len) {
                bool ok1 = decode_rice_chan(payload + 6u, sy_len, d.tile_w, d.tile_h, 8, tg_sy);
                bool ok2 = ok1 && decode_rice_chan(payload + 6u + sy_len, sco_len, d.tile_w, d.tile_h, 10, tg_sco);
                bool ok3 = ok2 && decode_rice_chan(payload + 6u + sy_len + sco_len, scg_len, d.tile_w, d.tile_h, 10, tg_scg);
                tg_ok = ok3;
            }
        }
        if (!tg_ok) {
            atomic_store_explicit(err_flag, 1u, memory_order_relaxed);
        }
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (!tg_ok) return;

    if (d.mode == 3u) {
        // MED sequential scanline reconstruction on thread 0 (L1 shared mem)
        if (lid == 0u) {
            for (uint y = 0u; y < d.tile_h; ++y) {
                for (uint x = 0u; x < d.tile_w; ++x) {
                    uint i = y * d.tile_w + x;
                    int py = med_predict_i((x > 0u) ? tg_y[i - 1u] : 0, (y > 0u) ? tg_y[i - d.tile_w] : 0,
                                           (x > 0u && y > 0u) ? tg_y[i - d.tile_w - 1u] : 0);
                    tg_y[i] = int(uchar((py + zigzag_decode_i(tg_sy[i])) & 0xFF));

                    int pco = med_predict_i((x > 0u) ? tg_co[i - 1u] : 0, (y > 0u) ? tg_co[i - d.tile_w] : 0,
                                            (x > 0u && y > 0u) ? tg_co[i - d.tile_w - 1u] : 0);
                    tg_co[i] = pco + zigzag_decode_i(tg_sco[i]);

                    int pcg = med_predict_i((x > 0u) ? tg_cg[i - 1u] : 0, (y > 0u) ? tg_cg[i - d.tile_w] : 0,
                                            (x > 0u && y > 0u) ? tg_cg[i - d.tile_w - 1u] : 0);
                    tg_cg[i] = pcg + zigzag_decode_i(tg_scg[i]);
                }
            }
        }
        threadgroup_barrier(mem_flags::mem_threadgroup);

        for (uint i = lid; i < pixels; i += 32u) {
            uint x = i % d.tile_w;
            uint y = i / d.tile_w;
            uchar r = 0, g = 0, b = 0;
            ycocgr_to_rgb8_i(tg_y[i], tg_co[i], tg_cg[i], r, g, b);
            device uchar* dst = curr_bgra + ulong(d.tile_y + y) * ulong(params.stride) + ulong(d.tile_x + x) * 4ul;
            dst[0] = b; dst[1] = g; dst[2] = r; dst[3] = 255u;
        }
    } else {
        // Mode 4: TEMPORAL_RICE (100% parallel across all 32 threads!)
        for (uint i = lid; i < pixels; i += 32u) {
            uint x = i % d.tile_w;
            uint y = i / d.tile_w;
            device uchar* px = curr_bgra + ulong(d.tile_y + y) * ulong(params.stride) + ulong(d.tile_x + x) * 4ul;
            int py = 0, pco = 0, pcg = 0;
            rgb8_to_ycocgr_i(px[2], px[1], px[0], py, pco, pcg);
            int yv = int(uchar((py + zigzag_decode_i(tg_sy[i])) & 0xFF));
            int cov = pco + zigzag_decode_i(tg_sco[i]);
            int cgv = pcg + zigzag_decode_i(tg_scg[i]);
            uchar r = 0, g = 0, b = 0;
            ycocgr_to_rgb8_i(yv, cov, cgv, r, g, b);
            px[0] = b; px[1] = g; px[2] = r; px[3] = 255u;
        }
    }
}

kernel void cs_crc32c_rows(
    device const uchar* bgra [[buffer(0)]],
    device uint* row_crcs [[buffer(1)]],
    constant uint* crc_table [[buffer(2)]],
    constant DecodeParams& params [[buffer(3)]],
    uint y [[thread_position_in_grid]])
{
    if (y >= params.height) return;
    device const uchar* row = bgra + ulong(y) * ulong(params.stride);
    uint crc = 0xFFFFFFFFu;
    for (uint x = 0u; x < params.width; ++x) {
        uint b = row[x * 4u + 0u];
        uint g = row[x * 4u + 1u];
        uint r = row[x * 4u + 2u];
        crc = (crc >> 8) ^ crc_table[(crc ^ r) & 0xFFu];
        crc = (crc >> 8) ^ crc_table[(crc ^ g) & 0xFFu];
        crc = (crc >> 8) ^ crc_table[(crc ^ b) & 0xFFu];
    }
    row_crcs[y] = crc ^ 0xFFFFFFFFu;
}

vertex VideoVertex vs_quad(uint vid [[vertex_id]], constant float4* verts [[buffer(0)]]) {
    float4 v = verts[vid];
    VideoVertex out;
    out.position = float4(v.xy, 0.0, 1.0);
    out.texCoord = v.zw;
    return out;
}

fragment half4 fs_video_nearest(VideoVertex in [[stage_in]], texture2d<half> tex [[texture(0)]]) {
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::nearest);
    half4 c = tex.sample(s, in.texCoord);
    return half4(c.rgb, 1.0h);
}

fragment half4 fs_video_linear(VideoVertex in [[stage_in]], texture2d<half> tex [[texture(0)]]) {
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    half4 c = tex.sample(s, in.texCoord);
    return half4(c.rgb, 1.0h);
}

fragment half4 fs_overlay(VideoVertex in [[stage_in]], texture2d<half> tex [[texture(0)]]) {
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::nearest);
    return tex.sample(s, in.texCoord);
}
