#include "carousel.h"

#include "hal_pins.h"
#include "motion.h"
#include "primitives.h"
#include "squircle.h"

// lv_draw_sw_transform is only declared in the software renderer's header.
#include "src/draw/sw/lv_draw_sw.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace {

constexpr int TILES = 5; // the middle one and two each side, for the glide
// Tiles sit this far apart beyond their own width.
constexpr int32_t GAP = 40;
constexpr int32_t UNIT = 1000; // positions are in thousandths of a tile
constexpr uint32_t GLIDE_MS = 280;
constexpr uint32_t SPREAD_MS = 260;
constexpr lv_opa_t SIDE_OPA = 80;
// The neighbors are a little smaller, like a cover flow seen face on.
constexpr int32_t FULL_SCALE = LV_SCALE_NONE;
constexpr int32_t SIDE_SCALE = 218; // about 85%

// A tile is drawn from its own pictures: the station's, baked into an app
// icon's shape when the tile gets it, and that scaled down when the tile is.
struct Tile {
  lv_obj_t *box;
  int32_t scale;
  int item;
  uint8_t *full_pixels;
  lv_image_dsc_t full;
  uint8_t *small_pixels;
  lv_image_dsc_t small;
  int32_t small_scale; // what `small` was scaled to, 0 for nothing yet
};

CarouselHooks s_hooks;
int32_t s_tile = CAROUSEL_TILE; // the middle tile's size
lv_obj_t *s_strip;
Tile s_tiles[TILES];
int s_count = 1;
int32_t s_pos;       // unwrapped: whole tiles times UNIT, between them too
int32_t s_target;    // unwrapped, a whole tile
int32_t s_spread;    // how far the neighbors show, 0..UNIT
void (*s_settled)(); // the callback for the open or close under way
lv_timer_t *s_hide_timer;
lv_obj_t *s_canvas; // draws a glyph into s_glyph_buf, never shown
lv_draw_buf_t *s_glyph_buf;

int wrap(int32_t i) { return ((i % s_count) + s_count) % s_count; }

int32_t nearest(int32_t pos) {
  int32_t shifted = pos + UNIT / 2;
  return (shifted >= 0 ? shifted : shifted - UNIT + 1) / UNIT;
}

// A glyph goes into the tile's picture, so it shrinks and fades with the
// tile instead of spilling over a smaller neighbor.
const lv_image_dsc_t *glyph_picture(const char *glyph, lv_color_t color) {
  if (!s_glyph_buf)
    s_glyph_buf = lv_draw_buf_create(s_tile, s_tile, LV_COLOR_FORMAT_RGB565, 0);
  if (!s_glyph_buf)
    return nullptr;
  if (!s_canvas) {
    s_canvas = lv_canvas_create(s_strip);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(s_canvas, s_glyph_buf);
  }
  lv_canvas_fill_bg(s_canvas, color, LV_OPA_COVER);
  lv_layer_t layer;
  lv_canvas_init_layer(s_canvas, &layer);
  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.text = glyph;
  dsc.font = s_hooks.glyph_font;
  dsc.color = lv_color_white();
  dsc.opa = LV_OPA_50;
  dsc.align = LV_TEXT_ALIGN_CENTER;
  int32_t top = (s_tile - lv_font_get_line_height(dsc.font)) / 2;
  lv_area_t area = {0, top, s_tile - 1, s_tile - 1};
  lv_draw_label(&layer, &dsc, &area);
  lv_canvas_finish_layer(s_canvas, &layer);
  // The same buffer holds every glyph in turn.
  lv_image_cache_drop(s_glyph_buf);
  // A draw buffer starts like an image descriptor, which LVGL relies on too.
  return reinterpret_cast<const lv_image_dsc_t *>(s_glyph_buf);
}

void fill(Tile &t, int item) {
  if (t.item == item)
    return;
  t.item = item;
  CarouselTile c = s_hooks.tile(item);
  const lv_image_dsc_t *picture = c.image;
  if (!picture && c.glyph)
    picture = glyph_picture(c.glyph, c.color);
  if (!t.full_pixels)
    t.full_pixels = squircle_alloc(s_tile);
  if (t.full_pixels)
    (c.round ? circle_bake : squircle_bake)(picture, c.color, s_tile,
                                            t.full_pixels, &t.full);
  t.small_scale = 0;
  lv_obj_invalidate(t.box);
}

// A tile dims as it leaves the middle and is gone two places out. While the
// strip opens or closes, only what passes the middle shows.
lv_opa_t opacity(int32_t distance) {
  int32_t d = std::abs(distance);
  int32_t spread = d <= UNIT
                       ? LV_OPA_COVER - (LV_OPA_COVER - SIDE_OPA) * d / UNIT
                       : SIDE_OPA * std::max<int32_t>(0, 2 * UNIT - d) / UNIT;
  int32_t alone = LV_OPA_COVER * std::max<int32_t>(0, UNIT - d) / UNIT;
  return static_cast<lv_opa_t>(alone + (spread - alone) * s_spread / UNIT);
}

// The tile is resized and its picture scaled as it's drawn, not the tile
// transformed, which would render it through a layer every frame. At rest in
// the middle it's full size, so the picture there is a plain copy.
void set_scale(Tile &t, int32_t distance) {
  int32_t d = std::min<int32_t>(std::abs(distance), UNIT);
  int32_t scale = FULL_SCALE - (FULL_SCALE - SIDE_SCALE) * d / UNIT;
  if (scale == t.scale)
    return;
  t.scale = scale;
  int32_t size = s_tile * scale / FULL_SCALE;
  lv_obj_set_size(t.box, size, size);
  lv_obj_invalidate(t.box);
}

// Scaled once per size by LVGL's own filter, so the picture is drawn as a
// plain copy rather than transformed in every band of every frame.
const lv_image_dsc_t *small_picture(Tile &t, int32_t size) {
  if (t.small_scale == t.scale)
    return &t.small;
  if (!t.small_pixels)
    t.small_pixels = squircle_alloc(s_tile);
  if (!t.small_pixels)
    return nullptr;
  lv_draw_image_dsc_t how;
  lv_draw_image_dsc_init(&how);
  how.scale_x = how.scale_y = t.scale;
  how.pivot = {s_tile / 2, s_tile / 2};
  how.antialias = 1;
  int32_t at = (s_tile - size) / 2;
  lv_area_t out = {at, at, at + size - 1, at + size - 1};
  lv_draw_sw_transform(&out, t.full.data, s_tile, s_tile,
                       static_cast<int32_t>(t.full.header.stride), &how,
                       nullptr, LV_COLOR_FORMAT_RGB565A8, t.small_pixels);
  // The decoder may keep what it last read from this descriptor.
  lv_image_cache_drop(&t.small);
  t.small = t.full;
  t.small.header.w = static_cast<uint32_t>(size);
  t.small.header.h = static_cast<uint32_t>(size);
  t.small.header.stride = static_cast<uint32_t>(size * 2);
  t.small.data_size = squircle_bytes(size);
  t.small.data = t.small_pixels;
  t.small_scale = t.scale;
  return &t.small;
}

void on_draw_tile(lv_event_t *e) {
  Tile &t = *static_cast<Tile *>(lv_event_get_user_data(e));
  if (!t.full.data)
    return;
  lv_area_t c;
  lv_obj_get_coords(t.box, &c);
  const lv_image_dsc_t *src =
      t.scale == FULL_SCALE ? &t.full : small_picture(t, lv_area_get_width(&c));
  if (!src)
    return;
  lv_draw_image_dsc_t dsc;
  lv_draw_image_dsc_init(&dsc);
  lv_obj_init_draw_image_dsc(t.box, LV_PART_MAIN, &dsc);
  dsc.src = src;
  lv_draw_image(lv_event_get_layer(e), &dsc, &c);
}

void layout() {
  int32_t base = nearest(s_pos);
  int32_t offset = s_pos - base * UNIT;
  for (int k = 0; k < TILES; k++) {
    Tile &t = s_tiles[k];
    int32_t rel = k - TILES / 2;
    fill(t, wrap(base + rel));
    int32_t distance = rel * UNIT - offset;
    lv_obj_set_style_translate_x(t.box, distance * (s_tile + GAP) / UNIT,
                                 LV_PART_MAIN);
    lv_obj_set_style_opa(t.box, opacity(distance), LV_PART_MAIN);
    set_scale(t, distance);
  }
  s_hooks.moved(wrap(base), offset);
}

void anim_pos_cb(void *, int32_t v) {
  s_pos = v;
  layout();
}

void anim_spread_cb(void *, int32_t v) {
  s_spread = v;
  layout();
}

void start(int32_t *var, lv_anim_exec_xcb_t cb, int32_t to, uint32_t ms,
           lv_anim_completed_cb_t done = nullptr) {
  lv_anim_delete(var, cb);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, var);
  lv_anim_set_exec_cb(&a, cb);
  lv_anim_set_values(&a, *var, to);
  lv_anim_set_duration(&a, ms);
  motion_ease_out(&a);
  if (done)
    lv_anim_set_completed_cb(&a, done);
  lv_anim_start(&a);
}

void settled() {
  if (auto cb = std::exchange(s_settled, nullptr))
    cb();
}

void closed(lv_anim_t *) {
  lv_obj_add_flag(s_strip, LV_OBJ_FLAG_HIDDEN);
  settled();
}

void stop_all() {
  lv_anim_delete(&s_pos, anim_pos_cb);
  lv_anim_delete(&s_spread, anim_spread_cb);
  if (s_hide_timer)
    lv_timer_delete(std::exchange(s_hide_timer, nullptr));
}

void on_strip_deleted(lv_event_t *) {
  stop_all();
  s_canvas = nullptr; // went with the strip
  lv_image_cache_drop(s_glyph_buf);
  if (s_glyph_buf)
    lv_draw_buf_destroy(std::exchange(s_glyph_buf, nullptr));
  for (Tile &t : s_tiles) {
    lv_image_cache_drop(&t.full);
    lv_image_cache_drop(&t.small);
    squircle_free(std::exchange(t.full_pixels, nullptr));
    squircle_free(std::exchange(t.small_pixels, nullptr));
    t.full = t.small = {};
  }
}

Tile make_tile(lv_obj_t *parent) {
  Tile t = {};
  t.box = ui_box(parent);
  lv_obj_set_size(t.box, s_tile, s_tile);
  lv_obj_center(t.box);
  t.scale = FULL_SCALE;
  t.item = -1;
  return t;
}

} // namespace

void carousel_create(lv_obj_t *parent, int32_t y, const CarouselHooks &hooks) {
  stop_all();
  s_hooks = hooks;
  s_tile = hooks.size;
  s_strip = ui_box(parent);
  lv_obj_set_size(s_strip, LCD_H_RES, s_tile);
  lv_obj_align(s_strip, LV_ALIGN_CENTER, 0, y);
  lv_obj_add_flag(s_strip, LV_OBJ_FLAG_HIDDEN);
  // The pager rebuilds the page; nothing may move the old tiles after.
  lv_obj_add_event_cb(s_strip, on_strip_deleted, LV_EVENT_DELETE, nullptr);
  // The middle tile goes last, so the neighbors pass under it.
  for (int k : {0, 4, 1, 3, 2}) {
    s_tiles[k] = make_tile(s_strip);
    lv_obj_add_event_cb(s_tiles[k].box, on_draw_tile, LV_EVENT_DRAW_MAIN,
                        &s_tiles[k]);
  }
}

void carousel_open(int count, int item, void (*opened)()) {
  stop_all();
  s_count = std::max(count, 1);
  s_settled = opened;
  s_pos = s_target = item * UNIT;
  s_spread = 0;
  for (Tile &t : s_tiles)
    t.item = -1;
  layout();
  lv_obj_remove_flag(s_strip, LV_OBJ_FLAG_HIDDEN);
  start(&s_spread, anim_spread_cb, UNIT, SPREAD_MS,
        [](lv_anim_t *) { settled(); });
}

void carousel_turn(int steps) {
  s_target += steps * UNIT;
  start(&s_pos, anim_pos_cb, s_target, GLIDE_MS);
}

void carousel_reload(int count, int item) {
  lv_anim_delete(&s_pos, anim_pos_cb);
  s_count = std::max(count, 1);
  s_pos = s_target = item * UNIT;
  for (Tile &t : s_tiles)
    t.item = -1;
  layout();
}

void carousel_refresh(int item) {
  for (Tile &t : s_tiles)
    if (t.item == item) {
      t.item = -1;
      fill(t, item);
    }
}

int carousel_item() { return wrap(s_target / UNIT); }

void carousel_hide_after(uint32_t ms, void (*done)()) {
  stop_all();
  s_settled = done;
  s_hide_timer = lv_timer_create(
      [](lv_timer_t *) {
        s_hide_timer = nullptr;
        closed(nullptr);
      },
      ms, nullptr);
  lv_timer_set_repeat_count(s_hide_timer, 1);
}

void carousel_close(int item, void (*done)()) {
  int32_t from = s_target / UNIT;
  int32_t ahead = wrap(item - from);
  if (ahead > s_count / 2)
    ahead -= s_count;
  s_target = (from + ahead) * UNIT;
  s_settled = done;
  if (s_pos != s_target)
    start(&s_pos, anim_pos_cb, s_target, GLIDE_MS);
  start(&s_spread, anim_spread_cb, 0, GLIDE_MS, closed);
}
