#include "ui/ui.h"
#include "app_config.h"
#include "art_decoder.h"
#include "display.h"
#include "encoder.h"
#include "haptic.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lvgl.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

static constexpr const char *TAG = "ui";

ESP_EVENT_DECLARE_BASE(APP_EVENT);

// Defined in main.cpp — enqueues an avatar fetch directly onto the worker queue
// (bypassing the small esp_event loop queue).
void kaffi_request_avatar(const char *url);

// ─── Coffee theme (lv_color_t field order is blue, green, red)
static constexpr lv_color_t COL_BG = {.blue = 0x0B, .green = 0x12, .red = 0x1A};
static constexpr lv_color_t COL_TEXT = {
    .blue = 0xE6, .green = 0xEF, .red = 0xF5};
static constexpr lv_color_t COL_CREAM = {
    .blue = 0xB0, .green = 0xD8, .red = 0xE8};
static constexpr lv_color_t COL_SUB = {
    .blue = 0x88, .green = 0xA0, .red = 0xB0};
static constexpr lv_color_t COL_BREW = {
    .blue = 0x37, .green = 0x4E, .red = 0x6F};
static constexpr lv_color_t COL_CUP = {
    .blue = 0x3A, .green = 0x8A, .red = 0xC8};
static constexpr lv_color_t COL_BACK = {
    .blue = 0x20, .green = 0x2C, .red = 0x3A};
static constexpr lv_color_t COL_GOOD = {
    .blue = 0x73, .green = 0xBF, .red = 0x6F};
static constexpr lv_color_t COL_BAD = {
    .blue = 0x4B, .green = 0x55, .red = 0xD9};
static constexpr lv_color_t COL_GOLD = {
    .blue = 0x58, .green = 0xC5, .red = 0xE8};
static constexpr lv_color_t COL_SILVER = {
    .blue = 0xD0, .green = 0xC8, .red = 0xC0};
static constexpr lv_color_t COL_BRONZE = {
    .blue = 0x5A, .green = 0x8A, .red = 0xC8};
static constexpr lv_color_t COL_FLAME = { // streak orange (#F07830)
    .blue = 0x30,
    .green = 0x78,
    .red = 0xF0};

LV_IMAGE_DECLARE(img_flame); // 16px A8 Lucide flame, tinted at draw time

enum class Mode { Status, List, Action, Pots, Leaderboard };

// True on Mon-Fri with a synced clock — the only days a streak can die.
static bool is_weekday_today() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  if (lt.tm_year + 1900 < 2023)
    return false;
  return lt.tm_wday >= 1 && lt.tm_wday <= 5;
}

static int balance_of(const Person *p) { return p->credit - p->cups; }

// ─── State
static KaffiState s_state = {};
static int s_sel = 0;
static Mode s_mode = Mode::Status;

// Avatar cache — one decoded RGB565 buffer per person, kept in PSRAM so
// switching people is instant. Filled by a background preload after each
// people refresh.
enum class AvState : uint8_t { Empty, Requested, Loaded };
struct AvatarSlot {
  uint8_t *pixels;       // full 96px photo (home screen)
  uint8_t *thumb_pixels; // pre-scaled thumb (lists) — see art_scale_rgb565()
  lv_image_dsc_t dsc;
  lv_image_dsc_t thumb_dsc;
  AvState state;
  char url[KAFFI_URL_LEN];
};
static constexpr int THUMB_SIZE = 40;
static AvatarSlot s_av[KAFFI_MAX_PEOPLE] = {};

// Leaderboard
static int64_t s_lb_last_activity = 0;
static int s_lb_highlight = 0;

// ─── Objects
static lv_obj_t *s_screen = nullptr;
static lv_obj_t *s_status_view = nullptr;
static lv_obj_t *s_status_lbl = nullptr;
static lv_obj_t *s_list_view = nullptr;
static lv_obj_t *s_list_arc = nullptr; // rim arc showing list position
static lv_obj_t *s_avatar_box = nullptr;
static lv_obj_t *s_avatar_img = nullptr;
static lv_obj_t *s_avatar_init = nullptr;
static lv_obj_t *s_streak_badge = nullptr; // flame "DAY N" pill on the avatar
static lv_obj_t *s_streak_flame = nullptr;
static lv_obj_t *s_streak_lbl = nullptr;
static lv_obj_t *s_list_name = nullptr;
static lv_obj_t *s_list_stats = nullptr;
static lv_obj_t *s_list_balance = nullptr;
static lv_obj_t *s_list_index = nullptr;
static lv_obj_t *s_action_view = nullptr;
static lv_obj_t *s_action_name = nullptr;
static lv_obj_t *s_action_avatar = nullptr; // who you're logging for
static lv_obj_t *s_action_init = nullptr;
static lv_obj_t *s_pots_view = nullptr;
static lv_obj_t *s_lb_view = nullptr;
static lv_obj_t *s_lb_list = nullptr;
static lv_obj_t *s_toast = nullptr;
static lv_obj_t *s_net_pill = nullptr; // "OFFLINE" / "N PENDING" on the list
static lv_obj_t *s_net_lbl = nullptr;
static lv_timer_t *s_toast_timer = nullptr;

// ─── Helpers

static void anim_opa_cb(void *obj, int32_t v) {
  lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(v),
                       0);
}

static void anim_ty_cb(void *obj, int32_t v) {
  lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(obj), v, 0);
}

// Springy nudge on the avatar when the selection moves — pure translation, so
// no per-pixel transform work.
static void avatar_nudge(int dir) {
  lv_anim_delete(s_avatar_box, anim_ty_cb);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_avatar_box);
  lv_anim_set_values(&a, dir > 0 ? 8 : -8, 0);
  lv_anim_set_duration(&a, 220);
  lv_anim_set_exec_cb(&a, anim_ty_cb);
  lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
  lv_anim_start(&a);
}

// Quick fade-in for a view when it becomes visible.
static void fade_in(lv_obj_t *o) {
  lv_anim_delete(o, anim_opa_cb);
  lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_duration(&a, 150);
  lv_anim_set_exec_cb(&a, anim_opa_cb);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

static void set_mode(Mode m) {
  s_mode = m;
  lv_obj_add_flag(s_status_view, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_action_view, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_pots_view, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_lb_view, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *show = nullptr;
  switch (m) {
  case Mode::Status:
    show = s_status_view;
    break;
  case Mode::List:
    show = s_list_view;
    break;
  case Mode::Action:
    show = s_action_view;
    break;
  case Mode::Pots:
    show = s_pots_view;
    break;
  case Mode::Leaderboard:
    show = s_lb_view;
    break;
  }
  if (show) {
    lv_obj_clear_flag(show, LV_OBJ_FLAG_HIDDEN);
    fade_in(show);
  }
}

// Show the cached photo for the current selection, else the initial
// placeholder.
static void update_avatar_view() {
  const char *name = s_state.count > 0 ? s_state.people[s_sel].name : "";
  char init[2] = {static_cast<char>(name[0] ? toupper(name[0]) : '?'), '\0'};
  lv_label_set_text(s_avatar_init, init);

  bool have = s_state.count > 0 && s_av[s_sel].state == AvState::Loaded &&
              s_av[s_sel].pixels;
  if (have) {
    lv_image_set_src(s_avatar_img, &s_av[s_sel].dsc);
    lv_obj_clear_flag(s_avatar_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_avatar_init, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_avatar_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_avatar_init, LV_OBJ_FLAG_HIDDEN);
  }
}

static void refresh_list() {
  if (s_state.count == 0) {
    lv_label_set_text(s_list_name, "No people yet");
    lv_label_set_text(s_list_stats, "");
    lv_label_set_text(s_list_balance, "Add people in the studio");
    lv_obj_set_style_text_color(s_list_balance, COL_SUB, 0);
    lv_label_set_text(s_list_index, "0 / 0");
    lv_obj_add_flag(s_list_arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_streak_badge, LV_OBJ_FLAG_HIDDEN);
    update_avatar_view();
    return;
  }
  if (s_sel >= s_state.count)
    s_sel = 0;
  const Person *p = &s_state.people[s_sel];

  lv_label_set_text(s_list_name, p->name);

  char stats[64];
  snprintf(stats, sizeof(stats), "POTS %d    CUPS %d", p->pots, p->cups);
  lv_label_set_text(s_list_stats, stats);

  int b = balance_of(p);
  char bal[48];
  lv_color_t bal_col;
  if (b > 0) {
    snprintf(bal, sizeof(bal), "%s  +%d", b >= 6 ? "coffee hero" : "in credit",
             b);
    bal_col = COL_GOOD;
  } else if (b < 0) {
    snprintf(bal, sizeof(bal), "%s  %d",
             b <= -6 ? "mega freeloader" : "freeloader", b);
    bal_col = COL_BAD;
  } else {
    snprintf(bal, sizeof(bal), "even");
    bal_col = COL_SUB;
  }
  lv_label_set_text(s_list_balance, bal);
  lv_obj_set_style_text_color(s_list_balance, bal_col, 0);

  // Glanceable standing: ring the avatar in the balance color (neutral when
  // even, so most people get a quiet dark ring).
  lv_obj_set_style_outline_color(s_avatar_box, b == 0 ? COL_BACK : bal_col, 0);

  // Streak badge on the avatar's chin. One day isn't a streak. A streak that
  // hasn't been fed today (weekdays only — weekends can't break it) inverts
  // to a dark "SAVE" pill: brew before midnight or lose it.
  if (p->streak >= 2) {
    bool at_risk = !p->brewed_today && is_weekday_today();
    char run[20];
    snprintf(run, sizeof(run), at_risk ? "SAVE DAY %d" : "DAY %d", p->streak);
    lv_label_set_text(s_streak_lbl, run);
    lv_obj_set_style_bg_color(s_streak_badge, at_risk ? COL_BG : COL_FLAME, 0);
    lv_obj_set_style_border_width(s_streak_badge, at_risk ? 1 : 0, 0);
    lv_obj_set_style_text_color(s_streak_lbl, at_risk ? COL_FLAME : COL_BG, 0);
    lv_obj_set_style_image_recolor(s_streak_flame, at_risk ? COL_FLAME : COL_BG,
                                   0);
    lv_obj_clear_flag(s_streak_badge, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_streak_badge, LV_OBJ_FLAG_HIDDEN);
  }

  char idx[16];
  snprintf(idx, sizeof(idx), "%d / %d", s_sel + 1, s_state.count);
  lv_label_set_text(s_list_index, idx);

  // Rim arc mirrors the rotation position (1-based so it never looks empty).
  if (s_state.count > 1) {
    lv_arc_set_range(s_list_arc, 1, s_state.count);
    lv_arc_set_value(s_list_arc, s_sel + 1);
    lv_obj_clear_flag(s_list_arc, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_list_arc, LV_OBJ_FLAG_HIDDEN);
  }

  update_avatar_view();
}

// Ask the background loader to fetch an avatar (once per person, unless its URL
// changes). No-op if already cached or in flight.
static void request_avatar(int index) {
  if (index < 0 || index >= s_state.count)
    return;
  if (s_av[index].state != AvState::Empty)
    return;
  if (!s_state.people[index].image_url[0])
    return;
  strncpy(s_av[index].url, s_state.people[index].image_url,
          sizeof(s_av[index].url) - 1);
  s_av[index].state = AvState::Requested;
  kaffi_request_avatar(s_state.people[index].image_url);
}

// Kick off background loads for everyone (the visible person first).
static void preload_avatars() {
  request_avatar(s_sel);
  for (int i = 0; i < s_state.count; i++)
    request_avatar(i);
}

// Re-map the avatar cache onto the (possibly reordered) people list by URL, so
// recency re-sorting never drops or re-fetches already-decoded photos.
static void reconcile_cache() {
  static AvatarSlot remapped[KAFFI_MAX_PEOPLE];
  bool used[KAFFI_MAX_PEOPLE] = {false};
  memset(remapped, 0, sizeof(remapped));

  for (int i = 0; i < s_state.count; i++) {
    const char *url = s_state.people[i].image_url;
    if (!url[0])
      continue;
    for (int j = 0; j < KAFFI_MAX_PEOPLE; j++) {
      if (!used[j] && s_av[j].state != AvState::Empty &&
          strcmp(s_av[j].url, url) == 0) {
        remapped[i] = s_av[j];
        used[j] = true;
        break;
      }
    }
  }
  // Free any decoded avatar that no longer maps to a current person.
  for (int j = 0; j < KAFFI_MAX_PEOPLE; j++) {
    if (used[j])
      continue;
    if (s_av[j].pixels)
      heap_caps_free(s_av[j].pixels);
    if (s_av[j].thumb_pixels)
      heap_caps_free(s_av[j].thumb_pixels);
  }
  memcpy(s_av, remapped, sizeof(s_av));
}

static void show_toast(const char *msg) {
  lv_label_set_text(s_toast, msg);
  lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_toast);

  if (s_toast_timer)
    lv_timer_delete(s_toast_timer);
  s_toast_timer = lv_timer_create(
      [](lv_timer_t *) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_timer = nullptr;
      },
      KAFFI_TOAST_MS, nullptr);
  lv_timer_set_repeat_count(s_toast_timer, 1);
}

// Post a coffee event for a specific person and reflect it optimistically.
static void log_action(const char *pid, const char *pname, int kind, int qty,
                       const char *toast) {
  KaffiLogCmd cmd = {};
  strncpy(cmd.person_id, pid, sizeof(cmd.person_id) - 1);
  strncpy(cmd.person_name, pname, sizeof(cmd.person_name) - 1);
  cmd.kind = kind;
  cmd.quantity = qty;
  esp_event_post(APP_EVENT, APP_EVENT_KAFFI_LOG, &cmd, sizeof(cmd), 0);
  haptic_buzz();
  for (int i = 0; i < s_state.count; i++) { // optimistic bump
    if (strcmp(s_state.people[i].id, pid) != 0)
      continue;
    Person *pe = &s_state.people[i];
    if (kind == KAFFI_BREW) {
      pe->pots += qty;
      pe->credit += kaffi_brew_credit(qty);
      if (!pe->brewed_today) { // first brew of the day extends the streak
        pe->brewed_today = true;
        pe->streak++;
      }
    } else {
      pe->cups += qty;
    }
    break;
  }

  set_mode(Mode::List);
  refresh_list();
  show_toast(toast);
}

static void do_log(int kind, int qty) {
  if (s_state.count == 0)
    return;
  const Person *p = &s_state.people[s_sel];

  // A little variety keeps the ritual fun (ASCII only — font has no glyphs
  // beyond it).
  static const char *cup_toasts[] = {"Enjoy!", "Cup logged", "Skol!",
                                     "Drink up!"};
  static const char *pot_toasts[] = {"What a hero!", "Fresh pot!",
                                     "Legend move", "Brewtiful!"};
  static uint32_t roll = 0;
  roll++;
  const char *toast;
  if (kind == KAFFI_BREW)
    toast = qty == 2 ? "Double hero!" : pot_toasts[roll % 4];
  else
    toast = cup_toasts[roll % 4];

  // First brew of the day extending a streak beats the regular quips — and
  // rescuing an at-risk streak gets its own cheer.
  static char streak_toast[24];
  if (kind == KAFFI_BREW && !p->brewed_today && p->streak + 1 >= 2) {
    bool saved = p->streak >= 2 && is_weekday_today();
    snprintf(streak_toast, sizeof(streak_toast),
             saved ? "Saved! Day %d" : "Day %d streak!", p->streak + 1);
    toast = streak_toast;
  }
  log_action(p->id, p->name, kind, qty, toast);
}

// ─── Leaderboard

static void build_leaderboard() {
  lv_obj_clean(s_lb_list);

  // People with no entries this year would just pad the middle with +0 rows.
  int order[KAFFI_MAX_PEOPLE];
  int n = 0;
  for (int i = 0; i < s_state.count; i++) {
    const Person *p = &s_state.people[i];
    if (p->pots > 0 || p->cups > 0)
      order[n++] = i;
  }

  if (n == 0) {
    lv_obj_t *l = lv_label_create(s_lb_list);
    lv_label_set_text(l, "Nothing brewed yet");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, COL_SUB, 0);
    lv_obj_t *sub = lv_label_create(s_lb_list);
    lv_label_set_text(sub, "be the first hero");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sub, COL_SUB, 0);
    return;
  }

  for (int i = 1; i < n; i++) { // insertion sort, balance desc
    int key = order[i], j = i - 1;
    while (j >= 0 && balance_of(&s_state.people[order[j]]) <
                         balance_of(&s_state.people[key])) {
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = key;
  }

  for (int rank = 0; rank < n; rank++) {
    int idx = order[rank];
    const Person *p = &s_state.people[idx];
    int b = balance_of(p);
    bool worst = (rank == n - 1) && (b < 0);
    bool podium = !worst && rank < 3;
    lv_color_t medal = (rank == 0)   ? COL_GOLD
                       : (rank == 1) ? COL_SILVER
                                     : COL_BRONZE;

    lv_obj_t *row = lv_obj_create(s_lb_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 248, 54);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_bg_color(row, worst ? COL_BAD : COL_BACK, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(row, 8, 0);
    lv_obj_set_style_pad_right(row, 14, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // Medal chip: filled circle for the podium, quiet number for the rest.
    lv_obj_t *chip = lv_obj_create(row);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, 24, 24);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    if (podium) {
      lv_obj_set_style_bg_color(chip, medal, 0);
      lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    }
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    char rankstr[8];
    snprintf(rankstr, sizeof(rankstr), "%d", rank + 1);
    lv_obj_t *lrank = lv_label_create(chip);
    lv_label_set_text(lrank, rankstr);
    lv_obj_set_style_text_font(lrank, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(
        lrank, podium ? COL_BG : (worst ? COL_TEXT : COL_SUB), 0);
    lv_obj_center(lrank);

    // Avatar thumbnail (pre-scaled, so no per-frame transform) or an initial
    // chip. Podium thumbs get a medal ring.
    lv_obj_t *thumb = lv_obj_create(row);
    lv_obj_remove_style_all(thumb);
    lv_obj_set_size(thumb, THUMB_SIZE, THUMB_SIZE);
    lv_obj_set_style_radius(thumb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(thumb, true, 0);
    lv_obj_set_style_bg_color(thumb, COL_BREW, 0);
    lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);
    if (podium) {
      lv_obj_set_style_border_width(thumb, 2, 0);
      lv_obj_set_style_border_color(thumb, medal, 0);
    }
    if (s_av[idx].state == AvState::Loaded && s_av[idx].thumb_pixels) {
      lv_obj_t *im = lv_image_create(thumb);
      lv_image_set_src(im, &s_av[idx].thumb_dsc);
      lv_obj_center(im);
    } else {
      char init[2] = {static_cast<char>(p->name[0] ? toupper(p->name[0]) : '?'),
                      '\0'};
      lv_obj_t *in = lv_label_create(thumb);
      lv_label_set_text(in, init);
      lv_obj_set_style_text_font(in, &lv_font_montserrat_20, 0);
      lv_obj_set_style_text_color(in, COL_TEXT, 0);
      lv_obj_center(in);
    }

    // Name over a small pots/cups detail line; both clip before the value.
    lv_obj_t *cell = lv_obj_create(row);
    lv_obj_remove_style_all(cell);
    lv_obj_set_flex_grow(cell, 1);
    lv_obj_set_height(cell, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    // Fixed one-line heights: without them long names wrap and blow the row
    // open instead of ellipsizing.
    lv_obj_t *lname = lv_label_create(cell);
    lv_label_set_text(lname, p->name);
    lv_label_set_long_mode(lname, LV_LABEL_LONG_DOT);
    lv_obj_set_size(lname, LV_PCT(100), 24);
    lv_obj_set_style_text_font(lname, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lname, COL_TEXT, 0);

    char detail[32];
    snprintf(detail, sizeof(detail), "%d pots  %d cups", p->pots, p->cups);
    lv_obj_t *ldetail = lv_label_create(cell);
    lv_label_set_text(ldetail, detail);
    lv_label_set_long_mode(ldetail, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(ldetail, LV_PCT(100), 16);
    lv_obj_set_style_text_font(ldetail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ldetail, worst ? COL_TEXT : COL_SUB, 0);
    lv_obj_set_style_text_opa(ldetail, worst ? LV_OPA_80 : LV_OPA_COVER, 0);

    // Signed balance; the red row already marks the freeloader.
    char right[16];
    snprintf(right, sizeof(right), "%+d", b);
    lv_obj_t *lbal = lv_label_create(row);
    lv_label_set_text(lbal, right);
    lv_obj_set_style_text_font(lbal, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(
        lbal, worst ? COL_TEXT : (b >= 0 ? COL_GOOD : COL_BAD), 0);
  }
}

// Step the highlight through the list (radio speaker-picker style): accent the
// active row and scroll it into view. No free-scrolling. Only the two affected
// rows are restyled so each detent invalidates the minimum screen area.
static int s_lb_prev_highlight = -1;
static void highlight_lb_item(int h) {
  if (!s_lb_list || s_state.count == 0)
    return;
  int n = lv_obj_get_child_count(s_lb_list);
  if (s_lb_prev_highlight >= 0 && s_lb_prev_highlight < n &&
      s_lb_prev_highlight != h) {
    lv_obj_set_style_outline_width(
        lv_obj_get_child(s_lb_list, s_lb_prev_highlight), 0, 0);
  }
  if (h >= 0 && h < n) {
    lv_obj_t *row = lv_obj_get_child(s_lb_list, h);
    // Outline draws outside the box, so toggling it never reflows the row.
    lv_obj_set_style_outline_color(row, COL_CREAM, 0);
    lv_obj_set_style_outline_width(row, 3, 0);
    lv_obj_set_style_outline_pad(row, 0, 0);
    lv_obj_scroll_to_view(row, LV_ANIM_ON);
  }
  s_lb_prev_highlight = h;
}

static void open_leaderboard() {
  s_lb_highlight = 0;
  s_lb_prev_highlight = -1; // rows were just rebuilt
  build_leaderboard();
  highlight_lb_item(s_lb_highlight);
  s_lb_last_activity = esp_timer_get_time();
  set_mode(Mode::Leaderboard);
}

// Open the action sheet for the current selection with their name + photo.
static void open_action() {
  const Person *p = &s_state.people[s_sel];
  lv_label_set_text(s_action_name, p->name);
  char init[2] = {static_cast<char>(p->name[0] ? toupper(p->name[0]) : '?'),
                  '\0'};
  lv_label_set_text(s_action_init, init);
  if (s_av[s_sel].state == AvState::Loaded && s_av[s_sel].thumb_pixels) {
    lv_image_set_src(s_action_avatar, &s_av[s_sel].thumb_dsc);
    lv_obj_clear_flag(s_action_avatar, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_action_avatar, LV_OBJ_FLAG_HIDDEN);
  }
  set_mode(Mode::Action);
}

// ─── Encoder + inactivity tick (runs on the UI task)

static void tick_cb(lv_timer_t *) {
  int64_t now = esp_timer_get_time();
  int32_t steps = encoder_take_steps();
  static int s_enc_accum = 0; // de-sensitize: require N detents per move

  if (s_mode == Mode::List) {
    if (steps != 0 && s_state.count > 0) {
      s_enc_accum += static_cast<int>(steps) * 2; // detents -> half-detents
      int moves = s_enc_accum / KAFFI_ENCODER_HALFSTEPS_PER_ITEM;
      if (moves != 0) {
        s_enc_accum -= moves * KAFFI_ENCODER_HALFSTEPS_PER_ITEM;
        s_sel =
            ((s_sel + moves) % s_state.count + s_state.count) % s_state.count;
        refresh_list();        // instant: avatar is already cached if loaded
        request_avatar(s_sel); // otherwise pull it now
        avatar_nudge(moves);
      }
    }
  } else if (s_mode == Mode::Leaderboard) {
    if (steps != 0 && s_state.count > 0) {
      s_lb_highlight += static_cast<int>(steps);
      if (s_lb_highlight < 0)
        s_lb_highlight = 0;
      if (s_lb_highlight >= s_state.count)
        s_lb_highlight = s_state.count - 1;
      highlight_lb_item(s_lb_highlight);
      s_lb_last_activity = now;
    }
    if (now - s_lb_last_activity >
        static_cast<int64_t>(KAFFI_LEADERBOARD_TIMEOUT_MS) * 1000) {
      set_mode(Mode::List);
      refresh_list();
    }
  }
}

// ─── View builders

static lv_obj_t *make_view() {
  lv_obj_t *v = lv_obj_create(s_screen);
  lv_obj_remove_style_all(v);
  lv_obj_set_size(v, 360, 360);
  lv_obj_center(v);
  lv_obj_clear_flag(v, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
  return v;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            lv_color_t color, int dy) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(l, 320);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, dy);
  return l;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_color_t col,
                             int w, int h, int dy, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_style_radius(btn, h / 2, 0);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_set_style_shadow_width(btn, 0, 0);
  lv_obj_set_style_bg_color(btn, col, 0);
  lv_obj_set_style_bg_color(btn, lv_color_darken(col, LV_OPA_30),
                            LV_STATE_PRESSED);

  // Smooth bg fade on press/release.
  static lv_style_transition_dsc_t tr;
  static bool tr_init = false;
  if (!tr_init) {
    static const lv_style_prop_t props[] = {LV_STYLE_BG_COLOR,
                                            LV_STYLE_PROP_INV};
    lv_style_transition_dsc_init(&tr, props, lv_anim_path_ease_out, 130, 0,
                                 nullptr);
    tr_init = true;
  }
  lv_obj_set_style_transition(btn, &tr, 0);

  lv_obj_align(btn, LV_ALIGN_CENTER, 0, dy);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *l = lv_label_create(btn);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(l, COL_TEXT, 0);
  lv_obj_center(l);
  return btn;
}

static void build_status() {
  s_status_view = make_view();
  s_status_lbl =
      make_label(s_status_view, &lv_font_montserrat_28, COL_CREAM, 0);
  lv_label_set_text(s_status_lbl, "kaffi");
}

static void build_avatar(lv_obj_t *parent) {
  s_avatar_box = lv_obj_create(parent);
  lv_obj_remove_style_all(s_avatar_box);
  lv_obj_set_size(s_avatar_box, 96, 96);
  lv_obj_set_style_radius(s_avatar_box, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_clip_corner(s_avatar_box, true, 0);
  lv_obj_set_style_bg_color(s_avatar_box, COL_BREW, 0);
  lv_obj_set_style_bg_opa(s_avatar_box, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_avatar_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(s_avatar_box, LV_ALIGN_CENTER, 0, -88);
  // Balance ring — outline draws outside the photo, recolored per person.
  lv_obj_set_style_outline_width(s_avatar_box, 3, 0);
  lv_obj_set_style_outline_pad(s_avatar_box, 3, 0);
  lv_obj_set_style_outline_color(s_avatar_box, COL_BACK, 0);

  s_avatar_init = lv_label_create(s_avatar_box);
  lv_obj_set_style_text_font(s_avatar_init, &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_color(s_avatar_init, COL_TEXT, 0);
  lv_label_set_text(s_avatar_init, "?");
  lv_obj_center(s_avatar_init);

  s_avatar_img = lv_image_create(s_avatar_box);
  lv_obj_center(s_avatar_img);
  lv_obj_add_flag(s_avatar_img, LV_OBJ_FLAG_HIDDEN);
}

static void build_list() {
  s_list_view = make_view();
  lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_CLICKABLE);

  // Thin progress ring hugging the bezel: fills clockwise from the top as the
  // encoder moves through the list. Not clickable, so taps fall through to the
  // view's tap-to-open-action.
  s_list_arc = lv_arc_create(s_list_view);
  lv_obj_set_size(s_list_arc, 348, 348);
  lv_obj_center(s_list_arc);
  lv_arc_set_rotation(s_list_arc, 270);
  lv_arc_set_bg_angles(s_list_arc, 0, 360);
  lv_obj_set_style_arc_width(s_list_arc, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_list_arc, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_list_arc, COL_BACK, LV_PART_MAIN);
  lv_obj_set_style_arc_color(s_list_arc, COL_CREAM, LV_PART_INDICATOR);
  lv_obj_remove_style(s_list_arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(s_list_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      s_list_view,
      [](lv_event_t *) {
        if (s_state.count == 0)
          return;
        open_action();
      },
      LV_EVENT_SHORT_CLICKED, nullptr);
  lv_obj_add_event_cb(
      s_list_view, [](lv_event_t *) { open_leaderboard(); },
      LV_EVENT_LONG_PRESSED, nullptr);

  build_avatar(s_list_view);

  // Streak pill straddling the avatar's bottom edge (box is 96px at -88, so
  // its edge sits at -40). Built after the avatar to draw above it.
  s_streak_badge = lv_obj_create(s_list_view);
  lv_obj_remove_style_all(s_streak_badge);
  lv_obj_set_size(s_streak_badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(s_streak_badge, COL_FLAME, 0);
  lv_obj_set_style_bg_opa(s_streak_badge, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_streak_badge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(s_streak_badge, 9, 0);
  lv_obj_set_style_pad_ver(s_streak_badge, 2, 0);
  lv_obj_set_flex_flow(s_streak_badge, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_streak_badge, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(s_streak_badge, 3, 0);
  lv_obj_align(s_streak_badge, LV_ALIGN_CENTER, 0, -40);
  lv_obj_clear_flag(s_streak_badge, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(s_streak_badge, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_streak_badge, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_border_color(s_streak_badge, COL_FLAME, 0);
  s_streak_flame = lv_image_create(s_streak_badge);
  lv_image_set_src(s_streak_flame, &img_flame);
  lv_obj_set_style_image_recolor(s_streak_flame, COL_BG, 0);
  lv_obj_set_style_image_recolor_opa(s_streak_flame, LV_OPA_COVER, 0);
  s_streak_lbl = lv_label_create(s_streak_badge);
  lv_obj_set_style_text_font(s_streak_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_streak_lbl, COL_BG, 0);

  s_list_name = make_label(s_list_view, &lv_font_montserrat_28, COL_TEXT, -12);
  s_list_stats = make_label(s_list_view, &lv_font_montserrat_20, COL_SUB, 24);
  s_list_balance =
      make_label(s_list_view, &lv_font_montserrat_20, COL_CREAM, 58);
  s_list_index = make_label(s_list_view, &lv_font_montserrat_14, COL_SUB, 92);
  lv_obj_t *hint =
      make_label(s_list_view, &lv_font_montserrat_14, COL_SUB, 128);
  lv_label_set_text(hint, "hold for stats");
  lv_obj_set_style_text_opa(hint, LV_OPA_60, 0);

  // Network pill at the top of the circle. Hidden while online with nothing
  // queued, so the screen looks exactly as before in the happy case.
  s_net_pill = lv_obj_create(s_list_view);
  lv_obj_remove_style_all(s_net_pill);
  lv_obj_set_size(s_net_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(s_net_pill, COL_BAD, 0);
  lv_obj_set_style_bg_opa(s_net_pill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_net_pill, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_pad_hor(s_net_pill, 10, 0);
  lv_obj_set_style_pad_ver(s_net_pill, 3, 0);
  lv_obj_align(s_net_pill, LV_ALIGN_CENTER, 0, -148);
  lv_obj_clear_flag(s_net_pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(s_net_pill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_net_pill, LV_OBJ_FLAG_HIDDEN);
  s_net_lbl = lv_label_create(s_net_pill);
  lv_obj_set_style_text_font(s_net_lbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_net_lbl, COL_BG, 0);
  lv_obj_center(s_net_lbl);
}

static void build_action() {
  s_action_view = make_view();

  // Small avatar so there's no doubt who the log lands on.
  lv_obj_t *thumb = lv_obj_create(s_action_view);
  lv_obj_remove_style_all(thumb);
  lv_obj_set_size(thumb, THUMB_SIZE, THUMB_SIZE);
  lv_obj_set_style_radius(thumb, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_clip_corner(thumb, true, 0);
  lv_obj_set_style_bg_color(thumb, COL_BREW, 0);
  lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
  lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(thumb, LV_ALIGN_CENTER, 0, -136);

  s_action_init = lv_label_create(thumb);
  lv_obj_set_style_text_font(s_action_init, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(s_action_init, COL_TEXT, 0);
  lv_label_set_text(s_action_init, "?");
  lv_obj_center(s_action_init);

  s_action_avatar = lv_image_create(thumb);
  lv_obj_center(s_action_avatar);
  lv_obj_add_flag(s_action_avatar, LV_OBJ_FLAG_HIDDEN);

  s_action_name =
      make_label(s_action_view, &lv_font_montserrat_20, COL_TEXT, -102);
  lv_obj_set_width(s_action_name, 240); // top of the circle is narrow
  // "Took a cup" first — it's the most common action.
  make_button(s_action_view, "Took a cup", COL_CUP, 264, 66, -34,
              [](lv_event_t *) { do_log(KAFFI_CUP, 1); });
  make_button(s_action_view, "Brewed a pot", COL_BREW, 264, 66, 44,
              [](lv_event_t *) { set_mode(Mode::Pots); });
  make_button(s_action_view, "Back", COL_BACK, 140, 48, 118,
              [](lv_event_t *) { set_mode(Mode::List); });
}

static void build_pots() {
  s_pots_view = make_view();
  lv_obj_t *q = make_label(s_pots_view, &lv_font_montserrat_28, COL_TEXT, -112);
  lv_label_set_text(q, "How many pots?");
  make_button(s_pots_view, "1 pot", COL_BREW, 264, 66, -34,
              [](lv_event_t *) { do_log(KAFFI_BREW, 1); });
  make_button(s_pots_view, "2 pots", COL_BREW, 264, 66, 44,
              [](lv_event_t *) { do_log(KAFFI_BREW, 2); });
  make_button(s_pots_view, "Back", COL_BACK, 140, 48, 118,
              [](lv_event_t *) { set_mode(Mode::Action); });
}

static void build_leaderboard_view() {
  s_lb_view = make_view();
  lv_obj_add_flag(s_lb_view, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(
      s_lb_view,
      [](lv_event_t *) {
        // tap to exit
        set_mode(Mode::List);
        refresh_list();
      },
      LV_EVENT_SHORT_CLICKED, nullptr);

  lv_obj_t *overline = lv_label_create(s_lb_view);
  lv_label_set_text(overline, "COFFEE HEROES");
  lv_obj_set_style_text_font(overline, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(overline, COL_GOLD, 0);
  lv_obj_align(overline, LV_ALIGN_TOP_MID, 0, 34);

  lv_obj_t *title = lv_label_create(s_lb_view);
  lv_label_set_text(title, "This year");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(title, COL_CREAM, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 54);

  s_lb_list = lv_obj_create(s_lb_view);
  lv_obj_remove_style_all(s_lb_list);
  // Runs from below the header to the bottom of the screen, so rows scroll
  // off the display edge instead of vanishing at a hard cutoff line. The
  // large bottom padding lets the last row scroll up clear of the curved
  // bottom corners.
  lv_obj_set_size(s_lb_list, 276, 254);
  lv_obj_align(s_lb_list, LV_ALIGN_TOP_MID, 0, 106);
  lv_obj_set_flex_flow(s_lb_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_lb_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_lb_list, 8, 0);
  lv_obj_set_style_pad_top(s_lb_list, 8, 0);
  lv_obj_set_style_pad_bottom(s_lb_list, 72, 0);
  lv_obj_set_scroll_dir(s_lb_list, LV_DIR_VER);
  lv_obj_add_event_cb(
      s_lb_list,
      [](lv_event_t *) { s_lb_last_activity = esp_timer_get_time(); },
      LV_EVENT_SCROLL_BEGIN, nullptr);
}

static void build_toast() {
  s_toast = lv_label_create(lv_layer_top());
  lv_obj_set_style_bg_color(s_toast, COL_BREW, 0);
  lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(s_toast, 24, 0);
  lv_obj_set_style_pad_all(s_toast, 16, 0);
  lv_obj_set_style_text_font(s_toast, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(s_toast, COL_TEXT, 0);
  lv_obj_align(s_toast, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
}

// ─── Public API

void ui_init() {
  lv_display_t *disp = nullptr;
  lv_indev_t *touch = nullptr;
  display_init(&disp, &touch);

  if (display_lock(1000)) {
    s_screen = lv_screen_active();
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    build_status();
    build_list();
    build_action();
    build_pots();
    build_leaderboard_view();
    build_toast();

    set_mode(Mode::Status);
    display_set_backlight(KAFFI_BACKLIGHT_NORMAL); // auto-dimming disabled
    lv_timer_create(tick_cb, KAFFI_ENCODER_POLL_MS, nullptr);

    display_unlock();
  }
  ESP_LOGI(TAG, "UI initialized");
}

void ui_show_splash() {
  if (!display_lock(200))
    return;
  lv_label_set_text(s_status_lbl, "kaffi");
  set_mode(Mode::Status);
  display_unlock();
}

// ─── First-boot setup screens (called from app_main, before the main flow)

void ui_show_wifi_setup(const char *ap_name) {
  if (!display_lock(1000))
    return;

  lv_obj_t *view = lv_obj_create(s_screen);
  lv_obj_remove_style_all(view);
  lv_obj_set_size(view, 360, 360);
  lv_obj_center(view);
  lv_obj_set_style_bg_color(view, COL_BG, 0);
  lv_obj_set_style_bg_opa(view, LV_OPA_COVER, 0);
  lv_obj_clear_flag(view, LV_OBJ_FLAG_SCROLLABLE);

  char qr_data[80];
  snprintf(qr_data, sizeof(qr_data), "WIFI:T:nopass;S:%s;;", ap_name);
  lv_obj_t *qr = lv_qrcode_create(view);
  lv_qrcode_set_size(qr, 150);
  lv_qrcode_set_dark_color(qr, COL_TEXT);
  lv_qrcode_set_light_color(qr, COL_BG);
  lv_qrcode_update(qr, qr_data, strlen(qr_data));
  lv_obj_align(qr, LV_ALIGN_CENTER, 0, -40);

  lv_obj_t *msg = make_label(view, &lv_font_montserrat_20, COL_CREAM, 80);
  char text[96];
  snprintf(text, sizeof(text), "Scan to join \"%s\"\nthen set up WiFi",
           ap_name);
  lv_label_set_text(msg, text);

  display_unlock();
}

static SemaphoreHandle_t s_office_sem = nullptr;
static int s_office_choice = -1;
static lv_obj_t *s_office_view = nullptr;

int ui_pick_office() {
  s_office_sem = xSemaphoreCreateBinary();
  if (!s_office_sem)
    return 0;

  if (display_lock(1000)) {
    s_office_view = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_office_view);
    lv_obj_set_size(s_office_view, 360, 360);
    lv_obj_center(s_office_view);
    lv_obj_set_style_bg_color(s_office_view, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_office_view, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_office_view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title =
        make_label(s_office_view, &lv_font_montserrat_28, COL_TEXT, -110);
    lv_label_set_text(title, "Which office?");

    // A few offices fit the round screen as stacked buttons; revisit the
    // layout before this outgrows it.
    for (int i = 0; i < KAFFI_OFFICE_COUNT; i++) {
      lv_obj_t *btn = make_button(
          s_office_view, KAFFI_OFFICES[i].name, COL_BREW, 264, 66, -30 + i * 80,
          [](lv_event_t *e) {
            auto *b = static_cast<lv_obj_t *>(lv_event_get_target(e));
            s_office_choice = static_cast<int>(
                reinterpret_cast<intptr_t>(lv_obj_get_user_data(b)));
            haptic_buzz();
            xSemaphoreGive(s_office_sem);
          });
      lv_obj_set_user_data(btn,
                           reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
    display_unlock();
  }

  xSemaphoreTake(s_office_sem, portMAX_DELAY);

  if (display_lock(1000)) {
    lv_obj_delete(s_office_view);
    s_office_view = nullptr;
    display_unlock();
  }
  vSemaphoreDelete(s_office_sem);
  s_office_sem = nullptr;
  return s_office_choice < 0 ? 0 : s_office_choice;
}

void ui_set_status(const char *msg) {
  if (!display_lock(200))
    return;
  lv_label_set_text(s_status_lbl, msg);
  set_mode(Mode::Status);
  display_unlock();
}

void ui_set_offline(bool offline, int pending) {
  if (!s_net_pill || !display_lock(200))
    return;
  if (!offline && pending <= 0) {
    lv_obj_add_flag(s_net_pill, LV_OBJ_FLAG_HIDDEN);
  } else {
    char buf[40];
    if (offline && pending > 0)
      snprintf(buf, sizeof(buf), "OFFLINE, %d PENDING", pending);
    else if (offline)
      snprintf(buf, sizeof(buf), "OFFLINE");
    else
      snprintf(buf, sizeof(buf), "%d PENDING", pending);
    lv_label_set_text(s_net_lbl, buf);
    // Red while the network is down; muted once it's back and just draining.
    lv_obj_set_style_bg_color(s_net_pill, offline ? COL_BAD : COL_SUB, 0);
    lv_obj_clear_flag(s_net_pill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_net_pill);
  }
  display_unlock();
}

void ui_update_people(const KaffiState *state) {
  if (!state)
    return;
  if (!display_lock(200))
    return;

  // Keep the cursor on the same person across refreshes: the order is stable
  // (alphabetical), but a joiner/leaver can still shift indices.
  char keep_id[KAFFI_ID_LEN] = {};
  if (s_state.count > 0 && s_sel < s_state.count)
    strncpy(keep_id, s_state.people[s_sel].id, sizeof(keep_id) - 1);

  memcpy(&s_state, state, sizeof(s_state));

  s_sel = 0;
  if (keep_id[0])
    for (int i = 0; i < s_state.count; i++)
      if (strcmp(s_state.people[i].id, keep_id) == 0) {
        s_sel = i;
        break;
      }
  reconcile_cache();
  refresh_list();
  preload_avatars();

  if (s_mode == Mode::Status && s_state.count > 0) {
    set_mode(Mode::List);
  }
  display_unlock();
}

void ui_set_avatar(const char *url, unsigned char *pixels, int w, int h) {
  if (!display_lock(200)) {
    if (pixels)
      heap_caps_free(pixels);
    return;
  }

  // Match the result to a person by URL (robust to list reordering).
  int idx = -1;
  if (url)
    for (int i = 0; i < s_state.count; i++)
      if (strcmp(s_state.people[i].image_url, url) == 0) {
        idx = i;
        break;
      }

  if (idx < 0) { // person no longer in the list
    if (pixels)
      heap_caps_free(pixels);
    display_unlock();
    return;
  }
  if (!pixels) { // fetch/decode failed — allow a retry on the next refresh
    s_av[idx].state = AvState::Empty;
    display_unlock();
    return;
  }

  AvatarSlot *a = &s_av[idx];
  if (a->pixels)
    heap_caps_free(a->pixels);
  if (a->thumb_pixels)
    heap_caps_free(a->thumb_pixels);
  a->pixels = pixels;
  a->state = AvState::Loaded;
  a->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  a->dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
  a->dsc.header.w = static_cast<uint32_t>(w);
  a->dsc.header.h = static_cast<uint32_t>(h);
  a->dsc.header.stride = static_cast<uint32_t>(w * 2);
  a->dsc.data_size = static_cast<uint32_t>(w * h * 2);
  a->dsc.data = a->pixels;
  // Pre-scaled once via the shared box-average — scaled lv_image transforms
  // run through LVGL's slow per-pixel CPU path on every frame.
  a->thumb_pixels = art_scale_rgb565(pixels, w, h, THUMB_SIZE, THUMB_SIZE);
  if (a->thumb_pixels) {
    a->thumb_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    a->thumb_dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
    a->thumb_dsc.header.w = THUMB_SIZE;
    a->thumb_dsc.header.h = THUMB_SIZE;
    a->thumb_dsc.header.stride = THUMB_SIZE * 2;
    a->thumb_dsc.data_size = THUMB_SIZE * THUMB_SIZE * 2;
    a->thumb_dsc.data = a->thumb_pixels;
  }
  strncpy(a->url, url, sizeof(a->url) - 1);

  if (idx == s_sel)
    update_avatar_view();
  display_unlock();
}
