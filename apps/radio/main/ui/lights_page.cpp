#include "ui/lights_page.h"
#include "fonts.h"
#include "hal_pins.h"
#include "haptic.h"
#include "lights.h"
#include "motion.h"
#include "primitives.h"

// lv_area_intersect is only declared in the private header.
#include "src/misc/lv_area_private.h"

#include <algorithm>
#include <cstdio>

// After the iOS Home app's light control: one tall rounded slider that fills
// from the bottom with the light's color, how bright near its top and a bulb
// glyph at its foot. The knob moves the fill; a tap switches.
namespace {

constexpr int SLIDER_W = 124;
constexpr int SLIDER_H = 220;
constexpr int SLIDER_R = 40;
constexpr int SLIDER_Y = -16;   // from the center
constexpr int HINT_Y = 28;      // from the top
constexpr int LEVEL_TOP = 20;   // the percent's box top to the slider's top
constexpr int BULB_BOTTOM = 22; // the bulb's box bottom to the slider's bottom

constexpr uint32_t COL_FILL = 0xFFDFA0; // warm white, like a lit bulb
constexpr lv_opa_t TRACK_OPA = 42;      // a white veil on the frosted sheet
constexpr lv_opa_t HINT_OPA = 150;
// The percent and the bulb are white on the track and dark ink on the fill,
// split along the fill's edge like iOS's sliders, so a figure the edge
// crosses is two colors at once.
constexpr lv_opa_t ON_TRACK_OPA = 220;
constexpr lv_opa_t ON_FILL_OPA = 150;

constexpr int32_t FULL = 10000; // fill, in 1/100 percent
constexpr uint32_t FILL_MS = 180;
// What the knob just set stays on screen, whatever the gateway says, until
// the hand has been off it this long: reports of the level a turn passed
// through would pull the slider back mid-turn.
constexpr uint32_t HOLD_MS = 1500;

lv_obj_t *s_slider;
lv_obj_t *s_hint;
int32_t s_fill;          // as drawn
char s_level_text[8];    // "60%", "Off", or nothing while unknown
const char *s_bulb_text; // the lit or the crossed-out bulb

LightsState s_shown;  // what the page shows: what was asked for, then reports
LightsState s_report; // the gateway's latest; held back while the hand is on
bool s_report_waiting;
int s_last_level = 100; // how bright they were when last on
lv_timer_t *s_hold;     // running while what was asked for stays on screen

bool holding() { return s_hold && !lv_timer_get_paused(s_hold); }

// Runs `draw` with the layer clipped to `area` too, if any of it shows.
template <typename Draw>
void clipped(lv_layer_t *layer, const lv_area_t &area, Draw draw) {
  lv_area_t was = layer->_clip_area;
  if (lv_area_intersect(&layer->_clip_area, &was, &area))
    draw();
  layer->_clip_area = was;
}

void draw_text(lv_layer_t *layer, const char *text, const lv_font_t *font,
               lv_area_t box, bool on_fill) {
  if (!text || !text[0])
    return;
  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.text = text;
  dsc.font = font;
  dsc.align = LV_TEXT_ALIGN_CENTER;
  dsc.color = on_fill ? lv_color_black() : lv_color_white();
  dsc.opa = on_fill ? ON_FILL_OPA : ON_TRACK_OPA;
  lv_draw_label(layer, &dsc, &box);
}

// The fill is the slider's own rounded shape cut off at the level, so its
// bottom corners follow the slider's and its top is a straight edge. The
// percent and the bulb are drawn once for each side of that edge.
void on_draw_slider(lv_event_t *e) {
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t c;
  lv_obj_get_coords(s_slider, &c);
  lv_area_t track = c, lit = c;
  lit.y1 = c.y2 + 1 - SLIDER_H * s_fill / FULL;
  track.y2 = lit.y1 - 1;

  lv_draw_rect_dsc_t rect;
  lv_draw_rect_dsc_init(&rect);
  rect.radius = SLIDER_R;
  rect.bg_color = lv_color_white();
  rect.bg_opa = TRACK_OPA;
  lv_draw_rect(layer, &rect, &c);
  rect.bg_color = lv_color_hex(COL_FILL);
  rect.bg_opa = LV_OPA_COVER;
  if (s_fill > 0)
    clipped(layer, lit, [&] { lv_draw_rect(layer, &rect, &c); });

  int32_t level_h = lv_font_get_line_height(&sf_semibold_28);
  int32_t bulb_h = lv_font_get_line_height(&lucide_34);
  lv_area_t level = {c.x1, c.y1 + LEVEL_TOP, c.x2, c.y1 + LEVEL_TOP + level_h};
  lv_area_t bulb = {c.x1, c.y2 - BULB_BOTTOM - bulb_h, c.x2,
                    c.y2 - BULB_BOTTOM};
  for (bool on_fill : {false, true})
    clipped(layer, on_fill ? lit : track, [&] {
      draw_text(layer, s_level_text, &sf_semibold_28, level, on_fill);
      draw_text(layer, s_bulb_text, &lucide_34, bulb, on_fill);
    });
}

void anim_fill_cb(void *, int32_t v) {
  s_fill = v;
  lv_obj_invalidate(s_slider);
}

void fill_to(int level, bool animate) {
  lv_anim_delete(&s_fill, anim_fill_cb);
  int32_t to = level * (FULL / 100);
  if (!animate || to == s_fill) {
    anim_fill_cb(nullptr, to);
    return;
  }
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &s_fill);
  lv_anim_set_exec_cb(&a, anim_fill_cb);
  lv_anim_set_values(&a, s_fill, to);
  lv_anim_set_duration(&a, FILL_MS);
  motion_ease_out(&a);
  lv_anim_start(&a);
}

const char *hint(LightsStatus status) {
  switch (status) {
  case LightsStatus::Unset:
    return "Not set up";
  case LightsStatus::Connecting:
    return "Connecting\u2026";
  case LightsStatus::Offline:
    return "Can't reach the lights";
  case LightsStatus::Ready:
    break;
  }
  return "";
}

void show(const LightsState &state, bool animate) {
  s_shown = state;
  bool known = state.status == LightsStatus::Ready || holding();
  bool lit = known && state.on && state.level > 0;
  if (lit)
    s_last_level = state.level;
  if (lit)
    snprintf(s_level_text, sizeof(s_level_text), "%d%%",
             std::min(state.level, 100));
  else
    snprintf(s_level_text, sizeof(s_level_text), "%s", known ? "Off" : "");
  s_bulb_text = lit ? ICON_LIGHTBULB : ICON_LIGHTBULB_OFF;
  lv_label_set_text(s_hint, known ? "" : hint(state.status));
  lv_obj_invalidate(s_slider);
  fill_to(lit ? state.level : 0, animate);
}

void on_hold_over(lv_timer_t *t) {
  lv_timer_pause(t);
  if (s_report_waiting) {
    s_report_waiting = false;
    show(s_report, true);
  }
}

void touched(const LightsState &asked) {
  lv_timer_reset(s_hold);
  lv_timer_resume(s_hold);
  show(asked, true);
}

void on_tap(lv_event_t *) {
  if (s_shown.status == LightsStatus::Unset)
    return;
  haptic_buzz();
  bool on = !(s_shown.on && s_shown.level > 0);
  lights_set_on(on);
  LightsState asked = s_shown;
  asked.on = on;
  // Switched on, each light comes back at its own level, which only the
  // gateway knows; until it says, the slider shows the last level it had.
  if (on)
    asked.level = s_last_level;
  touched(asked);
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, int y) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
  return l;
}

} // namespace

lv_obj_t *lights_page_create(lv_obj_t *parent) {
  lv_obj_t *page = ui_box(parent);
  lv_obj_set_size(page, LCD_H_RES, LCD_V_RES);
  lv_obj_t *tap = lv_obj_create(page);
  lv_obj_remove_style_all(tap);
  lv_obj_set_size(tap, LCD_H_RES, LCD_V_RES);
  lv_obj_remove_flag(tap, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_event_cb(tap, on_tap, LV_EVENT_CLICKED, nullptr);

  // Only says something when there's nothing to show in the slider.
  s_hint = make_label(page, &sf_regular_16, HINT_Y);
  lv_obj_set_style_text_opa(s_hint, HINT_OPA, LV_PART_MAIN);

  s_slider = ui_box(page);
  lv_obj_set_size(s_slider, SLIDER_W, SLIDER_H);
  lv_obj_align(s_slider, LV_ALIGN_CENTER, 0, SLIDER_Y);
  lv_obj_add_event_cb(s_slider, on_draw_slider, LV_EVENT_DRAW_MAIN, nullptr);

  if (!s_hold) {
    s_hold = lv_timer_create(on_hold_over, HOLD_MS, nullptr);
    lv_timer_pause(s_hold);
  }
  show(lights_state(), false);
  return page;
}

void lights_page_appear() { lights_refresh(); }

void lights_page_show(const LightsState &state) {
  s_report = state;
  s_report_waiting = holding();
  if (!s_report_waiting)
    show(state, true);
}

void lights_page_turn(int steps) {
  if (s_shown.status == LightsStatus::Unset || steps == 0)
    return;
  int level = s_shown.on ? s_shown.level : 0;
  int next = std::clamp(level + steps, 0, 100);
  if (next == level) {
    haptic_buzz(); // at an end
    return;
  }
  lights_set_level(next);
  LightsState asked = s_shown;
  asked.on = next > 0;
  asked.level = next;
  touched(asked);
}
