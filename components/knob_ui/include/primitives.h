#pragma once

#include "lvgl.h"

// How input works on the knob
// ───────────────────────────
// Gestures (tap, hold, swipe) are recognized once, on the screen object, and
// routed by UI state; in the radio: idle faces, now playing, browse. The
// knob's turns go through the same router. A widget takes a press itself only
// when it is a control with its own handler (skip buttons, picker rows, Back).
//
// Everything else must let presses fall through to the screen. LVGL makes
// every lv_obj_create() clickable, and a clickable box without a handler
// silently eats taps, so decoration and layout boxes come from here.

// A plain box for layout or decoration: no theme styling, no scrolling, and
// presses pass through it.
lv_obj_t *ui_box(lv_obj_t *parent);

// For surfaces laid over others, like sheets: taps on their background stop
// here instead of reaching whatever is underneath.
void ui_absorb_taps(lv_obj_t *obj);

// An invisible touch target for a control whose drawing is smaller than a
// finger needs, like a capsule near the bezel. Size and place it, then create
// it after `visual` so it sits on top. Presses show on `visual`, which should
// itself not be clickable; a tap calls `on_tap`.
lv_obj_t *ui_hit_area(lv_obj_t *parent, lv_obj_t *visual, lv_event_cb_t on_tap);

// A capsule holding an icon and a label, spaced on drawn ink rather than
// glyph boxes, the way the Dynamic Island insets its art:
// - the icon's ink sits 5 px further from the leading end than from the top
//   and bottom (rounded ends eat visual room), and the text's last letter
//   ends the same distance from the other end;
// - icon and text are 6 px apart, ink to ink;
// - both are centered on ink, the text on its capitals.
// An asymmetric icon can be off-center by eye even when its ink box is
// centered; `icon_nudge_y` moves it to where its blurred blob is centered.
// Style the capsule's background and colors through the returned objects.
struct IconPill {
  lv_obj_t *pill;
  lv_obj_t *icon;
  lv_obj_t *label;
};

IconPill ui_icon_pill(lv_obj_t *parent, int height, const lv_font_t *icon_font,
                      const char *icon, int icon_nudge_y,
                      const lv_font_t *text_font);

// Sets the label and refits the end padding to the new last letter.
void ui_icon_pill_set_text(const IconPill &pill, const char *text);
// Swaps the icon, from the same font, and refits around its ink.
void ui_icon_pill_set_icon(const IconPill &pill, const char *icon,
                           int icon_nudge_y);

// The pill every home screen uses: a white veil, a lucide_18 icon and an
// SF Medium 15 line in soft white, which ellipsizes before it outgrows the
// round screen. Recolor through the returned objects; place it with PILL_TOP,
// which keeps its corners 12 px inside the ring.
constexpr int PILL_H = 30;
constexpr int PILL_TOP = 58;
constexpr int PILL_TEXT_MAX_W = 172;
IconPill ui_pill(lv_obj_t *parent, const char *icon);
// Text and icon in one color and opacity.
void ui_pill_tint(const IconPill &pill, lv_color_t color, lv_opa_t opa);
