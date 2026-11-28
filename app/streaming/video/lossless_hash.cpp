/**
 * @file src/sds/lossless_hash.cpp
 * @brief Hardware-accelerated CRC32C (with GF(2) combine) and self-contained BLAKE3-256.
 */

#include "lossless_hash.h"

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(__aarch64__) || defined(_M_ARM64)
  #if defined(__ARM_FEATURE_CRC32)
    #include <arm_acle.h>
    #define SDS_HAVE_ARM_CRC32 1
  #endif
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #if defined(_MSC_VER)
    #include <intrin.h>
    #define SDS_HAVE_X86_CRC32 1
  #elif defined(__GNUC__) || defined(__clang__)
    #include <cpuid.h>
    #include <nmmintrin.h>
    #define SDS_HAVE_X86_CRC32 1
  #endif
#endif

namespace sds::lossless {
  namespace {
    constexpr std::uint32_t CRC32C_POLY = 0x82F63B78u;

    struct crc32c_tables_t {
      std::array<std::array<std::uint32_t, 256>, 8> t {};

      constexpr crc32c_tables_t() {
        for (std::uint32_t i = 0; i < 256; ++i) {
          std::uint32_t c = i;
          for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? ((c >> 1) ^ CRC32C_POLY) : (c >> 1);
          }
          t[0][i] = c;
        }
        for (int slice = 1; slice < 8; ++slice) {
          for (std::uint32_t i = 0; i < 256; ++i) {
            const std::uint32_t prev = t[slice - 1][i];
            t[slice][i] = (prev >> 8) ^ t[0][prev & 0xFFu];
          }
        }
      }
    };

    constexpr crc32c_tables_t kCrcTables {};

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

    inline void write_u32_le(std::uint8_t *p, std::uint32_t v) {
      p[0] = static_cast<std::uint8_t>(v & 0xFFu);
      p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
      p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
      p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    }

    [[maybe_unused]] std::uint32_t crc32c_sw(std::uint32_t crc, const std::uint8_t *data, std::size_t len) {
      std::uint32_t c = crc ^ 0xFFFFFFFFu;
      while (len >= 8) {
        const std::uint32_t lo = read_u32_le(data) ^ c;
        const std::uint32_t hi = read_u32_le(data + 4);
        c = kCrcTables.t[7][lo & 0xFFu] ^
            kCrcTables.t[6][(lo >> 8) & 0xFFu] ^
            kCrcTables.t[5][(lo >> 16) & 0xFFu] ^
            kCrcTables.t[4][(lo >> 24) & 0xFFu] ^
            kCrcTables.t[3][hi & 0xFFu] ^
            kCrcTables.t[2][(hi >> 8) & 0xFFu] ^
            kCrcTables.t[1][(hi >> 16) & 0xFFu] ^
            kCrcTables.t[0][(hi >> 24) & 0xFFu];
        data += 8;
        len -= 8;
      }
      while (len > 0) {
        c = (c >> 8) ^ kCrcTables.t[0][(c ^ *data++) & 0xFFu];
        --len;
      }
      return c ^ 0xFFFFFFFFu;
    }

#ifdef SDS_HAVE_X86_CRC32
    bool cpu_has_sse42() {
      static const bool supported = []() {
  #if defined(_MSC_VER)
        int info[4] = {};
        __cpuid(info, 1);
        return (info[2] & (1 << 20)) != 0;
  #else
        unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
          return (ecx & bit_SSE4_2) != 0;
        }
        return false;
  #endif
      }();
      return supported;
    }

  #if defined(__GNUC__) || defined(__clang__)
    __attribute__((target("sse4.2")))
  #endif
    std::uint32_t crc32c_x86_hw(std::uint32_t crc, const std::uint8_t *data, std::size_t len) {
      std::uint32_t c = crc ^ 0xFFFFFFFFu;
  #if defined(__x86_64__) || defined(_M_X64)
      std::uint64_t c64 = c;
      while (len >= 8) {
        c64 = _mm_crc32_u64(c64, read_u64_le(data));
        data += 8;
        len -= 8;
      }
      c = static_cast<std::uint32_t>(c64);
  #else
      while (len >= 4) {
        c = _mm_crc32_u32(c, read_u32_le(data));
        data += 4;
        len -= 4;
      }
  #endif
      while (len > 0) {
        c = _mm_crc32_u8(c, *data++);
        --len;
      }
      return c ^ 0xFFFFFFFFu;
    }
#endif

#ifdef SDS_HAVE_ARM_CRC32
    std::uint32_t crc32c_arm_hw(std::uint32_t crc, const std::uint8_t *data, std::size_t len) {
      std::uint32_t c = crc ^ 0xFFFFFFFFu;
      while (len >= 8) {
        c = __crc32cd(c, read_u64_le(data));
        data += 8;
        len -= 8;
      }
      if (len >= 4) {
        c = __crc32cw(c, read_u32_le(data));
        data += 4;
        len -= 4;
      }
      while (len > 0) {
        c = __crc32cb(c, *data++);
        --len;
      }
      return c ^ 0xFFFFFFFFu;
    }
#endif

    // GF(2) helpers for crc32c_combine (identical to zlib gf2_matrix_times / square)
    std::uint32_t gf2_matrix_times(const std::array<std::uint32_t, 32> &mat, std::uint32_t vec) {
      std::uint32_t sum = 0;
      std::size_t idx = 0;
      while (vec) {
        if (vec & 1u) {
          sum ^= mat[idx];
        }
        vec >>= 1;
        ++idx;
      }
      return sum;
    }

    void gf2_matrix_square(std::array<std::uint32_t, 32> &square, const std::array<std::uint32_t, 32> &mat) {
      for (std::size_t n = 0; n < 32; ++n) {
        square[n] = gf2_matrix_times(mat, mat[n]);
      }
    }

    // -------------------------------------------------------------------------
    // Self-contained BLAKE3-256 implementation (per official spec)
    // -------------------------------------------------------------------------
    constexpr std::array<std::uint32_t, 8> BLAKE3_IV {
      0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
      0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
    };

    constexpr std::uint32_t CHUNK_START = 1u << 0;
    constexpr std::uint32_t CHUNK_END = 1u << 1;
    constexpr std::uint32_t PARENT = 1u << 2;
    constexpr std::uint32_t ROOT = 1u << 3;

    constexpr std::size_t BLAKE3_BLOCK_LEN = 64;
    constexpr std::size_t BLAKE3_CHUNK_LEN = 1024;

    constexpr std::array<std::uint8_t, 16> MSG_PERMUTATION {
      2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8
    };

    inline std::uint32_t rotr32(std::uint32_t x, int n) {
      return (x >> n) | (x << (32 - n));
    }

    inline void g(
      std::array<std::uint32_t, 16> &state,
      std::size_t a,
      std::size_t b,
      std::size_t c,
      std::size_t d,
      std::uint32_t mx,
      std::uint32_t my
    ) {
      state[a] = state[a] + state[b] + mx;
      state[d] = rotr32(state[d] ^ state[a], 16);
      state[c] = state[c] + state[d];
      state[b] = rotr32(state[b] ^ state[c], 12);
      state[a] = state[a] + state[b] + my;
      state[d] = rotr32(state[d] ^ state[a], 8);
      state[c] = state[c] + state[d];
      state[b] = rotr32(state[b] ^ state[c], 7);
    }

    void blake3_round(std::array<std::uint32_t, 16> &state, const std::array<std::uint32_t, 16> &m) {
      g(state, 0, 4, 8, 12, m[0], m[1]);
      g(state, 1, 5, 9, 13, m[2], m[3]);
      g(state, 2, 6, 10, 14, m[4], m[5]);
      g(state, 3, 7, 11, 15, m[6], m[7]);
      g(state, 0, 5, 10, 15, m[8], m[9]);
      g(state, 1, 6, 11, 12, m[10], m[11]);
      g(state, 2, 7, 8, 13, m[12], m[13]);
      g(state, 3, 4, 9, 14, m[14], m[15]);
    }

    std::array<std::uint32_t, 16> blake3_compress(
      const std::array<std::uint32_t, 8> &cv,
      const std::uint8_t block[BLAKE3_BLOCK_LEN],
      std::uint8_t block_len,
      std::uint64_t counter,
      std::uint32_t flags
    ) {
      std::array<std::uint32_t, 16> m {};
      for (std::size_t i = 0; i < 16; ++i) {
        m[i] = read_u32_le(block + i * 4);
      }

      std::array<std::uint32_t, 16> state {
        cv[0], cv[1], cv[2], cv[3],
        cv[4], cv[5], cv[6], cv[7],
        BLAKE3_IV[0], BLAKE3_IV[1], BLAKE3_IV[2], BLAKE3_IV[3],
        static_cast<std::uint32_t>(counter),
        static_cast<std::uint32_t>(counter >> 32),
        static_cast<std::uint32_t>(block_len),
        flags,
      };

      for (int r = 0; r < 7; ++r) {
        blake3_round(state, m);
        if (r < 6) {
          std::array<std::uint32_t, 16> permuted {};
          for (std::size_t i = 0; i < 16; ++i) {
            permuted[i] = m[MSG_PERMUTATION[i]];
          }
          m = permuted;
        }
      }

      for (std::size_t i = 0; i < 8; ++i) {
        state[i] ^= state[i + 8];
        state[i + 8] ^= cv[i];
      }
      return state;
    }

    struct blake3_output_t {
      std::array<std::uint32_t, 8> input_cv {};
      std::array<std::uint8_t, BLAKE3_BLOCK_LEN> block {};
      std::uint8_t block_len = 0;
      std::uint64_t counter = 0;
      std::uint32_t flags = 0;

      std::array<std::uint32_t, 8> chaining_value() const {
        const auto out = blake3_compress(input_cv, block.data(), block_len, counter, flags);
        std::array<std::uint32_t, 8> cv {};
        std::copy_n(out.begin(), 8, cv.begin());
        return cv;
      }

      std::array<std::uint8_t, 32> root_hash() const {
        const auto out = blake3_compress(input_cv, block.data(), block_len, counter, flags | ROOT);
        std::array<std::uint8_t, 32> digest {};
        for (std::size_t i = 0; i < 8; ++i) {
          write_u32_le(digest.data() + i * 4, out[i]);
        }
        return digest;
      }
    };

    struct blake3_chunk_state_t {
      std::array<std::uint32_t, 8> cv = BLAKE3_IV;
      std::uint64_t chunk_counter = 0;
      std::array<std::uint8_t, BLAKE3_BLOCK_LEN> block {};
      std::uint8_t block_len = 0;
      std::uint8_t blocks_compressed = 0;

      void reset(std::uint64_t counter) {
        cv = BLAKE3_IV;
        chunk_counter = counter;
        block.fill(0);
        block_len = 0;
        blocks_compressed = 0;
      }

      std::size_t len() const {
        return static_cast<std::size_t>(blocks_compressed) * BLAKE3_BLOCK_LEN + block_len;
      }

      std::uint32_t start_flag() const {
        return blocks_compressed == 0 ? CHUNK_START : 0u;
      }

      void update(const std::uint8_t *input, std::size_t input_len) {
        while (input_len > 0) {
          if (block_len == BLAKE3_BLOCK_LEN) {
            const auto out = blake3_compress(cv, block.data(), BLAKE3_BLOCK_LEN, chunk_counter, start_flag());
            std::copy_n(out.begin(), 8, cv.begin());
            blocks_compressed++;
            block.fill(0);
            block_len = 0;
          }
          const std::size_t want = BLAKE3_BLOCK_LEN - block_len;
          const std::size_t take = std::min(want, input_len);
          std::memcpy(block.data() + block_len, input, take);
          block_len = static_cast<std::uint8_t>(block_len + take);
          input += take;
          input_len -= take;
        }
      }

      blake3_output_t output() const {
        blake3_output_t out;
        out.input_cv = cv;
        out.block = block;
        out.block_len = block_len;
        out.counter = chunk_counter;
        out.flags = start_flag() | CHUNK_END;
        return out;
      }
    };

    blake3_output_t make_parent_output(
      const std::array<std::uint32_t, 8> &left_cv,
      const std::array<std::uint32_t, 8> &right_cv
    ) {
      blake3_output_t out;
      out.input_cv = BLAKE3_IV;
      for (std::size_t i = 0; i < 8; ++i) {
        write_u32_le(out.block.data() + i * 4, left_cv[i]);
        write_u32_le(out.block.data() + 32 + i * 4, right_cv[i]);
      }
      out.block_len = BLAKE3_BLOCK_LEN;
      out.counter = 0;
      out.flags = PARENT;
      return out;
    }

    class blake3_hasher_t {
    public:
      void update(const std::uint8_t *input, std::size_t input_len) {
        while (input_len > 0) {
          if (chunk_state_.len() == BLAKE3_CHUNK_LEN) {
            const auto chunk_cv = chunk_state_.output().chaining_value();
            const std::uint64_t total_chunks = chunk_state_.chunk_counter + 1;
            add_chunk_cv(chunk_cv, total_chunks);
            chunk_state_.reset(total_chunks);
          }
          const std::size_t want = BLAKE3_CHUNK_LEN - chunk_state_.len();
          const std::size_t take = std::min(want, input_len);
          chunk_state_.update(input, take);
          input += take;
          input_len -= take;
        }
      }

      std::array<std::uint8_t, 32> finalize() const {
        blake3_output_t out = chunk_state_.output();
        std::size_t parent_idx = cv_stack_len_;
        while (parent_idx > 0) {
          --parent_idx;
          out = make_parent_output(cv_stack_[parent_idx], out.chaining_value());
        }
        return out.root_hash();
      }

    private:
      void add_chunk_cv(std::array<std::uint32_t, 8> new_cv, std::uint64_t total_chunks) {
        while ((total_chunks & 1u) == 0) {
          --cv_stack_len_;
          new_cv = make_parent_output(cv_stack_[cv_stack_len_], new_cv).chaining_value();
          total_chunks >>= 1;
        }
        cv_stack_[cv_stack_len_++] = new_cv;
      }

      blake3_chunk_state_t chunk_state_;
      std::array<std::array<std::uint32_t, 8>, 54> cv_stack_ {};
      std::size_t cv_stack_len_ = 0;
    };
  }  // namespace

  std::uint32_t crc32c_update(std::uint32_t crc, const std::uint8_t *data, std::size_t len) {
    if (!data || len == 0) {
      return crc;
    }
#ifdef SDS_HAVE_ARM_CRC32
    return crc32c_arm_hw(crc, data, len);
#elif defined(SDS_HAVE_X86_CRC32)
    if (cpu_has_sse42()) {
      return crc32c_x86_hw(crc, data, len);
    }
    return crc32c_sw(crc, data, len);
#else
    return crc32c_sw(crc, data, len);
#endif
  }

  std::uint32_t crc32c_combine(std::uint32_t crc1, std::uint32_t crc2, std::uint64_t len2) {
    if (len2 == 0) {
      return crc1;
    }

    std::array<std::uint32_t, 32> odd {};
    std::array<std::uint32_t, 32> even {};

    odd[0] = CRC32C_POLY;
    std::uint32_t row = 1;
    for (std::size_t n = 1; n < 32; ++n) {
      odd[n] = row;
      row <<= 1;
    }

    gf2_matrix_square(even, odd);
    gf2_matrix_square(odd, even);

    do {
      gf2_matrix_square(even, odd);
      if (len2 & 1ULL) {
        crc1 = gf2_matrix_times(even, crc1);
      }
      len2 >>= 1;
      if (len2 == 0) {
        break;
      }
      gf2_matrix_square(odd, even);
      if (len2 & 1ULL) {
        crc1 = gf2_matrix_times(odd, crc1);
      }
      len2 >>= 1;
    } while (len2 != 0);

    return crc1 ^ crc2;
  }

  std::uint32_t crc32c_rgb24_from_bgra_row(const std::uint8_t *bgra, int width) {
    if (!bgra || width <= 0) {
      return 0;
    }
    std::uint8_t stack_buf[4096 * 3];
    std::vector<std::uint8_t> heap_buf;
    const std::size_t rgb_len = static_cast<std::size_t>(width) * 3u;
    std::uint8_t *dst = stack_buf;
    if (rgb_len > sizeof(stack_buf)) {
      heap_buf.resize(rgb_len);
      dst = heap_buf.data();
    }

    int x = 0;
    for (; x + 4 <= width; x += 4) {
      const std::uint8_t *s = bgra + static_cast<std::size_t>(x) * 4u;
      std::uint8_t *d = dst + static_cast<std::size_t>(x) * 3u;
      d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
      d[3] = s[6]; d[4] = s[5]; d[5] = s[4];
      d[6] = s[10]; d[7] = s[9]; d[8] = s[8];
      d[9] = s[14]; d[10] = s[13]; d[11] = s[12];
    }
    for (; x < width; ++x) {
      const std::uint8_t *s = bgra + static_cast<std::size_t>(x) * 4u;
      std::uint8_t *d = dst + static_cast<std::size_t>(x) * 3u;
      d[0] = s[2];
      d[1] = s[1];
      d[2] = s[0];
    }
    return crc32c_update(0, dst, rgb_len);
  }

  std::uint32_t crc32c_rgb24_from_bgra(const std::uint8_t *bgra, int stride, int width, int height) {
    if (!bgra || width <= 0 || height <= 0 || stride < width * 4) {
      return 0;
    }
    std::uint8_t stack_buf[4096 * 3];
    std::vector<std::uint8_t> heap_buf;
    const std::size_t rgb_len = static_cast<std::size_t>(width) * 3u;
    std::uint8_t *dst = stack_buf;
    if (rgb_len > sizeof(stack_buf)) {
      heap_buf.resize(rgb_len);
      dst = heap_buf.data();
    }

    std::uint32_t crc = 0;
    for (int y = 0; y < height; ++y) {
      const std::uint8_t *row = bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
      int x = 0;
      for (; x + 4 <= width; x += 4) {
        const std::uint8_t *s = row + static_cast<std::size_t>(x) * 4u;
        std::uint8_t *d = dst + static_cast<std::size_t>(x) * 3u;
        d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
        d[3] = s[6]; d[4] = s[5]; d[5] = s[4];
        d[6] = s[10]; d[7] = s[9]; d[8] = s[8];
        d[9] = s[14]; d[10] = s[13]; d[11] = s[12];
      }
      for (; x < width; ++x) {
        const std::uint8_t *s = row + static_cast<std::size_t>(x) * 4u;
        std::uint8_t *d = dst + static_cast<std::size_t>(x) * 3u;
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
      }
      crc = crc32c_update(crc, dst, rgb_len);
    }
    return crc;
  }

  std::array<std::uint8_t, 32> blake3_hash(const std::uint8_t *data, std::size_t len) {
    blake3_hasher_t hasher;
    if (data && len > 0) {
      hasher.update(data, len);
    }
    return hasher.finalize();
  }

  std::array<std::uint8_t, 32> blake3_rgb24_from_bgra(const std::uint8_t *bgra, int stride, int width, int height) {
    blake3_hasher_t hasher;
    if (!bgra || width <= 0 || height <= 0 || stride < width * 4) {
      return hasher.finalize();
    }
    std::uint8_t stack_buf[4096 * 3];
    std::vector<std::uint8_t> heap_buf;
    const std::size_t rgb_len = static_cast<std::size_t>(width) * 3u;
    std::uint8_t *dst = stack_buf;
    if (rgb_len > sizeof(stack_buf)) {
      heap_buf.resize(rgb_len);
      dst = heap_buf.data();
    }

    for (int y = 0; y < height; ++y) {
      const std::uint8_t *row = bgra + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
      for (int x = 0; x < width; ++x) {
        const std::uint8_t *s = row + static_cast<std::size_t>(x) * 4u;
        std::uint8_t *d = dst + static_cast<std::size_t>(x) * 3u;
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
      }
      hasher.update(dst, rgb_len);
    }
    return hasher.finalize();
  }

  std::array<std::uint8_t, 32> compute_frame_hash_bgra(
    hash_algo_e algo,
    const std::uint8_t *bgra,
    int stride,
    int width,
    int height
  ) {
    std::array<std::uint8_t, 32> out {};
    if (algo == hash_algo_e::crc32c) {
      const std::uint32_t crc = crc32c_rgb24_from_bgra(bgra, stride, width, height);
      write_u32_le(out.data(), crc);
    } else if (algo == hash_algo_e::blake3) {
      out = blake3_rgb24_from_bgra(bgra, stride, width, height);
    }
    return out;
  }

}  // namespace sds::lossless
