#include "toast.h"

#include "fonts.h"
#include "haptic.h"
#include "motion.h"
#include "theme.h"

#include <cstdio>
#include <cstring>

namespace {
constexpr uint32_t SUCCESS_MS = 2000;
constexpr uint32_t FAILURE_MS = 2500;
constexpr uint32_t INFO_MS = 2000;
constexpr uint32_t HIDDEN_IN_MS = 160;
constexpr uint32_t SWAP_OUT_MS = 80;
constexpr uint32_t SWAP_IN_MS = 120;

IconPill s_pill = {};
void (*s_restore)() = nullptr;
lv_timer_t *s_timer = nullptr;
bool s_active = false;
ToastKind s_kind = ToastKind::Info;
char s_text[96] = {};
char s_icon[8] = {};

void opacity(void *object, int32_t value) {
  lv_obj_set_style_opa(static_cast<lv_obj_t *>(object), value, LV_PART_MAIN);
}

void animate(lv_opa_t from, lv_opa_t to, uint32_t ms,
             lv_anim_completed_cb_t done = nullptr) {
  lv_anim_delete(s_pill.pill, opacity);
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, s_pill.pill);
  lv_anim_set_exec_cb(&animation, opacity);
  lv_anim_set_values(&animation, from, to);
  lv_anim_set_duration(&animation, ms);
  motion_ease_out(&animation);
  if (done)
    lv_anim_set_completed_cb(&animation, done);
  lv_anim_start(&animation);
}

uint32_t default_ms(ToastKind kind) {
  if (kind == ToastKind::Failure)
    return FAILURE_MS;
  if (kind == ToastKind::Success)
    return SUCCESS_MS;
  return INFO_MS;
}

const char *default_icon(ToastKind kind) {
  if (kind == ToastKind::Failure)
    return ICON_X;
  if (kind == ToastKind::Success)
    return ICON_CHECK;
  return "";
}

void draw_toast() {
  ui_icon_pill_set_icon(s_pill, s_icon, 0);
  ui_icon_pill_set_text(s_pill, s_text);
  lv_color_t color = lv_color_white();
  lv_opa_t text_opa = TEXT_OPA_SOFT;
  if (s_kind == ToastKind::Success) {
    color = lv_color_hex(COL_GREEN);
    text_opa = LV_OPA_COVER;
  } else if (s_kind == ToastKind::Failure) {
    color = lv_color_hex(COL_RED);
    text_opa = LV_OPA_COVER;
  }
  ui_pill_tint(s_pill, color, text_opa);
  lv_obj_set_style_bg_color(s_pill.pill, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_pill.pill, PILL_OPA, LV_PART_MAIN);
  lv_obj_remove_flag(s_pill.pill, LV_OBJ_FLAG_HIDDEN);
}

void restore_done(lv_anim_t *) {
  s_active = false;
  if (s_restore)
    s_restore();
  if (lv_obj_has_flag(s_pill.pill, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_set_style_opa(s_pill.pill, LV_OPA_COVER, LV_PART_MAIN);
    return;
  }
  lv_obj_set_style_opa(s_pill.pill, LV_OPA_TRANSP, LV_PART_MAIN);
  animate(LV_OPA_TRANSP, LV_OPA_COVER, SWAP_IN_MS);
}

void end(lv_timer_t *) {
  s_timer = nullptr;
  s_text[0] = '\0';
  animate(LV_OPA_COVER, LV_OPA_TRANSP, SWAP_OUT_MS, restore_done);
}

void swapped(lv_anim_t *) {
  draw_toast();
  animate(LV_OPA_TRANSP, LV_OPA_COVER, SWAP_IN_MS);
}
} // namespace

void toast_attach(const IconPill &pill, void (*restore)()) {
  s_pill = pill;
  s_restore = restore;
  toast_cancel();
}

void toast_show(ToastKind kind, const char *text, const char *icon,
                uint32_t ms) {
  if (!s_pill.pill || !text)
    return;
  if (s_timer && s_kind == kind && strcmp(s_text, text) == 0) {
    lv_timer_set_period(s_timer, ms ? ms : default_ms(kind));
    lv_timer_reset(s_timer);
    return;
  }

  bool was_visible = s_timer != nullptr ||
                     !lv_obj_has_flag(s_pill.pill, LV_OBJ_FLAG_HIDDEN);
  if (s_timer)
    lv_timer_delete(s_timer);
  s_kind = kind;
  s_active = true;
  snprintf(s_text, sizeof(s_text), "%s", text);
  snprintf(s_icon, sizeof(s_icon), "%s", icon ? icon : default_icon(kind));
  s_timer = lv_timer_create(end, ms ? ms : default_ms(kind), nullptr);
  lv_timer_set_repeat_count(s_timer, 1);

  if (kind == ToastKind::Failure)
    haptic_buzz_triple();
  if (kind != ToastKind::Info)
    lv_display_trigger_activity(nullptr);

  if (was_visible) {
    lv_opa_t from = lv_obj_get_style_opa(s_pill.pill, LV_PART_MAIN);
    animate(from, LV_OPA_TRANSP, SWAP_OUT_MS, swapped);
  } else {
    draw_toast();
    lv_obj_set_style_opa(s_pill.pill, LV_OPA_TRANSP, LV_PART_MAIN);
    animate(LV_OPA_TRANSP, LV_OPA_COVER, HIDDEN_IN_MS);
  }
}

bool toast_active() { return s_active; }

void toast_cancel() {
  if (s_timer) {
    lv_timer_delete(s_timer);
    s_timer = nullptr;
  }
  s_text[0] = '\0';
  s_active = false;
  if (s_pill.pill)
    lv_anim_delete(s_pill.pill, opacity);
  if (s_restore)
    s_restore();
}
