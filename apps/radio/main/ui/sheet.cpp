#include "ui/sheet.h"
#include "hal_pins.h"
#include "ui/frost.h"
#include "ui/motion.h"
#include "ui/primitives.h"

// The cover check's verdict can only be raised through the public API.
#include "src/core/lv_obj_event_private.h"
#include "src/core/lv_obj_scroll_private.h"
#include "src/misc/lv_area_private.h"

#include <algorithm>
#include <cstdlib>

namespace {

constexpr int32_t RADIUS = 36;
constexpr int32_t GRABBER_W = 36;
constexpr int32_t GRABBER_H = 5;
constexpr int32_t GRABBER_INSET = 14;
constexpr lv_opa_t GRABBER_OPA = 90;

// A drag must move this far, mostly vertically, before a sheet takes it.
constexpr int32_t SLOP = 10;
// Past fully open the sheet gives like a rubber band (iOS's curve): it moves
// less and less, never more than this.
constexpr float RUBBER_LIMIT = 120.0f;
constexpr float RUBBER_STIFFNESS = 0.55f;
// The sheet runs on past its body on the side it opened from, as far as the
// rubber band can stretch, so a stretched sheet never shows a gap behind it.
constexpr int32_t SKIRT = static_cast<int32_t>(RUBBER_LIMIT);
// Release speed comes from the last stretch of the drag, and carries the
// sheet on this far ahead when deciding where it settles.
constexpr uint32_t VELOCITY_WINDOW_MS = 80;
constexpr float PROJECT_MS = 180.0f;
// Settling keeps the finger's speed: motion_ease_out starts with this slope,
// so a duration of slope * distance / speed continues the motion seamlessly.
constexpr float EASE_OUT_START_SLOPE = 1.0f / 0.23f;
constexpr uint32_t SETTLE_MIN_MS = 160;
constexpr uint32_t SETTLE_MAX_MS = 440;
constexpr uint32_t PRESENT_MS = 440; // a full open or close with no finger
constexpr uint32_t DISSOLVE_MS = 240;
// LVGL polls touch every 30 ms by default, so a dragged sheet visibly steps
// behind the finger.
constexpr uint32_t TOUCH_READ_MS = 10;
// A frame reaches the glass some 40 ms after the touch it shows, so anything
// dragged trails the finger. Like iOS, every drag is shown where the finger
// is heading: this far ahead along its current speed, and never further than
// PREDICT_MAX, so a sudden stop doesn't overshoot visibly.
constexpr float PREDICT_MS = 28.0f;
constexpr float PREDICT_MAX = 24.0f;
constexpr uint32_t PREDICT_WINDOW_MS = 40;

// Frosting takes about 100 ms. It starts as soon as a finger lands where a
// drag could pull a sheet open: the snapshot then (about 20 ms), and the blur
// on the other core while the finger travels the slop, so a drag rarely waits
// for it. A drag recognized within a second uses it.
constexpr uint32_t FROST_FRESH_MS = 1000;

constexpr lv_area_t SCREEN = {0, 0, LCD_H_RES - 1, LCD_V_RES - 1};

constexpr int MAX_SHEETS = 2;
constexpr int MAX_SAMPLES = 8;

struct Sheet {
  lv_obj_t *obj;               // the whole panel, skirt included
  lv_obj_t *body;              // the screen-sized part callers fill
  const lv_image_dsc_t *frost; // the screen behind, held still as it moves
  SheetEdge edge;
  SheetPolicy policy;
  bool open;
  int32_t y; // the body's top on screen: 0 when open
};

struct Sample {
  uint32_t t;
  int32_t x, y;
};

struct Touch {
  bool down;
  bool deciding; // within the slop and not yet ruled out
  lv_point_t start;
  lv_point_t shown; // where LVGL was last told the finger is
  Sheet *sheet;     // the sheet following this finger, if any
  int32_t grab_y;
  int32_t finger_y;
  Sample samples[MAX_SAMPLES];
  int count;
};

Sheet s_sheets[MAX_SHEETS];
int s_count;
Touch s_touch;
lv_indev_read_cb_t s_read;

struct Material {
  lv_obj_t *screen;
  uint32_t made_ms;
  bool started;
};
Material s_material;

Sheet *find(const lv_obj_t *obj) {
  for (int i = 0; i < s_count; i++)
    if (s_sheets[i].obj == obj || s_sheets[i].body == obj)
      return &s_sheets[i];
  return nullptr;
}

// Where the body sits inside the panel: below the skirt for a top sheet.
int32_t body_top(SheetEdge edge) { return edge == SheetEdge::Top ? SKIRT : 0; }

int32_t closed_y(const Sheet &s) {
  return s.edge == SheetEdge::Bottom ? LCD_V_RES : -LCD_V_RES;
}

bool visible(const Sheet &s) {
  return !lv_obj_has_flag(s.obj, LV_OBJ_FLAG_HIDDEN);
}

float rubber(float over) {
  return (1.0f - 1.0f / (over * RUBBER_STIFFNESS / RUBBER_LIMIT + 1.0f)) *
         RUBBER_LIMIT;
}

// Where a sheet dragged to `raw` is drawn: anywhere between closed and open,
// and rubber-banded past open.
int32_t shown_y(const Sheet &s, int32_t raw) {
  int32_t closed = closed_y(s);
  int32_t past_open = closed > 0 ? -raw : raw;
  if (past_open > 0) {
    auto give = static_cast<int32_t>(rubber(static_cast<float>(past_open)));
    return closed > 0 ? -give : give;
  }
  return std::clamp(raw, std::min<int32_t>(closed, 0),
                    std::max<int32_t>(closed, 0));
}

void move_to(Sheet &s, int32_t y) {
  s.y = y;
  lv_obj_set_y(s.obj, y - body_top(s.edge));
}

void anim_y_cb(void *obj, int32_t y) {
  if (Sheet *s = find(static_cast<lv_obj_t *>(obj)))
    move_to(*s, y);
}

bool moving(const Sheet &s) { return lv_anim_get(s.obj, anim_y_cb); }

// Layered, so the sheet fades as one picture. Plain opa would fade each part
// on its own, and the frost would veil whatever lies under the sheet's text.
void anim_opa_cb(void *obj, int32_t v) {
  lv_obj_set_style_opa_layered(static_cast<lv_obj_t *>(obj),
                               static_cast<lv_opa_t>(v), LV_PART_MAIN);
}

// A dissolving sheet taken back up is whole again.
void solidify(Sheet &s) {
  if (lv_anim_delete(s.obj, anim_opa_cb))
    anim_opa_cb(s.obj, LV_OPA_COVER);
}

void settled(lv_anim_t *a) {
  Sheet *s = find(static_cast<lv_obj_t *>(a->var));
  if (!s)
    return;
  if (s->open) {
    if (s->policy.on_opened)
      s->policy.on_opened();
    return;
  }
  lv_obj_add_flag(s->obj, LV_OBJ_FLAG_HIDDEN);
  if (s->policy.on_closed)
    s->policy.on_closed();
}

uint32_t settle_ms(int32_t distance, float speed) {
  auto ms = speed > 0.05f
                ? EASE_OUT_START_SLOPE * static_cast<float>(distance) / speed
                : static_cast<float>(PRESENT_MS) *
                      static_cast<float>(distance) /
                      static_cast<float>(LCD_V_RES);
  return std::clamp(static_cast<uint32_t>(ms), SETTLE_MIN_MS, SETTLE_MAX_MS);
}

// `velocity` is the finger's, in px/ms along y; it only counts when it heads
// where the sheet is going.
void settle(Sheet &s, bool open, float velocity) {
  s.open = open;
  solidify(s);
  lv_anim_delete(s.obj, anim_y_cb);
  int32_t from = s.y;
  int32_t to = open ? 0 : closed_y(s);
  if (from == to) {
    lv_anim_t done = {};
    done.var = s.obj;
    settled(&done);
    return;
  }
  float toward = (to > from) == (velocity > 0) ? std::abs(velocity) : 0.0f;

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s.obj);
  lv_anim_set_exec_cb(&a, anim_y_cb);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, settle_ms(std::abs(to - from), toward));
  motion_ease_out(&a);
  lv_anim_set_completed_cb(&a, settled);
  lv_anim_start(&a);
}

// Frosted from the screen as it is now, while every sheet is hidden and so
// not in the picture.
void make_material(lv_obj_t *screen) {
  s_material = {screen, lv_tick_get(), frost_begin(screen)};
}

bool material_fresh(lv_obj_t *screen) {
  return s_material.started && s_material.screen == screen &&
         lv_tick_elaps(s_material.made_ms) < FROST_FRESH_MS;
}

// Without memory for the material, the sheet stays plain black.
void frost(Sheet &s) {
  lv_obj_t *screen = lv_obj_get_screen(s.obj);
  if (!material_fresh(screen))
    make_material(screen);
  s.frost = s_material.started ? frost_take() : nullptr;
  lv_obj_set_style_bg_opa(s.obj, s.frost ? LV_OPA_TRANSP : LV_OPA_COVER,
                          LV_PART_MAIN);
  lv_obj_invalidate(s.obj);
}

void appear(Sheet &s) {
  if (visible(s))
    return;
  if (s.policy.on_appear)
    s.policy.on_appear();
  frost(s);
  move_to(s, closed_y(s));
  lv_obj_remove_flag(s.obj, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s.obj);
}

// ─── Drag recognition ───────────────────────────────────────────────────────

void record(lv_point_t p, uint32_t now) {
  if (s_touch.count == MAX_SAMPLES) {
    std::copy(s_touch.samples + 1, s_touch.samples + MAX_SAMPLES,
              s_touch.samples);
    s_touch.count--;
  }
  s_touch.samples[s_touch.count++] = {now, p.x, p.y};
}

struct Speed {
  float x, y; // px/ms
};

// Over the last `window_ms` of samples.
Speed speed(uint32_t window_ms) {
  if (s_touch.count < 2)
    return {0, 0};
  const Sample &last = s_touch.samples[s_touch.count - 1];
  const Sample *first = &s_touch.samples[0];
  for (int i = s_touch.count - 2; i >= 0; i--) {
    if (last.t - s_touch.samples[i].t > window_ms)
      break;
    first = &s_touch.samples[i];
  }
  if (last.t == first->t)
    return {0, 0};
  auto dt = static_cast<float>(last.t - first->t);
  return {static_cast<float>(last.x - first->x) / dt,
          static_cast<float>(last.y - first->y) / dt};
}

float velocity() { return speed(VELOCITY_WINDOW_MS).y; }

int32_t ahead(float v) {
  return static_cast<int32_t>(
      std::clamp(v * PREDICT_MS, -PREDICT_MAX, PREDICT_MAX));
}

lv_point_t predicted(lv_point_t p) {
  Speed v = speed(PREDICT_WINDOW_MS);
  return {std::clamp<int32_t>(p.x + ahead(v.x), 0, LCD_H_RES - 1),
          std::clamp<int32_t>(p.y + ahead(v.y), 0, LCD_V_RES - 1)};
}

bool contains(const Sheet &s, lv_point_t p) {
  lv_area_t a;
  lv_obj_get_coords(s.obj, &a);
  return p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2;
}

// Whatever LVGL thought was pressed lets go (its pressed look included), and
// the rest of this touch is ignored by everything but the sheet.
void claim(lv_indev_t *indev, Sheet &s, lv_point_t p) {

  lv_indev_reset(indev, nullptr);
  lv_indev_wait_release(indev);
  lv_anim_delete(s.obj, anim_y_cb);
  solidify(s);
  appear(s);
  s_touch.sheet = &s;
  s_touch.deciding = false;
  s_touch.grab_y = s.y;
  // From where the finger came down, so the sheet makes up the slop at once
  // and sits under the finger rather than a slop behind it.
  s_touch.finger_y = s_touch.start.y;
  move_to(s, shown_y(s, s_touch.grab_y + p.y - s_touch.finger_y));
}

bool any_visible() {
  for (int i = 0; i < s_count; i++)
    if (visible(s_sheets[i]))
      return true;
  return false;
}

Sheet *open_sheet() {
  for (int i = 0; i < s_count; i++)
    if (visible(s_sheets[i]) && s_sheets[i].open)
      return &s_sheets[i];
  return nullptr;
}

Sheet *closed_sheet_at(SheetEdge edge) {
  for (int i = 0; i < s_count; i++) {
    Sheet &s = s_sheets[i];
    if (s.edge == edge && !visible(s) &&
        (!s.policy.may_open || s.policy.may_open()))
      return &s;
  }
  return nullptr;
}

// An open sheet is only ever pushed back toward its own edge, and no other
// sheet opens over it.
Sheet *pick(int32_t dy) {
  if (Sheet *open = open_sheet()) {
    bool closing = open->edge == SheetEdge::Bottom ? dy > 0 : dy < 0;
    bool allowed = !open->policy.may_close_from ||
                   open->policy.may_close_from(s_touch.start);
    return closing && allowed ? open : nullptr;
  }
  if (any_visible())
    return nullptr; // still settling closed: only a catch takes it
  return closed_sheet_at(dy < 0 ? SheetEdge::Bottom : SheetEdge::Top);
}

void touch_down(lv_indev_t *indev, lv_point_t p, uint32_t now) {
  s_touch = {};
  s_touch.down = true;
  s_touch.deciding = true;
  s_touch.start = s_touch.shown = p;
  record(p, now);
  for (int i = 0; i < s_count; i++)
    if (moving(s_sheets[i]) && contains(s_sheets[i], p)) {
      claim(indev, s_sheets[i], p);
      return;
    }
  if (any_visible())
    return;
  for (SheetEdge edge : {SheetEdge::Bottom, SheetEdge::Top})
    if (Sheet *s = closed_sheet_at(edge)) {
      make_material(lv_obj_get_screen(s->obj));
      return;
    }
}

void touch_move(lv_indev_t *indev, lv_point_t p) {
  if (Sheet *s = s_touch.sheet) {
    move_to(*s, shown_y(*s, s_touch.grab_y + p.y - s_touch.finger_y));
    return;
  }
  if (!s_touch.deciding)
    return;
  int32_t dx = p.x - s_touch.start.x;
  int32_t dy = p.y - s_touch.start.y;
  if (std::abs(dx) < SLOP && std::abs(dy) < SLOP)
    return;
  s_touch.deciding = false;
  if (std::abs(dy) <= std::abs(dx))
    return; // sideways: LVGL's
  if (Sheet *s = pick(dy))
    claim(indev, *s, p);
}

void touch_up() {
  s_touch.down = false;
  Sheet *s = s_touch.sheet;
  if (!s)
    return;
  float v = velocity();
  float projected = static_cast<float>(s->y) + v * PROJECT_MS;
  float half = static_cast<float>(closed_y(*s)) / 2.0f;
  bool open = closed_y(*s) > 0 ? projected < half : projected > half;
  settle(*s, open, v);
  s_touch.sheet = nullptr;
}

// The sheets and everything LVGL drags (pages, lists) get the predicted
// point; samples and speeds are kept from the real ones.
void read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  s_read(indev, data);
  uint32_t now = lv_tick_get();
  bool pressed = data->state == LV_INDEV_STATE_PRESSED;
  if (pressed && !s_touch.down) {
    touch_down(indev, data->point, now);
    return;
  }
  if (pressed) {
    record(data->point, now);
    data->point = predicted(data->point);
    s_touch.shown = data->point;
    touch_move(indev, data->point);
  } else if (s_touch.down) {
    // Let go where it was last shown, or a list would jump back.
    data->point = s_touch.shown;
    touch_up();
  }
}

// The frost covers the sheet, rounded where the sheet is: the image's clip
// radius follows `image_area`, so that is the sheet while the picture stays
// where the screen is. Clipping the sheet's corners instead would draw every
// row near an edge, frost and body, through a layer.
void on_draw_frost(lv_event_t *e) {
  const Sheet *s =
      find(static_cast<lv_obj_t *>(lv_event_get_current_target(e)));
  if (!s || !s->frost)
    return;
  lv_draw_image_dsc_t dsc;
  lv_draw_image_dsc_init(&dsc);
  dsc.src = s->frost;
  dsc.clip_radius = RADIUS;
  lv_obj_get_coords(s->obj, &dsc.image_area);
  lv_draw_image(lv_event_get_layer(e), &dsc, &SCREEN);
}

// The frost is drawn in place of the background, so LVGL can't tell the sheet
// covers anything and would draw the whole home screen under it on every
// frame of a slide. Away from the rounded rows it is opaque.
void on_cover_check(lv_event_t *e) {
  auto *info = static_cast<lv_cover_check_info_t *>(lv_event_get_param(e));
  auto *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
  const Sheet *s = find(obj);
  if (info->res == LV_COVER_RES_COVER || !s || !s->frost ||
      lv_obj_get_style_opa(obj, LV_PART_MAIN) < LV_OPA_MAX ||
      lv_obj_get_style_opa_layered(obj, LV_PART_MAIN) < LV_OPA_MAX)
    return;
  lv_area_t solid;
  lv_obj_get_coords(obj, &solid);
  solid.y1 += RADIUS;
  solid.y2 -= RADIUS;
  if (lv_area_is_in(info->area, &solid, 0))
    info->res = LV_COVER_RES_COVER;
}

lv_obj_t *make_grabber(lv_obj_t *sheet, SheetEdge edge) {
  lv_obj_t *g = ui_box(sheet);
  lv_obj_set_size(g, GRABBER_W, GRABBER_H);
  lv_obj_set_style_radius(g, GRABBER_H / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(g, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g, GRABBER_OPA, LV_PART_MAIN);
  lv_obj_align(
      g, edge == SheetEdge::Bottom ? LV_ALIGN_TOP_MID : LV_ALIGN_BOTTOM_MID, 0,
      edge == SheetEdge::Bottom ? GRABBER_INSET : -GRABBER_INSET);
  return g;
}

} // namespace

lv_obj_t *sheet_create(lv_obj_t *parent, SheetEdge edge,
                       const SheetPolicy &policy) {
  if (s_count == MAX_SHEETS)
    return nullptr;
  lv_obj_t *obj = ui_box(parent);
  lv_obj_set_size(obj, LCD_H_RES, LCD_V_RES + SKIRT);
  lv_obj_set_style_bg_color(obj, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(obj, RADIUS, LV_PART_MAIN);
  lv_obj_add_event_cb(obj, on_draw_frost, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_event_cb(obj, on_cover_check, LV_EVENT_COVER_CHECK, nullptr);
  // Taps on the sheet's own background stop here, not on the home screen.
  ui_absorb_taps(obj);

  lv_obj_t *body = ui_box(obj);
  lv_obj_set_size(body, LCD_H_RES, LCD_V_RES);
  lv_obj_set_y(body, body_top(edge));
  make_grabber(body, edge);

  Sheet &s = s_sheets[s_count++];
  s = {obj, body, nullptr, edge, policy, false, 0};
  move_to(s, closed_y(s));
  lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  return body;
}

void sheets_track_touch(lv_indev_t *touch) {
  if (!touch || s_read)
    return;
  s_read = lv_indev_get_read_cb(touch);
  lv_indev_set_read_cb(touch, read_cb);
  if (lv_timer_t *t = lv_indev_get_read_timer(touch))
    lv_timer_set_period(t, TOUCH_READ_MS);
}

void sheet_open(lv_obj_t *sheet) {
  Sheet *s = find(sheet);
  if (!s || (s->open && visible(*s)))
    return;
  appear(*s);
  settle(*s, true, 0.0f);
}

void sheet_close(lv_obj_t *sheet) {
  Sheet *s = find(sheet);
  if (!s || !visible(*s) || (!s->open && moving(*s)) ||
      lv_anim_get(s->obj, anim_opa_cb))
    return;
  if (s_touch.sheet == s)
    s_touch.sheet = nullptr;
  settle(*s, false, 0.0f);
}

void sheet_dissolve(lv_obj_t *sheet) {
  Sheet *s = find(sheet);
  if (!s || !visible(*s) || !s->open)
    return;
  if (s_touch.sheet == s || moving(*s) || s->y != 0) {
    sheet_close(sheet);
    return;
  }
  s->open = false;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s->obj);
  lv_anim_set_exec_cb(&a, anim_opa_cb);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_duration(&a, DISSOLVE_MS);
  motion_ease_out(&a);
  lv_anim_set_completed_cb(&a, [](lv_anim_t *done) {
    Sheet *gone = find(static_cast<lv_obj_t *>(done->var));
    if (!gone)
      return;
    move_to(*gone, closed_y(*gone));
    lv_obj_add_flag(gone->obj, LV_OBJ_FLAG_HIDDEN);
    anim_opa_cb(gone->obj, LV_OPA_COVER);
    if (gone->policy.on_closed)
      gone->policy.on_closed();
  });
  lv_anim_start(&a);
}

// LVGL starts scrolling only once a drag passes its scroll limit, and drops
// the distance covered until then, so whatever it scrolls trails the finger
// by that much. This makes it up, as far as the content can go.
void sheets_scroll_with_finger(lv_event_t *e) {
  auto *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
  lv_indev_t *indev = lv_indev_active();
  if (!indev || !s_touch.down || lv_indev_get_scroll_obj(indev) != obj)
    return;
  lv_point_t vect;
  lv_indev_get_vect(indev, &vect);
  lv_dir_t dir = lv_obj_get_scroll_dir(obj);
  auto lost = [](int32_t moved, int32_t back, int32_t ahead) {
    return std::clamp(moved, -ahead, back);
  };
  int32_t dx =
      (dir & LV_DIR_HOR)
          ? lost(s_touch.shown.x - s_touch.start.x - vect.x,
                 lv_obj_get_scroll_left(obj), lv_obj_get_scroll_right(obj))
          : 0;
  int32_t dy =
      (dir & LV_DIR_VER)
          ? lost(s_touch.shown.y - s_touch.start.y - vect.y,
                 lv_obj_get_scroll_top(obj), lv_obj_get_scroll_bottom(obj))
          : 0;
  // The raw call: lv_obj_scroll_by sends LV_EVENT_SCROLL_BEGIN itself, which
  // would land back here, again and again, until the stack ran out.
  if (dx || dy)
    lv_obj_scroll_by_raw(obj, dx, dy);
}

void sheet_draw_material(lv_obj_t *sheet, lv_layer_t *layer,
                         const lv_area_t *area, lv_opa_t opa) {
  const Sheet *s = find(sheet);
  lv_area_t clip;
  if (!s || opa <= LV_OPA_MIN ||
      !lv_area_intersect(&clip, &layer->_clip_area, area))
    return;
  lv_area_t clip_was = layer->_clip_area;
  layer->_clip_area = clip;
  if (s->frost) {
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = s->frost;
    dsc.opa = opa;
    lv_draw_image(layer, &dsc, &SCREEN);
  } else {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_black();
    dsc.bg_opa = opa;
    lv_draw_rect(layer, &dsc, area);
  }
  layer->_clip_area = clip_was;
}

bool sheet_is_open(const lv_obj_t *sheet) {
  const Sheet *s = find(sheet);
  return s && s->open;
}

bool sheet_is_visible(const lv_obj_t *sheet) {
  const Sheet *s = find(sheet);
  return s && visible(*s);
}
