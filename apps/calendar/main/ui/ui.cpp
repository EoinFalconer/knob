#include "ui/ui.h"
#include "app_config.h"
#include "display.h"
#include "encoder.h"
#include "fonts.h"
#include "haptic.h"

#include "esp_log.h"
#include "lvgl.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

// ─── Weather State ──────────────────────────────────────────────────────────

static lv_obj_t *s_lbl_weather_date;
static lv_obj_t *s_lbl_weather_icon;
static lv_obj_t *s_lbl_weather_temp;
static char s_weather_date[16];
static char s_weather_icon[8];
static char s_weather_temp[16];
static uint32_t s_weather_color;
static bool s_weather_valid = false;

static void update_weather_positions();

static constexpr const char *TAG = "ui";

// ─── Constants ──────────────────────────────────────────────────────────────

static constexpr int ENCODER_POLL_MS = 20;
static constexpr int CLOCK_UPDATE_MS = 1000;
static constexpr int BROWSE_TIMEOUT_MS = 6000;
static constexpr int ANIM_MS = 150;
static constexpr int SLIDE_PX = 36;

// ─── Palette ────────────────────────────────────────────────────────────────

#define COL_BG lv_color_hex(0x000000)
#define COL_TEXT lv_color_hex(0xFFFFFF)
#define COL_DIM lv_color_hex(0x8E8E93)
#define COL_ACCENT lv_color_hex(0xFF9F0A)
#define COL_GREEN lv_color_hex(0x30D158)
#define COL_RED lv_color_hex(0xFF453A)
#define COL_DIVIDER lv_color_hex(0x2C2C2E)

// ─── State ──────────────────────────────────────────────────────────────────

static CalEvent s_events[MAX_EVENTS];
static int s_event_count = 0;
static int s_scroll_pos = 0;
static bool s_browsing = false;
static bool s_loaded = false;
static bool s_wifi_ok = false;
static bool s_animating = false;
static int s_anim_dir = 0; // +1 = into future, -1 = back to now

enum class WarnLevel { None, Yellow, Red };
static WarnLevel s_warn = WarnLevel::None;
static char s_current_day[24] = {}; // last day shown in sticky header

// ─── Widgets ────────────────────────────────────────────────────────────────
//
// Layout (360×360 round display, y measured from top):
//
//   26   WiFi dot (6px circle)
//   60   Clock top  (sf_rounded_semibold_52, ~58px tall → bottom ≈118)
//  122   Weather row: "Mon 20  ☀  9°"  (sf_regular_20 + lucide_weather_20)
//  164   Divider    (1px)
//  174   Clip panel top (height 146px → bottom 320)
//          └─ Card (fills clip, animated via translate_y)
//               6   "Next up" / day label  (sf_regular_16)
//              30   Event title            (sf_semibold_28, scroll-circular)
//              82   Time range             (sf_regular_16)
//             106   Relative time          (sf_regular_16)

static lv_obj_t *s_screen;
static lv_obj_t *s_wifi_dot;
static lv_obj_t *s_lbl_time;

static lv_obj_t *s_divider;
static lv_obj_t *s_clip;        // fixed panel; clips the animated card
static lv_obj_t *s_card;        // animated container inside s_clip
static lv_obj_t *s_lbl_section; // "Next up" or day header
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_time_range;
static lv_obj_t *s_lbl_rel;
static lv_obj_t *s_rec_dot;      // red blinking dot for "Happening now"
static lv_obj_t *s_warn_ring;    // orange arc border for 2-min warning
static lv_obj_t *s_warn_overlay; // red bg overlay for 1-min warning
static lv_obj_t *s_lbl_empty;

// ─── Timers ─────────────────────────────────────────────────────────────────

static lv_timer_t *s_clock_timer;
static lv_timer_t *s_browse_timer;

// ─── Forward declarations ────────────────────────────────────────────────────

static void populate_card();
static void update_clock();
static void slide(int dir);
static void update_warning();

// ─── Time helpers ───────────────────────────────────────────────────────────

static bool time_synced() { return time(nullptr) > 1'700'000'000; }

static bool same_day(time_t a, time_t b) {
  struct tm ta{}, tb_tm{};
  localtime_r(&a, &ta);
  localtime_r(&b, &tb_tm);
  return ta.tm_year == tb_tm.tm_year && ta.tm_yday == tb_tm.tm_yday;
}

static void fmt_day(const CalEvent &ev, char *buf, size_t len) {
  time_t now = time(nullptr);
  if (same_day(ev.start, now))
    snprintf(buf, len, "Today");
  else if (same_day(ev.start, now + 86400))
    snprintf(buf, len, "Tomorrow");
  else {
    struct tm t{};
    localtime_r(&ev.start, &t);
    strftime(buf, len, "%a %d %b", &t);
  }
}

// "Today · 10:00–11:00"  or  "10:00–11:00"  or  "All day"
static void fmt_time_range(const CalEvent &ev, char *buf, size_t len,
                           bool with_day) {
  time_t now = time(nullptr);
  bool is_today = same_day(ev.start, now);

  if (ev.all_day) {
    if (with_day && !is_today) {
      char day[24]{};
      fmt_day(ev, day, sizeof(day));
      snprintf(buf, len, "%s - All day", day);
    } else {
      snprintf(buf, len, "All day");
    }
    return;
  }
  struct tm ts{}, te{};
  localtime_r(&ev.start, &ts);
  localtime_r(&ev.end, &te);
  if (with_day && !is_today) {
    char day[24]{};
    fmt_day(ev, day, sizeof(day));
    snprintf(buf, len, "%s - %02d:%02d-%02d:%02d", day, ts.tm_hour, ts.tm_min,
             te.tm_hour, te.tm_min);
  } else {
    snprintf(buf, len, "%02d:%02d-%02d:%02d", ts.tm_hour, ts.tm_min, te.tm_hour,
             te.tm_min);
  }
}

// "in 12 min" / "Happening now" / "" (empty when far away)
// iOS-style: only show relative time when imminent (< 2h).
// The sticky day header already provides "Today" / "Tomorrow" context.
static void fmt_rel(const CalEvent &ev, char *buf, size_t len, bool *is_now) {
  *is_now = false;
  time_t now = time(nullptr);

  if (ev.all_day && same_day(ev.start, now)) {
    *is_now = true;
    snprintf(buf, len, "All day today");
    return;
  }
  if (!ev.all_day && now >= ev.start && now < ev.end) {
    *is_now = true;
    snprintf(buf, len, "Happening now");
    return;
  }
  int diff = (int)difftime(ev.start, now);
  if (diff < 0) {
    snprintf(buf, len, "Started");
    return;
  }
  if (diff < 60) {
    *is_now = true;
    snprintf(buf, len, "Starting now");
    return;
  }
  if (diff < 3600) {
    snprintf(buf, len, "in %d min", diff / 60);
    return;
  }
  if (diff < 7200) {
    int h = diff / 3600, m = (diff % 3600) / 60;
    m ? snprintf(buf, len, "in %dh %dm", h, m)
      : snprintf(buf, len, "in %dh", h);
    return;
  }
  // > 2 hours away — day header is enough context, skip relative time
  buf[0] = '\0';
}

// ─── Clock ──────────────────────────────────────────────────────────────────

static void update_clock() {
  if (!time_synced()) {
    lv_label_set_text(s_lbl_time, "--:--");
    return;
  }
  time_t now = time(nullptr);
  struct tm t{};
  localtime_r(&now, &t);
  char tb[6];
  snprintf(tb, sizeof(tb), "%02d:%02d", t.tm_hour, t.tm_min);
  lv_label_set_text(s_lbl_time, tb);

  // Update weather date portion
  if (s_weather_valid) {
    static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed",
                                       "Thu", "Fri", "Sat"};
    snprintf(s_weather_date, sizeof(s_weather_date), "%s %d", DAYS[t.tm_wday],
             t.tm_mday);
    lv_label_set_text(s_lbl_weather_date, s_weather_date);
    update_weather_positions();
  }
}

static void update_weather_positions() {
  lv_obj_update_layout(s_lbl_weather_date);
  lv_obj_update_layout(s_lbl_weather_temp);
  int32_t dw = lv_obj_get_width(s_lbl_weather_date);
  int32_t tw = lv_obj_get_width(s_lbl_weather_temp);
  int32_t total = dw + 8 + 20 + 4 + tw; // date + gap + icon + gap + temp
  int32_t x = (LCD_H_RES - total) / 2;
  lv_obj_set_pos(s_lbl_weather_date, x, 132);
  x += dw + 8;
  lv_obj_set_pos(s_lbl_weather_icon, x, 132);
  x += 20 + 4;
  lv_obj_set_pos(s_lbl_weather_temp, x, 132);
}

// ─── Card content ───────────────────────────────────────────────────────────

static void populate_card() {
  if (!s_loaded) {
    lv_label_set_text(s_lbl_section,
                      s_wifi_ok ? "Loading..." : "Connecting...");
    lv_obj_set_style_text_color(s_lbl_section, COL_DIM, 0);
    lv_label_set_text(s_lbl_title, "");
    lv_label_set_text(s_lbl_time_range, "");
    lv_label_set_text(s_lbl_rel, "");
    lv_obj_add_flag(s_lbl_empty, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  if (s_event_count == 0) {
    lv_label_set_text(s_lbl_section, "");
    lv_label_set_text(s_lbl_title, "");
    lv_label_set_text(s_lbl_time_range, "");
    lv_label_set_text(s_lbl_rel, "");
    lv_obj_remove_flag(s_lbl_empty, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  lv_obj_add_flag(s_lbl_empty, LV_OBJ_FLAG_HIDDEN);

  const auto &ev = s_events[s_scroll_pos];

  // Sticky day header — lives outside the sliding card so it stays fixed.
  // Only update text + color when the day actually changes.
  char day[24]{};
  fmt_day(ev, day, sizeof(day));
  if (strcmp(day, s_current_day) != 0) {
    memcpy(s_current_day, day, sizeof(s_current_day));
    lv_label_set_text(s_lbl_section, day);
    bool is_today = same_day(ev.start, time(nullptr));
    lv_obj_set_style_text_color(s_lbl_section, COL_DIM, 0);
  }

  // Title
  lv_label_set_text(s_lbl_title, ev.summary);

  // Time range — day is always in the sticky header, never repeat it here
  char tr[80]{};
  fmt_time_range(ev, tr, sizeof(tr), false);
  lv_label_set_text(s_lbl_time_range, tr);

  // Relative time
  char rel[32]{};
  bool is_now = false;
  fmt_rel(ev, rel, sizeof(rel), &is_now);
  lv_label_set_text(s_lbl_rel, rel);
  lv_color_t rel_color;
  if (is_now)
    rel_color = (s_warn == WarnLevel::Red) ? COL_TEXT : COL_ACCENT;
  else
    rel_color = COL_DIM;
  lv_obj_set_style_text_color(s_lbl_rel, rel_color, 0);

  // Recording dot — only for "Happening now", not "Starting now"
  bool is_happening = (strcmp(rel, "Happening now") == 0);
  if (is_happening) {
    lv_obj_remove_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_card);
    lv_obj_align_to(s_rec_dot, s_lbl_rel, LV_ALIGN_OUT_LEFT_MID, -8, 1);
  } else {
    lv_obj_add_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
  }
}

// ─── Animation ──────────────────────────────────────────────────────────────
//
// Two-phase slide:
//   Phase 1 (exit): current card translates out + fades out  (ease-in)
//   Phase 2 (enter): content swapped, card translates in + fades in  (ease-out)
//
// Direction: +1 = into future → old exits UP, new enters from BELOW
//            -1 = toward now  → old exits DOWN, new enters from ABOVE

static void anim_translate_y_cb(void *obj, int32_t v) {
  lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(obj), v, LV_PART_MAIN);
}

static void anim_opa_cb(void *obj, int32_t v) {
  lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), (lv_opa_t)v, LV_PART_MAIN);
}

static void on_exit_done(lv_anim_t *) {
  // Snap to opposite edge, populate new content, then slide in
  int32_t enter_from = s_anim_dir > 0 ? SLIDE_PX : -SLIDE_PX;
  lv_obj_set_style_translate_y(s_card, enter_from, LV_PART_MAIN);
  lv_obj_set_style_opa(s_card, LV_OPA_TRANSP, LV_PART_MAIN);
  populate_card();

  lv_anim_t ay;
  lv_anim_init(&ay);
  lv_anim_set_var(&ay, s_card);
  lv_anim_set_exec_cb(&ay, anim_translate_y_cb);
  lv_anim_set_values(&ay, enter_from, 0);
  lv_anim_set_duration(&ay, ANIM_MS);
  lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
  lv_anim_set_completed_cb(&ay, [](lv_anim_t *) { s_animating = false; });
  lv_anim_start(&ay);

  lv_anim_t ao;
  lv_anim_init(&ao);
  lv_anim_set_var(&ao, s_card);
  lv_anim_set_exec_cb(&ao, anim_opa_cb);
  lv_anim_set_values(&ao, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_duration(&ao, ANIM_MS);
  lv_anim_set_path_cb(&ao, lv_anim_path_ease_out);
  lv_anim_start(&ao);
}

static void slide(int dir) {
  s_animating = true;
  s_anim_dir = dir;

  // Cancel any in-flight card animation cleanly
  lv_anim_delete(s_card, anim_translate_y_cb);
  lv_anim_delete(s_card, anim_opa_cb);

  int32_t exit_to = dir > 0 ? -SLIDE_PX : SLIDE_PX;
  int32_t cur_y = lv_obj_get_style_translate_y(s_card, LV_PART_MAIN);
  int32_t cur_opa = lv_obj_get_style_opa(s_card, LV_PART_MAIN);
  if (cur_opa == 0)
    cur_opa = LV_OPA_COVER; // guard for uninitialised state

  lv_anim_t ay;
  lv_anim_init(&ay);
  lv_anim_set_var(&ay, s_card);
  lv_anim_set_exec_cb(&ay, anim_translate_y_cb);
  lv_anim_set_values(&ay, cur_y, exit_to);
  lv_anim_set_duration(&ay, ANIM_MS);
  lv_anim_set_path_cb(&ay, lv_anim_path_ease_in);
  lv_anim_set_completed_cb(&ay, on_exit_done);
  lv_anim_start(&ay);

  lv_anim_t ao;
  lv_anim_init(&ao);
  lv_anim_set_var(&ao, s_card);
  lv_anim_set_exec_cb(&ao, anim_opa_cb);
  lv_anim_set_values(&ao, cur_opa, LV_OPA_TRANSP);
  lv_anim_set_duration(&ao, ANIM_MS);
  lv_anim_set_path_cb(&ao, lv_anim_path_ease_in);
  lv_anim_start(&ao);
}

// Instant (no animation) update — for initial load, clock tick, new data
static void update_display_instant() {
  lv_anim_delete(s_card, anim_translate_y_cb);
  lv_anim_delete(s_card, anim_opa_cb);
  s_animating = false;
  lv_obj_set_style_translate_y(s_card, 0, LV_PART_MAIN);
  lv_obj_set_style_opa(s_card, LV_OPA_COVER, LV_PART_MAIN);
  populate_card();
}

// ─── Timer callbacks ─────────────────────────────────────────────────────────

static void on_browse_timeout(lv_timer_t *) {
  int old = s_scroll_pos;
  s_browsing = false;
  s_scroll_pos = 0;
  lv_timer_pause(s_browse_timer);
  if (old != 0 && s_event_count > 0)
    slide(-1); // slide back toward the present
  else
    update_display_instant();
}

static void on_encoder_poll(lv_timer_t *) {
  int32_t steps = encoder_take_steps();
  if (steps == 0 || s_event_count <= 0)
    return;

  int new_pos = std::clamp((int)(s_scroll_pos + steps), 0, s_event_count - 1);
  if (new_pos == s_scroll_pos)
    return;

  int dir = new_pos > s_scroll_pos ? 1 : -1;
  s_scroll_pos = new_pos;

  if (!s_browsing) {
    s_browsing = true;
    lv_timer_resume(s_browse_timer);
  }
  lv_timer_reset(s_browse_timer);

  slide(dir);
}

static void on_clock_tick(lv_timer_t *) {
  update_clock();
  if (s_loaded && s_event_count > 0 && !s_animating)
    populate_card();
  update_warning();
}

// ─── Warning animations ──────────────────────────────────────────────────────

static constexpr uint8_t BL_NORMAL = 80;

static void anim_border_opa_cb(void *obj, int32_t v) {
  lv_obj_set_style_border_opa(static_cast<lv_obj_t *>(obj), (lv_opa_t)v,
                              LV_PART_MAIN);
}

static void anim_bg_opa_cb(void *obj, int32_t v) {
  lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(obj), (lv_opa_t)v,
                          LV_PART_MAIN);
}

static void stop_warn() {
  lv_anim_delete(s_warn_ring, anim_border_opa_cb);
  lv_anim_delete(s_warn_overlay, anim_bg_opa_cb);
  lv_obj_set_style_border_opa(s_warn_ring, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_warn_overlay, LV_OPA_TRANSP, LV_PART_MAIN);
  display_set_backlight(BL_NORMAL);
  // Refresh card so text colors reset to normal
  if (!s_animating)
    populate_card();
}

static void start_warn_anim(lv_obj_t *obj, lv_anim_exec_xcb_t cb,
                            lv_opa_t max_opa, uint32_t period) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, cb);
  lv_anim_set_values(&a, LV_OPA_TRANSP, max_opa);
  lv_anim_set_duration(&a, period);
  lv_anim_set_playback_duration(&a, period);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

static void update_warning() {
  if (!s_loaded || s_event_count == 0 || !time_synced())
    return;

  time_t now = time(nullptr);
  WarnLevel new_warn = WarnLevel::None;
  for (int i = 0; i < s_event_count; ++i) {
    int diff = (int)difftime(s_events[i].start, now);
    if (diff >= 0 && diff <= 60) {
      new_warn = WarnLevel::Red;
      break;
    }
    if (diff > 60 && diff <= 120) {
      new_warn = WarnLevel::Yellow;
      break;
    }
  }

  if (new_warn == s_warn)
    return;
  s_warn = new_warn;

  // Haptic fires once on state entry
  if (new_warn == WarnLevel::Yellow)
    haptic_buzz_double();
  else if (new_warn == WarnLevel::Red)
    haptic_buzz_triple();

  stop_warn();

  if (new_warn == WarnLevel::None)
    return;

  // Refresh card immediately so text colors reflect new warn level
  if (!s_animating)
    populate_card();

  // Both levels use the same overlay, just different color + speed
  if (new_warn == WarnLevel::Yellow) {
    lv_obj_set_style_bg_color(s_warn_overlay, lv_color_hex(0xFF9F0A),
                              LV_PART_MAIN);
    start_warn_anim(s_warn_overlay, anim_bg_opa_cb, 100, 1400);
  } else {
    lv_obj_set_style_bg_color(s_warn_overlay, lv_color_hex(0xFF3B30),
                              LV_PART_MAIN);
    start_warn_anim(s_warn_overlay, anim_bg_opa_cb, 160, 600);
  }
}

// ─── Build screen ────────────────────────────────────────────────────────────

static lv_obj_t *make_panel(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

static void build_screen() {
  s_screen = lv_obj_create(nullptr);
  lv_obj_set_size(s_screen, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
  lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

  // ── WiFi dot ──
  s_wifi_dot = lv_obj_create(s_screen);
  lv_obj_set_size(s_wifi_dot, 6, 6);
  lv_obj_set_style_radius(s_wifi_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s_wifi_dot, COL_RED, 0);
  lv_obj_set_style_bg_opa(s_wifi_dot, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_wifi_dot, 0, 0);
  lv_obj_remove_flag(s_wifi_dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(s_wifi_dot, LV_ALIGN_TOP_MID, 0, 26);
  lv_obj_add_flag(s_wifi_dot, LV_OBJ_FLAG_HIDDEN);

  // ── Clock ──
  s_lbl_time = lv_label_create(s_screen);
  lv_obj_set_style_text_font(s_lbl_time, &sf_rounded_semibold_52, 0);
  lv_obj_set_style_text_color(s_lbl_time, COL_TEXT, 0);
  lv_label_set_text(s_lbl_time, "--:--");
  lv_obj_align(s_lbl_time, LV_ALIGN_TOP_MID, 0, 80);

  // ── Weather row: "Mon 20  ☀  9°" ──
  s_lbl_weather_date = lv_label_create(s_screen);
  lv_obj_set_style_text_font(s_lbl_weather_date, &sf_regular_20, 0);
  lv_obj_set_style_text_color(s_lbl_weather_date, COL_TEXT, 0);
  lv_label_set_text(s_lbl_weather_date, "");
  lv_obj_set_pos(s_lbl_weather_date, 100, 132);
  lv_obj_add_flag(s_lbl_weather_date, LV_OBJ_FLAG_HIDDEN);

  s_lbl_weather_icon = lv_label_create(s_screen);
  lv_obj_set_style_text_font(s_lbl_weather_icon, &lucide_weather_20, 0);
  lv_obj_set_style_text_color(s_lbl_weather_icon, COL_TEXT, 0);
  lv_label_set_text(s_lbl_weather_icon, "");
  lv_obj_set_pos(s_lbl_weather_icon, 160, 132);
  lv_obj_add_flag(s_lbl_weather_icon, LV_OBJ_FLAG_HIDDEN);

  s_lbl_weather_temp = lv_label_create(s_screen);
  lv_obj_set_style_text_font(s_lbl_weather_temp, &sf_regular_20, 0);
  lv_obj_set_style_text_color(s_lbl_weather_temp, COL_TEXT, 0);
  lv_label_set_text(s_lbl_weather_temp, "");
  lv_obj_set_pos(s_lbl_weather_temp, 190, 132);
  lv_obj_add_flag(s_lbl_weather_temp, LV_OBJ_FLAG_HIDDEN);

  // ── Divider ──
  s_divider = lv_obj_create(s_screen);
  lv_obj_set_size(s_divider, 120, 1);
  lv_obj_set_style_bg_color(s_divider, COL_DIVIDER, 0);
  lv_obj_set_style_bg_opa(s_divider, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_divider, 0, 0);
  lv_obj_set_style_radius(s_divider, 0, 0);
  lv_obj_remove_flag(s_divider, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_divider, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(s_divider, LV_ALIGN_TOP_MID, 0, 162);

  // ── Clip panel — sits below divider, constrains card animation ──
  // Children that translate outside this rect are clipped (LVGL default).
  s_clip = make_panel(s_screen);
  lv_obj_set_pos(s_clip, 20, 174);
  lv_obj_set_size(s_clip, LCD_H_RES - 40, 148);

  int card_w = LCD_H_RES - 60; // usable label width

  // ── Card — starts below the sticky day header, slides on encoder input ──
  s_card = make_panel(s_clip);
  lv_obj_set_size(s_card, LCD_H_RES - 40, 122);
  lv_obj_set_pos(s_card, 0, 26);

  // Event title
  s_lbl_title = lv_label_create(s_card);
  lv_obj_set_style_text_font(s_lbl_title, &sf_semibold_28, 0);
  lv_obj_set_style_text_color(s_lbl_title, COL_TEXT, 0);
  lv_obj_set_style_text_align(s_lbl_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(s_lbl_title, card_w);
  lv_label_set_long_mode(s_lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_label_set_text(s_lbl_title, "");
  lv_obj_align(s_lbl_title, LV_ALIGN_TOP_MID, 0, 4);

  // Time range
  s_lbl_time_range = lv_label_create(s_card);
  lv_obj_set_style_text_font(s_lbl_time_range, &sf_regular_16, 0);
  lv_obj_set_style_text_color(s_lbl_time_range, COL_DIM, 0);
  lv_obj_set_style_text_align(s_lbl_time_range, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(s_lbl_time_range, card_w);
  lv_label_set_text(s_lbl_time_range, "");
  lv_obj_align(s_lbl_time_range, LV_ALIGN_TOP_MID, 0, 46);

  // Relative time — LV_SIZE_CONTENT width so align_to(rec_dot) hits the
  // actual text edge, not the full card width.
  s_lbl_rel = lv_label_create(s_card);
  lv_obj_set_style_text_font(s_lbl_rel, &sf_regular_16, 0);
  lv_obj_set_style_text_color(s_lbl_rel, COL_ACCENT, 0);
  lv_obj_set_width(s_lbl_rel, LV_SIZE_CONTENT);
  lv_label_set_text(s_lbl_rel, "");
  lv_obj_align(s_lbl_rel, LV_ALIGN_TOP_MID, 0, 70);

  // Empty-state placeholder
  s_lbl_empty = lv_label_create(s_card);
  lv_obj_set_style_text_font(s_lbl_empty, &sf_regular_18, 0);
  lv_obj_set_style_text_color(s_lbl_empty, COL_DIM, 0);
  lv_obj_set_style_text_align(s_lbl_empty, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(s_lbl_empty, "No upcoming events");
  lv_obj_align(s_lbl_empty, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(s_lbl_empty, LV_OBJ_FLAG_HIDDEN);

  // ── Day header — child of clip, created AFTER card so it renders on top
  //    of the sliding card when the card translates up through the header row
  //    ──
  s_lbl_section = lv_label_create(s_clip);
  lv_obj_set_style_text_font(s_lbl_section, &sf_regular_16, 0);
  lv_obj_set_style_text_color(s_lbl_section, COL_ACCENT, 0);
  lv_obj_set_style_text_align(s_lbl_section, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(s_lbl_section, card_w);
  lv_label_set_text(s_lbl_section, "");
  lv_obj_align(s_lbl_section, LV_ALIGN_TOP_MID, 0, 4);

  // ── Recording dot — blinking red circle, left of "Happening now" ──
  s_rec_dot = lv_obj_create(s_card);
  lv_obj_set_size(s_rec_dot, 10, 10);
  lv_obj_set_style_radius(s_rec_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_rec_dot, lv_color_hex(0xFF453A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_rec_dot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_rec_dot, 0, LV_PART_MAIN);
  lv_obj_remove_flag(s_rec_dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_rec_dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_rec_dot, LV_OBJ_FLAG_HIDDEN);
  // Pulse: slow ease-in-out fade, 1200ms per phase
  lv_anim_t blink;
  lv_anim_init(&blink);
  lv_anim_set_var(&blink, s_rec_dot);
  lv_anim_set_exec_cb(&blink, [](void *obj, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), (lv_opa_t)v,
                         LV_PART_MAIN);
  });
  lv_anim_set_values(&blink, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_duration(&blink, 1200);
  lv_anim_set_playback_duration(&blink, 1200);
  lv_anim_set_repeat_count(&blink, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&blink, lv_anim_path_ease_in_out);
  lv_anim_start(&blink);

  // ── Warning ring — orange border arc for 2-min warning ──
  // Created after card so it renders on top.
  s_warn_ring = lv_obj_create(s_screen);
  lv_obj_set_size(s_warn_ring, LCD_H_RES - 10, LCD_V_RES - 10);
  lv_obj_center(s_warn_ring);
  lv_obj_set_style_radius(s_warn_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_warn_ring, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_warn_ring, 8, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_warn_ring, lv_color_hex(0xFF9F0A),
                                LV_PART_MAIN);
  lv_obj_set_style_border_opa(s_warn_ring, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_remove_flag(s_warn_ring, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_warn_ring, LV_OBJ_FLAG_CLICKABLE);

  // ── Warning overlay — red bg for 1-min warning ──
  s_warn_overlay = lv_obj_create(s_screen);
  lv_obj_set_size(s_warn_overlay, LCD_H_RES, LCD_V_RES);
  lv_obj_center(s_warn_overlay);
  lv_obj_set_style_radius(s_warn_overlay, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_warn_overlay, lv_color_hex(0xFF3B30),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_warn_overlay, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_warn_overlay, 0, LV_PART_MAIN);
  lv_obj_remove_flag(s_warn_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_warn_overlay, LV_OBJ_FLAG_CLICKABLE);
}

// ─── Public API ──────────────────────────────────────────────────────────────

void ui_init() {
  lv_display_t *disp = nullptr;
  lv_indev_t *touch = nullptr;
  display_init(&disp, &touch);

  if (display_lock(200)) {
    build_screen();

    s_browse_timer =
        lv_timer_create(on_browse_timeout, BROWSE_TIMEOUT_MS, nullptr);
    lv_timer_pause(s_browse_timer);

    s_clock_timer = lv_timer_create(on_clock_tick, CLOCK_UPDATE_MS, nullptr);

    lv_timer_create(on_encoder_poll, ENCODER_POLL_MS, nullptr);

    update_clock();
    update_display_instant();

    lv_screen_load(s_screen);
    display_unlock();
  }
  ESP_LOGI(TAG, "UI ready");
}

void ui_set_events(const CalEvent *events, int count) {
  int n = std::min(count, MAX_EVENTS);
  if (n > 0)
    memcpy(s_events, events, n * sizeof(CalEvent));
  s_event_count = n;
  bool first_load = !s_loaded;
  s_loaded = true;

  // Clamp scroll position — preserve it across background refreshes
  if (s_event_count == 0) {
    s_scroll_pos = 0;
    s_browsing = false;
  } else if (s_scroll_pos >= s_event_count) {
    s_scroll_pos = s_event_count - 1;
  }

  if (display_lock(100)) {
    if (first_load) {
      // First fetch: full reset, show content from rest position
      update_display_instant();
    } else {
      // Background refresh: silently update content, preserve scroll +
      // animation state
      if (!s_animating)
        populate_card();
    }
    display_unlock();
  }
}

void ui_set_wifi_status(bool connected) {
  s_wifi_ok = connected;
  if (display_lock(100)) {
    if (connected) {
      lv_obj_add_flag(s_wifi_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_remove_flag(s_wifi_dot, LV_OBJ_FLAG_HIDDEN);
    }
    if (!s_loaded)
      populate_card();
    display_unlock();
  }
}

void ui_set_weather(const WeatherData *data) {
  if (!display_lock(50))
    return;

  s_weather_valid = data && data->valid && data->condition[0];
  if (s_weather_valid) {
    int t = static_cast<int>(data->temperature +
                             (data->temperature >= 0 ? 0.5f : -0.5f));
    snprintf(s_weather_temp, sizeof(s_weather_temp), "%d\xC2\xB0", t);
    if (data->icon) {
      strncpy(s_weather_icon, data->icon, sizeof(s_weather_icon) - 1);
      s_weather_icon[sizeof(s_weather_icon) - 1] = '\0';
    }
    s_weather_color = data->color;

    // Update date from current time
    time_t now = time(nullptr);
    struct tm t_info{};
    localtime_r(&now, &t_info);
    static const char *const DAYS[] = {"Sun", "Mon", "Tue", "Wed",
                                       "Thu", "Fri", "Sat"};
    snprintf(s_weather_date, sizeof(s_weather_date), "%s %d",
             DAYS[t_info.tm_wday], t_info.tm_mday);

    lv_label_set_text(s_lbl_weather_date, s_weather_date);
    lv_label_set_text(s_lbl_weather_icon, s_weather_icon);
    lv_obj_set_style_text_color(s_lbl_weather_icon,
                                lv_color_hex(s_weather_color), 0);
    lv_label_set_text(s_lbl_weather_temp, s_weather_temp);

    update_weather_positions();

    lv_obj_remove_flag(s_lbl_weather_date, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_lbl_weather_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_lbl_weather_temp, LV_OBJ_FLAG_HIDDEN);
  }

  display_unlock();
}

void ui_on_encoder_rotate(int32_t steps) {
  // Encoder input is consumed by the LVGL-task poll (on_encoder_poll).
  // This entry point is kept for interface compatibility.
  (void)steps;
}
