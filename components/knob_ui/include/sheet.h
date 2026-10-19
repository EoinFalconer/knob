#pragma once

#include "lvgl.h"

#include <cstdint>

// Drawers that slide over the home screen from the bottom or top edge and
// behave like iOS sheets:
// - a vertical drag pulls one open, or pushes an open one closed, and it
//   follows the finger the whole way, rubber-banding past fully open;
// - on release it keeps the finger's speed, and a flick is enough to open or
//   close it even if it only moved a little;
// - a moving sheet can be caught by touching it, and then drags from there.
//
// A sheet is a child of the active screen, not a screen of its own. LVGL then
// skips drawing whatever the sheet covers, which keeps a slide cheap, input
// keeps working while it moves, and nothing is freed under an animation.
//
// The gesture is recognized on raw touch samples (`sheets_track_touch`), ahead
// of LVGL's own handling. Once a drag is clearly vertical and the sheet's
// policy agrees, the sheet takes the touch: whatever was pressed is reset, so
// rows and buttons never fire from a drag.
enum class SheetEdge : uint8_t { Bottom, Top };

struct SheetPolicy {
  // May a drag pull the sheet open right now?
  bool (*may_open)();
  // May a drag that started at this point close the open sheet? Return false
  // where content should scroll instead, like a list that isn't at its top.
  bool (*may_close_from)(lv_point_t start);
  // The sheet is about to appear, by drag or by sheet_open: fill it in.
  void (*on_appear)();
  void (*on_opened)();
  void (*on_closed)();
};

// Returns the sheet's screen-sized body to fill; the sheet functions below take
// it too.
lv_obj_t *sheet_create(lv_obj_t *parent, SheetEdge edge,
                       const SheetPolicy &policy);

// Installs the drag recognizer on the touch input. Call once. From then on,
// everything dragged is shown a little ahead of the touch, where the finger
// is by the time the frame shows.
void sheets_track_touch(lv_indev_t *touch);
// For LV_EVENT_SCROLL_BEGIN on anything scrollable, so it starts under the
// finger instead of the scroll limit behind it.
void sheets_scroll_with_finger(lv_event_t *e);

// A screen-sized box on the sheet for content that's rebuilt as it appears:
// lv_obj_clean() it and fill it again, the sheet itself (and its grabber)
// stays. Not drawn; its black only shades things like the activity spinner.
lv_obj_t *sheet_add_content(lv_obj_t *sheet);

void sheet_open(lv_obj_t *sheet);
void sheet_close(lv_obj_t *sheet);
// Closes an open sheet at rest by fading it where it is, for when what's
// underneath already shows its content and a slide would show it twice. A
// sheet in a finger or in motion slides closed as usual.
void sheet_dissolve(lv_obj_t *sheet);
// Paints the sheet's own background over `area` of the sheet at `opa`, from a
// draw event of something on it. Whatever is under fades into the sheet as if
// it were that transparent, without drawing it through a layer.
void sheet_draw_material(lv_obj_t *sheet, lv_layer_t *layer,
                         const lv_area_t *area, lv_opa_t opa);
// Where the sheet is headed: true from the moment it starts opening until it
// starts closing.
bool sheet_is_open(const lv_obj_t *sheet);
// On screen at all: open, opening, closing or being dragged.
bool sheet_is_visible(const lv_obj_t *sheet);
// Closes an open sheet once nothing has touched the screen or turned the
// knob for `idle_ms`; for a periodic timer. LVGL's own inactivity clock sees
// every touch, even on rows that take the press themselves.
void sheet_close_if_idle(lv_obj_t *sheet, uint32_t idle_ms);
