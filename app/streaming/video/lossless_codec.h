/**
 * @file src/sds/lossless_codec.h
 * @brief Bit-exact SDLL v1 lossless desktop codec (32x32 tiles, scroll CopyRect, YCoCg-R + MED/Temporal Golomb-Rice, Palette, Solid, Raw).
 */
#pragma once
#ifndef SDS_LOSSLESS_CODEC_H
#define SDS_LOSSLESS_CODEC_H

#include "lossless_hash.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace sds::lossless {

  inline constexpr std::array<std::uint8_t, 4> SDLL_MAGIC {'S', 'D', 'L', 'L'};
  inline constexpr std::array<std::uint8_t, 4> SDLC_MAGIC {'S', 'D', 'L', 'C'};
  inline constexpr std::uint16_t PROTOCOL_VERSION = 1;

  inline constexpr std::size_t HEADER_SIZE = 72;
  inline constexpr std::size_t COPY_RECT_WIRE_SIZE = 12;
  inline constexpr std::size_t TILE_HEADER_SIZE = 3;
  inline constexpr std::size_t CONTROL_MSG_SIZE = 12;

  inline constexpr int TILE_SIZE = 32;
  inline constexpr int MAX_TILE_PIXELS = TILE_SIZE * TILE_SIZE;  // 1024
  inline constexpr int MAX_TILE_PAYLOAD = MAX_TILE_PIXELS * 3;   // 3072 bytes (RAW_RGB24 ceiling)
  inline constexpr int GPU_TILE_SLOT_STRIDE = 3088;              // 16-byte aligned fixed slot for GPU shaders
  inline constexpr int MAX_COPY_RECTS = 4;
  inline constexpr int MAX_FRAME_DIM = 16384;
  inline constexpr std::size_t MAX_PAYLOAD_BYTES = 128u * 1024u * 1024u;
  inline constexpr int RICE_ESCAPE_Q = 16;

  inline constexpr std::uint16_t FLAG_KEYFRAME = 0x0001u;
  inline constexpr std::uint16_t FLAG_GPU_ENCODED = 0x0002u;

  enum class tile_mode_e : std::uint8_t {
    solid = 1,
    palette = 2,
    ycocgr_med_rice = 3,
    ycocgr_temporal_rice = 4,
    raw_rgb24 = 5,
  };

  enum class control_msg_e : std::uint16_t {
    request_keyframe = 1,
    set_hash_algo = 2,
    ack_frame = 3,
  };

  struct copy_rect_t {
    std::uint16_t src_x = 0;
    std::uint16_t src_y = 0;
    std::uint16_t dst_x = 0;
    std::uint16_t dst_y = 0;
    std::uint16_t w = 0;
    std::uint16_t h = 0;

    bool operator==(const copy_rect_t &o) const {
      return src_x == o.src_x && src_y == o.src_y &&
             dst_x == o.dst_x && dst_y == o.dst_y &&
             w == o.w && h == o.h;
    }
  };

  struct frame_header_t {
    std::uint16_t version = PROTOCOL_VERSION;
    std::uint16_t flags = 0;
    std::uint64_t frame_id = 0;
    std::uint64_t capture_ts_us = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t tile_size = TILE_SIZE;
    hash_algo_e hash_algo = hash_algo_e::crc32c;
    std::uint8_t copy_rect_count = 0;
    std::uint32_t dirty_tile_count = 0;
    std::uint32_t payload_len = 0;
    std::array<std::uint8_t, 32> frame_hash {};
  };

  struct parsed_frame_t {
    frame_header_t header {};
    std::vector<copy_rect_t> copy_rects;
    const std::uint8_t *skip_bitmap = nullptr;
    std::size_t skip_bitmap_bytes = 0;
    const std::uint8_t *tiles_data = nullptr;
    std::size_t tiles_data_bytes = 0;
  };

  struct control_msg_t {
    control_msg_e type = control_msg_e::request_keyframe;
    std::uint32_t arg = 0;
  };

  struct encode_stats_t {
    std::uint32_t total_tiles = 0;
    std::uint32_t dirty_tiles = 0;
    std::uint32_t copy_rects = 0;
    std::uint32_t solid_tiles = 0;
    std::uint32_t palette_tiles = 0;
    std::uint32_t med_rice_tiles = 0;
    std::uint32_t temporal_rice_tiles = 0;
    std::uint32_t raw_tiles = 0;
  };

  struct decode_stats_t {
    bool hash_verified = false;
    bool hash_matched = false;
    std::uint32_t dirty_tiles = 0;
    std::uint32_t copy_rects = 0;
    std::uint64_t hash_us = 0;
    std::array<std::uint8_t, 32> computed_hash {};
  };

  // Pure integer reversible YCoCg-R (bit-exact across all 16,777,216 RGB8 triples)
  constexpr inline std::int32_t floor_div2(std::int32_t v) {
    return (v >= 0) ? (v >> 1) : -((-v + 1) >> 1);
  }

  inline void rgb8_to_ycocgr(
    std::uint8_t r,
    std::uint8_t g,
    std::uint8_t b,
    std::int32_t &out_y,
    std::int32_t &out_co,
    std::int32_t &out_cg
  ) {
    const std::int32_t ri = r;
    const std::int32_t gi = g;
    const std::int32_t bi = b;
    out_co = ri - bi;
    const std::int32_t t = bi + floor_div2(out_co);
    out_cg = gi - t;
    out_y = t + floor_div2(out_cg);
  }

  inline void ycocgr_to_rgb8(
    std::int32_t y,
    std::int32_t co,
    std::int32_t cg,
    std::uint8_t &out_r,
    std::uint8_t &out_g,
    std::uint8_t &out_b
  ) {
    const std::int32_t t = y - floor_div2(cg);
    const std::int32_t g = cg + t;
    const std::int32_t b = t - floor_div2(co);
    const std::int32_t r = b + co;
    out_r = static_cast<std::uint8_t>(r);
    out_g = static_cast<std::uint8_t>(g);
    out_b = static_cast<std::uint8_t>(b);
  }

  // Header & control serialization
  void write_frame_header(std::uint8_t out[HEADER_SIZE], const frame_header_t &h);
  bool parse_frame_header(const std::uint8_t *data, std::size_t len, frame_header_t &out);
  bool parse_frame_packet(const std::uint8_t *data, std::size_t len, parsed_frame_t &out);

  struct alignas(16) gpu_tile_desc_t {
    std::uint32_t tile_x = 0;
    std::uint32_t tile_y = 0;
    std::uint32_t tile_w = 0;
    std::uint32_t tile_h = 0;
    std::uint32_t mode = 0;
    std::uint32_t payload_offset = 0;
    std::uint32_t payload_len = 0;
    std::uint32_t pad0 = 0;
  };

  bool build_dirty_tile_descs(
    const parsed_frame_t &parsed,
    const std::uint8_t *packet_base,
    std::vector<gpu_tile_desc_t> &out_descs
  );

  void serialize_control_msg(control_msg_e type, std::uint32_t arg, std::uint8_t out[CONTROL_MSG_SIZE]);
  bool parse_control_msg(const std::uint8_t *data, std::size_t len, control_msg_t &out);

  // Scroll CopyRect detection & BGRA ping-pong application
  std::vector<copy_rect_t> detect_scroll_copy_rects(
    const std::uint8_t *prev_bgra,
    int prev_stride,
    const std::uint8_t *curr_bgra,
    int curr_stride,
    int width,
    int height
  );

  bool validate_copy_rect(const copy_rect_t &r, int width, int height);

  void apply_copy_rects_bgra(
    const std::uint8_t *prev_bgra,
    int prev_stride,
    std::uint8_t *dst_bgra,
    int dst_stride,
    int width,
    int height,
    const std::vector<copy_rect_t> &rects
  );

  // Single-tile encode / decode primitives (used by both CPU encoder/decoder and GPU cross-verification)
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
  );

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
  );

  /**
   * @brief Multi-threaded CPU SIMD bit-exact encoder with persistent reference buffer.
   */
  class cpu_encoder_t {
  public:
    explicit cpu_encoder_t(int thread_count = 0);
    ~cpu_encoder_t();

    cpu_encoder_t(const cpu_encoder_t &) = delete;
    cpu_encoder_t &operator=(const cpu_encoder_t &) = delete;

    void reset();

    bool encode_frame(
      const std::uint8_t *bgra,
      int stride,
      int width,
      int height,
      std::uint64_t frame_id,
      std::uint64_t capture_ts_us,
      hash_algo_e hash_algo,
      bool force_keyframe,
      std::vector<std::uint8_t> &out_packet,
      encode_stats_t *out_stats = nullptr
    );

  private:
    void ensure_workers(int count);
    void stop_workers();
    void run_parallel_rows(int rows, const std::function<void(int row_begin, int row_end)> &fn);

    int requested_threads_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool has_prev_ = false;
    std::vector<std::uint8_t> prev_bgra_;
    std::vector<std::uint8_t> pred_bgra_;
    std::vector<std::uint32_t> row_crcs_;

    struct tile_slot_t {
      bool dirty = false;
      tile_mode_e mode = tile_mode_e::raw_rgb24;
      std::vector<std::uint8_t> payload;
    };
    std::vector<tile_slot_t> tile_slots_;

    // Persistent worker pool for sub-millisecond dispatch
    std::mutex pool_mutex_;
    std::condition_variable pool_cv_;
    std::condition_variable pool_done_cv_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    std::uint64_t job_epoch_ = 0;
    int pending_workers_ = 0;
    int job_rows_ = 0;
    const std::function<void(int, int)> *job_fn_ = nullptr;
  };

  /**
   * @brief Multi-threaded CPU bit-exact decoder with ping-pong reference buffers and hash verification.
   */
  class cpu_decoder_t {
  public:
    explicit cpu_decoder_t(int thread_count = 0);
    ~cpu_decoder_t();

    cpu_decoder_t(const cpu_decoder_t &) = delete;
    cpu_decoder_t &operator=(const cpu_decoder_t &) = delete;

    void reset();

    bool decode_frame(
      const std::uint8_t *packet,
      std::size_t packet_len,
      bool verify_hash,
      frame_header_t *out_header = nullptr,
      decode_stats_t *out_stats = nullptr
    );

    const std::uint8_t *current_bgra() const {
      return curr_bgra_.data();
    }

    int width() const { return width_; }
    int height() const { return height_; }
    int stride() const { return width_ * 4; }

  private:
    void ensure_workers(int count);
    void stop_workers();
    void run_parallel_jobs(int count, const std::function<void(int idx_begin, int idx_end)> &fn);

    int requested_threads_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool has_prev_ = false;
    std::vector<std::uint8_t> prev_bgra_;
    std::vector<std::uint8_t> curr_bgra_;
    std::vector<std::uint32_t> row_crcs_;

    struct dirty_ref_t {
      std::uint16_t tx = 0;
      std::uint16_t ty = 0;
      tile_mode_e mode = tile_mode_e::raw_rgb24;
      const std::uint8_t *payload = nullptr;
      std::uint16_t payload_len = 0;
    };
    std::vector<dirty_ref_t> dirty_refs_;

    std::mutex pool_mutex_;
    std::condition_variable pool_cv_;
    std::condition_variable pool_done_cv_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    std::uint64_t job_epoch_ = 0;
    int pending_workers_ = 0;
    int job_count_ = 0;
    const std::function<void(int, int)> *job_fn_ = nullptr;
  };

}  // namespace sds::lossless
#endif  // SDS_LOSSLESS_CODEC_H
