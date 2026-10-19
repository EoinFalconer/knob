#include "backdrop.h"
#include "hal_pins.h"

namespace {

constexpr uint32_t FADE_MS = 250;

lv_obj_t *s_back;
lv_obj_t *s_front;

void anim_opa_cb(void *obj, int32_t v) {
  lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(obj),
                          static_cast<lv_opa_t>(v), LV_PART_MAIN);
}

// The front is opaque now: its color moves to the back, and it's cleared.
void settle(lv_anim_t *) {
  lv_obj_set_style_bg_color(
      s_back, lv_obj_get_style_bg_color(s_front, LV_PART_MAIN), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_front, LV_OPA_TRANSP, LV_PART_MAIN);
}

lv_obj_t *make_layer(lv_obj_t *parent, lv_color_t color, lv_opa_t opa) {
  lv_obj_t *layer = lv_obj_create(parent);
  lv_obj_remove_style_all(layer);
  lv_obj_set_size(layer, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_bg_color(layer, color, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(layer, opa, LV_PART_MAIN);
  lv_obj_remove_flag(layer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
  return layer;
}

void forget(lv_event_t *) { s_back = s_front = nullptr; }

} // namespace

void backdrop_create(lv_obj_t *parent, lv_color_t color) {
  s_back = make_layer(parent, color, LV_OPA_COVER);
  s_front = make_layer(parent, lv_color_black(), LV_OPA_TRANSP);
  // A rebuilt page makes a new one; nothing may touch the old layers after.
  lv_obj_add_event_cb(s_back, forget, LV_EVENT_DELETE, nullptr);
}

void backdrop_finish() {
  if (!s_back)
    return;
  lv_anim_delete(s_front, anim_opa_cb);
  if (lv_obj_get_style_bg_opa(s_front, LV_PART_MAIN) > LV_OPA_TRANSP)
    settle(nullptr);
}

void backdrop_set(lv_color_t color, bool animate) {
  if (!s_back)
    return;
  backdrop_finish();
  if (lv_color_eq(lv_obj_get_style_bg_color(s_back, LV_PART_MAIN), color))
    return;
  if (!animate) {
    lv_obj_set_style_bg_color(s_back, color, LV_PART_MAIN);
    return;
  }
  lv_obj_set_style_bg_color(s_front, color, LV_PART_MAIN);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_front);
  lv_anim_set_exec_cb(&a, anim_opa_cb);
  lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_duration(&a, FADE_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_completed_cb(&a, settle);
  lv_anim_start(&a);
}
