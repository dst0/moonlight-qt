/**
 * @file src/sds/lossless_hash.h
 * @brief Canonical RGB24 frame hashing (hardware-accelerated CRC32C with GF(2) combine, plus BLAKE3).
 */
#pragma once
#ifndef SDS_LOSSLESS_HASH_H
#define SDS_LOSSLESS_HASH_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace sds::lossless {

  enum class hash_algo_e : std::uint8_t {
    none = 0,
    crc32c = 1,
    blake3 = 2,
  };

  /**
   * @brief Update a running CRC32C (Castagnoli, poly 0x82F63B78) over arbitrary bytes.
   *
   * Pass `0` as initial `crc` for a new stream. Uses hardware instructions (ARMv8 / SSE4.2)
   * when available, falling back to slicing-by-8 software tables.
   */
  std::uint32_t crc32c_update(std::uint32_t crc, const std::uint8_t *data, std::size_t len);

  /**
   * @brief Combine two finalized CRC32C values `crc1 = CRC32C(A)` and `crc2 = CRC32C(B)`
   *        into `CRC32C(A || B)` in O(log len2) using GF(2) matrix squaring.
   */
  std::uint32_t crc32c_combine(std::uint32_t crc1, std::uint32_t crc2, std::uint64_t len2);

  /**
   * @brief Compute CRC32C of one BGRA row interpreted as tightly packed RGB24 (`width * 3` bytes).
   *
   * Alpha / X byte at offset +3 of each pixel is ignored so garbage alpha never affects the hash.
   */
  std::uint32_t crc32c_rgb24_from_bgra_row(const std::uint8_t *bgra, int width);

  /**
   * @brief Compute canonical top-down RGB24 CRC32C over a full BGRA image.
   */
  std::uint32_t crc32c_rgb24_from_bgra(const std::uint8_t *bgra, int stride, int width, int height);

  /**
   * @brief Compute standard 256-bit BLAKE3 hash over arbitrary bytes.
   */
  std::array<std::uint8_t, 32> blake3_hash(const std::uint8_t *data, std::size_t len);

  /**
   * @brief Compute canonical top-down RGB24 BLAKE3-256 hash over a full BGRA image.
   */
  std::array<std::uint8_t, 32> blake3_rgb24_from_bgra(const std::uint8_t *bgra, int stride, int width, int height);

  /**
   * @brief Compute the 32-byte wire hash field for `algo` over a BGRA image.
   *        For `crc32c`, the 32-bit CRC is stored little-endian in bytes [0..3], bytes [4..31] = 0.
   *        For `none`, all 32 bytes are zero.
   */
  std::array<std::uint8_t, 32> compute_frame_hash_bgra(
    hash_algo_e algo,
    const std::uint8_t *bgra,
    int stride,
    int width,
    int height
  );

}  // namespace sds::lossless
#endif  // SDS_LOSSLESS_HASH_H
