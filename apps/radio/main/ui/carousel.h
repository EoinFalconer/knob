#pragma once

#include "lvgl.h"

#include <cstdint>

// Browsing as a strip of tiles, the way iOS pages through cards: the chosen
// tile sits in the middle with its neighbors peeking in at the sides, and
// each detent glides the strip one tile. A detent that lands mid-glide
// retargets it from where it is, so a fast spin keeps it moving rather than
// restarting. The list wraps around.

struct CarouselTile {
  const lv_image_dsc_t *image; // null shows a music glyph
  lv_color_t color;            // behind the image
};

struct CarouselHooks {
  CarouselTile (*tile)(int item);
  // Every frame of a move: the item nearest the middle, and how far the strip
  // sits from resting on it, in thousandths of a tile (-500..500).
  void (*moved)(int item, int32_t offset);
};

// Centered horizontally, `y` from the parent's middle. Hidden until opened.
void carousel_create(lv_obj_t *parent, int32_t y, const CarouselHooks &hooks);

// Shows `count` items centered on `item`; the neighbors fade in, and then
// `opened` is called.
void carousel_open(int count, int item, void (*opened)());
void carousel_turn(int steps);
// Where the strip is headed.
int carousel_item();
// Glides to `item` as the neighbors fade out, then hides and calls `done`.
void carousel_close(int item, void (*done)());
// Stops where it is and hides after `ms`, for when something covers it
// meanwhile and a glide underneath would only cost frames.
void carousel_hide_after(uint32_t ms, void (*done)());
