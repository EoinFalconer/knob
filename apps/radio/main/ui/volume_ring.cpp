#include "ui/volume_ring.h"
#include "hal_pins.h"
#include "ui/motion.h"
#include "ui/primitives.h"
#include "ui/ring_strips.h"

#include "esp_heap_caps.h"

#include <algorithm>
#include <cmath>
#include <numbers>

// The ring is rasterized here into two A8 bitmaps, the track and the level,
// and drawn as tinted images, LVGL's cheapest draw. LVGL's own arcs mask every
// row of the area being repainted, angle by angle: about 22 ms a frame on the
// device whenever anything near the ring moved, or the ring itself did.
//
// - The pixels the ring can ever cover, a 13 px band, are listed once, sorted
//   by angle. When only the ends move (opening, closing, a volume change),
//   only the pixels around the old and new ends are redrawn and repainted.
// - The bitmaps hold shape only; opacity and color are applied as they're
//   drawn, so browse dimming the ring rasterizes nothing.
// - They're drawn in strips that cover just the band, so a repaint inside the
//   ring, like the browse strip, never blits the empty middle.

namespace {

constexpr int32_t SIZE = LCD_H_RES - 4; // kept off the very edge of the glass
constexpr float R = SIZE / 2.0f;
constexpr float MAX_WIDTH = 10.0f;
constexpr float SPAN = 270.0f;
constexpr float TOP = 270.0f; // 12 o'clock, clockwise from 3 o'clock
constexpr float CHUNK_DEG = 30.0f;
constexpr int32_t BLOCK_ROWS = 12;
constexpr int32_t ONE = 1024;
constexpr float DEG = 180.0f / std::numbers::pi_v<float>;

struct State {
  lv_color_t color;
  int32_t indicator_opa;
  int32_t track_opa;
  int32_t width;
  int32_t sweep; // 0 (retracted) to ONE (full span)
};

struct BandPixel {
  float angle; // degrees, clockwise from 3 o'clock
  float r;
  uint16_t x, y;
};

struct Bitmap {
  uint8_t *pixels;
  lv_image_dsc_t dsc;
};

// What the bitmaps depend on; opacity and color are applied when drawing.
struct Shape {
  int32_t width, sweep, level;
  bool operator==(const Shape &) const = default;
};

// An angular stretch, from `from` clockwise over `len` degrees.
struct Stretch {
  float from, len;
};

lv_obj_t *s_ring;
State s_from, s_to, s_now;
int32_t s_min, s_max = 1, s_level;

BandPixel *s_band;
int s_band_count;
Bitmap s_track, s_indicator;
constexpr Shape NONE = {-1, -1, -1};
Shape s_drawn = NONE; // what the bitmaps hold
Shape s_shown = NONE; // what the last repaint request was for

State state_of(const RingLook &look) {
  return {look.color, look.indicator_opa, look.track_opa,
          std::min<int32_t>(look.width, static_cast<int32_t>(MAX_WIDTH)),
          look.shown ? ONE : 0};
}

int32_t lerp(int32_t a, int32_t b, int32_t t) { return a + (b - a) * t / ONE; }

float wrap360(float a) {
  a = std::fmod(a, 360.0f);
  return a < 0 ? a + 360.0f : a;
}

bool in_band(float r) { return r >= R - MAX_WIDTH - 1.5f && r <= R + 0.5f; }

// PSRAM: each bitmap is 125 KB and the band list about as much.
bool make_bitmap(Bitmap *b) {
  b->pixels = static_cast<uint8_t *>(
      heap_caps_calloc(1, static_cast<size_t>(SIZE) * SIZE, MALLOC_CAP_SPIRAM));
  if (!b->pixels)
    return false;
  b->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  b->dsc.header.cf = LV_COLOR_FORMAT_A8;
  b->dsc.header.w = SIZE;
  b->dsc.header.h = SIZE;
  b->dsc.header.stride = SIZE;
  b->dsc.data_size = SIZE * SIZE;
  b->dsc.data = b->pixels;
  return true;
}

bool build_band() {
  if (s_band)
    return true;
  int count = 0;
  for (int y = 0; y < SIZE; y++)
    for (int x = 0; x < SIZE; x++)
      count += in_band(std::hypot(x + 0.5f - R, y + 0.5f - R));
  s_band = static_cast<BandPixel *>(heap_caps_malloc(
      sizeof(BandPixel) * static_cast<size_t>(count), MALLOC_CAP_SPIRAM));
  if (!s_band || !make_bitmap(&s_track) || !make_bitmap(&s_indicator)) {
    heap_caps_free(s_band);
    s_band = nullptr;
    return false;
  }
  for (int y = 0; y < SIZE; y++) {
    for (int x = 0; x < SIZE; x++) {
      float dx = x + 0.5f - R, dy = y + 0.5f - R;
      float r = std::hypot(dx, dy);
      if (in_band(r))
        s_band[s_band_count++] = {wrap360(std::atan2(dy, dx) * DEG), r,
                                  static_cast<uint16_t>(x),
                                  static_cast<uint16_t>(y)};
    }
  }
  std::sort(
      s_band, s_band + s_band_count,
      [](const BandPixel &a, const BandPixel &b) { return a.angle < b.angle; });
  return true;
}

// An arc from `start` over `span` degrees, `width` px thick inward from the
// rim, with round ends like LVGL's rounded arcs.
struct Arc {
  float start, span, half, mid;
  float cap_x[2], cap_y[2];

  Arc(float start_deg, float span_deg, float width)
      : start(start_deg), span(span_deg), half(width / 2.0f),
        mid(R - width / 2.0f) {
    for (int i = 0; i < 2; i++) {
      float a = (start + (i ? span : 0.0f)) / DEG;
      cap_x[i] = R + mid * std::cos(a);
      cap_y[i] = R + mid * std::sin(a);
    }
  }

  float cap(const BandPixel &p, int i) const {
    float dx = p.x + 0.5f - cap_x[i], dy = p.y + 0.5f - cap_y[i];
    float d2 = dx * dx + dy * dy;
    if (d2 >= (half + 0.5f) * (half + 0.5f))
      return 0.0f;
    return std::min(1.0f, half + 0.5f - std::sqrt(d2));
  }

  // `radial` is the coverage across the ring's width at this pixel.
  uint8_t coverage(const BandPixel &p, float radial) const {
    if (span <= 0.0f)
      return 0;
    float rel = wrap360(p.angle - start);
    float c = rel <= span ? radial : std::max(cap(p, 0), cap(p, 1));
    return static_cast<uint8_t>(std::lround(c * 255.0f));
  }
};

struct Geometry {
  float width, start, span, level_span;
};

Geometry geometry(const Shape &s) {
  float span = SPAN * static_cast<float>(s.sweep) / ONE;
  float level = static_cast<float>(s.level - s_min) / (s_max - s_min);
  return {std::max(1.0f, static_cast<float>(s.width)),
          wrap360(TOP - span / 2.0f), span, span * level};
}

// Past the end of an arc, its round cap reaches this far round the ring.
float cap_deg(float width) { return (width / 2.0f + 1.5f) / (R - width) * DEG; }

// Where the ends moved from `a` to `b`, with their caps, when the width is the
// same. Returns how many stretches it wrote.
int moved_ends(const Shape &a, const Shape &b, Stretch out[3]) {
  Geometry ga = geometry(a), gb = geometry(b);
  float pad = cap_deg(gb.width);
  float ends_a[3] = {ga.start, ga.start + ga.span, ga.start + ga.level_span};
  float ends_b[3] = {gb.start, gb.start + gb.span, gb.start + gb.level_span};
  int n = 0;
  for (int i = 0; i < 3; i++) {
    float delta = wrap360(ends_b[i] - ends_a[i] + 180.0f) - 180.0f;
    if (delta == 0.0f && !(i == 2 && a.level != b.level))
      continue;
    float from = std::min(ends_a[i], ends_a[i] + delta);
    out[n++] = {wrap360(from - pad), std::fabs(delta) + 2.0f * pad};
  }
  return n;
}

void redraw_pixel(const BandPixel &p, const Arc &track, const Arc &indicator) {
  int at = p.y * SIZE + p.x;
  float radial = track.half + 0.5f - std::fabs(p.r - track.mid);
  if (radial <= 0.0f) {
    s_track.pixels[at] = 0;
    s_indicator.pixels[at] = 0;
    return;
  }
  radial = std::min(radial, 1.0f);
  s_track.pixels[at] = track.coverage(p, radial);
  s_indicator.pixels[at] = indicator.coverage(p, radial);
}

template <typename Fn> void for_each_in(const Stretch &s, Fn fn) {
  auto visit = [&](float lo, float hi) {
    BandPixel *begin = std::lower_bound(
        s_band, s_band + s_band_count, lo,
        [](const BandPixel &p, float a) { return p.angle < a; });
    for (BandPixel *p = begin; p < s_band + s_band_count && p->angle <= hi; p++)
      fn(*p);
  };
  float to = s.from + s.len;
  visit(s.from, std::min(to, 360.0f));
  if (to > 360.0f)
    visit(0.0f, to - 360.0f);
}

void rasterize(const Shape &shape) {
  Geometry g = geometry(shape);
  Arc track(g.start, g.span, g.width);
  Arc indicator(g.start, g.level_span, g.width);
  Stretch moved[3];
  int n = s_drawn.width == shape.width ? moved_ends(s_drawn, shape, moved) : -1;
  s_drawn = shape;
  if (n < 0) {
    for (int i = 0; i < s_band_count; i++)
      redraw_pixel(s_band[i], track, indicator);
    return;
  }
  for (int i = 0; i < n; i++)
    for_each_in(moved[i],
                [&](const BandPixel &p) { redraw_pixel(p, track, indicator); });
}

void invalidate_stretch(const Stretch &s) {
  lv_area_t c;
  lv_obj_get_coords(s_ring, &c);
  float cx = c.x1 + R, cy = c.y1 + R;
  for (float a = 0; a < s.len; a += CHUNK_DEG) {
    float to = std::min(s.len, a + CHUNK_DEG);
    float x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
    // The chunk's corners and enough points between that no bulge is missed.
    for (float d = a;; d = std::min(to, d + 5.0f)) {
      float rad = (s.from + d) / DEG;
      for (float radius : {R + 1.0f, R - MAX_WIDTH - 2.0f}) {
        float x = cx + radius * std::cos(rad), y = cy + radius * std::sin(rad);
        x1 = std::min(x1, x);
        y1 = std::min(y1, y);
        x2 = std::max(x2, x);
        y2 = std::max(y2, y);
      }
      if (d >= to)
        break;
    }
    lv_area_t area = {static_cast<int32_t>(std::floor(x1)) - 1,
                      static_cast<int32_t>(std::floor(y1)) - 1,
                      static_cast<int32_t>(std::ceil(x2)) + 1,
                      static_cast<int32_t>(std::ceil(y2)) + 1};
    lv_obj_invalidate_area(s_ring, &area);
  }
}

void invalidate_band() { invalidate_stretch({0.0f, 360.0f}); }

int32_t fade_of(int32_t sweep) { return std::min(ONE, sweep * 5 / 2); }

// Repaints what changed since `before`: just the moved ends when only they
// did, else the whole band.
void repaint(const State &before) {
  Shape shape = {s_now.width, s_now.sweep, s_level};
  bool same_paint = lv_color_eq(before.color, s_now.color) &&
                    before.indicator_opa == s_now.indicator_opa &&
                    before.track_opa == s_now.track_opa &&
                    fade_of(before.sweep) == fade_of(s_now.sweep);
  Stretch moved[3];
  if (same_paint && s_shown.width == shape.width) {
    int n = moved_ends(s_shown, shape, moved);
    for (int i = 0; i < n; i++)
      invalidate_stretch(moved[i]);
  } else if (!(same_paint && s_shown == shape)) {
    invalidate_band();
  }
  s_shown = shape;
}

void step(void *, int32_t t) {
  State before = s_now;
  s_now = {lv_color_mix(s_to.color, s_from.color, t * 255 / ONE),
           lerp(s_from.indicator_opa, s_to.indicator_opa, t),
           lerp(s_from.track_opa, s_to.track_opa, t),
           lerp(s_from.width, s_to.width, t),
           lerp(s_from.sweep, s_to.sweep, t)};
  repaint(before);
}

// A retracting ring closes toward 12 o'clock and fades over the last stretch,
// so it never ends on a hard dot.
void on_draw(lv_event_t *e) {
  if (s_now.sweep == 0 || !s_band)
    return;
  Shape shape = {s_now.width, s_now.sweep, s_level};
  if (!(shape == s_drawn))
    rasterize(shape);
  int32_t fade = fade_of(s_now.sweep);
  lv_draw_image_dsc_t track, indicator;
  for (lv_draw_image_dsc_t *d : {&track, &indicator}) {
    lv_draw_image_dsc_init(d);
    d->recolor = s_now.color;
    d->recolor_opa = LV_OPA_COVER;
  }
  track.src = &s_track.dsc;
  track.opa = static_cast<lv_opa_t>(s_now.track_opa * fade / ONE);
  indicator.src = &s_indicator.dsc;
  indicator.opa = static_cast<lv_opa_t>(s_now.indicator_opa * fade / ONE);

  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t c;
  lv_obj_get_coords(s_ring, &c);
  lv_point_t center = {c.x1 + SIZE / 2, c.y1 + SIZE / 2};
  ui_for_each_ring_strip(layer, center, R + 1.0f, R - MAX_WIDTH - 2.0f,
                         BLOCK_ROWS, [&] {
                           if (track.opa > LV_OPA_MIN)
                             lv_draw_image(layer, &track, &c);
                           if (indicator.opa > LV_OPA_MIN && s_level > s_min)
                             lv_draw_image(layer, &indicator, &c);
                         });
}

void on_delete(lv_event_t *) { s_ring = nullptr; }

bool same(const State &a, const State &b) {
  return lv_color_eq(a.color, b.color) && a.indicator_opa == b.indicator_opa &&
         a.track_opa == b.track_opa && a.width == b.width && a.sweep == b.sweep;
}

} // namespace

void volume_ring_create(lv_obj_t *parent, int32_t min, int32_t max,
                        int32_t level) {
  build_band();
  s_min = min;
  s_max = std::max(max, min + 1);
  s_level = std::clamp(level, s_min, s_max);
  s_drawn = s_shown = NONE;
  s_ring = ui_box(parent);
  lv_obj_set_size(s_ring, SIZE, SIZE);
  lv_obj_center(s_ring);
  lv_obj_add_event_cb(s_ring, on_draw, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_event_cb(s_ring, on_delete, LV_EVENT_DELETE, nullptr);
}

void volume_ring_set_level(int32_t level) {
  level = std::clamp(level, s_min, s_max);
  if (!s_ring || level == s_level)
    return;
  s_level = level;
  if (!lv_anim_get(s_ring, step))
    repaint(s_now);
}

// Every state change in the UI asks for a look; most don't change it, and an
// animation to where it already is would repaint the band for nothing.
void volume_ring_set_look(const RingLook &look, uint32_t duration_ms) {
  if (!s_ring)
    return;
  State to = state_of(look);
  if (same(to, s_to) && (same(s_now, s_to) || lv_anim_get(s_ring, step)))
    return;
  lv_anim_delete(s_ring, step);
  s_from = s_now;
  s_to = to;
  if (duration_ms == 0) {
    step(nullptr, ONE);
    return;
  }
  bool rising = s_to.sweep > s_from.sweep || s_to.width > s_from.width ||
                s_to.indicator_opa > s_from.indicator_opa;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_ring);
  lv_anim_set_exec_cb(&a, step);
  lv_anim_set_values(&a, 0, ONE);
  lv_anim_set_duration(&a, duration_ms);
  if (rising)
    motion_ease_out(&a);
  else
    motion_ease_in_out(&a);
  lv_anim_start(&a);
}

lv_obj_t *volume_ring_obj() { return s_ring; }
