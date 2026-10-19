#include "ui/faces.h"
#include "art_decoder.h"
#include "fonts.h"
#include "hal_pins.h"
#include "ui/clock_art.h"
#include "ui/face_palette.h"
#include "motion.h"
#include "primitives.h"
#include "ui/timer_dial.h"
#include "ui/timer_sheet.h"
#include "ui/weather_assets.h"
#include "ui/weather_kind.h"

#include "esp_heap_caps.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <numbers>

namespace {

constexpr int SIZE = LCD_H_RES;
constexpr int CENTER = SIZE / 2;

constexpr uint32_t CLOCK_FACE_MS = 20000;
constexpr uint32_t WEATHER_FACE_MS = 10000;

// Faces behave like a stack: the old one leaves upward and the next rises from
// below, top to bottom like a wave. Each spot on screen shows one face at a
// time, so there's none of the double exposure a crossfade has. The sky color
// morphs in place because the screen itself doesn't move.
constexpr int EXIT_MS = 160;
constexpr int EXIT_WAVE_MS = 100; // top of the screen to the bottom
constexpr int EXIT_SHIFT = -10;
constexpr int ENTER_MS = 360;
constexpr int ENTER_DELAY_MS = 100;
constexpr int ENTER_WAVE_MS = 160;
constexpr int ENTER_SHIFT = 16;
// A swipe sideways changes faces the same way, turned on its side: the old
// face leaves the way the finger went and the next comes in behind it,
// further, since the hand set it moving.
constexpr int SWIPE_EXIT_SHIFT = 24;
constexpr int SWIPE_ENTER_SHIFT = 40;
constexpr int SKY_MS = 450;
constexpr int SHOW_MS = 250;
constexpr int HIDE_MS = 100;
constexpr int32_t PROGRESS_END = 256;

constexpr uint32_t COL_WHITE = 0xFFFFFF;
// The clock's colors follow the time of day; this is how long they take to
// change when the part of the day turns over.
constexpr int PALETTE_MS = 1200;

// Numerals as big as the round face allows, so they meet in the middle like
// Apple's: 12 and 6 nearly touch the center, and 9 and 3 overlap the 12.
constexpr int NUMERAL_TRACKING_12 = -8;

constexpr int HAND_STEM_LEN = 22;
constexpr int HAND_STEM_W = 3;
constexpr int HAND_W = 11;
constexpr int HOUR_LEN = 94;
constexpr int MINUTE_LEN = 156;
constexpr int SECOND_LEN = 162;
constexpr int SECOND_TAIL = 26;
constexpr float SECOND_W = 2.2f;
constexpr int CAP_D = 12;
constexpr int PIN_D = 4;

// The hands sweep. All three are anti-aliased bitmaps drawn at sub-pixel
// angles. The second hand is redrawn 30 times a second, its tip moving about
// half a pixel each time; the minute and hour hands whenever the minute hand's
// tip has moved a quarter pixel (once a second), so they glide too.
constexpr uint32_t SWEEP_MS = 33;
constexpr float HAND_REDRAW_DEG = 0.1f;
// The numerals' wobble. Each detent pushes a numeral's lean along the dial,
// and the lean fades back to rest; the numeral follows it on an underdamped
// spring, so a single click overshoots and settles in about half a second.
// A fast spin keeps topping the lean up to its limit, where the numerals
// hold still, leaning into the turn, instead of shaking (and repainting
// every frame); let go and they spring home.
constexpr float WOBBLE_OMEGA = 0.0138f; // rad/ms, about 2.2 Hz
constexpr float WOBBLE_DAMPING = 0.28f;
constexpr float LEAN_PER_DETENT = 9.0f; // px
constexpr float LEAN_MAX = 14.0f;       // px
constexpr float LEAN_FADE_MS = 100.0f;  // time constant
// The lean holds while detents keep coming this close together, so a spin
// doesn't saw-tooth it (and ripple the numerals a pixel each frame).
constexpr uint32_t LEAN_HOLD_MS = 80;
constexpr uint32_t WOBBLE_STAGGER_MS = 45;
constexpr uint32_t WOBBLE_FRAME_MS = 16;

// Weather face type, after HomeView: weight runs opposite to size. The hero
// number is light and big; everything small is semibold and fully white, so
// hierarchy comes from size and weight, not from dimming text over a photo.
// Rows are placed by ink center, relative to the screen center.
constexpr int TIME_TOP = 38;
constexpr int TEMP_Y = -50;
constexpr int TEMP_TRACKING = -5; // display sizes set tight
const lv_font_t &TEMP_FONT = sf_rounded_regular_132;
constexpr int CONDITION_Y = 26;
constexpr int RANGE_Y = 50;
constexpr int FACTS_Y = 98;
constexpr int FACT_W = 76;
constexpr int FACT_H = 62;
// The hero number is glass: translucent white over the sky, lifted by a soft
// shadow, instead of a flat sticker.
constexpr lv_opa_t GLASS_OPA = 175;
constexpr lv_opa_t GLASS_SHADOW_OPA = 46;
constexpr int GLASS_SHADOW_Y = 3;
constexpr uint32_t COL_SKY_FALLBACK = 0x3A86D6;

// While a timer runs, the idle screen shows it instead of the clock and the
// weather, and doesn't rotate.
enum class Face { Clock, Weather, Timer };
constexpr uint32_t COL_TIMER_SKY = 0x000000;
constexpr uint32_t COL_TIMER_ACCENT = 0xFF9F0A;

// A rounded lv_line is a slanted body with a circle stamped on each end, and
// at a hand's width the circle sits visibly off the body. Its ends also snap
// to whole pixels, so a thin hand wobbles about the center as it turns. The
// hands are drawn as true capsules at sub-pixel positions instead
// (clock_art). The bitmap outlives the widget, which the pager rebuilds.
struct Hand {
  HandShape shape;
  ClockArt art;
  lv_obj_t *img;
  float drawn; // degrees, negative until first drawn
};

enum Numeral { N12, N9, N3, N6, NUMERAL_COUNT };

// Turning the knob on the clock jostles the numerals: each is pushed along the
// dial in the turn's direction, one after another around the face, and springs
// back with a little wobble, as if the knob had nudged the dial.
struct Wobble {
  float offset;  // px along the dial, clockwise positive
  float speed;   // px/ms
  float lean;    // where the spring pulls, fading to 0
  float pending; // lean still to add at push_at, for the stagger
  uint32_t push_at;
  uint32_t pushed_at; // when the lean last grew
};

lv_obj_t *s_root;
lv_obj_t *s_clock;
lv_obj_t *s_weather;
Face s_face = Face::Clock;
bool s_visible;
lv_timer_t *s_rotate_timer;
lv_timer_t *s_minute_timer;
lv_color_t s_sky_from;
lv_color_t s_sky_to;

lv_obj_t *s_date;
// The stem runs into the bar's rounded end, so the joint has no gap.
constexpr HandShape watch_hand(int length) {
  return {
      {{0.0f, HAND_STEM_LEN + HAND_W / 2.0f + HAND_STEM_W / 2.0f, HAND_STEM_W},
       {HAND_STEM_LEN, static_cast<float>(length), HAND_W}},
      2};
}

Hand s_hour_hand = {watch_hand(HOUR_LEN), {}, nullptr, -1.0f};
Hand s_minute_hand = {watch_hand(MINUTE_LEN), {}, nullptr, -1.0f};
Hand s_second_hand = {
    {{{-SECOND_TAIL, SECOND_LEN, SECOND_W}, {}}, 1}, {}, nullptr, -1.0f};
lv_obj_t *s_cap;
lv_obj_t *s_pin;
lv_obj_t *s_numerals[NUMERAL_COUNT];
ClockArt s_numeral_art[NUMERAL_COUNT];
lv_point_t s_numeral_home[NUMERAL_COUNT];
Wobble s_wobble[NUMERAL_COUNT];
lv_timer_t *s_wobble_timer;
uint32_t s_wobble_last_ms;
lv_timer_t *s_sweep_timer;
DayPart s_part = DayPart::Day;
FacePalette s_palette = face_palette(DayPart::Day);
FacePalette s_palette_from;

// The decoded sky stays in PSRAM while the weather keeps the same look. A new
// one is decoded outside the display lock, then swapped in under it.
struct Sky {
  uint8_t *pixels = nullptr;
  int key = -1;
  lv_color_t average;
};
Sky s_sky;
Sky s_next_sky;
lv_image_dsc_t s_sky_dsc;
lv_obj_t *s_sky_img;

enum Fact { FACT_SUN, FACT_WIND, FACT_HUMIDITY, FACT_COUNT };

struct FactCell {
  lv_obj_t *icon, *value, *label;
};

lv_obj_t *s_wx_time;
lv_obj_t *s_wx_hero;
lv_obj_t *s_wx_temp;
lv_obj_t *s_wx_temp_shadow;
lv_obj_t *s_wx_icon;
lv_obj_t *s_wx_condition;
lv_obj_t *s_wx_range;
lv_obj_t *s_wx_facts;
FactCell s_facts[FACT_COUNT];

WeatherData s_wx;
bool s_wx_valid;

// ─── Helpers ────────────────────────────────────────────────────────────────

lv_obj_t *make_layer(lv_obj_t *parent) {
  lv_obj_t *obj = ui_box(parent);
  lv_obj_set_size(obj, SIZE, SIZE);
  return obj;
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                     lv_opa_t opa = LV_OPA_COVER) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_text_opa(lbl, opa, LV_PART_MAIN);
  return lbl;
}

lv_obj_t *make_dot(lv_obj_t *parent, int diameter, uint32_t color) {
  lv_obj_t *dot = ui_box(parent);
  lv_obj_set_size(dot, diameter, diameter);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(dot, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_pos(dot, CENTER - diameter / 2, CENTER - diameter / 2);
  return dot;
}

int round_temp(float t) { return static_cast<int>(std::lround(t)); }

tm local_now() {
  time_t t = time(nullptr);
  tm now = {};
  localtime_r(&t, &now);
  return now;
}

// Where an ASCII string's glyphs actually draw, relative to the label's
// top-left. Big numerals are placed by this so they line up on their ink, not
// on the font's side bearings and line gap. The first `skip` characters move
// the pen but don't count, so a run can be measured where it sits in a label.
lv_area_t ink_bounds(const lv_font_t *font, const char *text, int tracking = 0,
                     int skip = 0) {
  lv_area_t ink = {INT16_MAX, INT16_MAX, INT16_MIN, INT16_MIN};
  int32_t pen = 0;
  int32_t baseline = lv_font_get_line_height(font) - font->base_line;
  for (int i = 0; text[i]; i++) {
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(font, &g, static_cast<uint8_t>(text[i]), 0))
      continue;
    if (g.box_w > 0 && i >= skip) {
      ink.x1 = std::min<int32_t>(ink.x1, pen + g.ofs_x);
      ink.x2 = std::max<int32_t>(ink.x2, pen + g.ofs_x + g.box_w - 1);
      ink.y1 = std::min<int32_t>(ink.y1, baseline - g.ofs_y - g.box_h);
      ink.y2 = std::max<int32_t>(ink.y2, baseline - g.ofs_y - 1);
    }
    pen += g.adv_w + tracking;
  }
  return ink;
}

// ─── Clock face ─────────────────────────────────────────────────────────────

struct NumeralSpot {
  const char *text;
  lv_dir_t side;
  // From the ink box to the screen edge on that side. Measured so the ink
  // that sticks out furthest (the 12's top corners, the 6's bowl) sits 12 px
  // inside the round edge.
  int edge;
};

constexpr NumeralSpot NUMERAL_SPOTS[NUMERAL_COUNT] = {
    {"12", LV_DIR_TOP, 20},
    {"9", LV_DIR_LEFT, 15},
    {"3", LV_DIR_RIGHT, 16},
    {"6", LV_DIR_BOTTOM, 12},
};

// Shows clock art in one color.
lv_obj_t *make_art(lv_obj_t *parent, uint32_t color) {
  lv_obj_t *img = lv_image_create(parent);
  lv_obj_set_style_image_recolor(img, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, LV_PART_MAIN);
  return img;
}

// A label this size repaints a quarter of its font height around itself and
// lets ink spill past its box, so a moving one is slow and clips. As a bitmap
// cropped to the ink, it repaints exactly what it covers.
lv_obj_t *add_numeral(lv_obj_t *face, int i, lv_point_t *home) {
  const NumeralSpot &spot = NUMERAL_SPOTS[i];
  ClockArt &art = s_numeral_art[i];
  int tracking = strlen(spot.text) > 1 ? NUMERAL_TRACKING_12 : 0;
  if (!art.pixels)
    clock_art_text(&art, &sf_rounded_regular_200, spot.text, tracking);
  lv_obj_t *img = make_art(face, s_palette.ink);
  if (art.pixels)
    lv_image_set_src(img, &art.dsc);
  auto w = static_cast<int32_t>(art.dsc.header.w);
  auto h = static_cast<int32_t>(art.dsc.header.h);
  int32_t x = CENTER - w / 2;
  int32_t y = CENTER - h / 2;
  if (spot.side == LV_DIR_TOP)
    y = spot.edge;
  else if (spot.side == LV_DIR_BOTTOM)
    y = SIZE - spot.edge - h;
  else if (spot.side == LV_DIR_LEFT)
    x = spot.edge;
  else
    x = SIZE - spot.edge - w;
  *home = {x, y};
  lv_obj_set_pos(img, x, y);
  return img;
}

void point_hand(Hand &hand, float degrees, bool force) {
  bool moved =
      hand.drawn < 0.0f || std::fabs(degrees - hand.drawn) >= HAND_REDRAW_DEG;
  lv_point_t at;
  if (!(force || moved) ||
      !clock_art_hand(&hand.art, hand.shape, degrees, CENTER, CENTER, &at))
    return;
  hand.drawn = degrees;
  lv_image_set_src(hand.img, &hand.art.dsc);
  lv_obj_set_pos(hand.img, at.x, at.y);
}

void make_hand(lv_obj_t *hands, Hand &hand) {
  hand.img = make_art(hands, s_palette.hand);
  hand.drawn = -1.0f;
}

void build_clock(lv_obj_t *face) {
  for (int i = 0; i < NUMERAL_COUNT; i++) {
    s_numerals[i] = add_numeral(face, i, &s_numeral_home[i]);
    s_wobble[i] = {};
  }
  s_date = make_label(face, &sf_semibold_16, s_palette.ink);
  // Small caps read better a little apart.
  lv_obj_set_style_text_letter_space(s_date, 1, LV_PART_MAIN);

  // Last, so the hands pass over the numerals and the date.
  lv_obj_t *hands = make_layer(face);
  make_hand(hands, s_hour_hand);
  make_hand(hands, s_minute_hand);
  make_hand(hands, s_second_hand);
  s_cap = make_dot(hands, CAP_D, s_palette.hand);
  s_pin = make_dot(hands, PIN_D, s_palette.sky);
}

// Local time to the millisecond, which the hands sweep from.
struct Now {
  tm local;
  int ms;
};

Now precise_now() {
  timespec ts = {};
  clock_gettime(CLOCK_REALTIME, &ts);
  Now now = {};
  localtime_r(&ts.tv_sec, &now.local);
  now.ms = static_cast<int>(ts.tv_nsec / 1000000);
  return now;
}

void sweep_hands(bool force) {
  Now now = precise_now();
  float sec = static_cast<float>(now.local.tm_sec) +
              static_cast<float>(now.ms) / 1000.0f;
  float min = static_cast<float>(now.local.tm_min) + sec / 60.0f;
  float hour = static_cast<float>(now.local.tm_hour % 12) + min / 60.0f;
  point_hand(s_second_hand, sec * 6.0f, force);
  point_hand(s_minute_hand, min * 6.0f, force);
  point_hand(s_hour_hand, hour * 30.0f, force);
}

lv_obj_t *s_timer_face;
lv_obj_t *s_timer_dial;
bool s_timer_active;

void tick_timer_face();

void update_timer_face() {
  timer_dial_set_caption(s_timer_dial, timer_caption(), false, false);
  tick_timer_face();
}

void tick_timer_face() {
  uint32_t total = std::max<uint32_t>(timer_total_ms(), 1);
  uint32_t left = timer_left_ms();
  float drained =
      60.0f * (1.0f - static_cast<float>(left) / static_cast<float>(total));
  timer_dial_set_ticks(s_timer_dial, drained, 60.0f);
  timer_dial_set_seconds(s_timer_dial, static_cast<int>((left + 999) / 1000));
}

void on_sweep(lv_timer_t *) {
  if (!s_visible)
    return;
  if (s_face == Face::Clock)
    sweep_hands(false);
  else if (s_face == Face::Timer)
    tick_timer_face();
}

// ─── Numeral wobble

// Which way "clockwise along the dial" points at each numeral.
constexpr lv_point_t DIAL_TANGENT[NUMERAL_COUNT] = {
    {1, 0}, {0, -1}, {0, 1}, {-1, 0}};
// The order a clockwise turn travels around the face.
constexpr Numeral CLOCKWISE[NUMERAL_COUNT] = {N12, N3, N6, N9};

void place_numeral(int i) {
  auto off = static_cast<int32_t>(std::lround(s_wobble[i].offset));
  lv_obj_set_pos(s_numerals[i], s_numeral_home[i].x + DIAL_TANGENT[i].x * off,
                 s_numeral_home[i].y + DIAL_TANGENT[i].y * off);
}

bool step_wobble(Wobble &w, uint32_t now, float dt) {
  if (w.pending != 0.0f && static_cast<int32_t>(now - w.push_at) >= 0) {
    w.lean = std::clamp(w.lean + w.pending, -LEAN_MAX, LEAN_MAX);
    w.pending = 0.0f;
    w.pushed_at = now;
  }
  bool holding = now - w.pushed_at < LEAN_HOLD_MS;
  // Semi-implicit Euler in small steps stays stable at this stiffness.
  constexpr float STEP = 4.0f;
  for (float t = 0.0f; t < dt; t += STEP) {
    float h = std::min(STEP, dt - t);
    float accel = -WOBBLE_OMEGA * WOBBLE_OMEGA * (w.offset - w.lean) -
                  2.0f * WOBBLE_DAMPING * WOBBLE_OMEGA * w.speed;
    w.speed += accel * h;
    w.offset = std::clamp(w.offset + w.speed * h, -LEAN_MAX, LEAN_MAX);
    if (!holding)
      w.lean *= 1.0f - h / LEAN_FADE_MS;
  }
  bool resting = w.pending == 0.0f && std::fabs(w.lean) < 0.3f &&
                 std::fabs(w.offset) < 0.3f && std::fabs(w.speed) < 0.005f;
  if (resting)
    w = {};
  return !resting;
}

void on_wobble(lv_timer_t *) {
  uint32_t now = lv_tick_get();
  float dt = static_cast<float>(std::min<uint32_t>(now - s_wobble_last_ms, 40));
  s_wobble_last_ms = now;
  bool any = false;
  for (int i = 0; i < NUMERAL_COUNT; i++) {
    any = step_wobble(s_wobble[i], now, dt) || any;
    place_numeral(i);
  }
  if (!any)
    lv_timer_pause(s_wobble_timer);
}

void update_clock(const tm &now) {
  sweep_hands(true);

  static const char *const DAYS[] = {"SUN", "MON", "TUE", "WED",
                                     "THU", "FRI", "SAT"};
  char date[12];
  snprintf(date, sizeof(date), "%s %d", DAYS[now.tm_wday], now.tm_mday);
  lv_label_set_text(s_date, date);
  // Tucked between the 9 and the 6, where Apple's faces keep the date.
  lv_obj_align(s_date, LV_ALIGN_CENTER, -88, 88);
}

// ─── Weather face ───────────────────────────────────────────────────────────

#define ICON(name)                                                             \
  IconArt { &icon_lg_##name, &icon_sm_##name }

struct IconArt {
  const lv_image_dsc_t *large;
  const lv_image_dsc_t *small;
};

IconArt icon_art(const char *symbol, bool night_fallback) {
  bool night = weather_is_night(symbol, night_fallback);
  switch (weather_kind(symbol)) {
  case WeatherKind::Clear:
    return night ? ICON(clear_night) : ICON(clear_day);
  case WeatherKind::Fair:
    return night ? ICON(fair_night) : ICON(fair_day);
  case WeatherKind::PartlyCloudy:
    return night ? ICON(partly_night) : ICON(partly_day);
  case WeatherKind::Cloudy:
    return ICON(cloudy);
  case WeatherKind::Fog:
    return ICON(fog);
  case WeatherKind::Drizzle:
    return ICON(drizzle);
  case WeatherKind::Rain:
    return ICON(rain);
  case WeatherKind::Showers:
    return night ? ICON(showers_night) : ICON(showers_day);
  case WeatherKind::Thunder:
    return ICON(thunder);
  case WeatherKind::Snow:
    return ICON(snow);
  case WeatherKind::SnowShowers:
    return night ? ICON(snow_showers_night) : ICON(snow_showers_day);
  case WeatherKind::Sleet:
    return ICON(sleet);
  }
  return ICON(cloudy);
}

#undef ICON

struct SkyArt {
  const uint8_t *jpg;
  size_t len;
};

// Each look gets a key so an unchanged sky isn't decoded again.
int sky_key(const WeatherData &w) {
  int night = weather_is_night(w.symbol, w.night) ? 1 : 0;
  return static_cast<int>(weather_kind(w.symbol)) * 2 + night;
}

SkyArt sky_art(const WeatherData &w) {
  bool night = weather_is_night(w.symbol, w.night);
  switch (weather_kind(w.symbol)) {
  case WeatherKind::Clear:
  case WeatherKind::Fair:
    return night ? SkyArt{sky_clear_night_jpg, sky_clear_night_jpg_len}
                 : SkyArt{sky_clear_day_jpg, sky_clear_day_jpg_len};
  case WeatherKind::PartlyCloudy:
    return night ? SkyArt{sky_cloudy_night_jpg, sky_cloudy_night_jpg_len}
                 : SkyArt{sky_partly_day_jpg, sky_partly_day_jpg_len};
  case WeatherKind::Cloudy:
    return night ? SkyArt{sky_cloudy_night_jpg, sky_cloudy_night_jpg_len}
                 : SkyArt{sky_cloudy_day_jpg, sky_cloudy_day_jpg_len};
  case WeatherKind::Fog:
    return {sky_fog_jpg, sky_fog_jpg_len};
  case WeatherKind::Drizzle:
  case WeatherKind::Rain:
  case WeatherKind::Showers:
    return night ? SkyArt{sky_rain_night_jpg, sky_rain_night_jpg_len}
                 : SkyArt{sky_rain_day_jpg, sky_rain_day_jpg_len};
  case WeatherKind::Thunder:
    return {sky_thunder_jpg, sky_thunder_jpg_len};
  case WeatherKind::Snow:
  case WeatherKind::SnowShowers:
  case WeatherKind::Sleet:
    return night ? SkyArt{sky_snow_night_jpg, sky_snow_night_jpg_len}
                 : SkyArt{sky_snow_day_jpg, sky_snow_day_jpg_len};
  }
  return {sky_cloudy_day_jpg, sky_cloudy_day_jpg_len};
}

lv_color_t average_color(const uint8_t *swapped565, int count) {
  uint32_t r = 0, g = 0, b = 0;
  for (int i = 0; i < count; i++) {
    auto v =
        static_cast<uint16_t>(swapped565[i * 2] << 8 | swapped565[i * 2 + 1]);
    r += (v >> 11) & 0x1F;
    g += (v >> 5) & 0x3F;
    b += v & 0x1F;
  }
  return lv_color_make(static_cast<uint8_t>(r * 255 / (31u * count)),
                       static_cast<uint8_t>(g * 255 / (63u * count)),
                       static_cast<uint8_t>(b * 255 / (31u * count)));
}

void free_sky(Sky &sky) {
  heap_caps_free(sky.pixels);
  sky = {};
}

lv_obj_t *make_row(lv_obj_t *parent, int gap) {
  lv_obj_t *row = ui_box(parent);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, gap, LV_PART_MAIN);
  return row;
}

FactCell make_fact(lv_obj_t *parent, int column) {
  auto centered = [&](const lv_font_t *font, int y) {
    lv_obj_t *lbl = make_label(parent, font, COL_WHITE);
    lv_obj_set_width(lbl, FACT_W);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(lbl, column * FACT_W, y);
    return lbl;
  };
  return {centered(&lucide_22, 0), centered(&sf_semibold_16, 25),
          centered(&sf_medium_13, 44)};
}

void build_weather(lv_obj_t *face) {
  s_wx_time = make_label(face, &sf_semibold_20, COL_WHITE);

  s_wx_hero = make_layer(face);
  s_wx_temp_shadow =
      make_label(s_wx_hero, &TEMP_FONT, 0x000000, GLASS_SHADOW_OPA);
  s_wx_temp = make_label(s_wx_hero, &TEMP_FONT, COL_WHITE, GLASS_OPA);
  for (lv_obj_t *t : {s_wx_temp_shadow, s_wx_temp})
    lv_obj_set_style_text_letter_space(t, TEMP_TRACKING, LV_PART_MAIN);

  lv_obj_t *condition = make_row(face, 4);
  lv_obj_align(condition, LV_ALIGN_CENTER, 0, CONDITION_Y);
  s_wx_icon = lv_image_create(condition);
  s_wx_condition = make_label(condition, &sf_semibold_20, COL_WHITE);

  s_wx_range = make_label(face, &sf_semibold_16, COL_WHITE);

  s_wx_facts = ui_box(face);
  lv_obj_set_size(s_wx_facts, FACT_W * FACT_COUNT, FACT_H);
  lv_obj_align(s_wx_facts, LV_ALIGN_CENTER, 0, FACTS_Y);
  for (int i = 0; i < FACT_COUNT; i++)
    s_facts[i] = make_fact(s_wx_facts, i);
}

void update_weather_time(const tm &now) {
  char hm[8];
  snprintf(hm, sizeof(hm), "%02d:%02d", now.tm_hour, now.tm_min);
  lv_label_set_text(s_wx_time, hm);
  lv_obj_align(s_wx_time, LV_ALIGN_TOP_MID, 0, TIME_TOP);
}

lv_color_t weather_sky_color() {
  return s_sky.pixels ? s_sky.average : lv_color_hex(COL_SKY_FALLBACK);
}

void adopt_next_sky() {
  if (!s_next_sky.pixels)
    return;
  free_sky(s_sky);
  s_sky = s_next_sky;
  s_next_sky = {};
  s_sky_dsc = {};
  s_sky_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  s_sky_dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
  s_sky_dsc.header.w = SIZE;
  s_sky_dsc.header.h = SIZE;
  s_sky_dsc.header.stride = SIZE * 2;
  s_sky_dsc.data_size = SIZE * SIZE * 2;
  s_sky_dsc.data = s_sky.pixels;
  lv_image_set_src(s_sky_img, &s_sky_dsc);
}

void format_clock(char *out, size_t len, int minutes) {
  snprintf(out, len, "%02d:%02d", minutes / 60, minutes % 60);
}

// Apple's first fact: whichever of sunrise and sunset comes next.
void update_sun_fact(const tm &now) {
  FactCell &f = s_facts[FACT_SUN];
  int minute = now.tm_hour * 60 + now.tm_min;
  bool known = s_wx.sunrise >= 0 && s_wx.sunset >= 0;
  bool rising = known && (minute < s_wx.sunrise || minute >= s_wx.sunset);
  char value[16] = "--";
  if (known)
    format_clock(value, sizeof(value), rising ? s_wx.sunrise : s_wx.sunset);
  lv_label_set_text(f.icon, rising ? ICON_SUNRISE : ICON_SUNSET);
  lv_label_set_text(f.value, value);
  lv_label_set_text(f.label, rising ? "Sunrise" : "Sunset");
}

void set_fact(Fact which, const char *icon, const char *value,
              const char *label) {
  lv_label_set_text(s_facts[which].icon, icon);
  lv_label_set_text(s_facts[which].value, value);
  lv_label_set_text(s_facts[which].label, label);
}

void update_weather() {
  if (!s_wx_valid)
    return;
  char digits[8];
  snprintf(digits, sizeof(digits), "%d", round_temp(s_wx.temperature));
  char text[12];
  snprintf(text, sizeof(text), "%s\xC2\xB0", digits);
  // Only the digits are centered. The minus and the degree sign hang off
  // either side, so the number doesn't jump as they come and go.
  int skip = digits[0] == '-' ? 1 : 0;
  lv_area_t num = ink_bounds(&TEMP_FONT, digits, TEMP_TRACKING, skip);
  int32_t x = CENTER - (num.x1 + num.x2 + 1) / 2;
  int32_t y = CENTER + TEMP_Y - (num.y1 + num.y2 + 1) / 2;
  lv_label_set_text(s_wx_temp, text);
  lv_label_set_text(s_wx_temp_shadow, text);
  lv_obj_set_pos(s_wx_temp, x, y);
  lv_obj_set_pos(s_wx_temp_shadow, x, y + GLASS_SHADOW_Y);

  lv_image_set_src(s_wx_icon, icon_art(s_wx.symbol, s_wx.night).small);
  lv_label_set_text(s_wx_condition, s_wx.condition);

  char range[24];
  snprintf(range, sizeof(range), "H:%d\xC2\xB0  L:%d\xC2\xB0",
           round_temp(s_wx.high), round_temp(s_wx.low));
  lv_label_set_text(s_wx_range, range);
  lv_obj_align(s_wx_range, LV_ALIGN_CENTER, 0, RANGE_Y);

  char wind[12];
  snprintf(wind, sizeof(wind), "%d m/s", round_temp(s_wx.wind_speed));
  set_fact(FACT_WIND, ICON_WIND, wind, "Wind");
  char humidity[8];
  snprintf(humidity, sizeof(humidity), "%d%%", round_temp(s_wx.humidity));
  set_fact(FACT_HUMIDITY, ICON_DROPLETS, humidity, "Humidity");
  update_sun_fact(local_now());
}

// ─── Transitions ────────────────────────────────────────────────────────────

// Which way the current change moves the faces: up the screen as they take
// turns, or sideways along a swipe (-1 left, +1 right).
int s_swipe;

void set_progress(lv_obj_t *obj, int32_t visible, int shift) {
  lv_obj_set_style_opa(
      obj, static_cast<lv_opa_t>(visible * LV_OPA_COVER / PROGRESS_END),
      LV_PART_MAIN);
  lv_obj_set_style_translate_x(obj, s_swipe ? shift : 0, LV_PART_MAIN);
  lv_obj_set_style_translate_y(obj, s_swipe ? 0 : shift, LV_PART_MAIN);
}

int enter_shift() {
  return s_swipe ? -s_swipe * SWIPE_ENTER_SHIFT : ENTER_SHIFT;
}

int exit_shift() { return s_swipe ? s_swipe * SWIPE_EXIT_SHIFT : EXIT_SHIFT; }

void anim_enter_cb(void *obj, int32_t v) {
  set_progress(static_cast<lv_obj_t *>(obj), v,
               enter_shift() * (PROGRESS_END - v) / PROGRESS_END);
}

void anim_exit_cb(void *obj, int32_t v) {
  set_progress(static_cast<lv_obj_t *>(obj), PROGRESS_END - v,
               exit_shift() * v / PROGRESS_END);
}

void anim_opa_cb(void *obj, int32_t v) {
  lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(v),
                       LV_PART_MAIN);
}

void anim_sky_cb(void *obj, int32_t v) {
  lv_obj_set_style_bg_color(
      static_cast<lv_obj_t *>(obj),
      lv_color_mix(s_sky_to, s_sky_from, static_cast<uint8_t>(v)),
      LV_PART_MAIN);
}

void for_each_child(lv_obj_t *face, void (*fn)(lv_obj_t *, uint32_t)) {
  uint32_t n = lv_obj_get_child_count(face);
  for (uint32_t i = 0; i < n; i++)
    fn(lv_obj_get_child(face, static_cast<int32_t>(i)), i);
}

void rest(lv_obj_t *face) {
  for_each_child(face, [](lv_obj_t *c, uint32_t) {
    lv_anim_delete(c, nullptr);
    set_progress(c, PROGRESS_END, 0);
  });
}

void hide_done(lv_anim_t *a) {
  lv_obj_add_flag(static_cast<lv_obj_t *>(a->var), LV_OBJ_FLAG_HIDDEN);
}

void exit_done(lv_anim_t *a) {
  lv_obj_t *face = lv_obj_get_parent(static_cast<lv_obj_t *>(a->var));
  lv_obj_add_flag(face, LV_OBJ_FLAG_HIDDEN);
  rest(face);
}

void start(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to,
           int duration, int delay = 0, lv_anim_completed_cb_t done = nullptr) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, cb);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, duration);
  lv_anim_set_delay(&a, static_cast<uint32_t>(delay));
  motion_ease_out(&a);
  if (done)
    lv_anim_set_completed_cb(&a, done);
  lv_anim_start(&a);
}

// The stagger follows where each element sits along the way the faces move,
// leading side first, so the change sweeps across the face whatever order
// the widgets were created in.
int wave_delay(lv_obj_t *obj, int span_ms) {
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  int32_t mid = s_swipe ? (a.x1 + a.x2) / 2 : (a.y1 + a.y2) / 2;
  mid = std::clamp<int32_t>(mid, 0, SIZE);
  if (s_swipe > 0)
    mid = SIZE - mid;
  return static_cast<int>(mid * span_ms / SIZE);
}

void enter(lv_obj_t *face, int delay) {
  rest(face);
  lv_obj_remove_flag(face, LV_OBJ_FLAG_HIDDEN);
  lv_obj_update_layout(face);
  for_each_child(face, [](lv_obj_t *c, uint32_t) { anim_enter_cb(c, 0); });
  uint32_t n = lv_obj_get_child_count(face);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t *c = lv_obj_get_child(face, static_cast<int32_t>(i));
    start(c, anim_enter_cb, 0, PROGRESS_END, ENTER_MS,
          delay + wave_delay(c, ENTER_WAVE_MS));
  }
}

void leave(lv_obj_t *face) {
  rest(face);
  uint32_t n = lv_obj_get_child_count(face);
  int last = 0;
  for (uint32_t i = 0; i < n; i++)
    last = std::max(last,
                    wave_delay(lv_obj_get_child(face, static_cast<int32_t>(i)),
                               EXIT_WAVE_MS));
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t *c = lv_obj_get_child(face, static_cast<int32_t>(i));
    int delay = wave_delay(c, EXIT_WAVE_MS);
    // Only one animation hides the face, and it must be the last to finish.
    bool closes = delay == last;
    last = closes ? -1 : last;
    start(c, anim_exit_cb, 0, PROGRESS_END, EXIT_MS, delay,
          closes ? exit_done : nullptr);
  }
}

void morph_sky(lv_color_t color) {
  lv_anim_delete(s_root, anim_sky_cb);
  s_sky_from = lv_obj_get_style_bg_color(s_root, LV_PART_MAIN);
  s_sky_to = color;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_root);
  lv_anim_set_exec_cb(&a, anim_sky_cb);
  lv_anim_set_values(&a, 0, LV_OPA_COVER);
  lv_anim_set_duration(&a, SKY_MS);
  motion_ease_in_out(&a);
  lv_anim_start(&a);
}

lv_obj_t *face_obj(Face f) {
  switch (f) {
  case Face::Clock:
    return s_clock;
  case Face::Weather:
    return s_weather;
  case Face::Timer:
    return s_timer_face;
  }
  return s_clock;
}

// For the weather face this is the painted sky's average, so the color the
// background morphs to is what the picture fades in over.
lv_color_t face_sky(Face f) {
  switch (f) {
  case Face::Clock:
    return lv_color_hex(s_palette.sky);
  case Face::Weather:
    return weather_sky_color();
  case Face::Timer:
    return lv_color_hex(COL_TIMER_SKY);
  }
  return lv_color_hex(s_palette.sky);
}

void fade_sky_image(bool in) {
  lv_anim_delete(s_sky_img, nullptr);
  if (!s_sky.pixels) {
    lv_obj_add_flag(s_sky_img, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_remove_flag(s_sky_img, LV_OBJ_FLAG_HIDDEN);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_sky_img);
  lv_anim_set_exec_cb(&a, anim_opa_cb);
  lv_anim_set_values(&a, lv_obj_get_style_opa(s_sky_img, LV_PART_MAIN),
                     in ? LV_OPA_COVER : LV_OPA_TRANSP);
  lv_anim_set_duration(&a, SKY_MS);
  motion_ease_in_out(&a);
  if (!in)
    lv_anim_set_completed_cb(&a, hide_done);
  lv_anim_start(&a);
}

void hide_sky_image() {
  lv_anim_delete(s_sky_img, nullptr);
  lv_obj_set_style_opa(s_sky_img, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_add_flag(s_sky_img, LV_OBJ_FLAG_HIDDEN);
}

uint32_t face_period(Face f) {
  return f == Face::Clock ? CLOCK_FACE_MS : WEATHER_FACE_MS;
}

// ─── Time-of-day colors ─────────────────────────────────────────────────────

lv_color_t mix(uint32_t to, uint32_t from, int32_t v) {
  return lv_color_mix(lv_color_hex(to), lv_color_hex(from),
                      static_cast<uint8_t>(v));
}

// v runs 0..255 from s_palette_from to s_palette.
void paint_clock(int32_t v) {
  const FacePalette &a = s_palette_from;
  const FacePalette &b = s_palette;
  lv_color_t ink = mix(b.ink, a.ink, v);
  lv_color_t hand = mix(b.hand, a.hand, v);
  lv_color_t sky = mix(b.sky, a.sky, v);
  for (lv_obj_t *n : s_numerals)
    lv_obj_set_style_image_recolor(n, ink, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_date, ink, LV_PART_MAIN);
  for (Hand *h : {&s_hour_hand, &s_minute_hand, &s_second_hand})
    lv_obj_set_style_image_recolor(h->img, hand, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_cap, hand, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_pin, sky, LV_PART_MAIN);
  if (s_face == Face::Clock)
    lv_obj_set_style_bg_color(s_root, sky, LV_PART_MAIN);
}

void anim_palette_cb(void *, int32_t v) { paint_clock(v); }

DayPart current_part(const tm &now) {
  SunClock sun;
  if (s_wx_valid) {
    sun = {s_wx.sunrise, s_wx.sunset, &s_wx.night};
  }
  return day_part(now.tm_hour * 60 + now.tm_min, sun);
}

// Recolors the clock if the part of the day changed. On screen it eases over,
// slowly, because nothing the user did caused it.
void update_palette(const tm &now, bool animate) {
  DayPart part = current_part(now);
  if (part == s_part)
    return;
  s_part = part;
  s_palette_from = s_palette;
  s_palette = face_palette(part);
  lv_anim_delete(s_clock, anim_palette_cb);
  if (!animate || !s_visible) {
    paint_clock(LV_OPA_COVER);
    return;
  }
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_clock);
  lv_anim_set_exec_cb(&a, anim_palette_cb);
  lv_anim_set_values(&a, 0, LV_OPA_COVER);
  lv_anim_set_duration(&a, PALETTE_MS);
  motion_ease_in_out(&a);
  lv_anim_start(&a);
}

void refresh_time() {
  tm now = local_now();
  update_palette(now, true);
  update_clock(now);
  update_weather_time(now);
  if (s_wx_valid)
    update_sun_fact(now);
  // Fire again right as the next minute starts.
  lv_timer_set_period(s_minute_timer,
                      static_cast<uint32_t>(60 - now.tm_sec) * 1000);
}

void on_minute(lv_timer_t *) { refresh_time(); }

void switch_to(Face next, int swipe = 0) {
  if (next == s_face)
    return;
  s_swipe = swipe;
  leave(face_obj(s_face));
  s_face = next;
  morph_sky(face_sky(next));
  fade_sky_image(next == Face::Weather);
  enter(face_obj(next), ENTER_DELAY_MS);
  lv_timer_set_period(s_rotate_timer, face_period(next));
  lv_timer_reset(s_rotate_timer);
}

Face other_face() {
  return s_face == Face::Clock ? Face::Weather : Face::Clock;
}

bool may_change() {
  return !s_timer_active && (s_face != Face::Clock || s_wx_valid);
}

void on_rotate(lv_timer_t *) {
  if (may_change())
    switch_to(other_face());
}

} // namespace

// ─── Public API ─────────────────────────────────────────────────────────────

void faces_create(lv_obj_t *parent) {
  // The pager rebuilds the home page when pages come and go, so this runs more
  // than once: the widgets are new, the timers are kept.
  s_visible = false;
  s_root = make_layer(parent);
  lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_root, lv_color_hex(s_palette.sky), LV_PART_MAIN);
  lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);

  s_sky_img = lv_image_create(s_root);
  lv_obj_set_size(s_sky_img, SIZE, SIZE);
  if (s_sky.pixels)
    lv_image_set_src(s_sky_img, &s_sky_dsc);
  hide_sky_image();

  s_clock = make_layer(s_root);
  build_clock(s_clock);
  s_weather = make_layer(s_root);
  build_weather(s_weather);
  lv_obj_add_flag(s_weather, LV_OBJ_FLAG_HIDDEN);
  s_timer_face = make_layer(s_root);
  s_timer_dial = timer_dial_create(s_timer_face);
  lv_obj_add_flag(s_timer_face, LV_OBJ_FLAG_HIDDEN);

  if (!s_minute_timer) {
    s_minute_timer = lv_timer_create(on_minute, 60000, nullptr);
    s_rotate_timer = lv_timer_create(on_rotate, CLOCK_FACE_MS, nullptr);
    s_sweep_timer = lv_timer_create(on_sweep, SWEEP_MS, nullptr);
    s_wobble_timer = lv_timer_create(on_wobble, WOBBLE_FRAME_MS, nullptr);
  }
  paint_clock(LV_OPA_COVER);
  lv_timer_pause(s_minute_timer);
  lv_timer_pause(s_rotate_timer);
  lv_timer_pause(s_sweep_timer);
  lv_timer_pause(s_wobble_timer);
}

void faces_show(bool visible) {
  if (visible == s_visible)
    return;
  s_visible = visible;
  lv_anim_delete(s_root, nullptr);

  if (!visible) {
    lv_timer_pause(s_minute_timer);
    lv_timer_pause(s_rotate_timer);
    lv_timer_pause(s_sweep_timer);
    start(s_root, anim_opa_cb, lv_obj_get_style_opa(s_root, LV_PART_MAIN),
          LV_OPA_TRANSP, HIDE_MS, 0, hide_done);
    return;
  }

  // Every idle spell opens on the clock, or on the timer while one runs.
  s_face = s_timer_active ? Face::Timer : Face::Clock;
  for (lv_obj_t *f : {s_clock, s_weather, s_timer_face}) {
    rest(f);
    lv_obj_add_flag(f, LV_OBJ_FLAG_HIDDEN);
  }
  hide_sky_image();
  update_palette(local_now(), false);
  refresh_time();
  update_weather();
  if (s_timer_active)
    update_timer_face();
  lv_obj_set_style_bg_color(s_root, face_sky(s_face), LV_PART_MAIN);
  lv_obj_set_style_opa(s_root, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
  start(s_root, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, SHOW_MS);
  s_swipe = 0;
  enter(face_obj(s_face), SHOW_MS / 3);

  lv_timer_resume(s_minute_timer);
  lv_timer_resume(s_sweep_timer);
  lv_timer_set_period(s_rotate_timer, CLOCK_FACE_MS);
  lv_timer_reset(s_rotate_timer);
  lv_timer_resume(s_rotate_timer);
}

void faces_nudge(int steps) {
  if (!s_visible || s_face != Face::Clock || steps == 0)
    return;
  float push = static_cast<float>(std::clamp(steps, -2, 2)) * LEAN_PER_DETENT;
  uint32_t now = lv_tick_get();
  for (int k = 0; k < NUMERAL_COUNT; k++) {
    // Counterclockwise runs the same circle the other way: 12, 9, 6, 3.
    Numeral n = steps > 0 ? CLOCKWISE[k]
                          : CLOCKWISE[(NUMERAL_COUNT - k) % NUMERAL_COUNT];
    Wobble &w = s_wobble[n];
    // During a fast spin a numeral keeps its first turn in the queue, so the
    // later ones still follow 45 ms apart instead of waiting for the end.
    if (w.pending == 0.0f)
      w.push_at = now + static_cast<uint32_t>(k) * WOBBLE_STAGGER_MS;
    w.pending = std::clamp(w.pending + push, -LEAN_MAX, LEAN_MAX);
  }
  if (lv_timer_get_paused(s_wobble_timer)) {
    s_wobble_last_ms = now;
    lv_timer_resume(s_wobble_timer);
  }
}

void faces_swipe(int direction) {
  if (s_visible && direction != 0 && may_change())
    switch_to(other_face(), direction < 0 ? -1 : 1);
}

lv_color_t faces_accent() {
  switch (s_face) {
  case Face::Weather:
    return lv_color_hex(COL_WHITE);
  case Face::Timer:
    return lv_color_hex(COL_TIMER_ACCENT);
  case Face::Clock:
    break;
  }
  return lv_color_hex(s_palette.hand);
}

void faces_set_timer(bool running, bool animate) {
  if (running == s_timer_active)
    return;
  s_timer_active = running;
  if (!s_visible)
    return;
  if (running)
    update_timer_face();
  Face next = running ? Face::Timer : Face::Clock;
  if (animate) {
    switch_to(next);
    return;
  }
  lv_anim_delete(s_root, anim_sky_cb);
  for (lv_obj_t *f : {s_clock, s_weather, s_timer_face}) {
    rest(f);
    lv_obj_set_flag(f, LV_OBJ_FLAG_HIDDEN, f != face_obj(next));
  }
  hide_sky_image();
  s_face = next;
  lv_obj_set_style_bg_color(s_root, face_sky(next), LV_PART_MAIN);
  if (next == Face::Clock)
    sweep_hands(true);
  lv_timer_set_period(s_rotate_timer, face_period(next));
  lv_timer_reset(s_rotate_timer);
}

void faces_prepare_weather(const WeatherData *data) {
  if (!data || !data->valid)
    return;
  int key = sky_key(*data);
  if (key == s_sky.key || key == s_next_sky.key)
    return;
  SkyArt art = sky_art(*data);
  uint8_t *pixels = nullptr;
  int w = 0;
  int h = 0;
  if (!art_decode_jpeg(art.jpg, static_cast<int>(art.len), &pixels, &w, &h,
                       SIZE) ||
      w != SIZE || h != SIZE) {
    heap_caps_free(pixels);
    return;
  }
  free_sky(s_next_sky);
  s_next_sky = {pixels, key, average_color(pixels, SIZE * SIZE)};
}

void faces_set_weather(const WeatherData *data) {
  s_wx_valid = data && data->valid && data->condition[0];
  if (s_wx_valid)
    s_wx = *data;
  adopt_next_sky();
  update_weather();
  update_palette(local_now(), true);
  if (!s_visible || s_face != Face::Weather)
    return;
  if (!s_wx_valid) {
    switch_to(Face::Clock);
    return;
  }
  lv_obj_set_style_bg_color(s_root, weather_sky_color(), LV_PART_MAIN);
  lv_obj_set_style_opa(s_sky_img, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_flag(s_sky_img, LV_OBJ_FLAG_HIDDEN, !s_sky.pixels);
}
