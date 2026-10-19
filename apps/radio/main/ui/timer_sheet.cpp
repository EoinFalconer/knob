#include "ui/timer_sheet.h"
#include "app_config.h"

#include "fonts.h"
#include "hal_pins.h"
#include "haptic.h"
#include "sonos.h"
#include "timer.h"
#include "ui/lights_page.h"
#include "ui/motion.h"
#include "ui/primitives.h"
#include "ui/sheet.h"
#include "ui/timer_dial.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

enum class Phase { Setting, Running, Ringing };

constexpr int MAX_MINUTES = 60;
constexpr int DEFAULT_MINUTES = 5;
constexpr int SNOOZE_MINUTES = 5;
constexpr uint32_t FRAME_MS = 100;
constexpr uint32_t BUZZ_MS = 2000;
// The speaker gives up after a minute (ALARM_MAX_MS); so does the screen.
constexpr uint32_t RING_MAX_MS = 60000;

constexpr uint32_t VOLUME_SHOWN_MS = 1200;
// A knob detent lights the next tick with a short slide rather than a jump.
constexpr uint32_t TICK_SLIDE_MS = 180;
// Starting fills the ring from the minutes set to all 60 ticks, which the
// countdown then drains: the press visibly winds the timer up.
constexpr uint32_t WIND_UP_MS = 420;

constexpr uint32_t COL_ORANGE = 0xFF9F0A;
constexpr uint32_t COL_RED = 0xFF453A; // Apple's system red, dark mode
// Swiped down while running, the sheet is a cancel button: one big red
// capsule, cut like Stop. A tap anywhere on the sheet takes it. The ring and
// the countdown already show on the screen underneath, and a second ring
// sliding over them would only double up, so only the time left stays, as
// the caption.
constexpr int CANCEL_W = 220;
constexpr int CANCEL_H = 76;
constexpr int CANCEL_Y = 14;
constexpr int CANCEL_LEFT_GAP = 18; // caption to capsule
constexpr lv_opa_t FILLED_OPA = LV_OPA_COVER;
constexpr lv_opa_t FILLED_PRESSED_OPA = LV_OPA_80;
constexpr lv_opa_t CAPSULE_OPA = 38;
constexpr lv_opa_t CAPSULE_PRESSED_OPA = 70;
constexpr int CAPSULE_H = 40;
constexpr int RINGING_DIGITS_Y = -14;
constexpr int ANSWER_Y = 66;
constexpr int ANSWER_W = 92;
constexpr int ANSWER_GAP = 12;
constexpr int PILL_H = 30;
constexpr int PILL_BOTTOM = 26;

// The sheet has two pages side by side, the timer and the lights, swiped
// between like iOS pages with dots to say which is up. The dots sit inside
// the dial, below everything either page shows.
enum Page { TIMER_PAGE, LIGHTS_PAGE, PAGE_COUNT };
constexpr int DOT_D = 7;
constexpr int DOT_GAP = 9;
constexpr int DOTS_Y = 118; // from the center
constexpr lv_opa_t DOT_DIM_OPA = 90;

TimerSheetHooks s_hooks;
lv_obj_t *s_sheet;
lv_obj_t *s_pages; // scrolls sideways, one page at a time
lv_obj_t *s_dots[PAGE_COUNT];
int s_page = TIMER_PAGE;
lv_obj_t *s_dial;
lv_obj_t *s_tap;    // the whole sheet, while setting
lv_obj_t *s_cancel; // the round button and its hit area, while running
lv_obj_t *s_cancel_left;
lv_obj_t *s_stop, *s_snooze;
IconPill s_pill;
int s_pill_seconds = -1;

Phase s_phase = Phase::Setting;
int s_minutes = DEFAULT_MINUTES;
int32_t s_shown_ticks; // the setting's ticks as drawn, in 1/100
int32_t s_fill = 6000; // how far the running ring reaches, in 1/100
uint32_t s_total_ms;
uint32_t s_end_ms;
uint32_t s_ring_ms;
uint32_t s_buzz_ms;
uint32_t s_volume_ms;
bool s_volume_shown;
// The phase moved on but the sheet still shows the last one: it is on its way
// out, and keeps what it showed until it's gone rather than flashing the next
// phase as it leaves.
bool s_stale;
char s_label[32] = "Timer";

uint32_t left_ms() {
  auto left = static_cast<int32_t>(s_end_ms - lv_tick_get());
  return left > 0 ? static_cast<uint32_t>(left) : 0;
}

int left_seconds() { return static_cast<int>((left_ms() + 999) / 1000); }

// What the running timer is: its own name when it has one (a voice timer
// for the pasta), else how long it was set for.
void running_caption(char *out, size_t len) {
  if (strcmp(s_label, "Timer") != 0) {
    snprintf(out, len, "%s", s_label);
    return;
  }
  uint32_t s = s_total_ms / 1000;
  if (s % 60 == 0)
    snprintf(out, len, "%u min", static_cast<unsigned>(s / 60));
  else
    snprintf(out, len, "%u:%02u", static_cast<unsigned>(s / 60),
             static_cast<unsigned>(s % 60));
}

void show_only(lv_obj_t *visible) {
  for (lv_obj_t *o : {s_tap, s_cancel, s_stop, s_snooze})
    lv_obj_set_flag(o, LV_OBJ_FLAG_HIDDEN, o != visible);
  lv_obj_set_flag(s_dial, LV_OBJ_FLAG_HIDDEN, visible == s_cancel);
}

void anim_ticks_cb(void *, int32_t v) {
  s_shown_ticks = v;
  timer_dial_set_ticks(s_dial, 0.0f, static_cast<float>(v) / 100.0f);
}

void slide_ticks_to(int minutes, bool animate) {
  lv_anim_delete(&s_shown_ticks, anim_ticks_cb);
  int32_t to = minutes * 100;
  if (!animate) {
    anim_ticks_cb(nullptr, to);
    return;
  }
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &s_shown_ticks);
  lv_anim_set_exec_cb(&a, anim_ticks_cb);
  lv_anim_set_values(&a, s_shown_ticks, to);
  lv_anim_set_duration(&a, TICK_SLIDE_MS);
  motion_ease_out(&a);
  lv_anim_start(&a);
}

void show_setting() {
  timer_dial_set_digits_y(s_dial, TIMER_DIAL_DIGITS_Y);
  timer_dial_set_seconds(s_dial, s_minutes * 60);
  slide_ticks_to(s_minutes, false);
  timer_dial_set_caption(s_dial, "Tap to start", false, true);
  show_only(s_tap);
}

void show_running() {
  lv_anim_delete(&s_shown_ticks, anim_ticks_cb);
  show_only(s_cancel);
}

void show_ringing() {
  lv_anim_delete(&s_shown_ticks, anim_ticks_cb);
  timer_dial_set_ticks(s_dial, 0.0f, 60.0f);
  timer_dial_set_seconds(s_dial, 0);
  timer_dial_set_digits_y(s_dial, RINGING_DIGITS_Y);
  timer_dial_set_caption(s_dial, s_label, true, false);
  show_only(s_stop);
  lv_obj_remove_flag(s_snooze, LV_OBJ_FLAG_HIDDEN);
}

void set_page(int page) {
  if (page == s_page)
    return;
  s_page = page;

  for (int i = 0; i < PAGE_COUNT; i++)
    lv_obj_set_style_bg_opa(s_dots[i], i == page ? LV_OPA_COVER : DOT_DIM_OPA,
                            LV_PART_MAIN);
  if (page == LIGHTS_PAGE)
    lights_page_appear();
}

void go_to_page(int page, bool animate) {
  lv_obj_scroll_to_x(s_pages, page * LCD_H_RES,
                     animate ? LV_ANIM_ON : LV_ANIM_OFF);
  set_page(page);
}

// The dot moves as the page passes halfway, not once it has settled.
void on_pages_scroll(lv_event_t *) {
  int32_t x = lv_obj_get_scroll_x(s_pages);
  set_page(std::clamp<int>((x + LCD_H_RES / 2) / LCD_H_RES, 0, PAGE_COUNT - 1));
}

// A ringing timer must be answered, so the sheet stays on it.
void hold_on_timer(bool hold) {
  lv_obj_set_flag(s_pages, LV_OBJ_FLAG_SCROLLABLE, !hold);
  for (lv_obj_t *dot : s_dots)
    lv_obj_set_flag(dot, LV_OBJ_FLAG_HIDDEN, hold);
  if (hold && s_page != TIMER_PAGE)
    go_to_page(TIMER_PAGE, sheet_is_visible(s_sheet));
}

void show_phase() {
  s_volume_shown = false;
  s_stale = false;
  if (s_phase == Phase::Setting)
    show_setting();
  else if (s_phase == Phase::Running)
    show_running();
  else
    show_ringing();
  hold_on_timer(s_phase == Phase::Ringing);
}

void tick_running() {
  float drained = 60.0f * (1.0f - static_cast<float>(left_ms()) /
                                      static_cast<float>(s_total_ms));
  float reach = static_cast<float>(s_fill) / 100.0f;
  timer_dial_set_ticks(s_dial, std::min(drained, reach), reach);
  int seconds = left_seconds();
  timer_dial_set_seconds(s_dial, seconds);
  if (s_volume_shown)
    return;
  char left[24];
  snprintf(left, sizeof(left), "%d:%02d LEFT", seconds / 60, seconds % 60);
  if (strcmp(left, lv_label_get_text(s_cancel_left)) != 0)
    lv_label_set_text(s_cancel_left, left);
}

void anim_fill_cb(void *, int32_t v) {
  s_fill = v;
  tick_running();
}

void update_pill() {
  if (!s_pill.pill)
    return;
  bool shown =
      s_phase == Phase::Running && s_hooks.pill_shown && s_hooks.pill_shown();
  lv_obj_set_flag(s_pill.pill, LV_OBJ_FLAG_HIDDEN, !shown);
  int seconds = left_seconds();
  if (!shown || seconds == s_pill_seconds)
    return;
  s_pill_seconds = seconds;
  char text[12];
  snprintf(text, sizeof(text), "%d:%02d", seconds / 60, seconds % 60);
  ui_icon_pill_set_text(s_pill, text);
}

// Once wound up, the sheet fades where it is: the idle face underneath shows
// the same dial, and elsewhere the pill takes over the countdown.
void wound_up(lv_anim_t *) {
  if (s_phase != Phase::Running)
    return;
  sheet_dissolve(s_sheet);
  update_pill();
}

void wind_up(int32_t from) {
  lv_anim_delete(&s_fill, anim_fill_cb);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &s_fill);
  lv_anim_set_exec_cb(&a, anim_fill_cb);
  lv_anim_set_values(&a, from, 6000);
  lv_anim_set_duration(&a, WIND_UP_MS);
  motion_ease_out(&a);
  lv_anim_set_completed_cb(&a, wound_up);
  lv_anim_start(&a);
}

// Moves on to `next`. A sheet on screen keeps showing the old phase as it
// slides away; it shows the new one when it next appears.
void move_on(Phase next) {
  s_phase = next;
  if (!sheet_is_visible(s_sheet)) {
    show_phase();
    return;
  }
  s_stale = true;
  s_volume_shown = false;
  sheet_close(s_sheet);
}

void finish_ringing() {
  sonos_stop_alarm();
  move_on(Phase::Setting);
  s_hooks.changed();
}

void on_frame(lv_timer_t *) {
  uint32_t now = lv_tick_get();
  if (s_volume_shown && now - s_volume_ms >= VOLUME_SHOWN_MS)
    show_phase();
  switch (s_phase) {
  case Phase::Setting:
    if (sheet_is_open(s_sheet) &&
        lv_display_get_inactive_time(nullptr) >= IDLE_MS)
      sheet_close(s_sheet);
    break;
  case Phase::Running:
    tick_running();
    break;
  case Phase::Ringing:
    if (now - s_ring_ms >= RING_MAX_MS) {
      finish_ringing();
      break;
    }
    if (now - s_buzz_ms >= BUZZ_MS) {
      s_buzz_ms = now;
      haptic_buzz();
      s_hooks.wake();
    }
    break;
  }
  update_pill();
}

void on_tap(lv_event_t *) {
  haptic_buzz();
  if (s_phase == Phase::Setting)
    timer_start(s_minutes * 60, "Timer");
  else if (s_phase == Phase::Running)
    timer_cancel();
}

void on_stop(lv_event_t *) {
  haptic_buzz();
  finish_ringing();
}

void on_snooze(lv_event_t *) {
  haptic_buzz();
  sonos_stop_alarm();
  timer_start(SNOOZE_MINUTES * 60, s_label);
}

void on_pill(lv_event_t *) {
  haptic_buzz();
  timer_sheet_open();
}

struct Capsule {
  lv_obj_t *box;
  lv_obj_t *label;
};

// A filled capsule is the sheet's answer, like Stop; a frosted white one is
// the alternative, like Snooze.
Capsule make_capsule(lv_obj_t *parent, const char *text, uint32_t fill,
                     bool filled) {
  lv_obj_t *c = ui_box(parent);
  lv_obj_set_height(c, CAPSULE_H);
  lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(c, lv_color_hex(fill), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(c, filled ? FILLED_OPA : CAPSULE_OPA, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(
      c, filled ? FILLED_PRESSED_OPA : CAPSULE_PRESSED_OPA,
      static_cast<lv_style_selector_t>(LV_PART_MAIN | LV_STATE_PRESSED));
  lv_obj_t *lbl = lv_label_create(c);
  lv_obj_set_style_text_font(lbl, &sf_medium_15, LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl, filled ? lv_color_black() : lv_color_white(),
                              LV_PART_MAIN);
  lv_label_set_text(lbl, text);
  lv_obj_center(lbl);
  return {c, lbl};
}

lv_obj_t *make_answer(lv_obj_t *parent, const char *text, bool primary,
                      lv_event_cb_t on_tap, int x) {
  Capsule c =
      make_capsule(parent, text, primary ? COL_ORANGE : 0xFFFFFF, primary);
  lv_obj_set_width(c.box, ANSWER_W);
  lv_obj_align(c.box, LV_ALIGN_CENTER, x, ANSWER_Y);
  lv_obj_add_flag(c.box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(c.box, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_event_cb(c.box, on_tap, LV_EVENT_CLICKED, nullptr);
  return c.box;
}

lv_obj_t *make_cancel(lv_obj_t *parent) {
  lv_obj_t *c = ui_box(parent);
  lv_obj_set_size(c, LCD_H_RES, LCD_V_RES);

  Capsule button = make_capsule(c, "Stop timer", COL_RED, true);
  lv_obj_set_size(button.box, CANCEL_W, CANCEL_H);
  lv_obj_align(button.box, LV_ALIGN_CENTER, 0, CANCEL_Y);
  lv_obj_set_style_text_font(button.label, &sf_semibold_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(button.label, lv_color_white(), LV_PART_MAIN);

  s_cancel_left = lv_label_create(c);
  lv_obj_set_style_text_font(s_cancel_left, &sf_semibold_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_cancel_left, lv_color_hex(COL_ORANGE),
                              LV_PART_MAIN);
  lv_label_set_text(s_cancel_left, "");
  lv_obj_align(s_cancel_left, LV_ALIGN_BOTTOM_MID, 0,
               CANCEL_Y - CANCEL_H / 2 - CANCEL_LEFT_GAP - LCD_V_RES / 2);

  lv_obj_t *hit = ui_hit_area(c, button.box, on_tap);
  lv_obj_set_size(hit, LCD_H_RES, LCD_V_RES);
  return c;
}

// A sheet that rings must be answered.
bool may_close_from(lv_point_t) { return s_phase != Phase::Ringing; }

void refresh() {
  if (s_phase == Phase::Setting)
    s_minutes = DEFAULT_MINUTES;
  show_phase();
  if (s_phase == Phase::Running)
    tick_running();
  if (s_page == LIGHTS_PAGE)
    lights_page_appear();
  s_hooks.wake();
}

// Swiped down afresh, it always opens on the timer.
void on_appear() {
  go_to_page(TIMER_PAGE, false);
  refresh();
}

// Caught on its way out and pulled back open, it must offer what's current,
// on the page it was leaving. A ringing sheet that just dropped now hides the
// face, which can change.
void on_opened() {
  if (s_stale)
    refresh();
  s_hooks.changed();
  lv_display_trigger_activity(nullptr);
}

bool may_open() { return s_hooks.may_open(); }

lv_obj_t *make_pages(lv_obj_t *sheet) {
  lv_obj_t *pages = lv_obj_create(sheet);
  lv_obj_remove_style_all(pages);
  lv_obj_set_size(pages, LCD_H_RES, LCD_V_RES);
  lv_obj_set_scroll_dir(pages, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(pages, LV_SCROLL_SNAP_CENTER);
  lv_obj_add_flag(pages, LV_OBJ_FLAG_SCROLL_ONE);
  lv_obj_set_scrollbar_mode(pages, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(pages, on_pages_scroll, LV_EVENT_SCROLL, nullptr);
  lv_obj_add_event_cb(pages, sheets_scroll_with_finger, LV_EVENT_SCROLL_BEGIN,
                      nullptr);
  return pages;
}

lv_obj_t *make_page(lv_obj_t *page) {
  lv_obj_set_size(page, LCD_H_RES, LCD_V_RES);
  lv_obj_set_x(page, lv_obj_get_index(page) * LCD_H_RES);
  return page;
}

void make_dots(lv_obj_t *sheet) {
  int32_t width = PAGE_COUNT * DOT_D + (PAGE_COUNT - 1) * DOT_GAP;
  for (int i = 0; i < PAGE_COUNT; i++) {
    lv_obj_t *dot = ui_box(sheet);
    lv_obj_set_size(dot, DOT_D, DOT_D);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, i == s_page ? LV_OPA_COVER : DOT_DIM_OPA,
                            LV_PART_MAIN);
    lv_obj_align(dot, LV_ALIGN_CENTER,
                 -width / 2 + DOT_D / 2 + i * (DOT_D + DOT_GAP), DOTS_Y);
    s_dots[i] = dot;
  }
}

} // namespace

void timer_sheet_create(lv_obj_t *screen, const TimerSheetHooks &hooks) {
  s_hooks = hooks;
  static constexpr SheetPolicy POLICY = {
      .may_open = may_open,
      .may_close_from = may_close_from,
      .on_appear = on_appear,
      .on_opened = on_opened,
      .on_closed = nullptr,
  };
  s_sheet = sheet_create(screen, SheetEdge::Top, POLICY);
  s_pages = make_pages(s_sheet);
  lv_obj_t *timer_page = make_page(ui_box(s_pages));
  make_page(lights_page_create(s_pages));
  make_dots(s_sheet);

  s_tap = lv_obj_create(timer_page);
  lv_obj_remove_style_all(s_tap);
  lv_obj_set_size(s_tap, LCD_H_RES, LCD_V_RES);
  lv_obj_remove_flag(s_tap, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_event_cb(s_tap, on_tap, LV_EVENT_CLICKED, nullptr);

  s_dial = timer_dial_create(timer_page);

  int offset = (ANSWER_W + ANSWER_GAP) / 2;
  s_snooze = make_answer(timer_page, "Snooze", false, on_snooze, -offset);
  s_stop = make_answer(timer_page, "Stop", true, on_stop, offset);

  s_cancel = make_cancel(timer_page);

  show_phase();
  lv_timer_create(on_frame, FRAME_MS, nullptr);
}

bool timer_sheet_is_open() { return s_sheet && sheet_is_open(s_sheet); }

bool timer_sheet_is_visible() { return s_sheet && sheet_is_visible(s_sheet); }

void timer_sheet_open() { sheet_open(s_sheet); }

bool timer_sheet_turn(int steps) {
  if (s_page == LIGHTS_PAGE) {
    lights_page_turn(steps);
    return true;
  }
  if (s_phase != Phase::Setting)
    return false;
  int raw = s_minutes + steps;
  s_minutes = std::clamp(raw, 1, MAX_MINUTES);
  if (raw != s_minutes)
    haptic_buzz();
  timer_dial_set_seconds(s_dial, s_minutes * 60);
  slide_ticks_to(s_minutes, true);
  return true;
}

void timer_sheet_set_lights(const LightsState &state) {
  lights_page_show(state);
}

void timer_sheet_show_volume(int level) {
  if (!sheet_is_visible(s_sheet) || s_phase == Phase::Setting || s_stale)
    return;
  char text[24];
  snprintf(text, sizeof(text), "Volume %d", level);
  timer_dial_set_caption(s_dial, text, s_phase == Phase::Ringing, true);
  snprintf(text, sizeof(text), "VOLUME %d", level);
  lv_label_set_text(s_cancel_left, text);
  s_volume_shown = true;
  s_volume_ms = lv_tick_get();
}

// Started from the open sheet, by a tap or by Snooze, the ring winds up from
// the ticks it showed: the minutes set, or as many as were snoozed.
void timer_sheet_started(int total_s, uint32_t left_ms, const char *label) {
  bool winding =
      s_phase != Phase::Running && sheet_is_open(s_sheet) && !s_stale;
  int32_t wound = s_phase == Phase::Setting
                      ? s_shown_ticks
                      : std::min(total_s, MAX_MINUTES * 60) * 100 / 60;
  snprintf(s_label, sizeof(s_label), "%s", label && label[0] ? label : "Timer");
  s_total_ms = static_cast<uint32_t>(std::max(total_s, 1)) * 1000;
  s_end_ms = lv_tick_get() + std::min(left_ms, s_total_ms);

  s_phase = Phase::Running;
  s_pill_seconds = -1;
  show_phase();
  s_fill = 6000;
  if (winding) {
    // On its way out it names the timer, rather than offering to cancel
    // what was just started.
    show_only(nullptr);
    timer_dial_set_digits_y(s_dial, TIMER_DIAL_DIGITS_Y);
    char text[40];
    running_caption(text, sizeof(text));
    timer_dial_set_caption(s_dial, text, false, false);
    wind_up(wound);
  }
  tick_running();
  update_pill();
  s_hooks.changed();
}

void timer_sheet_fired(const char *label) {
  if (label && label[0])
    snprintf(s_label, sizeof(s_label), "%s", label);
  s_phase = Phase::Ringing;
  s_ring_ms = s_buzz_ms = lv_tick_get();
  show_phase();
  update_pill();
  haptic_buzz();
  s_hooks.wake();
  bool covering = sheet_is_open(s_sheet);
  sheet_open(s_sheet);
  if (covering)
    s_hooks.changed();
}

void timer_sheet_cancelled() {
  if (s_phase != Phase::Running)
    return;
  lv_anim_delete(&s_fill, anim_fill_cb);
  move_on(Phase::Setting);
  update_pill();
  s_hooks.changed();
}

bool timer_running() { return s_phase == Phase::Running; }
bool timer_ringing() { return s_phase == Phase::Ringing; }
uint32_t timer_left_ms() { return s_phase == Phase::Running ? left_ms() : 0; }
uint32_t timer_total_ms() { return s_total_ms; }

const char *timer_caption() {
  static char text[40];
  running_caption(text, sizeof(text));
  return text;
}

void timer_pill_create(lv_obj_t *parent) {
  s_pill =
      ui_icon_pill(parent, PILL_H, &lucide_18, ICON_TIMER, 0, &sf_medium_15);
  s_pill_seconds = -1;
  lv_obj_align(s_pill.pill, LV_ALIGN_BOTTOM_MID, 0, -PILL_BOTTOM);
  lv_obj_set_style_bg_color(s_pill.pill, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_pill.pill, LV_OPA_10, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(
      s_pill.pill, LV_OPA_20,
      static_cast<lv_style_selector_t>(LV_PART_MAIN | LV_STATE_PRESSED));
  for (lv_obj_t *o : {s_pill.icon, s_pill.label})
    lv_obj_set_style_text_color(o, lv_color_hex(COL_ORANGE), LV_PART_MAIN);
  lv_obj_add_flag(s_pill.pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_pill.pill, on_pill, LV_EVENT_SHORT_CLICKED, nullptr);
  // The pager rebuilds the page; forget the pill with it.
  lv_obj_add_event_cb(
      s_pill.pill, [](lv_event_t *) { s_pill = {}; }, LV_EVENT_DELETE, nullptr);
  ui_icon_pill_set_text(s_pill, "0:00");
  update_pill();
}
