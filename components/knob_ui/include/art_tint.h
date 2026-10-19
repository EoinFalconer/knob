#pragma once

#include <cstdint>

// Background color for a picture (byte-swapped RGB565, as decoded): the
// picture's most vivid dominant color, darkened to the same depth as the
// station colors so white text stays readable. Like Apple Music's backdrop,
// but one solid color: gradients band on RGB565 and cost more to repaint.
// What's behind a tile with no picture, and what a picture with no color
// tints to.
constexpr uint32_t ART_TINT_NONE = 0x141414;

uint32_t artwork_tint(const uint8_t *rgb565_swapped, int pixel_count);
