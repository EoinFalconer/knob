#pragma once

#include "lvgl.h"

#include <cstdint>

// A home screen's layout, as the radio set it: a pill at the top, a 120 px
// tile a touch above center, and three lines of text under it. In the radio
// they're the station, its state and "3 / 12"; in kaffi a person's name,
// their standing and their pots and cups. Measured from the screen's center.
// The tile's bottom edge stays put above the title; a bigger tile grows up
// into the room under the pill.
constexpr int STAGE_TILE_BOTTOM = 48;
constexpr int stage_tile_y(int size) { return STAGE_TILE_BOTTOM - size / 2; }
constexpr int STAGE_TILE_Y = stage_tile_y(120);
constexpr int CAPTION_TITLE_Y = 76;
constexpr int CAPTION_SUBTITLE_Y = 102;
constexpr int CAPTION_DETAIL_Y = 126;
constexpr uint32_t COL_CAPTION_DETAIL = 0x888888;

struct Caption {
  lv_obj_t *title;    // SF Semibold 28
  lv_obj_t *subtitle; // SF 16, white at secondary opacity
  lv_obj_t *detail;   // SF 16, gray
};

Caption ui_caption(lv_obj_t *parent);

// Long text scrolls through once after a pause, then rests truncated the way
// iOS does: an endless marquee would repaint its strip on every frame. Only a
// new text gets a pass; the same text again leaves the label alone, or a
// pass restarted by every poll would never move.
void ui_marquee_text(lv_obj_t *label, const char *text);

// A title over a tile strip (carousel.h) fades as its tile leaves the middle,
// gone halfway, when it changes to the next one's.
lv_opa_t ui_caption_fade(int32_t carousel_offset);
