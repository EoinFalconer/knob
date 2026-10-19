#include "art_tint.h"

#include <algorithm>
#include <cstdlib>

namespace {

struct Rgb {
  int r, g, b;
};

Rgb unpack_swapped_rgb565(const uint8_t *px) {
  auto v = static_cast<uint16_t>(px[0] << 8 | px[1]);
  int r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
  return {r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2};
}

// Same depth as the station palette (brightest channel 0x28).
constexpr int TINT_PEAK = 0x28;

struct Bin {
  uint32_t weight, r, g, b;
};

// 3 bits per channel: coarse enough that a color region lands in one bin,
// small enough (8 KB) to allocate briefly once per track.
constexpr int BINS = 512;

} // namespace

uint32_t artwork_tint(const uint8_t *rgb565_swapped, int pixel_count) {
  auto *bins = static_cast<Bin *>(calloc(BINS, sizeof(Bin)));
  if (!bins)
    return ART_TINT_NONE;

  for (int i = 0; i < pixel_count; i += 3) {
    Rgb c = unpack_swapped_rgb565(rgb565_swapped + i * 2);
    int hi = std::max({c.r, c.g, c.b});
    int lo = std::min({c.r, c.g, c.b});
    if (hi < 24)
      continue; // near-black says nothing about the artwork's color
    // Favour vivid pixels so a red logo on a white sleeve still reads red.
    uint32_t w = 1 + static_cast<uint32_t>(hi - lo);
    Bin &bin = bins[(c.r >> 5) << 6 | (c.g >> 5) << 3 | (c.b >> 5)];
    bin.weight += w;
    bin.r += w * static_cast<uint32_t>(c.r);
    bin.g += w * static_cast<uint32_t>(c.g);
    bin.b += w * static_cast<uint32_t>(c.b);
  }

  const Bin *best =
      std::max_element(bins, bins + BINS, [](const Bin &a, const Bin &b) {
        return a.weight < b.weight;
      });
  if (!best->weight) {
    free(bins);
    return ART_TINT_NONE;
  }
  int r = static_cast<int>(best->r / best->weight);
  int g = static_cast<int>(best->g / best->weight);
  int b = static_cast<int>(best->b / best->weight);
  free(bins);

  int hi = std::max({r, g, b, 1});
  return static_cast<uint32_t>(r * TINT_PEAK / hi) << 16 |
         static_cast<uint32_t>(g * TINT_PEAK / hi) << 8 |
         static_cast<uint32_t>(b * TINT_PEAK / hi);
}
