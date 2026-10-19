#include "ui/ui.h"
#include "activity.h"
#include "app_config.h"
#include "backdrop.h"
#include "caption.h"
#include "carousel.h"
#include "display.h"
#include "encoder.h"
#include "fonts.h"
#include "hal_pins.h"
#include "haptic.h"
#include "primitives.h"
#include "sheet.h"
#include "sheet_list.h"
#include "theme.h"
#include "ui/avatars.h"
#include "ui/standing.h"
#include "volume_ring.h"

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#include "lvgl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

ESP_EVENT_DECLARE_BASE(APP_EVENT);

// ─── How the knob works
//
//  People         turn → the next person glides in (wraps)
//                 tap or swipe up → the log sheet, for them
//                 hold or swipe down → the leaderboard sheet
//                 swipe sideways → one person over
//  Log sheet      turn → focus a row; tap a row → log it and close
//  Leaderboard    turn → move through it
//  A sheet nobody touches closes by itself.
//
// The people screen is the radio's browse view, with people for stations:
// the same tile strip, title, subtitle and position line, the same ring round
// the bezel, and the same pill at the top.

namespace {

constexpr const char *TAG = "ui";

constexpr int32_t RING_MAX = 1000;
// The screen takes a tile's color once the strip has rested on it this long:
// a fade on every detent of a spin would repaint the whole screen each time.
constexpr uint32_t TINT_SETTLE_MS = 300;
constexpr uint32_t IDLE_CHECK_MS = 1000;

// The podium's medals, after Apple's yellow, silver and a bronze to match.
constexpr uint32_t COL_SILVER = 0xC7C7CC;
constexpr uint32_t COL_BRONZE = 0xAC8E68;

enum class Mode { Status, Office, People };

EXT_RAM_BSS_ATTR KaffiState s_state;
int s_sel;   // where the strip is headed: who a tap logs for
int s_shown; // the tile nearest the middle: whose name shows
Mode s_mode = Mode::Status;
bool s_offline;
int s_pending;
char s_toast[32];
lv_timer_t *s_toast_timer;
int s_enc_halfsteps;
bool s_swiped; // this touch already moved the strip
lv_timer_t *s_tint_timer;

lv_obj_t *s_screen;
Caption s_caption;
IconPill s_pill;
lv_obj_t *s_status;
lv_obj_t *s_status_label;

lv_obj_t *s_log_sheet;
lv_obj_t *s_log_content;
lv_obj_t *s_log_list;
lv_obj_t *s_board_sheet;
lv_obj_t *s_board_content;
lv_obj_t *s_board_list;
int s_focus;

void (*s_office_picked)(int);

int balance_of(const Person &p) { return p.credit - p.cups; }

bool have_people() { return s_mode == Mode::People && s_state.count > 0; }

const Person *person(int index) {
  return index >= 0 && index < s_state.count ? &s_state.people[index] : nullptr;
}

lv_obj_t *open_sheet() {
  if (s_log_sheet && sheet_is_visible(s_log_sheet))
    return s_log_sheet;
  if (s_board_sheet && sheet_is_visible(s_board_sheet))
    return s_board_sheet;
  return nullptr;
}

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, int y) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(l, "");
  lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
  return l;
}

// ─── The pill at the top: a toast just after logging, else the network's
// state, else the person's streak.

// The usual white veil, or filled with the color for a streak that needs
// saving today.
void style_pill(const char *icon, uint32_t color, lv_opa_t opa,
                bool filled = false) {
  ui_icon_pill_set_icon(s_pill, icon, 0);
  ui_pill_tint(s_pill, filled ? lv_color_black() : lv_color_hex(color), opa);
  lv_obj_set_style_bg_color(s_pill.pill,
                            filled ? lv_color_hex(color) : lv_color_white(),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_pill.pill, filled ? LV_OPA_COVER : PILL_OPA,
                          LV_PART_MAIN);
}

// Whether the pill follows the strip, fading with the name.
bool pill_is_streak() {
  return s_mode == Mode::People && !s_toast[0] && !s_offline && s_pending <= 0;
}

void update_pill() {
  char text[40] = "";
  if (s_mode == Mode::Office) {
    style_pill(ICON_COFFEE, 0xFFFFFF, TEXT_OPA_SOFT);
    snprintf(text, sizeof(text), "Which office?");
  } else if (s_toast[0]) {
    style_pill(ICON_CHECK, COL_GREEN, LV_OPA_COVER);
    snprintf(text, sizeof(text), "%s", s_toast);
  } else if (s_offline) {
    style_pill(ICON_WIFI_OFF, COL_RED, LV_OPA_COVER);
    if (s_pending > 0)
      snprintf(text, sizeof(text), "Offline, %d waiting", s_pending);
    else
      snprintf(text, sizeof(text), "Offline");
  } else if (s_pending > 0) {
    style_pill(ICON_UPLOAD, 0xFFFFFF, TEXT_OPA_SOFT);
    snprintf(text, sizeof(text), "%d waiting", s_pending);
  } else if (const Person *p = person(s_shown)) {
    Streak streak = streak_of(*p, is_weekday_today());
    if (streak.at_risk)
      style_pill(ICON_FLAME, COL_ORANGE, LV_OPA_COVER, true);
    else
      style_pill(ICON_FLAME, COL_ORANGE, LV_OPA_COVER);
    streak_text(streak, text, sizeof(text));
  }
  ui_icon_pill_set_text(s_pill, text);
  lv_obj_set_flag(s_pill.pill, LV_OBJ_FLAG_HIDDEN,
                  !text[0] || s_mode == Mode::Status);
}

void end_toast(lv_timer_t *) {
  s_toast_timer = nullptr;
  s_toast[0] = '\0';
  update_pill();
}

void show_toast(const char *text) {
  snprintf(s_toast, sizeof(s_toast), "%s", text);
  if (s_toast_timer)
    lv_timer_delete(s_toast_timer);
  s_toast_timer = lv_timer_create(end_toast, KAFFI_TOAST_MS, nullptr);
  lv_timer_set_repeat_count(s_toast_timer, 1);
  lv_obj_set_style_opa(s_pill.pill, LV_OPA_COVER, LV_PART_MAIN);
  update_pill();
}

// ─── The strip: offices on first boot, then people

int strip_count() {
  if (s_mode == Mode::Office)
    return KAFFI_OFFICE_COUNT;
  return s_mode == Mode::People ? s_state.count : 0;
}

void show_ring() {
  volume_ring_set_level(RING_MAX * (s_shown + 1) / std::max(strip_count(), 1));
}

void set_subtitle(const char *text, uint32_t color, lv_opa_t opa) {
  lv_label_set_text(s_caption.subtitle, text);
  lv_obj_set_style_text_color(s_caption.subtitle, lv_color_hex(color),
                              LV_PART_MAIN);
  lv_obj_set_style_text_opa(s_caption.subtitle, opa, LV_PART_MAIN);
}

void show_person() {
  const Person *p = person(s_shown);
  if (!p)
    return;
  ui_marquee_text(s_caption.title, p->name);
  char line[48];
  int balance = balance_of(*p);
  standing_text(balance, line, sizeof(line));
  if (balance == 0)
    set_subtitle(line, 0xFFFFFF, TEXT_OPA_SECONDARY);
  else
    set_subtitle(line, balance > 0 ? COL_GREEN : COL_RED, LV_OPA_COVER);
  stats_text(*p, line, sizeof(line));
  lv_label_set_text(s_caption.detail, line);
  show_ring();
  update_pill();
}

// Like a station in the radio's browse: name, what a tap does, and where.
void show_office() {
  ui_marquee_text(s_caption.title, KAFFI_OFFICES[s_shown].name);
  set_subtitle("Tap to choose", 0xFFFFFF, TEXT_OPA_SECONDARY);
  char position[16];
  snprintf(position, sizeof(position), "%d / %d", s_shown + 1,
           KAFFI_OFFICE_COUNT);
  lv_label_set_text(s_caption.detail, position);
  show_ring();
  update_pill();
}

void show_item() {
  if (s_mode == Mode::Office)
    show_office();
  else
    show_person();
}

lv_color_t sky_of(int item) {
  if (s_mode == Mode::Office)
    return lv_color_hex(KAFFI_OFFICES[item].sky);
  return avatar_tint(item);
}

CarouselTile strip_tile(int item) {
  if (s_mode == Mode::Office) {
    const KaffiOffice &o = KAFFI_OFFICES[item];
    return {nullptr, lv_color_hex(o.tile), o.code, false};
  }
  avatars_request(s_state, item);
  if (const lv_image_dsc_t *photo = avatar_photo(item))
    return {photo, avatar_tint(item), nullptr, true};
  static char initial[8];
  const Person *p = person(item);
  initial_of(p ? p->name : "", initial, sizeof(initial));
  return {nullptr, lv_color_hex(COL_PLACEHOLDER), initial, true};
}

void on_tint_settled(lv_timer_t *t) {
  lv_timer_pause(t);
  if (strip_count() > 0)
    backdrop_set(sky_of(s_shown), true);
}

// The name follows the strip like a station's: it fades as a tile leaves the
// middle and changes at the halfway point, when it's gone.
void strip_moved(int item, int32_t offset) {
  if (item != s_shown) {
    s_shown = item;
    show_item();
  }
  lv_opa_t opa = ui_caption_fade(offset);
  lv_obj_set_style_opa(s_caption.title, opa, LV_PART_MAIN);
  if (pill_is_streak())
    lv_obj_set_style_opa(s_pill.pill, opa, LV_PART_MAIN);
  if (offset != 0) {
    lv_timer_pause(s_tint_timer);
    return;
  }
  lv_timer_reset(s_tint_timer);
  lv_timer_resume(s_tint_timer);
}

void turn_strip(int steps) {
  if (strip_count() == 0 || steps == 0)
    return;
  carousel_turn(steps);
  s_sel = carousel_item();
  if (s_mode == Mode::People)
    avatars_request(s_state, s_sel);
}

void show_mode(Mode mode) {
  s_mode = mode;
  lv_obj_set_flag(s_status, LV_OBJ_FLAG_HIDDEN, mode != Mode::Status);
  update_pill();
}

void pick_office(int office) {
  haptic_buzz();
  lv_label_set_text(s_status_label, "");
  show_mode(Mode::Status);
  if (s_office_picked)
    s_office_picked(office);
}

// ─── Logging

void log_for_selected(int kind, int qty) {
  if (!person(s_sel))
    return;
  Person &p = s_state.people[s_sel];
  KaffiLogCmd cmd = {};
  strncpy(cmd.person_id, p.id, sizeof(cmd.person_id) - 1);
  strncpy(cmd.person_name, p.name, sizeof(cmd.person_name) - 1);
  cmd.kind = kind;
  cmd.quantity = qty;
  esp_event_post(APP_EVENT, APP_EVENT_KAFFI_LOG, &cmd, sizeof(cmd), 0);
  haptic_buzz();
  sheet_close(s_log_sheet);

  const char *toast = log_toast(p, kind, qty, is_weekday_today());
  // Shown at once; the next fetch confirms it.
  if (kind == KAFFI_BREW) {
    p.pots += qty;
    p.credit += kaffi_brew_credit(qty);
    if (!p.brewed_today) {
      p.brewed_today = true;
      p.streak++;
    }
  } else {
    p.cups += qty;
  }
  show_person();
  show_toast(toast);
}

// ─── Lists on sheets

void move_focus(lv_obj_t *list, int steps) {
  int count = ui_list_count(list);
  if (count == 0 || steps == 0)
    return;
  s_focus = std::clamp(s_focus + steps, 0, count - 1);
  ui_list_focus(list, s_focus);
}

void close_sheet(lv_event_t *) {
  if (lv_obj_t *sheet = open_sheet())
    sheet_close(sheet);
}

void on_log_row(lv_event_t *e) {
  auto row =
      static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  if (row == 0)
    log_for_selected(KAFFI_CUP, 1);
  else
    log_for_selected(KAFFI_BREW, row);
}

void build_log() {
  lv_obj_clean(s_log_content);
  const Person *p = person(s_sel);
  if (!p)
    return;
  ui_list_title(s_log_content, p->name, nullptr);
  s_log_list = ui_list_create(s_log_content, s_log_sheet, ListSize::Large);
  struct Choice {
    const char *text;
    int change;
  };
  const Choice choices[] = {{"Took a cup", -1},
                            {"Brewed a pot", kaffi_brew_credit(1)},
                            {"Brewed two pots", kaffi_brew_credit(2)}};
  for (int i = 0; i < 3; i++) {
    lv_obj_t *row = ui_list_row(s_log_list, choices[i].text, false);
    char change[16];
    signed_text(choices[i].change, change, sizeof(change));
    lv_obj_t *value = ui_list_trailing(row, change);
    lv_obj_set_style_text_opa(value, TEXT_OPA_SECONDARY, LV_PART_MAIN);
    lv_obj_add_event_cb(row, on_log_row, LV_EVENT_SHORT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(i)));
  }
  s_focus = 0;
  ui_list_focus(s_log_list, 0);
  ui_list_back(s_log_content, ICON_ARROW_LEFT, "Back", close_sheet);
}

// A person's circle in a row: their photo, else their initial. The podium
// gets a rim in its medal's color, on the photo's own edge so it keeps the
// row's spacing to the name.
void board_avatar(lv_obj_t *row, int index, int rank) {
  lv_obj_t *slot = ui_box(row);
  lv_obj_set_size(slot, THUMB_SIZE, THUMB_SIZE);
  lv_obj_set_style_radius(slot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_align(slot, LV_ALIGN_LEFT_MID, LIST_ROW_INSET, 0);
  if (rank < 3) {
    const uint32_t medals[] = {COL_YELLOW, COL_SILVER, COL_BRONZE};
    lv_obj_set_style_border_color(slot, lv_color_hex(medals[rank]),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_width(slot, 2, LV_PART_MAIN);
    lv_obj_set_style_border_post(slot, true, LV_PART_MAIN);
  }
  if (const lv_image_dsc_t *thumb = avatar_thumb(index)) {
    lv_obj_t *image = lv_image_create(slot);
    lv_image_set_src(image, thumb);
    lv_obj_center(image);
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    return;
  }
  lv_obj_set_style_bg_color(slot, lv_color_hex(COL_PLACEHOLDER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, LV_PART_MAIN);
  char initial[8];
  initial_of(s_state.people[index].name, initial, sizeof(initial));
  lv_obj_t *letter = lv_label_create(slot);
  lv_obj_set_style_text_font(letter, &sf_medium_15, LV_PART_MAIN);
  lv_obj_set_style_text_color(letter, lv_color_white(), LV_PART_MAIN);
  lv_label_set_text(letter, initial);
  lv_obj_center(letter);
}

void build_board() {
  lv_obj_clean(s_board_content);
  s_board_list = nullptr;
  ui_list_title(s_board_content, "Coffee heroes", nullptr);
  int order[KAFFI_MAX_PEOPLE];
  int count = leaderboard_order(s_state, order);
  if (count == 0) {
    lv_obj_t *empty = label(s_board_content, &sf_regular_16, 0);
    lv_obj_set_style_text_opa(empty, TEXT_OPA_SECONDARY, LV_PART_MAIN);
    lv_label_set_text(empty, "Nothing brewed yet.\nBe the first hero.");
  } else {
    s_board_list =
        ui_list_create(s_board_content, s_board_sheet, ListSize::Tall);
    for (int rank = 0; rank < count; rank++) {
      const Person &p = s_state.people[order[rank]];
      int balance = balance_of(p);
      lv_obj_t *row = ui_list_row(s_board_list, p.name);
      board_avatar(row, order[rank], rank);
      char text[16];
      signed_text(balance, text, sizeof(text));
      lv_obj_t *value = ui_list_trailing(row, text);
      lv_obj_set_style_text_color(value,
                                  lv_color_hex(balance > 0   ? COL_GREEN
                                               : balance < 0 ? COL_RED
                                                             : 0xFFFFFF),
                                  LV_PART_MAIN);
      // Rows only show; the knob and the list's own scroll move through it.
      lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    }
    s_focus = 0;
    ui_list_focus(s_board_list, 0);
  }
}

bool log_may_open() {
  return have_people() && !sheet_is_visible(s_board_sheet);
}
bool log_may_close_from(lv_point_t start) {
  return ui_list_may_close_from(s_log_list, start);
}
bool board_may_open() {
  return have_people() && !sheet_is_visible(s_log_sheet);
}
bool board_may_close_from(lv_point_t start) {
  return ui_list_may_close_from(s_board_list, start, SheetEdge::Top);
}

lv_obj_t *make_sheet(SheetEdge edge, const SheetPolicy &policy,
                     lv_obj_t **content) {
  lv_obj_t *sheet = sheet_create(s_screen, edge, policy);
  *content = sheet_add_content(sheet);
  return sheet;
}

void build_sheets() {
  static constexpr SheetPolicy LOG = {
      .may_open = log_may_open,
      .may_close_from = log_may_close_from,
      .on_appear = build_log,
      .on_opened = nullptr,
      .on_closed = nullptr,
  };
  static constexpr SheetPolicy BOARD = {
      .may_open = board_may_open,
      .may_close_from = board_may_close_from,
      .on_appear = build_board,
      .on_opened = nullptr,
      .on_closed = nullptr,
  };
  s_log_sheet = make_sheet(SheetEdge::Bottom, LOG, &s_log_content);
  s_board_sheet = make_sheet(SheetEdge::Top, BOARD, &s_board_content);
}

// ─── Input

void on_press(lv_event_t *) { s_swiped = false; }

void on_tap(lv_event_t *) {
  if (s_swiped || open_sheet())
    return;
  if (s_mode == Mode::Office)
    pick_office(carousel_item());
  else if (have_people())
    sheet_open(s_log_sheet);
}

void on_hold(lv_event_t *) {
  if (have_people() && !open_sheet() && !s_swiped) {
    haptic_buzz();
    sheet_open(s_board_sheet);
  }
}

void on_gesture(lv_event_t *) {
  if (open_sheet() || s_swiped || strip_count() == 0)
    return;
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
  if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT)
    return;
  s_swiped = true;
  turn_strip(dir == LV_DIR_LEFT ? 1 : -1);
}

void on_encoder_poll(lv_timer_t *) {
  int32_t steps = encoder_take_steps();
  if (steps == 0)
    return;
  if (lv_obj_t *sheet = open_sheet()) {
    move_focus(sheet == s_log_sheet ? s_log_list : s_board_list,
               static_cast<int>(steps));
    lv_display_trigger_activity(nullptr);
    return;
  }
  s_enc_halfsteps += static_cast<int>(steps) * 2;
  int moves = s_enc_halfsteps / KAFFI_ENCODER_HALFSTEPS_PER_ITEM;
  s_enc_halfsteps -= moves * KAFFI_ENCODER_HALFSTEPS_PER_ITEM;
  turn_strip(moves);
}

void on_idle_check(lv_timer_t *) {
  sheet_close_if_idle(s_log_sheet, KAFFI_SHEET_IDLE_MS);
  sheet_close_if_idle(s_board_sheet, KAFFI_SHEET_IDLE_MS);
}

// ─── Building

void build_people() {
  backdrop_create(s_screen, lv_color_black());
  volume_ring_create(s_screen, 0, RING_MAX, 0);
  volume_ring_set_look(ring_rest_look(lv_color_white()), 0);

  // People are round, so a circle as wide as the radio's squircle looks
  // smaller; they take some of the room under the pill instead.
  static constexpr CarouselHooks HOOKS = {
      strip_tile, strip_moved, &sf_rounded_semibold_48, KAFFI_PHOTO_PX};
  carousel_create(s_screen, stage_tile_y(KAFFI_PHOTO_PX), HOOKS);
  // The side tiles reach the bezel; the ring stays over them, as in the radio.
  lv_obj_move_foreground(volume_ring_obj());

  s_caption = ui_caption(s_screen);
  s_pill = ui_pill(s_screen, ICON_FLAME);
  lv_obj_align(s_pill.pill, LV_ALIGN_TOP_MID, 0, PILL_TOP);
  lv_obj_add_flag(s_pill.pill, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *make_overlay() {
  lv_obj_t *overlay = ui_box(s_screen);
  lv_obj_set_size(overlay, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_bg_color(overlay, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
  ui_absorb_taps(overlay);
  return overlay;
}

void build_status() {
  s_status = make_overlay();
  s_status_label = ui_activity_message(s_status, "");
}

} // namespace

// ─── Public API

void ui_init() {
  lv_display_t *disp = nullptr;
  lv_indev_t *touch = nullptr;
  display_init(&disp, &touch);

  if (!display_lock(1000))
    return;
  s_screen = lv_screen_active();
  lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
  lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(s_screen, on_press, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(s_screen, on_tap, LV_EVENT_SHORT_CLICKED, nullptr);
  lv_obj_add_event_cb(s_screen, on_hold, LV_EVENT_LONG_PRESSED, nullptr);
  lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, nullptr);

  build_people();
  build_status();
  build_sheets();
  sheets_track_touch(touch);

  show_mode(Mode::Status);
  display_set_backlight(KAFFI_BACKLIGHT_NORMAL);
  lv_timer_create(on_encoder_poll, KAFFI_ENCODER_POLL_MS, nullptr);
  lv_timer_create(on_idle_check, IDLE_CHECK_MS, nullptr);
  s_tint_timer = lv_timer_create(on_tint_settled, TINT_SETTLE_MS, nullptr);
  lv_timer_pause(s_tint_timer);
  display_unlock();
  ESP_LOGI(TAG, "UI initialized");
}

void ui_show_splash() { ui_set_status(""); }

void ui_set_status(const char *msg) {
  if (!display_lock(200))
    return;
  lv_label_set_text(s_status_label, msg);
  show_mode(Mode::Status);
  display_unlock();
}

// A white card, so the code reads dark on light the way every camera expects.
void ui_show_wifi_setup(const char *ap_name) {
  constexpr int CARD = 164;
  constexpr int CODE = 136;
  if (!display_lock(1000))
    return;
  lv_obj_t *view = make_overlay();
  ui_list_title(view, "Set up Wi-Fi", nullptr);

  lv_obj_t *card = ui_box(view);
  lv_obj_set_size(card, CARD, CARD);
  lv_obj_set_style_radius(card, 24, LV_PART_MAIN);
  lv_obj_set_style_bg_color(card, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(card, LV_ALIGN_CENTER, 0, 4);

  char data[80];
  snprintf(data, sizeof(data), "WIFI:T:nopass;S:%s;;", ap_name);
  lv_obj_t *code = lv_qrcode_create(card);
  lv_qrcode_set_size(code, CODE);
  lv_qrcode_set_dark_color(code, lv_color_black());
  lv_qrcode_set_light_color(code, lv_color_white());
  lv_qrcode_update(code, data, strlen(data));
  lv_obj_center(code);

  lv_obj_t *hint = label(view, &sf_regular_16, 118);
  lv_obj_set_style_text_opa(hint, TEXT_OPA_SECONDARY, LV_PART_MAIN);
  char text[96];
  snprintf(text, sizeof(text),
           "Scan to join \u201C%s\u201D,\nthen pick the Wi-Fi", ap_name);
  lv_label_set_text(hint, text);
  display_unlock();
}

// The radio's browse, with offices for stations.
void ui_show_office_picker(void (*picked)(int office)) {
  if (!display_lock(1000))
    return;
  s_office_picked = picked;
  s_sel = s_shown = 0;
  show_mode(Mode::Office);
  carousel_open(KAFFI_OFFICE_COUNT, 0, nullptr);
  show_office();
  backdrop_set(sky_of(0), false);
  display_unlock();
}

#ifdef ESP_PLATFORM
int ui_pick_office() {
  static SemaphoreHandle_t picked;
  static int office;
  picked = xSemaphoreCreateBinary();
  if (!picked)
    return 0;
  ui_show_office_picker([](int choice) {
    office = choice;
    xSemaphoreGive(picked);
  });
  xSemaphoreTake(picked, portMAX_DELAY);
  vSemaphoreDelete(picked);
  return office;
}
#endif

void ui_set_offline(bool offline, int pending) {
  if (!display_lock(200))
    return;
  s_offline = offline;
  s_pending = pending;
  update_pill();
  display_unlock();
}

// The same people in the same order, with the same photos: only their
// numbers can have changed.
static bool same_people(const KaffiState &a, const KaffiState &b) {
  if (a.count != b.count)
    return false;
  for (int i = 0; i < a.count; i++)
    if (strcmp(a.people[i].id, b.people[i].id) != 0 ||
        strcmp(a.people[i].image_url, b.people[i].image_url) != 0)
      return false;
  return true;
}

void ui_update_people(const KaffiState *state) {
  if (!state || !display_lock(200))
    return;
  // The periodic refresh mostly brings new numbers. Leave the strip alone
  // then: rebuilding it would stop a turn mid-glide.
  if (s_mode == Mode::People && same_people(*state, s_state)) {
    memcpy(&s_state, state, sizeof(s_state));
    show_person();
    display_unlock();
    return;
  }
  // Stay on the same person across refreshes: someone joining or leaving
  // shifts everyone after them.
  char keep[KAFFI_ID_LEN] = {};
  if (const Person *p = person(s_sel))
    strncpy(keep, p->id, sizeof(keep) - 1);
  memcpy(&s_state, state, sizeof(s_state));
  s_sel = 0;
  for (int i = 0; keep[0] && i < s_state.count; i++)
    if (strcmp(s_state.people[i].id, keep) == 0)
      s_sel = i;
  avatars_follow(s_state);

  if (s_state.count == 0) {
    lv_label_set_text(s_status_label,
                      "No one here yet.\nAdd people in the Studio.");
    show_mode(Mode::Status);
    display_unlock();
    return;
  }
  bool first = s_mode != Mode::People;
  s_shown = s_sel;
  if (first)
    carousel_open(s_state.count, s_sel, nullptr);
  else
    carousel_reload(s_state.count, s_sel);
  show_person();
  backdrop_set(avatar_tint(s_sel), !first);
  avatars_preload(s_state, s_sel);
  show_mode(Mode::People);
  display_unlock();
}

void ui_set_avatar(const char *url, unsigned char *pixels, int w, int h) {
  if (!display_lock(200)) {
    heap_caps_free(pixels);
    return;
  }
  int index = avatars_store(s_state, url, pixels, w, h);
  if (index >= 0) {
    carousel_refresh(index);
    if (index == s_shown && have_people())
      backdrop_set(avatar_tint(index), true);
  }
  display_unlock();
}
