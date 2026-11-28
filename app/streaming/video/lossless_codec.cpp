/**
 * @file src/sds/lossless_codec.cpp
 * @brief Bit-exact SDLL v1 lossless desktop codec implementation.
 */

#include "lossless_codec.h"

#include <algorithm>
#include <cstring>

namespace sds::lossless {
  namespace {
    inline std::uint16_t read_u16_le(const std::uint8_t *p) {
      return static_cast<std::uint16_t>(p[0]) |
             (static_cast<std::uint16_t>(p[1]) << 8);
    }

    inline std::uint32_t read_u32_le(const std::uint8_t *p) {
      return static_cast<std::uint32_t>(p[0]) |
             (static_cast<std::uint32_t>(p[1]) << 8) |
             (static_cast<std::uint32_t>(p[2]) << 16) |
             (static_cast<std::uint32_t>(p[3]) << 24);
    }

    inline std::uint64_t read_u64_le(const std::uint8_t *p) {
      return static_cast<std::uint64_t>(read_u32_le(p)) |
             (static_cast<std::uint64_t>(read_u32_le(p + 4)) << 32);
    }

    inline void write_u16_le(std::uint8_t *p, std::uint16_t v) {
      p[0] = static_cast<std::uint8_t>(v & 0xFFu);
      p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    }

    inline void write_u32_le(std::uint8_t *p, std::uint32_t v) {
      p[0] = static_cast<std::uint8_t>(v & 0xFFu);
      p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
      p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
      p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    }

    inline void write_u64_le(std::uint8_t *p, std::uint64_t v) {
      write_u32_le(p, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
      write_u32_le(p + 4, static_cast<std::uint32_t>(v >> 32));
    }

    inline std::uint32_t pack_rgb24_u32(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
      return static_cast<std::uint32_t>(r) |
             (static_cast<std::uint32_t>(g) << 8) |
             (static_cast<std::uint32_t>(b) << 16);
    }

    inline std::int32_t med_predict(std::int32_t a, std::int32_t b, std::int32_t c) {
      const std::int32_t mn = std::min(a, b);
      const std::int32_t mx = std::max(a, b);
      if (c >= mx) {
        return mn;
      }
      if (c <= mn) {
        return mx;
      }
      return a + b - c;
    }

    inline std::uint32_t zigzag_encode(std::int32_t v) {
      return (v >= 0) ? static_cast<std::uint32_t>(v * 2) : static_cast<std::uint32_t>((-v) * 2 - 1);
    }

    inline std::int32_t zigzag_decode(std::uint32_t u) {
      return (u & 1u) ? -static_cast<std::int32_t>((u >> 1) + 1u) : static_cast<std::int32_t>(u >> 1);
    }

    class bit_writer_t {
    public:
      explicit bit_writer_t(std::vector<std::uint8_t> &buf): buf_(buf) {}

      void write_bit(std::uint32_t bit) {
        cur_ = (cur_ << 1) | (bit & 1u);
        ++bits_;
        if (bits_ == 8) {
          buf_.push_back(static_cast<std::uint8_t>(cur_));
          cur_ = 0;
          bits_ = 0;
        }
      }

      void write_bits(std::uint32_t val, int count) {
        for (int i = count - 1; i >= 0; --i) {
          write_bit((val >> i) & 1u);
        }
      }

      void write_zeros(int count) {
        for (int i = 0; i < count; ++i) {
          write_bit(0);
        }
      }

      void align() {
        if (bits_ > 0) {
          cur_ <<= (8 - bits_);
          buf_.push_back(static_cast<std::uint8_t>(cur_));
          cur_ = 0;
          bits_ = 0;
        }
      }

    private:
      std::vector<std::uint8_t> &buf_;
      std::uint32_t cur_ = 0;
      int bits_ = 0;
    };

    class bit_reader_t {
    public:
      bit_reader_t(const std::uint8_t *data, std::size_t len): data_(data), len_(len) {}

      bool read_bit(std::uint32_t &out_bit) {
        if (bit_pos_ >= len_ * 8u) {
          return false;
        }
        const std::size_t byte_idx = bit_pos_ >> 3;
        const int shift = 7 - static_cast<int>(bit_pos_ & 7u);
        out_bit = (data_[byte_idx] >> shift) & 1u;
        ++bit_pos_;
        return true;
      }

      bool read_bits(int count, std::uint32_t &out_val) {
        if (count == 0) {
          out_val = 0;
          return true;
        }
        if (bit_pos_ + static_cast<std::size_t>(count) > len_ * 8u) {
          return false;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < count; ++i) {
          const std::size_t byte_idx = bit_pos_ >> 3;
          const int shift = 7 - static_cast<int>(bit_pos_ & 7u);
          v = (v << 1) | ((data_[byte_idx] >> shift) & 1u);
          ++bit_pos_;
        }
        out_val = v;
        return true;
      }

    private:
      const std::uint8_t *data_;
      std::size_t len_;
      std::size_t bit_pos_ = 0;
    };

    void encode_rice_channel(
      const std::uint16_t *symbols,
      int tile_w,
      int tile_h,
      int raw_bits,
      std::vector<std::uint8_t> &out
    ) {
      out.clear();
      bit_writer_t bw(out);
      for (int y = 0; y < tile_h; ++y) {
        const std::uint16_t *row = symbols + y * tile_w;
        bool all_zero = true;
        for (int x = 0; x < tile_w; ++x) {
          if (row[x] != 0) {
            all_zero = false;
            break;
          }
        }
        if (all_zero) {
          bw.write_bit(0);
          continue;
        }

        int best_k = 0;
        int best_bits = 1 << 29;
        for (int k = 0; k < 8; ++k) {
          int bits = 4;  // 1 flag bit + 3 k bits
          for (int x = 0; x < tile_w; ++x) {
            const std::uint32_t q = row[x] >> k;
            if (q < static_cast<std::uint32_t>(RICE_ESCAPE_Q)) {
              bits += static_cast<int>(q) + 1 + k;
            } else {
              bits += RICE_ESCAPE_Q + raw_bits;
            }
          }
          if (bits < best_bits) {
            best_bits = bits;
            best_k = k;
          }
        }

        bw.write_bit(1);
        bw.write_bits(static_cast<std::uint32_t>(best_k), 3);
        const std::uint32_t rem_mask = (1u << best_k) - 1u;
        for (int x = 0; x < tile_w; ++x) {
          const std::uint32_t u = row[x];
          const std::uint32_t q = u >> best_k;
          if (q < static_cast<std::uint32_t>(RICE_ESCAPE_Q)) {
            bw.write_zeros(static_cast<int>(q));
            bw.write_bit(1);
            if (best_k > 0) {
              bw.write_bits(u & rem_mask, best_k);
            }
          } else {
            bw.write_zeros(RICE_ESCAPE_Q);
            bw.write_bits(u, raw_bits);
          }
        }
      }
      bw.align();
    }

    bool decode_rice_channel(
      const std::uint8_t *data,
      std::size_t len,
      int tile_w,
      int tile_h,
      int raw_bits,
      std::uint16_t *out_symbols
    ) {
      bit_reader_t br(data, len);
      for (int y = 0; y < tile_h; ++y) {
        std::uint16_t *row = out_symbols + y * tile_w;
        std::uint32_t non_zero = 0;
        if (!br.read_bit(non_zero)) {
          return false;
        }
        if (non_zero == 0) {
          std::fill_n(row, tile_w, static_cast<std::uint16_t>(0));
          continue;
        }
        std::uint32_t k = 0;
        if (!br.read_bits(3, k)) {
          return false;
        }
        for (int x = 0; x < tile_w; ++x) {
          int q = 0;
          while (q < RICE_ESCAPE_Q) {
            std::uint32_t b = 0;
            if (!br.read_bit(b)) {
              return false;
            }
            if (b != 0) {
              break;
            }
            ++q;
          }
          if (q < RICE_ESCAPE_Q) {
            std::uint32_t rem = 0;
            if (!br.read_bits(static_cast<int>(k), rem)) {
              return false;
            }
            row[x] = static_cast<std::uint16_t>((static_cast<std::uint32_t>(q) << k) | rem);
          } else {
            std::uint32_t raw = 0;
            if (!br.read_bits(raw_bits, raw)) {
              return false;
            }
            row[x] = static_cast<std::uint16_t>(raw);
          }
        }
      }
      return true;
    }

    bool tiles_equal_rgb24(
      const std::uint8_t *a_bgra,
      int a_stride,
      const std::uint8_t *b_bgra,
      int b_stride,
      int tile_x,
      int tile_y,
      int tile_w,
      int tile_h
    ) {
      for (int y = 0; y < tile_h; ++y) {
        const std::uint8_t *pa = a_bgra + static_cast<std::size_t>(tile_y + y) * static_cast<std::size_t>(a_stride) + static_cast<std::size_t>(tile_x) * 4u;
        const std::uint8_t *pb = b_bgra + static_cast<std::size_t>(tile_y + y) * static_cast<std::size_t>(b_stride) + static_cast<std::size_t>(tile_x) * 4u;
        for (int x = 0; x < tile_w; ++x) {
          if (((read_u32_le(pa + x * 4) ^ read_u32_le(pb + x * 4)) & 0x00FFFFFFu) != 0) {
            return false;
          }
        }
      }
      return true;
    }

    std::uint64_t hash_bgra_row_rgb24(const std::uint8_t *row, int width) {
      std::uint64_t h = 14695981039346656037ULL;
      for (int x = 0; x < width; ++x) {
        const std::uint32_t px = read_u32_le(row + static_cast<std::size_t>(x) * 4u) & 0x00FFFFFFu;
        h ^= px;
        h *= 1099511628211ULL;
      }
      h ^= h >> 33;
      h *= 0xff51afd7ed558ccdULL;
      h ^= h >> 33;
      return h;
    }

    bool rows_exact_equal_rgb24(const std::uint8_t *row_a, const std::uint8_t *row_b, int width) {
      for (int x = 0; x < width; ++x) {
        if (((read_u32_le(row_a + x * 4) ^ read_u32_le(row_b + x * 4)) & 0x00FFFFFFu) != 0) {
          return false;
        }
      }
      return true;
    }
  }  // namespace

  void write_frame_header(std::uint8_t out[HEADER_SIZE], const frame_header_t &h) {
    std::memset(out, 0, HEADER_SIZE);
    std::memcpy(out, SDLL_MAGIC.data(), 4);
    write_u16_le(out + 4, h.version);
    write_u16_le(out + 6, h.flags);
    write_u64_le(out + 8, h.frame_id);
    write_u64_le(out + 16, h.capture_ts_us);
    write_u16_le(out + 24, h.width);
    write_u16_le(out + 26, h.height);
    out[28] = h.tile_size;
    out[29] = static_cast<std::uint8_t>(h.hash_algo);
    out[30] = h.copy_rect_count;
    out[31] = 0;
    write_u32_le(out + 32, h.dirty_tile_count);
    write_u32_le(out + 36, h.payload_len);
    std::memcpy(out + 40, h.frame_hash.data(), 32);
  }

  bool parse_frame_header(const std::uint8_t *data, std::size_t len, frame_header_t &out) {
    if (!data || len < HEADER_SIZE) {
      return false;
    }
    if (std::memcmp(data, SDLL_MAGIC.data(), 4) != 0) {
      return false;
    }
    out.version = read_u16_le(data + 4);
    out.flags = read_u16_le(data + 6);
    out.frame_id = read_u64_le(data + 8);
    out.capture_ts_us = read_u64_le(data + 16);
    out.width = read_u16_le(data + 24);
    out.height = read_u16_le(data + 26);
    out.tile_size = data[28];
    const std::uint8_t algo = data[29];
    out.copy_rect_count = data[30];
    out.dirty_tile_count = read_u32_le(data + 32);
    out.payload_len = read_u32_le(data + 36);
    std::memcpy(out.frame_hash.data(), data + 40, 32);

    if (out.version != PROTOCOL_VERSION ||
        out.width == 0 || out.width > MAX_FRAME_DIM ||
        out.height == 0 || out.height > MAX_FRAME_DIM ||
        out.tile_size != TILE_SIZE ||
        algo > static_cast<std::uint8_t>(hash_algo_e::blake3) ||
        out.copy_rect_count > MAX_COPY_RECTS ||
        out.payload_len > MAX_PAYLOAD_BYTES) {
      return false;
    }
    out.hash_algo = static_cast<hash_algo_e>(algo);
    return true;
  }

  bool parse_frame_packet(const std::uint8_t *data, std::size_t len, parsed_frame_t &out) {
    out = parsed_frame_t {};
    if (!parse_frame_header(data, len, out.header)) {
      return false;
    }
    if (len != HEADER_SIZE + static_cast<std::size_t>(out.header.payload_len)) {
      return false;
    }

    const int tiles_x = (out.header.width + TILE_SIZE - 1) / TILE_SIZE;
    const int tiles_y = (out.header.height + TILE_SIZE - 1) / TILE_SIZE;
    const std::size_t total_tiles = static_cast<std::size_t>(tiles_x) * static_cast<std::size_t>(tiles_y);
    const std::size_t bitmap_bytes = (total_tiles + 7u) / 8u;
    const std::size_t copy_bytes = static_cast<std::size_t>(out.header.copy_rect_count) * COPY_RECT_WIRE_SIZE;

    if (out.header.dirty_tile_count > total_tiles ||
        copy_bytes + bitmap_bytes > out.header.payload_len) {
      return false;
    }

    const std::uint8_t *p = data + HEADER_SIZE;
    out.copy_rects.resize(out.header.copy_rect_count);
    for (std::uint8_t i = 0; i < out.header.copy_rect_count; ++i) {
      copy_rect_t &r = out.copy_rects[i];
      r.src_x = read_u16_le(p + 0);
      r.src_y = read_u16_le(p + 2);
      r.dst_x = read_u16_le(p + 4);
      r.dst_y = read_u16_le(p + 6);
      r.w = read_u16_le(p + 8);
      r.h = read_u16_le(p + 10);
      if (!validate_copy_rect(r, out.header.width, out.header.height)) {
        return false;
      }
      p += COPY_RECT_WIRE_SIZE;
    }

    out.skip_bitmap = p;
    out.skip_bitmap_bytes = bitmap_bytes;
    p += bitmap_bytes;

    std::size_t dirty_bits = 0;
    for (std::size_t idx = 0; idx < total_tiles; ++idx) {
      if ((out.skip_bitmap[idx >> 3] >> (idx & 7u)) & 1u) {
        ++dirty_bits;
      }
    }
    if (dirty_bits != out.header.dirty_tile_count) {
      return false;
    }

    out.tiles_data = p;
    out.tiles_data_bytes = static_cast<std::size_t>((data + len) - p);
    return true;
  }

  bool build_dirty_tile_descs(
    const parsed_frame_t &parsed,
    const std::uint8_t *packet_base,
    std::vector<gpu_tile_desc_t> &out_descs
  ) {
    out_descs.clear();
    if (!packet_base || parsed.header.width == 0 || parsed.header.height == 0) {
      return false;
    }
    const int w = parsed.header.width;
    const int h = parsed.header.height;
    const int tiles_x = (w + TILE_SIZE - 1) / TILE_SIZE;
    const int tiles_y = (h + TILE_SIZE - 1) / TILE_SIZE;
    const std::size_t total_tiles = static_cast<std::size_t>(tiles_x) * static_cast<std::size_t>(tiles_y);

    out_descs.reserve(parsed.header.dirty_tile_count);
    std::size_t pos = 0;
    for (std::size_t idx = 0; idx < total_tiles; ++idx) {
      if (((parsed.skip_bitmap[idx >> 3] >> (idx & 7u)) & 1u) == 0u) {
        continue;
      }
      if (pos + TILE_HEADER_SIZE > parsed.tiles_data_bytes) {
        return false;
      }
      const std::uint8_t mode = parsed.tiles_data[pos];
      const std::uint16_t plen = read_u16_le(parsed.tiles_data + pos + 1);
      pos += TILE_HEADER_SIZE;
      if (plen == 0 || plen > MAX_TILE_PAYLOAD || pos + plen > parsed.tiles_data_bytes ||
          mode < static_cast<std::uint8_t>(tile_mode_e::solid) ||
          mode > static_cast<std::uint8_t>(tile_mode_e::raw_rgb24)) {
        return false;
      }
      const int tx = static_cast<int>(idx % static_cast<std::size_t>(tiles_x));
      const int ty = static_cast<int>(idx / static_cast<std::size_t>(tiles_x));
      const int tile_x = tx * TILE_SIZE;
      const int tile_y = ty * TILE_SIZE;

      gpu_tile_desc_t desc;
      desc.tile_x = static_cast<std::uint32_t>(tile_x);
      desc.tile_y = static_cast<std::uint32_t>(tile_y);
      desc.tile_w = static_cast<std::uint32_t>(std::min(TILE_SIZE, w - tile_x));
      desc.tile_h = static_cast<std::uint32_t>(std::min(TILE_SIZE, h - tile_y));
      desc.mode = static_cast<std::uint32_t>(mode);
      desc.payload_offset = static_cast<std::uint32_t>((parsed.tiles_data + pos) - packet_base);
      desc.payload_len = static_cast<std::uint32_t>(plen);
      out_descs.push_back(desc);
      pos += plen;
    }
    return out_descs.size() == parsed.header.dirty_tile_count && pos == parsed.tiles_data_bytes;
  }

  void serialize_control_msg(control_msg_e type, std::uint32_t arg, std::uint8_t out[CONTROL_MSG_SIZE]) {
    std::memcpy(out, SDLC_MAGIC.data(), 4);
    write_u16_le(out + 4, PROTOCOL_VERSION);
    write_u16_le(out + 6, static_cast<std::uint16_t>(type));
    write_u32_le(out + 8, arg);
  }

  bool parse_control_msg(const std::uint8_t *data, std::size_t len, control_msg_t &out) {
    if (!data || len != CONTROL_MSG_SIZE) {
      return false;
    }
    if (std::memcmp(data, SDLC_MAGIC.data(), 4) != 0) {
      return false;
    }
    const std::uint16_t ver = read_u16_le(data + 4);
    const std::uint16_t type = read_u16_le(data + 6);
    if (ver != PROTOCOL_VERSION ||
        type < static_cast<std::uint16_t>(control_msg_e::request_keyframe) ||
        type > static_cast<std::uint16_t>(control_msg_e::ack_frame)) {
      return false;
    }
    out.type = static_cast<control_msg_e>(type);
    out.arg = read_u32_le(data + 8);
    return true;
  }

  bool validate_copy_rect(const copy_rect_t &r, int width, int height) {
    if (r.w == 0 || r.h == 0) {
      return false;
    }
    if (static_cast<int>(r.src_x) + static_cast<int>(r.w) > width ||
        static_cast<int>(r.src_y) + static_cast<int>(r.h) > height ||
        static_cast<int>(r.dst_x) + static_cast<int>(r.w) > width ||
        static_cast<int>(r.dst_y) + static_cast<int>(r.h) > height) {
      return false;
    }
    return true;
  }

  std::vector<copy_rect_t> detect_scroll_copy_rects(
    const std::uint8_t *prev_bgra,
    int prev_stride,
    const std::uint8_t *curr_bgra,
    int curr_stride,
    int width,
    int height
  ) {
    std::vector<copy_rect_t> rects;
    if (!prev_bgra || !curr_bgra || width < 64 || height < 128) {
      return rects;
    }

    std::vector<std::uint64_t> prev_h(static_cast<std::size_t>(height));
    std::vector<std::uint64_t> curr_h(static_cast<std::size_t>(height));
    for (int y = 0; y < height; ++y) {
      prev_h[static_cast<std::size_t>(y)] = hash_bgra_row_rgb24(
        prev_bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(prev_stride), width);
      curr_h[static_cast<std::size_t>(y)] = hash_bgra_row_rgb24(
        curr_bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(curr_stride), width);
    }

    // Sample a stride of rows in curr and vote for dominant non-zero vertical shift dy = dst_y - src_y
    constexpr int MAX_SHIFT = 512;
    std::vector<std::pair<int, int>> candidates;
    const int step = std::max(4, height / 96);
    for (int y = 32; y < height - 32; y += step) {
      if (curr_h[y] == prev_h[y]) {
        continue;  // unchanged stationary row
      }
      for (int delta = 1; delta <= MAX_SHIFT; ++delta) {
        const int up_src = y + delta;
        if (up_src < height && curr_h[y] == prev_h[up_src]) {
          candidates.emplace_back(-delta, y);
          break;
        }
        const int dn_src = y - delta;
        if (dn_src >= 0 && curr_h[y] == prev_h[dn_src]) {
          candidates.emplace_back(+delta, y);
          break;
        }
      }
    }

    if (candidates.empty()) {
      return rects;
    }

    // Pick strongest shift dy
    std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
      return a.first < b.first;
    });
    int best_dy = 0;
    int best_votes = 0;
    for (std::size_t i = 0; i < candidates.size();) {
      std::size_t j = i + 1;
      while (j < candidates.size() && candidates[j].first == candidates[i].first) {
        ++j;
      }
      const int votes = static_cast<int>(j - i);
      if (votes > best_votes) {
        best_votes = votes;
        best_dy = candidates[i].first;
      }
      i = j;
    }

    if (best_dy == 0 || best_votes < 3) {
      return rects;
    }

    // Extract contiguous matching vertical bands at shift best_dy (verify exact row match)
    int y = 0;
    while (y < height && static_cast<int>(rects.size()) < MAX_COPY_RECTS) {
      const int sy = y - best_dy;
      if (sy < 0 || sy >= height || curr_h[y] != prev_h[sy] ||
          !rows_exact_equal_rgb24(curr_bgra + static_cast<std::size_t>(y) * curr_stride,
                                  prev_bgra + static_cast<std::size_t>(sy) * prev_stride,
                                  width)) {
        ++y;
        continue;
      }
      int band_end = y + 1;
      while (band_end < height) {
        const int sy2 = band_end - best_dy;
        if (sy2 < 0 || sy2 >= height || curr_h[band_end] != prev_h[sy2] ||
            !rows_exact_equal_rgb24(curr_bgra + static_cast<std::size_t>(band_end) * curr_stride,
                                    prev_bgra + static_cast<std::size_t>(sy2) * prev_stride,
                                    width)) {
          break;
        }
        ++band_end;
      }
      const int band_h = band_end - y;
      if (band_h >= 64) {
        // Align dst_y and h down/up nicely? Exact row copy is completely valid!
        rects.push_back(copy_rect_t {
          0,
          static_cast<std::uint16_t>(y - best_dy),
          0,
          static_cast<std::uint16_t>(y),
          static_cast<std::uint16_t>(width),
          static_cast<std::uint16_t>(band_h),
        });
      }
      y = band_end;
    }

    return rects;
  }

  void apply_copy_rects_bgra(
    const std::uint8_t *prev_bgra,
    int prev_stride,
    std::uint8_t *dst_bgra,
    int dst_stride,
    int width,
    int height,
    const std::vector<copy_rect_t> &rects
  ) {
    // Copy full prev -> dst first so untouched tiles match prev, then apply CopyRects reading from immutable prev
    if (prev_stride == dst_stride && prev_stride == width * 4) {
      std::memcpy(dst_bgra, prev_bgra, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    } else {
      const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
      for (int y = 0; y < height; ++y) {
        std::memcpy(dst_bgra + static_cast<std::size_t>(y) * dst_stride,
                    prev_bgra + static_cast<std::size_t>(y) * prev_stride,
                    row_bytes);
      }
    }

    for (const copy_rect_t &r : rects) {
      if (!validate_copy_rect(r, width, height)) {
        continue;
      }
      const std::size_t row_bytes = static_cast<std::size_t>(r.w) * 4u;
      for (int row = 0; row < r.h; ++row) {
        std::memcpy(
          dst_bgra + static_cast<std::size_t>(r.dst_y + row) * dst_stride + static_cast<std::size_t>(r.dst_x) * 4u,
          prev_bgra + static_cast<std::size_t>(r.src_y + row) * prev_stride + static_cast<std::size_t>(r.src_x) * 4u,
          row_bytes
        );
      }
    }
  }

  tile_mode_e encode_tile_best(
    const std::uint8_t *curr_bgra,
    int curr_stride,
    const std::uint8_t *pred_bgra,
    int pred_stride,
    int tile_x,
    int tile_y,
    int tile_w,
    int tile_h,
    bool allow_temporal,
    std::vector<std::uint8_t> &out_payload
  ) {
    out_payload.clear();
    const int pixels = tile_w * tile_h;
    const std::size_t raw_size = static_cast<std::size_t>(pixels) * 3u;

    // 1) Check SOLID + collect up to 16 palette colors in one pass
    std::array<std::uint32_t, 16> palette {};
    int palette_count = 0;
    bool palette_overflow = false;
    std::array<std::uint8_t, MAX_TILE_PIXELS> indices {};

    std::array<std::int32_t, MAX_TILE_PIXELS> curr_y {};
    std::array<std::int32_t, MAX_TILE_PIXELS> curr_co {};
    std::array<std::int32_t, MAX_TILE_PIXELS> curr_cg {};

    for (int y = 0; y < tile_h; ++y) {
      const std::uint8_t *src = curr_bgra + static_cast<std::size_t>(tile_y + y) * curr_stride + static_cast<std::size_t>(tile_x) * 4u;
      for (int x = 0; x < tile_w; ++x) {
        const std::uint8_t b = src[x * 4 + 0];
        const std::uint8_t g = src[x * 4 + 1];
        const std::uint8_t r = src[x * 4 + 2];
        const int idx_px = y * tile_w + x;

        rgb8_to_ycocgr(r, g, b, curr_y[idx_px], curr_co[idx_px], curr_cg[idx_px]);

        if (!palette_overflow) {
          const std::uint32_t rgb = pack_rgb24_u32(r, g, b);
          int found = -1;
          for (int p = 0; p < palette_count; ++p) {
            if (palette[p] == rgb) {
              found = p;
              break;
            }
          }
          if (found < 0) {
            if (palette_count < 16) {
              found = palette_count;
              palette[palette_count++] = rgb;
            } else {
              palette_overflow = true;
            }
          }
          if (found >= 0) {
            indices[idx_px] = static_cast<std::uint8_t>(found);
          }
        }
      }
    }

    if (!palette_overflow && palette_count == 1) {
      out_payload.resize(3);
      out_payload[0] = static_cast<std::uint8_t>(palette[0] & 0xFFu);
      out_payload[1] = static_cast<std::uint8_t>((palette[0] >> 8) & 0xFFu);
      out_payload[2] = static_cast<std::uint8_t>((palette[0] >> 16) & 0xFFu);
      return tile_mode_e::solid;
    }

    tile_mode_e best_mode = tile_mode_e::raw_rgb24;
    std::size_t best_size = raw_size;
    std::vector<std::uint8_t> best_buf;

    // 2) Candidate: PALETTE (2..16 colors)
    if (!palette_overflow && palette_count >= 2) {
      const int bpp = (palette_count <= 2) ? 1 : (palette_count <= 4 ? 2 : 4);
      const std::size_t idx_bytes = (static_cast<std::size_t>(pixels) * static_cast<std::size_t>(bpp) + 7u) / 8u;
      const std::size_t pal_size = 1u + static_cast<std::size_t>(palette_count) * 3u + idx_bytes;
      if (pal_size < best_size) {
        best_size = pal_size;
        best_mode = tile_mode_e::palette;
        best_buf.clear();
        best_buf.reserve(pal_size);
        best_buf.push_back(static_cast<std::uint8_t>(palette_count));
        for (int p = 0; p < palette_count; ++p) {
          best_buf.push_back(static_cast<std::uint8_t>(palette[p] & 0xFFu));
          best_buf.push_back(static_cast<std::uint8_t>((palette[p] >> 8) & 0xFFu));
          best_buf.push_back(static_cast<std::uint8_t>((palette[p] >> 16) & 0xFFu));
        }
        bit_writer_t bw(best_buf);
        for (int i = 0; i < pixels; ++i) {
          bw.write_bits(indices[i], bpp);
        }
        bw.align();
      }
    }

    // 3) Candidate: YCOCGR_MED_RICE
    std::array<std::uint16_t, MAX_TILE_PIXELS> sym_y {};
    std::array<std::uint16_t, MAX_TILE_PIXELS> sym_co {};
    std::array<std::uint16_t, MAX_TILE_PIXELS> sym_cg {};

    for (int y = 0; y < tile_h; ++y) {
      for (int x = 0; x < tile_w; ++x) {
        const int i = y * tile_w + x;
        const std::int32_t ya = (x > 0) ? curr_y[i - 1] : 0;
        const std::int32_t yb = (y > 0) ? curr_y[i - tile_w] : 0;
        const std::int32_t yc = (x > 0 && y > 0) ? curr_y[i - tile_w - 1] : 0;
        const std::int32_t py = med_predict(ya, yb, yc);
        const std::int8_t dy = static_cast<std::int8_t>(static_cast<std::uint8_t>(curr_y[i]) - static_cast<std::uint8_t>(py));
        sym_y[i] = static_cast<std::uint16_t>(zigzag_encode(dy));

        const std::int32_t coa = (x > 0) ? curr_co[i - 1] : 0;
        const std::int32_t cob = (y > 0) ? curr_co[i - tile_w] : 0;
        const std::int32_t coc = (x > 0 && y > 0) ? curr_co[i - tile_w - 1] : 0;
        const std::int32_t pco = med_predict(coa, cob, coc);
        sym_co[i] = static_cast<std::uint16_t>(zigzag_encode(curr_co[i] - pco));

        const std::int32_t cga = (x > 0) ? curr_cg[i - 1] : 0;
        const std::int32_t cgb = (y > 0) ? curr_cg[i - tile_w] : 0;
        const std::int32_t cgc = (x > 0 && y > 0) ? curr_cg[i - tile_w - 1] : 0;
        const std::int32_t pcg = med_predict(cga, cgb, cgc);
        sym_cg[i] = static_cast<std::uint16_t>(zigzag_encode(curr_cg[i] - pcg));
      }
    }

    std::vector<std::uint8_t> s_y, s_co, s_cg;
    encode_rice_channel(sym_y.data(), tile_w, tile_h, 8, s_y);
    encode_rice_channel(sym_co.data(), tile_w, tile_h, 10, s_co);
    encode_rice_channel(sym_cg.data(), tile_w, tile_h, 10, s_cg);

    const std::size_t med_size = 6u + s_y.size() + s_co.size() + s_cg.size();
    if (med_size < best_size) {
      best_size = med_size;
      best_mode = tile_mode_e::ycocgr_med_rice;
      best_buf.resize(med_size);
      write_u16_le(best_buf.data() + 0, static_cast<std::uint16_t>(s_y.size()));
      write_u16_le(best_buf.data() + 2, static_cast<std::uint16_t>(s_co.size()));
      write_u16_le(best_buf.data() + 4, static_cast<std::uint16_t>(s_cg.size()));
      std::memcpy(best_buf.data() + 6, s_y.data(), s_y.size());
      std::memcpy(best_buf.data() + 6 + s_y.size(), s_co.data(), s_co.size());
      std::memcpy(best_buf.data() + 6 + s_y.size() + s_co.size(), s_cg.data(), s_cg.size());
    }

    // 4) Candidate: YCOCGR_TEMPORAL_RICE (only on inter frames)
    if (allow_temporal && pred_bgra) {
      for (int y = 0; y < tile_h; ++y) {
        const std::uint8_t *psrc = pred_bgra + static_cast<std::size_t>(tile_y + y) * pred_stride + static_cast<std::size_t>(tile_x) * 4u;
        for (int x = 0; x < tile_w; ++x) {
          const int i = y * tile_w + x;
          std::int32_t py = 0, pco = 0, pcg = 0;
          rgb8_to_ycocgr(psrc[x * 4 + 2], psrc[x * 4 + 1], psrc[x * 4 + 0], py, pco, pcg);
          const std::int8_t dy = static_cast<std::int8_t>(static_cast<std::uint8_t>(curr_y[i]) - static_cast<std::uint8_t>(py));
          sym_y[i] = static_cast<std::uint16_t>(zigzag_encode(dy));
          sym_co[i] = static_cast<std::uint16_t>(zigzag_encode(curr_co[i] - pco));
          sym_cg[i] = static_cast<std::uint16_t>(zigzag_encode(curr_cg[i] - pcg));
        }
      }

      encode_rice_channel(sym_y.data(), tile_w, tile_h, 8, s_y);
      encode_rice_channel(sym_co.data(), tile_w, tile_h, 10, s_co);
      encode_rice_channel(sym_cg.data(), tile_w, tile_h, 10, s_cg);

      const std::size_t temp_size = 6u + s_y.size() + s_co.size() + s_cg.size();
      if (temp_size < best_size) {
        best_size = temp_size;
        best_mode = tile_mode_e::ycocgr_temporal_rice;
        best_buf.resize(temp_size);
        write_u16_le(best_buf.data() + 0, static_cast<std::uint16_t>(s_y.size()));
        write_u16_le(best_buf.data() + 2, static_cast<std::uint16_t>(s_co.size()));
        write_u16_le(best_buf.data() + 4, static_cast<std::uint16_t>(s_cg.size()));
        std::memcpy(best_buf.data() + 6, s_y.data(), s_y.size());
        std::memcpy(best_buf.data() + 6 + s_y.size(), s_co.data(), s_co.size());
        std::memcpy(best_buf.data() + 6 + s_y.size() + s_co.size(), s_cg.data(), s_cg.size());
      }
    }

    if (best_mode != tile_mode_e::raw_rgb24) {
      out_payload = std::move(best_buf);
      return best_mode;
    }

    // 5) Fallback: RAW_RGB24 (guaranteed <= 3072 bytes)
    out_payload.resize(raw_size);
    for (int y = 0; y < tile_h; ++y) {
      const std::uint8_t *src = curr_bgra + static_cast<std::size_t>(tile_y + y) * curr_stride + static_cast<std::size_t>(tile_x) * 4u;
      std::uint8_t *dst = out_payload.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(tile_w) * 3u;
      for (int x = 0; x < tile_w; ++x) {
        dst[x * 3 + 0] = src[x * 4 + 2];
        dst[x * 3 + 1] = src[x * 4 + 1];
        dst[x * 3 + 2] = src[x * 4 + 0];
      }
    }
    return tile_mode_e::raw_rgb24;
  }

  bool decode_tile_to_bgra(
    tile_mode_e mode,
    const std::uint8_t *payload,
    std::size_t payload_len,
    const std::uint8_t *pred_bgra,
    int pred_stride,
    std::uint8_t *dst_bgra,
    int dst_stride,
    int tile_x,
    int tile_y,
    int tile_w,
    int tile_h
  ) {
    if (!payload || !dst_bgra || tile_w <= 0 || tile_w > TILE_SIZE || tile_h <= 0 || tile_h > TILE_SIZE) {
      return false;
    }
    const int pixels = tile_w * tile_h;

    if (mode == tile_mode_e::solid) {
      if (payload_len != 3) {
        return false;
      }
      const std::uint8_t r = payload[0], g = payload[1], b = payload[2];
      for (int y = 0; y < tile_h; ++y) {
        std::uint8_t *dst = dst_bgra + static_cast<std::size_t>(tile_y + y) * dst_stride + static_cast<std::size_t>(tile_x) * 4u;
        for (int x = 0; x < tile_w; ++x) {
          dst[x * 4 + 0] = b;
          dst[x * 4 + 1] = g;
          dst[x * 4 + 2] = r;
          dst[x * 4 + 3] = 255;
        }
      }
      return true;
    }

    if (mode == tile_mode_e::raw_rgb24) {
      if (payload_len != static_cast<std::size_t>(pixels) * 3u) {
        return false;
      }
      for (int y = 0; y < tile_h; ++y) {
        const std::uint8_t *src = payload + static_cast<std::size_t>(y) * static_cast<std::size_t>(tile_w) * 3u;
        std::uint8_t *dst = dst_bgra + static_cast<std::size_t>(tile_y + y) * dst_stride + static_cast<std::size_t>(tile_x) * 4u;
        for (int x = 0; x < tile_w; ++x) {
          dst[x * 4 + 0] = src[x * 3 + 2];
          dst[x * 4 + 1] = src[x * 3 + 1];
          dst[x * 4 + 2] = src[x * 3 + 0];
          dst[x * 4 + 3] = 255;
        }
      }
      return true;
    }

    if (mode == tile_mode_e::palette) {
      if (payload_len < 1) {
        return false;
      }
      const int count = payload[0];
      if (count < 2 || count > 16) {
        return false;
      }
      const int bpp = (count <= 2) ? 1 : (count <= 4 ? 2 : 4);
      const std::size_t idx_bytes = (static_cast<std::size_t>(pixels) * static_cast<std::size_t>(bpp) + 7u) / 8u;
      if (payload_len != 1u + static_cast<std::size_t>(count) * 3u + idx_bytes) {
        return false;
      }
      const std::uint8_t *pal = payload + 1;
      bit_reader_t br(payload + 1 + static_cast<std::size_t>(count) * 3u, idx_bytes);
      for (int y = 0; y < tile_h; ++y) {
        std::uint8_t *dst = dst_bgra + static_cast<std::size_t>(tile_y + y) * dst_stride + static_cast<std::size_t>(tile_x) * 4u;
        for (int x = 0; x < tile_w; ++x) {
          std::uint32_t idx = 0;
          if (!br.read_bits(bpp, idx) || static_cast<int>(idx) >= count) {
            return false;
          }
          dst[x * 4 + 0] = pal[idx * 3 + 2];
          dst[x * 4 + 1] = pal[idx * 3 + 1];
          dst[x * 4 + 2] = pal[idx * 3 + 0];
          dst[x * 4 + 3] = 255;
        }
      }
      return true;
    }

    if (mode == tile_mode_e::ycocgr_med_rice || mode == tile_mode_e::ycocgr_temporal_rice) {
      if (payload_len < 6) {
        return false;
      }
      const std::size_t len_y = read_u16_le(payload + 0);
      const std::size_t len_co = read_u16_le(payload + 2);
      const std::size_t len_cg = read_u16_le(payload + 4);
      if (6u + len_y + len_co + len_cg != payload_len) {
        return false;
      }

      std::array<std::uint16_t, MAX_TILE_PIXELS> sym_y {};
      std::array<std::uint16_t, MAX_TILE_PIXELS> sym_co {};
      std::array<std::uint16_t, MAX_TILE_PIXELS> sym_cg {};
      if (!decode_rice_channel(payload + 6, len_y, tile_w, tile_h, 8, sym_y.data()) ||
          !decode_rice_channel(payload + 6 + len_y, len_co, tile_w, tile_h, 10, sym_co.data()) ||
          !decode_rice_channel(payload + 6 + len_y + len_co, len_cg, tile_w, tile_h, 10, sym_cg.data())) {
        return false;
      }

      if (mode == tile_mode_e::ycocgr_med_rice) {
        std::array<std::int32_t, MAX_TILE_PIXELS> plane_y {};
        std::array<std::int32_t, MAX_TILE_PIXELS> plane_co {};
        std::array<std::int32_t, MAX_TILE_PIXELS> plane_cg {};

        for (int y = 0; y < tile_h; ++y) {
          std::uint8_t *dst = dst_bgra + static_cast<std::size_t>(tile_y + y) * dst_stride + static_cast<std::size_t>(tile_x) * 4u;
          for (int x = 0; x < tile_w; ++x) {
            const int i = y * tile_w + x;
            const std::int32_t py = med_predict(
              (x > 0) ? plane_y[i - 1] : 0,
              (y > 0) ? plane_y[i - tile_w] : 0,
              (x > 0 && y > 0) ? plane_y[i - tile_w - 1] : 0);
            const std::int32_t yv = static_cast<std::int32_t>(static_cast<std::uint8_t>((py + zigzag_decode(sym_y[i])) & 0xFF));

            const std::int32_t pco = med_predict(
              (x > 0) ? plane_co[i - 1] : 0,
              (y > 0) ? plane_co[i - tile_w] : 0,
              (x > 0 && y > 0) ? plane_co[i - tile_w - 1] : 0);
            const std::int32_t cov = pco + zigzag_decode(sym_co[i]);

            const std::int32_t pcg = med_predict(
              (x > 0) ? plane_cg[i - 1] : 0,
              (y > 0) ? plane_cg[i - tile_w] : 0,
              (x > 0 && y > 0) ? plane_cg[i - tile_w - 1] : 0);
            const std::int32_t cgv = pcg + zigzag_decode(sym_cg[i]);

            plane_y[i] = yv;
            plane_co[i] = cov;
            plane_cg[i] = cgv;

            std::uint8_t r = 0, g = 0, b = 0;
            ycocgr_to_rgb8(yv, cov, cgv, r, g, b);
            dst[x * 4 + 0] = b;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = r;
            dst[x * 4 + 3] = 255;
          }
        }
        return true;
      }

      if (!pred_bgra) {
        return false;
      }
      for (int y = 0; y < tile_h; ++y) {
        const std::uint8_t *psrc = pred_bgra + static_cast<std::size_t>(tile_y + y) * pred_stride + static_cast<std::size_t>(tile_x) * 4u;
        std::uint8_t *dst = dst_bgra + static_cast<std::size_t>(tile_y + y) * dst_stride + static_cast<std::size_t>(tile_x) * 4u;
        for (int x = 0; x < tile_w; ++x) {
          const int i = y * tile_w + x;
          std::int32_t py = 0, pco = 0, pcg = 0;
          rgb8_to_ycocgr(psrc[x * 4 + 2], psrc[x * 4 + 1], psrc[x * 4 + 0], py, pco, pcg);
          const std::int32_t yv = static_cast<std::int32_t>(static_cast<std::uint8_t>((py + zigzag_decode(sym_y[i])) & 0xFF));
          const std::int32_t cov = pco + zigzag_decode(sym_co[i]);
          const std::int32_t cgv = pcg + zigzag_decode(sym_cg[i]);
          std::uint8_t r = 0, g = 0, b = 0;
          ycocgr_to_rgb8(yv, cov, cgv, r, g, b);
          dst[x * 4 + 0] = b;
          dst[x * 4 + 1] = g;
          dst[x * 4 + 2] = r;
          dst[x * 4 + 3] = 255;
        }
      }
      return true;
    }

    return false;
  }

  // ---------------------------------------------------------------------------
  // cpu_encoder_t
  // ---------------------------------------------------------------------------
  cpu_encoder_t::cpu_encoder_t(int thread_count):
      requested_threads_(thread_count) {
    if (requested_threads_ <= 0) {
      const unsigned hc = std::thread::hardware_concurrency();
      requested_threads_ = static_cast<int>(std::clamp(hc ? hc : 4u, 2u, 8u));
    }
  }

  cpu_encoder_t::~cpu_encoder_t() {
    stop_workers();
  }

  void cpu_encoder_t::reset() {
    has_prev_ = false;
    width_ = 0;
    height_ = 0;
    prev_bgra_.clear();
    pred_bgra_.clear();
  }

  void cpu_encoder_t::ensure_workers(int count) {
    if (!workers_.empty() || count <= 1) {
      return;
    }
    const int spawn = count - 1;
    workers_.reserve(spawn);
    for (int i = 0; i < spawn; ++i) {
      workers_.emplace_back([this, idx = i + 1, total = count]() {
        std::uint64_t seen_epoch = 0;
        while (true) {
          const std::function<void(int, int)> *fn = nullptr;
          int rows = 0;
          {
            std::unique_lock lock {pool_mutex_};
            pool_cv_.wait(lock, [&]() {
              return stopping_ || job_epoch_ != seen_epoch;
            });
            if (stopping_) {
              return;
            }
            seen_epoch = job_epoch_;
            fn = job_fn_;
            rows = job_rows_;
          }
          const int b = (rows * idx) / total;
          const int e = (rows * (idx + 1)) / total;
          if (fn && b < e) {
            (*fn)(b, e);
          }
          {
            std::lock_guard lock {pool_mutex_};
            if (--pending_workers_ == 0) {
              pool_done_cv_.notify_one();
            }
          }
        }
      });
    }
  }

  void cpu_encoder_t::stop_workers() {
    {
      std::lock_guard lock {pool_mutex_};
      stopping_ = true;
    }
    pool_cv_.notify_all();
    for (auto &t : workers_) {
      if (t.joinable()) {
        t.join();
      }
    }
    workers_.clear();
    stopping_ = false;
  }

  void cpu_encoder_t::run_parallel_rows(int rows, const std::function<void(int, int)> &fn) {
    if (rows <= 1 || requested_threads_ <= 1) {
      fn(0, rows);
      return;
    }
    ensure_workers(requested_threads_);
    const int total = static_cast<int>(workers_.size()) + 1;
    {
      std::lock_guard lock {pool_mutex_};
      job_fn_ = &fn;
      job_rows_ = rows;
      pending_workers_ = total - 1;
      ++job_epoch_;
    }
    pool_cv_.notify_all();
    const int b = 0;
    const int e = rows / total;
    if (b < e) {
      fn(b, e);
    }
    {
      std::unique_lock lock {pool_mutex_};
      pool_done_cv_.wait(lock, [&]() { return pending_workers_ == 0; });
    }
  }

  bool cpu_encoder_t::encode_frame(
    const std::uint8_t *bgra,
    int stride,
    int width,
    int height,
    std::uint64_t frame_id,
    std::uint64_t capture_ts_us,
    hash_algo_e hash_algo,
    bool force_keyframe,
    std::vector<std::uint8_t> &out_packet,
    encode_stats_t *out_stats
  ) {
    if (!bgra || width <= 0 || height <= 0 || width > MAX_FRAME_DIM || height > MAX_FRAME_DIM || stride < width * 4) {
      return false;
    }

    const bool dims_changed = (width != width_ || height != height_);
    const bool is_keyframe = force_keyframe || !has_prev_ || dims_changed;
    const int dst_stride = width * 4;
    const std::size_t frame_bytes = static_cast<std::size_t>(dst_stride) * static_cast<std::size_t>(height);

    if (dims_changed) {
      width_ = width;
      height_ = height;
      prev_bgra_.resize(frame_bytes);
      pred_bgra_.resize(frame_bytes);
      row_crcs_.resize(static_cast<std::size_t>(height));
    }

    std::vector<copy_rect_t> copy_rects;
    if (!is_keyframe) {
      copy_rects = detect_scroll_copy_rects(prev_bgra_.data(), dst_stride, bgra, stride, width, height);
      apply_copy_rects_bgra(prev_bgra_.data(), dst_stride, pred_bgra_.data(), dst_stride, width, height, copy_rects);
    }

    const int tiles_x = (width + TILE_SIZE - 1) / TILE_SIZE;
    const int tiles_y = (height + TILE_SIZE - 1) / TILE_SIZE;
    const std::size_t total_tiles = static_cast<std::size_t>(tiles_x) * static_cast<std::size_t>(tiles_y);
    tile_slots_.resize(total_tiles);

    const std::uint8_t *pred_ptr = is_keyframe ? nullptr : pred_bgra_.data();
    std::function<void(int, int)> tile_job = [&](int ty_begin, int ty_end) {
      for (int ty = ty_begin; ty < ty_end; ++ty) {
        const int tile_y = ty * TILE_SIZE;
        const int tile_h = std::min(TILE_SIZE, height - tile_y);
        for (int tx = 0; tx < tiles_x; ++tx) {
          const int tile_x = tx * TILE_SIZE;
          const int tile_w = std::min(TILE_SIZE, width - tile_x);
          auto &slot = tile_slots_[static_cast<std::size_t>(ty) * tiles_x + tx];
          if (!is_keyframe && tiles_equal_rgb24(bgra, stride, pred_ptr, dst_stride, tile_x, tile_y, tile_w, tile_h)) {
            slot.dirty = false;
            slot.payload.clear();
            continue;
          }
          slot.dirty = true;
          slot.mode = encode_tile_best(
            bgra, stride, pred_ptr, dst_stride, tile_x, tile_y, tile_w, tile_h, !is_keyframe, slot.payload);
        }
      }
    };
    run_parallel_rows(tiles_y, tile_job);

    // Compute canonical RGB24 frame hash (parallelize CRC32C by row!)
    std::array<std::uint8_t, 32> frame_hash {};
    if (hash_algo == hash_algo_e::crc32c) {
      std::function<void(int, int)> hash_job = [&](int y_begin, int y_end) {
        for (int y = y_begin; y < y_end; ++y) {
          row_crcs_[static_cast<std::size_t>(y)] = crc32c_rgb24_from_bgra_row(
            bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride), width);
        }
      };
      run_parallel_rows(height, hash_job);
      const std::uint64_t row_bytes_rgb = static_cast<std::uint64_t>(width) * 3ULL;
      std::uint32_t combined = row_crcs_[0];
      for (int y = 1; y < height; ++y) {
        combined = crc32c_combine(combined, row_crcs_[static_cast<std::size_t>(y)], row_bytes_rgb);
      }
      write_u32_le(frame_hash.data(), combined);
    } else if (hash_algo == hash_algo_e::blake3) {
      frame_hash = blake3_rgb24_from_bgra(bgra, stride, width, height);
    }

    // Update prev_bgra_ from curr bgra (canonical stride = width * 4)
    if (stride == dst_stride) {
      std::memcpy(prev_bgra_.data(), bgra, frame_bytes);
    } else {
      for (int y = 0; y < height; ++y) {
        std::memcpy(prev_bgra_.data() + static_cast<std::size_t>(y) * dst_stride,
                    bgra + static_cast<std::size_t>(y) * stride,
                    static_cast<std::size_t>(dst_stride));
      }
    }
    has_prev_ = true;

    // Pack output packet
    const std::size_t copy_bytes = copy_rects.size() * COPY_RECT_WIRE_SIZE;
    const std::size_t bitmap_bytes = (total_tiles + 7u) / 8u;
    std::uint32_t dirty_count = 0;
    std::size_t dirty_bytes = 0;
    encode_stats_t stats {};
    stats.total_tiles = static_cast<std::uint32_t>(total_tiles);
    stats.copy_rects = static_cast<std::uint32_t>(copy_rects.size());

    for (std::size_t i = 0; i < total_tiles; ++i) {
      if (tile_slots_[i].dirty) {
        ++dirty_count;
        dirty_bytes += TILE_HEADER_SIZE + tile_slots_[i].payload.size();
        switch (tile_slots_[i].mode) {
          case tile_mode_e::solid: ++stats.solid_tiles; break;
          case tile_mode_e::palette: ++stats.palette_tiles; break;
          case tile_mode_e::ycocgr_med_rice: ++stats.med_rice_tiles; break;
          case tile_mode_e::ycocgr_temporal_rice: ++stats.temporal_rice_tiles; break;
          case tile_mode_e::raw_rgb24: ++stats.raw_tiles; break;
        }
      }
    }
    stats.dirty_tiles = dirty_count;

    const std::size_t payload_len = copy_bytes + bitmap_bytes + dirty_bytes;
    if (payload_len > MAX_PAYLOAD_BYTES) {
      return false;
    }

    out_packet.resize(HEADER_SIZE + payload_len);
    frame_header_t hdr;
    hdr.version = PROTOCOL_VERSION;
    hdr.flags = is_keyframe ? FLAG_KEYFRAME : 0;
    hdr.frame_id = frame_id;
    hdr.capture_ts_us = capture_ts_us;
    hdr.width = static_cast<std::uint16_t>(width);
    hdr.height = static_cast<std::uint16_t>(height);
    hdr.tile_size = TILE_SIZE;
    hdr.hash_algo = hash_algo;
    hdr.copy_rect_count = static_cast<std::uint8_t>(copy_rects.size());
    hdr.dirty_tile_count = dirty_count;
    hdr.payload_len = static_cast<std::uint32_t>(payload_len);
    hdr.frame_hash = frame_hash;
    write_frame_header(out_packet.data(), hdr);

    std::uint8_t *p = out_packet.data() + HEADER_SIZE;
    for (const auto &r : copy_rects) {
      write_u16_le(p + 0, r.src_x);
      write_u16_le(p + 2, r.src_y);
      write_u16_le(p + 4, r.dst_x);
      write_u16_le(p + 6, r.dst_y);
      write_u16_le(p + 8, r.w);
      write_u16_le(p + 10, r.h);
      p += COPY_RECT_WIRE_SIZE;
    }

    std::memset(p, 0, bitmap_bytes);
    for (std::size_t i = 0; i < total_tiles; ++i) {
      if (tile_slots_[i].dirty) {
        p[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7u));
      }
    }
    p += bitmap_bytes;

    for (std::size_t i = 0; i < total_tiles; ++i) {
      const auto &slot = tile_slots_[i];
      if (!slot.dirty) {
        continue;
      }
      p[0] = static_cast<std::uint8_t>(slot.mode);
      write_u16_le(p + 1, static_cast<std::uint16_t>(slot.payload.size()));
      std::memcpy(p + TILE_HEADER_SIZE, slot.payload.data(), slot.payload.size());
      p += TILE_HEADER_SIZE + slot.payload.size();
    }

    if (out_stats) {
      *out_stats = stats;
    }
    return true;
  }

  // ---------------------------------------------------------------------------
  // cpu_decoder_t
  // ---------------------------------------------------------------------------
  cpu_decoder_t::cpu_decoder_t(int thread_count):
      requested_threads_(thread_count) {
    if (requested_threads_ <= 0) {
      const unsigned hc = std::thread::hardware_concurrency();
      requested_threads_ = static_cast<int>(std::clamp(hc ? hc : 4u, 2u, 8u));
    }
  }

  cpu_decoder_t::~cpu_decoder_t() {
    stop_workers();
  }

  void cpu_decoder_t::reset() {
    has_prev_ = false;
    width_ = 0;
    height_ = 0;
    prev_bgra_.clear();
    curr_bgra_.clear();
  }

  void cpu_decoder_t::ensure_workers(int count) {
    if (!workers_.empty() || count <= 1) {
      return;
    }
    const int spawn = count - 1;
    workers_.reserve(spawn);
    for (int i = 0; i < spawn; ++i) {
      workers_.emplace_back([this, idx = i + 1, total = count]() {
        std::uint64_t seen_epoch = 0;
        while (true) {
          const std::function<void(int, int)> *fn = nullptr;
          int count_items = 0;
          {
            std::unique_lock lock {pool_mutex_};
            pool_cv_.wait(lock, [&]() {
              return stopping_ || job_epoch_ != seen_epoch;
            });
            if (stopping_) {
              return;
            }
            seen_epoch = job_epoch_;
            fn = job_fn_;
            count_items = job_count_;
          }
          const int b = (count_items * idx) / total;
          const int e = (count_items * (idx + 1)) / total;
          if (fn && b < e) {
            (*fn)(b, e);
          }
          {
            std::lock_guard lock {pool_mutex_};
            if (--pending_workers_ == 0) {
              pool_done_cv_.notify_one();
            }
          }
        }
      });
    }
  }

  void cpu_decoder_t::stop_workers() {
    {
      std::lock_guard lock {pool_mutex_};
      stopping_ = true;
    }
    pool_cv_.notify_all();
    for (auto &t : workers_) {
      if (t.joinable()) {
        t.join();
      }
    }
    workers_.clear();
    stopping_ = false;
  }

  void cpu_decoder_t::run_parallel_jobs(int count, const std::function<void(int, int)> &fn) {
    if (count <= 1 || requested_threads_ <= 1) {
      fn(0, count);
      return;
    }
    ensure_workers(requested_threads_);
    const int total = static_cast<int>(workers_.size()) + 1;
    {
      std::lock_guard lock {pool_mutex_};
      job_fn_ = &fn;
      job_count_ = count;
      pending_workers_ = total - 1;
      ++job_epoch_;
    }
    pool_cv_.notify_all();
    const int b = 0;
    const int e = count / total;
    if (b < e) {
      fn(b, e);
    }
    {
      std::unique_lock lock {pool_mutex_};
      pool_done_cv_.wait(lock, [&]() { return pending_workers_ == 0; });
    }
  }

  bool cpu_decoder_t::decode_frame(
    const std::uint8_t *packet,
    std::size_t packet_len,
    bool verify_hash,
    frame_header_t *out_header,
    decode_stats_t *out_stats
  ) {
    parsed_frame_t parsed;
    if (!parse_frame_packet(packet, packet_len, parsed)) {
      return false;
    }

    const int w = parsed.header.width;
    const int h = parsed.header.height;
    const int stride = w * 4;
    const bool is_keyframe = (parsed.header.flags & FLAG_KEYFRAME) != 0;
    const bool dims_changed = (w != width_ || h != height_);

    if (!is_keyframe && (!has_prev_ || dims_changed)) {
      return false;
    }

    const int tiles_x = (w + TILE_SIZE - 1) / TILE_SIZE;
    const int tiles_y = (h + TILE_SIZE - 1) / TILE_SIZE;
    const std::size_t total_tiles = static_cast<std::size_t>(tiles_x) * static_cast<std::size_t>(tiles_y);

    if (is_keyframe && (parsed.header.copy_rect_count != 0 || parsed.header.dirty_tile_count != total_tiles)) {
      return false;
    }

    dirty_refs_.clear();
    dirty_refs_.reserve(parsed.header.dirty_tile_count);
    std::size_t offset = 0;

    for (std::size_t idx = 0; idx < total_tiles; ++idx) {
      if (!((parsed.skip_bitmap[idx >> 3] >> (idx & 7u)) & 1u)) {
        continue;
      }
      if (offset + TILE_HEADER_SIZE > parsed.tiles_data_bytes) {
        return false;
      }
      const std::uint8_t mode_raw = parsed.tiles_data[offset];
      const std::uint16_t len = read_u16_le(parsed.tiles_data + offset + 1);
      offset += TILE_HEADER_SIZE;
      if (len > MAX_TILE_PAYLOAD || offset + len > parsed.tiles_data_bytes) {
        return false;
      }
      if (mode_raw < static_cast<std::uint8_t>(tile_mode_e::solid) ||
          mode_raw > static_cast<std::uint8_t>(tile_mode_e::raw_rgb24)) {
        return false;
      }
      if (is_keyframe && mode_raw == static_cast<std::uint8_t>(tile_mode_e::ycocgr_temporal_rice)) {
        return false;
      }
      dirty_refs_.push_back(dirty_ref_t {
        static_cast<std::uint16_t>(idx % static_cast<std::size_t>(tiles_x)),
        static_cast<std::uint16_t>(idx / static_cast<std::size_t>(tiles_x)),
        static_cast<tile_mode_e>(mode_raw),
        parsed.tiles_data + offset,
        len,
      });
      offset += len;
    }

    if (offset != parsed.tiles_data_bytes) {
      return false;
    }

    const std::size_t frame_bytes = static_cast<std::size_t>(stride) * static_cast<std::size_t>(h);
    if (dims_changed) {
      width_ = w;
      height_ = h;
      prev_bgra_.resize(frame_bytes);
      curr_bgra_.resize(frame_bytes);
      row_crcs_.resize(static_cast<std::size_t>(h));
    }

    if (!is_keyframe) {
      // Zero-copy ping-pong: previous decoded frame in curr_bgra_ becomes prev_bgra_
      prev_bgra_.swap(curr_bgra_);
      apply_copy_rects_bgra(prev_bgra_.data(), stride, curr_bgra_.data(), stride, w, h, parsed.copy_rects);
    }

    std::atomic_bool decode_ok {true};

    std::function<void(int, int)> decode_job = [&](int begin_idx, int end_idx) {
      for (int i = begin_idx; i < end_idx; ++i) {
        if (!decode_ok.load(std::memory_order_relaxed)) {
          return;
        }
        const dirty_ref_t &ref = dirty_refs_[static_cast<std::size_t>(i)];
        const int tile_x = static_cast<int>(ref.tx) * TILE_SIZE;
        const int tile_y = static_cast<int>(ref.ty) * TILE_SIZE;
        const int tile_w = std::min(TILE_SIZE, w - tile_x);
        const int tile_h = std::min(TILE_SIZE, h - tile_y);
        if (!decode_tile_to_bgra(
              ref.mode,
              ref.payload,
              ref.payload_len,
              curr_bgra_.data(),
              stride,
              curr_bgra_.data(),
              stride,
              tile_x,
              tile_y,
              tile_w,
              tile_h)) {
          decode_ok.store(false, std::memory_order_relaxed);
          return;
        }
      }
    };
    run_parallel_jobs(static_cast<int>(dirty_refs_.size()), decode_job);

    if (!decode_ok.load(std::memory_order_acquire)) {
      has_prev_ = false;
      return false;
    }

    decode_stats_t stats {};
    stats.dirty_tiles = parsed.header.dirty_tile_count;
    stats.copy_rects = parsed.header.copy_rect_count;
    if (verify_hash && parsed.header.hash_algo != hash_algo_e::none) {
      const auto hash_t0 = std::chrono::steady_clock::now();
      stats.hash_verified = true;
      if (parsed.header.hash_algo == hash_algo_e::crc32c) {
        std::function<void(int, int)> hash_job = [&](int y_begin, int y_end) {
          for (int y = y_begin; y < y_end; ++y) {
            row_crcs_[static_cast<std::size_t>(y)] = crc32c_rgb24_from_bgra_row(
              curr_bgra_.data() + static_cast<std::size_t>(y) * stride, w);
          }
        };
        run_parallel_jobs(h, hash_job);
        const std::uint64_t row_bytes_rgb = static_cast<std::uint64_t>(w) * 3ULL;
        std::uint32_t combined = row_crcs_[0];
        for (int y = 1; y < h; ++y) {
          combined = crc32c_combine(combined, row_crcs_[static_cast<std::size_t>(y)], row_bytes_rgb);
        }
        write_u32_le(stats.computed_hash.data(), combined);
      } else {
        stats.computed_hash = blake3_rgb24_from_bgra(curr_bgra_.data(), stride, w, h);
      }
      stats.hash_matched = (stats.computed_hash == parsed.header.frame_hash);
      const auto hash_t1 = std::chrono::steady_clock::now();
      stats.hash_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(hash_t1 - hash_t0).count());
    }

    has_prev_ = true;

    if (out_header) {
      *out_header = parsed.header;
    }
    if (out_stats) {
      *out_stats = stats;
    }
    return true;
  }

}  // namespace sds::lossless
