#pragma once

#include "lvgl.h"

#include <cstdint>

// The clock's big shapes, drawn into A8 bitmaps and shown as images tinted
// with `image_recolor`. Blitting an A8 image is LVGL's cheapest draw, so moving
// numerals or a turning hand repaints only their own pixels, and the shapes
// keep sub-pixel positions that LVGL's integer coordinates would snap.

struct ClockArt {
  lv_image_dsc_t dsc;
  uint8_t *pixels;
};

// `text` rendered with `tracking`, cropped to its ink. Returns false without
// memory.
bool clock_art_text(ClockArt *art, const lv_font_t *font, const char *text,
                    int tracking);

// One stretch of a hand: `from` to `to` px out from the center (negative runs
// behind it), `width` px wide, with true round ends inside that span.
struct HandPart {
  float from, to, width;
};

// A hand is up to two parts, such as a thin stem running into a wide bar.
struct HandShape {
  HandPart parts[2];
  int count;
};

// Draws the hand at `degrees` (0 is 12 o'clock) around (`cx`, `cy`),
// anti-aliased. `origin` gets the bitmap's top-left on screen.
bool clock_art_hand(ClockArt *art, const HandShape &shape, float degrees,
                    int cx, int cy, lv_point_t *origin);
