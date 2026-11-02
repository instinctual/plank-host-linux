/**
 * @file src/platform/linux/x11_rgb10.h
 * @brief Exact depth-30 unpacking and contiguous row partitions for x264.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#if defined(__SSE2__)
  #include <emmintrin.h>
#endif

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
  inline void unpack_xrgb10_to_gbr10_row(const std::uint32_t *source,
      std::uint16_t *green, std::uint16_t *blue, std::uint16_t *red,
      std::size_t pixel_count) {
    std::size_t x = 0;
#if defined(__SSE2__)
    // SSE2 is part of the x86-64 baseline. Mask before signed packing, so all
    // ten bits survive; alpha/padding bits never enter a component plane.
    const auto mask = _mm_set1_epi32(1023);
    for (; pixel_count - x >= 8; x += 8) {
      const auto a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source + x));
      const auto b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source + x + 4));
      _mm_storeu_si128(reinterpret_cast<__m128i *>(red + x),
        _mm_packs_epi32(_mm_and_si128(a, mask), _mm_and_si128(b, mask)));
      _mm_storeu_si128(reinterpret_cast<__m128i *>(green + x),
        _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(a, 10), mask),
                       _mm_and_si128(_mm_srli_epi32(b, 10), mask)));
      _mm_storeu_si128(reinterpret_cast<__m128i *>(blue + x),
        _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(a, 20), mask),
                       _mm_and_si128(_mm_srli_epi32(b, 20), mask)));
    }
#endif
    for (; x < pixel_count; ++x) {
      const auto pixel = source[x];
      red[x] = static_cast<std::uint16_t>(pixel & 0x3ffU);
      green[x] = static_cast<std::uint16_t>((pixel >> 10U) & 0x3ffU);
      blue[x] = static_cast<std::uint16_t>((pixel >> 20U) & 0x3ffU);
    }
  }
}
