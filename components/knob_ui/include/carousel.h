#pragma once

#include "lvgl.h"

#include <cstdint>

// Browsing as a strip of tiles, the way iOS pages through cards: the chosen
// tile sits in the middle with its neighbors peeking in at the sides, and
// each detent glides the strip one tile. A detent that lands mid-glide
// retargets it from where it is, so a fast spin keeps it moving rather than
// restarting. The list wraps around.

struct CarouselTile {
  const lv_image_dsc_t *image; // 120 px square, or null
  lv_color_t color;            // behind the image
  const char *glyph;           // shown when there's no image, or null
  bool round;                  // a circle, for a person; an app icon if not
  bool visible = true;
};

constexpr int32_t CAROUSEL_TILE = 120;

struct CarouselHooks {
  CarouselTile (*tile)(int item);
  // Every frame of a move: the item nearest the middle, and how far the strip
  // sits from resting on it, in thousandths of a tile (-500..500).
  void (*moved)(int item, int32_t offset);
  const lv_font_t *glyph_font;
  int32_t size = CAROUSEL_TILE; // the middle tile; images come this size
  bool wrap = true;
  bool scale_sides = true;
};

// Centered horizontally, `y` from the parent's middle. Hidden until opened.
void carousel_create(lv_obj_t *parent, int32_t y, const CarouselHooks &hooks);

// Shows `count` items centered on `item`; the neighbors fade in, and then
// `opened` is called.
void carousel_open(int count, int item, void (*opened)());
void carousel_turn(int steps);
// For a strip that stays open while its items change: `count` items, resting
// on `item`, every tile asked for again.
void carousel_reload(int count, int item);
// Asks again for `item`'s tile, if it's showing.
void carousel_refresh(int item);
// Moves an open strip with the finger. `pixels` is measured from touch-down;
// positive is right. Non-wrapping strips rubber-band at their ends.
void carousel_begin_drag();
void carousel_drag(int32_t pixels);
// Settles from a drag by `steps` items (-1, 0, or 1).
void carousel_settle(int steps, uint32_t ms, void (*done)() = nullptr);
// Fades the whole strip out, reloads it at the midpoint, then fades it in.
void carousel_crossfade_reload(int count, int item, uint32_t ms);
// Where the strip is headed.
int carousel_item();
// Glides to `item` as the neighbors fade out, then hides and calls `done`.
void carousel_close(int item, void (*done)());
// Stops where it is and hides after `ms`, for when something covers it
// meanwhile and a glide underneath would only cost frames.
void carousel_hide_after(uint32_t ms, void (*done)());
