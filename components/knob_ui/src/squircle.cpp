#include "squircle.h"

#include "esp_heap_caps.h"

// The decoder's descriptor is only in the private header.
#include "src/image/lv_image_decoder_private.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace {

// Close to Apple's icon outline: about as round as a 28 px corner at 120 px,
// but the curve starts further along the sides.
constexpr float SQUIRCLE = 5.0f;
constexpr float CIRCLE = 2.0f;
constexpr int SUBROWS = 4; // samples per pixel row, for the edge's coverage
constexpr int32_t MAX_SIZE = 256;

// Each row is two edges from the superellipse |u|^n + |v|^n = 1, so a pixel's
// coverage is exact across and sampled down.
void make_mask(uint8_t *mask, int32_t size, float exponent) {
  float half = static_cast<float>(size) / 2.0f;
  float cover[MAX_SIZE];
  for (int32_t y = 0; y < size; y++) {
    std::fill(cover, cover + size, 0.0f);
    for (int s = 0; s < SUBROWS; s++) {
      float v = std::fabs((static_cast<float>(y) +
                           (static_cast<float>(s) + 0.5f) / SUBROWS - half) /
                          half);
      if (v >= 1.0f)
        continue;
      float w = std::pow(1.0f - std::pow(v, exponent), 1.0f / exponent) * half;
      for (int32_t x = 0; x < size; x++) {
        auto px = static_cast<float>(x);
        float in = std::min(half + w, px + 1.0f) - std::max(half - w, px);
        cover[x] += std::clamp(in, 0.0f, 1.0f) / SUBROWS;
      }
    }
    for (int32_t x = 0; x < size; x++)
      mask[y * size + x] = static_cast<uint8_t>(std::lround(cover[x] * 255.0f));
  }
}

// Apps bake at a size or two, so each mask is made once.
struct Mask {
  float exponent;
  int32_t size;
  uint8_t *alpha;
};
constexpr int MASKS = 4;
Mask s_masks[MASKS];
int s_next_mask;

const uint8_t *mask_for(int32_t size, float exponent) {
  for (const Mask &m : s_masks)
    if (m.alpha && m.size == size && m.exponent == exponent)
      return m.alpha;
  Mask &m = s_masks[s_next_mask];
  s_next_mask = (s_next_mask + 1) % MASKS;
  heap_caps_free(m.alpha);
  m = {exponent, size,
       static_cast<uint8_t *>(
           heap_caps_malloc(static_cast<size_t>(size) * size,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))};
  if (m.alpha)
    make_mask(m.alpha, size, exponent);
  return m.alpha;
}

uint16_t over(uint16_t fg, uint16_t bg, uint8_t a, bool premultiplied) {
  if (a == LV_OPA_COVER)
    return fg;
  int take = premultiplied ? int{LV_OPA_COVER} : int{a};
  int keep = int{LV_OPA_COVER} - a;
  auto channel = [&](int shift, int max) {
    int f = (fg >> shift) & max;
    int b = (bg >> shift) & max;
    return std::min(max, (f * take + b * keep + 127) / 255) << shift;
  };
  return static_cast<uint16_t>(channel(11, 31) | channel(5, 63) |
                               channel(0, 31));
}

void paint(const lv_draw_buf_t &plain, int32_t size, uint16_t *rgb) {
  const lv_image_header_t &h = plain.header;
  bool premultiplied = h.flags & LV_IMAGE_FLAGS_PREMULTIPLIED;
  const uint8_t *alpha = plain.data + h.stride * h.h;
  for (int32_t y = 0; y < size; y++) {
    const auto *src =
        reinterpret_cast<const uint16_t *>(plain.data + y * h.stride);
    uint16_t *dst = rgb + y * size;
    for (int32_t x = 0; x < size; x++) {
      switch (h.cf) {
      case LV_COLOR_FORMAT_RGB565:
        dst[x] = src[x];
        break;
      case LV_COLOR_FORMAT_RGB565_SWAPPED:
        dst[x] = static_cast<uint16_t>(src[x] << 8 | src[x] >> 8);
        break;
      case LV_COLOR_FORMAT_RGB565A8:
        dst[x] =
            over(src[x], dst[x], alpha[y * (h.stride / 2) + x], premultiplied);
        break;
      default:
        return;
      }
    }
  }
}

void paint_picture(const lv_image_dsc_t *picture, int32_t size, uint16_t *rgb) {
  if (!picture || std::cmp_not_equal(picture->header.w, size) ||
      std::cmp_not_equal(picture->header.h, size))
    return;
  // Logos are stored compressed; the decoder hands back plain pixels.
  lv_image_decoder_dsc_t decoder;
  if (lv_image_decoder_open(&decoder, picture, nullptr) != LV_RESULT_OK)
    return;
  if (decoder.decoded)
    paint(*decoder.decoded, size, rgb);
  lv_image_decoder_close(&decoder);
}

void bake(const lv_image_dsc_t *picture, lv_color_t color, int32_t size,
          uint8_t *pixels, lv_image_dsc_t *out, float exponent) {
  size = std::min(size, MAX_SIZE);
  auto *rgb = reinterpret_cast<uint16_t *>(pixels);
  std::fill(rgb, rgb + size * size, lv_color_to_u16(color));
  paint_picture(picture, size, rgb);
  uint8_t *alpha = pixels + size * size * 2;
  const uint8_t *mask = mask_for(size, exponent);
  if (mask)
    memcpy(alpha, mask, static_cast<size_t>(size) * size);
  else
    memset(alpha, LV_OPA_COVER, static_cast<size_t>(size) * size);

  // The decoder may keep what it last read from this descriptor.
  lv_image_cache_drop(out);
  *out = {};
  out->header.magic = LV_IMAGE_HEADER_MAGIC;
  out->header.cf = LV_COLOR_FORMAT_RGB565A8;
  out->header.w = static_cast<uint32_t>(size);
  out->header.h = static_cast<uint32_t>(size);
  out->header.stride = static_cast<uint32_t>(size * 2);
  out->data_size = squircle_bytes(size);
  out->data = pixels;
}

} // namespace

uint8_t *squircle_alloc(int32_t size) {
  return static_cast<uint8_t *>(heap_caps_malloc(
      squircle_bytes(size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

void squircle_free(uint8_t *pixels) { heap_caps_free(pixels); }

void squircle_bake(const lv_image_dsc_t *picture, lv_color_t color,
                   int32_t size, uint8_t *pixels, lv_image_dsc_t *out) {
  bake(picture, color, size, pixels, out, SQUIRCLE);
}

void circle_bake(const lv_image_dsc_t *picture, lv_color_t color, int32_t size,
                 uint8_t *pixels, lv_image_dsc_t *out) {
  bake(picture, color, size, pixels, out, CIRCLE);
}
