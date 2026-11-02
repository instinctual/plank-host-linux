/**
 * @file src/platform/linux/x11_rgb10.h
 * @brief Exact depth-30 unpacking and contiguous row partitions for x264.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace platf::x11 {
  /** @brief Partition rows without overlap, including heights smaller than the worker count. */
  inline std::pair<std::size_t, std::size_t> row_range(
      std::size_t height, unsigned int index, unsigned int count) {
    const auto quotient = height / count;
    const auto remainder = height % count;
    const auto first = quotient * index + std::min<std::size_t>(index, remainder);
    return {first, first + quotient + (index < remainder)};
  }

  /**
   * @brief Unpack depth-30 X11 pixels into full-range 10-bit G, B, R planes.
   * @param source Packed pixels, red in bits 0-9, green 10-19, blue 20-29.
   * @param green Nonoverlapping plane 0, with at least pixel_count elements.
   * @param blue Nonoverlapping plane 1, with at least pixel_count elements.
   * @param red Nonoverlapping plane 2, with at least pixel_count elements.
   * @param pixel_count Row width; SIMD alignment and padding are not required.
   */
  inline void unpack_xrgb10_to_gbr10_row(const std::uint32_t *__restrict source,
      std::uint16_t *__restrict green, std::uint16_t *__restrict blue,
      std::uint16_t *__restrict red,
      std::size_t pixel_count) {
    // Nonoverlapping planes let the compiler vectorize this exact integer loop
    // without runtime alias checks or architecture-specific dispatch.
    for (std::size_t x = 0; x < pixel_count; ++x) {
      const auto pixel = source[x];
      red[x] = static_cast<std::uint16_t>(pixel & 0x3ffU);
      green[x] = static_cast<std::uint16_t>((pixel >> 10U) & 0x3ffU);
      blue[x] = static_cast<std::uint16_t>((pixel >> 20U) & 0x3ffU);
    }
  }
}
