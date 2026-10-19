#pragma once

#include "lvgl.h"

#include <cstdint>

  // The iOS app icon's shape: a superellipse, whose corners ease into the
  // sides instead of a circle's arc meeting them. LVGL only rounds with arcs,
  // so the shape is baked into the picture's alpha, which also draws without
  // the layer that clip_corner needs.

  // A buffer for one size×size picture, RGB565 then an A8 plane.
  constexpr uint32_t squircle_bytes(int32_t size) {
    return static_cast<uint32_t>(size * size * 3);
  }

  // In PSRAM: internal RAM is kept for Wi-Fi and DMA, and runs out at boot.
  uint8_t *squircle_alloc(int32_t size);
  void squircle_free(uint8_t *pixels);

  // Bakes `picture` (or nothing, when null or not size×size) onto `color` in
  // the shape, into `pixels`, and points `out` at it.
  void squircle_bake(const lv_image_dsc_t *picture, lv_color_t color,
                     int32_t size, uint8_t *pixels, lv_image_dsc_t *out);

  
  
  
                     