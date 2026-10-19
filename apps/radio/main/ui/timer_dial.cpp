#include "ui/timer_dial.h"
#include "fonts.h"
#include "hal_pins.h"
#include "ui/clock_art.h"
#include "ui/primitives.h"
#include "ui/ring_strips.h"

#include "esp_heap_caps.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// Proportions from Apple's timer: a tick is about 3.5 times as long as it is
// wide, and as wide as a third of the gap between ticks.
constexpr int TICKS = 60;
constexpr int CENTER = LCD_H_RES / 2;
constexpr float OUTER_R = 167.0f;
constexpr float TICK_LEN = 23.0f;
constexpr float TICK_W = 6.5f;
constexpr int32_t STRIP_ROWS = 12;
constexpr uint32_t COL_LIT = 0xFF9F0A; // Apple's timer orange
constexpr lv_opa_t DIM_OPA = 64;       // a spent tick: the same orange, dark
// Fractions below this don't show, so a draining tick isn't repainted for
// changes nobody can see.
constexpr float VISIBLE_STEP = 1.0f / 64.0f;

const lv_font_t &DIGIT_FONT = sf_rounded_semibold_84;
constexpr int MAX_CHARS = 5; // "60:00"
// Display figures set a little tight, as Apple sets big numbers.
constexpr int DIGIT_TRACKING = -3;
constexpr int COLON_TRACKING = -4;
constexpr int CAPTION_GAP = 18; // digit ink to caption, either side
constexpr lv_opa_t MUTED_OPA = 150;

struct Dial {
  float from, to;
  int digits_y;
  bool above;
  lv_obj_t *chars[MAX_CHARS];
  lv_obj_t *caption;
  char shown[MAX_CHARS + 1];
  ClockArt ring; // every tick at its own opacity, the size of the dial
};

// Each tick is a capsule bitmap drawn once at its exact angle and shared by
// every dial. LVGL's lines snap their ends to whole pixels, which made the
// ticks uneven in length and weight around the ring. A dial copies them into
// one picture at their opacities, so a frame draws one image, not 60.
// Neighbouring ticks' boxes may overlap but their ink never does, so a tick
// only ever writes its own ink.
struct Tick {
  ClockArt art;
  lv_point_t at; // top-left, relative to the dial
};

const Tick &tick(int i) {
  static Tick ticks[TICKS];
  static bool ready;
  if (!ready) {
    constexpr HandShape SHAPE = {{{OUTER_R - TICK_LEN, OUTER_R, TICK_W}, {}},
                                 1};
    for (int k = 0; k < TICKS; k++)
      clock_art_hand(&ticks[k].art, SHAPE, static_cast<float>(k) * 6.0f, CENTER,
                     CENTER, &ticks[k].at);
    ready = true;
  }
  return ticks[i];
}

lv_area_t tick_area(lv_obj_t *obj, int i) {
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  const Tick &t = tick(i);
  int32_t x = coords.x1 + t.at.x, y = coords.y1 + t.at.y;
  return {x, y, x + static_cast<int32_t>(t.art.dsc.header.w) - 1,
          y + static_cast<int32_t>(t.art.dsc.header.h) - 1};
}

Dial *dial_of(lv_obj_t *obj) {
  return static_cast<Dial *>(lv_obj_get_user_data(obj));
}

// How much of tick `i` the lit span covers, quantized to what shows.
float coverage(float from, float to, int i) {
  float lo = std::max(from, static_cast<float>(i));
  float hi = std::min(to, static_cast<float>(i + 1));
  float c = std::clamp(hi - lo, 0.0f, 1.0f);
  return std::round(c / VISIBLE_STEP) * VISIBLE_STEP;
}

// A partly lit tick is copied once at the blended opacity: two layers on top
// of each other would show a darker seam along the round ends.
void paint_tick(Dial *d, int i) {
  const Tick &t = tick(i);
  if (!t.art.pixels || !d->ring.pixels)
    return;
  float c = coverage(d->from, d->to, i);
  auto opa = static_cast<uint32_t>(DIM_OPA + c * (LV_OPA_COVER - DIM_OPA));
  auto w = static_cast<int>(t.art.dsc.header.w);
  auto h = static_cast<int>(t.art.dsc.header.h);
  for (int y = 0; y < h; y++) {
    const uint8_t *src = t.art.pixels + y * w;
    uint8_t *dst = d->ring.pixels + (t.at.y + y) * LCD_H_RES + t.at.x;
    for (int x = 0; x < w; x++)
      if (src[x])
        dst[x] = static_cast<uint8_t>(src[x] * opa / LV_OPA_COVER);
  }
}

void make_ring(Dial *d) {
  constexpr size_t BYTES = static_cast<size_t>(LCD_H_RES) * LCD_V_RES;
  d->ring.pixels = static_cast<uint8_t *>(
      heap_caps_calloc(1, BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!d->ring.pixels)
    return;
  lv_image_dsc_t &dsc = d->ring.dsc;
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = LV_COLOR_FORMAT_A8;
  dsc.header.w = LCD_H_RES;
  dsc.header.h = LCD_V_RES;
  dsc.header.stride = LCD_H_RES;
  dsc.data_size = BYTES;
  dsc.data = d->ring.pixels;
  for (int i = 0; i < TICKS; i++)
    paint_tick(d, i);
}

void on_draw(lv_event_t *e) {
  auto *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
  Dial *d = dial_of(obj);
  if (!d->ring.pixels)
    return;
  lv_draw_image_dsc_t img;
  lv_draw_image_dsc_init(&img);
  img.src = &d->ring.dsc;
  img.recolor = lv_color_hex(COL_LIT);
  img.recolor_opa = LV_OPA_COVER;
  lv_area_t c;
  lv_obj_get_coords(obj, &c);
  lv_layer_t *layer = lv_event_get_layer(e);
  ui_for_each_ring_strip(layer, {c.x1 + CENTER, c.y1 + CENTER},
                         OUTER_R + TICK_W, OUTER_R - TICK_LEN - TICK_W,
                         STRIP_ROWS, [&] { lv_draw_image(layer, &img, &c); });
}

void on_delete(lv_event_t *e) {
  auto *obj = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
  Dial *d = dial_of(obj);
  heap_caps_free(d->ring.pixels);
  lv_free(d);
}

lv_font_glyph_dsc_t glyph(char c) {
  lv_font_glyph_dsc_t g = {};
  lv_font_get_glyph_dsc(&DIGIT_FONT, &g, static_cast<uint8_t>(c), 0);
  return g;
}

int32_t ink_mid(char c) {
  lv_font_glyph_dsc_t g = glyph(c);
  int32_t baseline =
      lv_font_get_line_height(&DIGIT_FONT) - DIGIT_FONT.base_line;
  return baseline - g.ofs_y - g.box_h / 2;
}

int32_t digit_ink_h() { return glyph('0').box_h; }

void place_caption(Dial *d) {
  int32_t half = digit_ink_h() / 2;
  int32_t y = d->above ? d->digits_y - half - CAPTION_GAP
                       : d->digits_y + half + CAPTION_GAP;
  lv_obj_align(d->caption, d->above ? LV_ALIGN_BOTTOM_MID : LV_ALIGN_TOP_MID, 0,
               d->above ? y - LCD_V_RES / 2 : y + LCD_V_RES / 2);
}

int advance(char c) {
  return glyph(c).adv_w + (c == ':' ? COLON_TRACKING : DIGIT_TRACKING);
}

// Proportional figures, like Apple's timer: the number is re-centered as it
// counts, rather than each digit sitting in a fixed slot with a 1 floating in
// a gap. Each character has its own label, so the colon can be lifted to the
// digits' middle should the font set it low.
void layout_digits(Dial *d) {
  int n = static_cast<int>(strlen(d->shown));
  int width = 0;
  for (int i = 0; i < n; i++)
    width += advance(d->shown[i]);
  int x = CENTER - width / 2;
  int32_t top = CENTER + d->digits_y - ink_mid('0');
  int32_t colon_lift = ink_mid(':') - ink_mid('0');
  for (int i = 0; i < MAX_CHARS; i++) {
    lv_obj_t *c = d->chars[i];
    lv_obj_set_flag(c, LV_OBJ_FLAG_HIDDEN, i >= n);
    if (i >= n)
      continue;
    bool is_colon = d->shown[i] == ':';
    int w = advance(d->shown[i]);
    lv_obj_set_pos(c, x, top - (is_colon ? colon_lift : 0));
    lv_obj_set_width(c, w);
    x += w;
  }
  place_caption(d);
}

} // namespace

lv_obj_t *timer_dial_create(lv_obj_t *parent) {
  lv_obj_t *obj = ui_box(parent);
  lv_obj_set_size(obj, LCD_H_RES, LCD_V_RES);
  auto *d = static_cast<Dial *>(lv_malloc_zeroed(sizeof(Dial)));
  d->digits_y = TIMER_DIAL_DIGITS_Y;
  make_ring(d);
  lv_obj_set_user_data(obj, d);
  lv_obj_add_event_cb(obj, on_draw, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_event_cb(obj, on_delete, LV_EVENT_DELETE, nullptr);
  for (lv_obj_t *&c : d->chars) {
    c = lv_label_create(obj);
    lv_obj_set_style_text_font(c, &DIGIT_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(c, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(c, "");
  }
  d->caption = lv_label_create(obj);
  lv_obj_set_style_text_font(d->caption, &sf_semibold_16, LV_PART_MAIN);
  lv_label_set_text(d->caption, "");
  timer_dial_set_seconds(obj, 0);
  return obj;
}

void timer_dial_set_ticks(lv_obj_t *dial, float from, float to) {
  Dial *d = dial_of(dial);
  float was_from = d->from, was_to = d->to;
  d->from = from;
  d->to = to;
  for (int i = 0; i < TICKS; i++) {
    if (coverage(was_from, was_to, i) == coverage(from, to, i))
      continue;
    paint_tick(d, i);
    lv_area_t a = tick_area(dial, i);
    lv_obj_invalidate_area(dial, &a);
  }
}

void timer_dial_set_seconds(lv_obj_t *dial, int seconds) {
  Dial *d = dial_of(dial);
  seconds = std::max(seconds, 0);
  char text[sizeof(Dial::shown)];
  snprintf(text, sizeof(text), "%d:%02d", std::min(seconds / 60, 99) % 100,
           seconds % 60);
  bool relayout = strcmp(text, d->shown) != 0;
  for (int i = 0; text[i]; i++) {
    if (text[i] == d->shown[i])
      continue;
    char one[2] = {text[i], '\0'};
    lv_label_set_text(d->chars[i], one);
  }
  memcpy(d->shown, text, sizeof(d->shown));
  if (relayout)
    layout_digits(d);
}

void timer_dial_set_digits_y(lv_obj_t *dial, int y) {
  Dial *d = dial_of(dial);
  d->digits_y = y;
  layout_digits(d);
}

void timer_dial_set_caption(lv_obj_t *dial, const char *text, bool above,
                            bool muted) {
  Dial *d = dial_of(dial);
  char shown[40];
  size_t i = 0;
  for (; text[i] && i < sizeof(shown) - 1; i++)
    shown[i] = muted ? text[i]
                     : static_cast<char>(
                           std::toupper(static_cast<unsigned char>(text[i])));
  shown[i] = '\0';
  lv_label_set_text(d->caption, shown);
  lv_obj_set_style_text_color(d->caption,
                              muted ? lv_color_white() : lv_color_hex(COL_LIT),
                              LV_PART_MAIN);
  lv_obj_set_style_text_opa(d->caption, muted ? MUTED_OPA : LV_OPA_COVER,
                            LV_PART_MAIN);
  d->above = above;
  place_caption(d);
}
