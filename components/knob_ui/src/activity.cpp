#include "activity.h"
#include "fonts.h"
#include "primitives.h"
#include "theme.h"

#include <cmath>
#include <numbers>

namespace {

constexpr int SPOKES = 8;
constexpr int INNER_R = 9;
constexpr int OUTER_R = 17;
constexpr int SPOKE_W = 4;
constexpr int SIZE = 2 * (OUTER_R + SPOKE_W);
constexpr int TURN_MS = 800;
// The trailing spokes fade to this, so the ring always reads as a ring.
constexpr lv_opa_t TAIL_LEVEL = 64;

lv_point_precise_t s_points[SPOKES][2];
lv_color_t s_backdrop;
int32_t s_head = -1;

void step_cb(void *obj, int32_t head) {
  // The animation ticks every frame; only a new step may repaint.
  head %= SPOKES;
  if (head == s_head)
    return;
  s_head = head;
  auto *ring = static_cast<lv_obj_t *>(obj);
  for (int i = 0; i < SPOKES; i++) {
    int behind = (static_cast<int>(head) - i + SPOKES) % SPOKES;
    auto level = static_cast<uint8_t>(
        LV_OPA_COVER - behind * (LV_OPA_COVER - TAIL_LEVEL) / (SPOKES - 1));
    // Shaded by color, not opacity: a see-through round cap darkens where it
    // overlaps the line body.
    lv_obj_set_style_line_color(
        lv_obj_get_child(ring, i),
        lv_color_mix(lv_color_white(), s_backdrop, level), LV_PART_MAIN);
  }
}

// A hard step, like UIActivityIndicatorView: spokes don't smear between
// positions.
int32_t step_path(const lv_anim_t *a) {
  return a->start_value + (a->act_time * SPOKES) / a->duration;
}

} // namespace

lv_obj_t *activity_create(lv_obj_t *parent) {
  s_backdrop = lv_obj_get_style_bg_color(parent, LV_PART_MAIN);
  lv_obj_t *ring = ui_box(parent);
  lv_obj_set_size(ring, SIZE, SIZE);

  constexpr float CENTER = SIZE / 2.0f;
  for (int i = 0; i < SPOKES; i++) {
    float a = static_cast<float>(i) * 2.0f * std::numbers::pi_v<float> /
              static_cast<float>(SPOKES);
    float dx = std::sin(a);
    float dy = -std::cos(a);
    auto px = [](float v) { return static_cast<int32_t>(std::lround(v)); };
    s_points[i][0] = {px(CENTER + INNER_R * dx), px(CENTER + INNER_R * dy)};
    s_points[i][1] = {px(CENTER + OUTER_R * dx), px(CENTER + OUTER_R * dy)};
    lv_obj_t *spoke = lv_line_create(ring);
    lv_line_set_points(spoke, s_points[i], 2);
    lv_obj_set_style_line_width(spoke, SPOKE_W, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(spoke, true, LV_PART_MAIN);
  }
  s_head = -1;
  step_cb(ring, 0);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, ring);
  lv_anim_set_exec_cb(&a, step_cb);
  lv_anim_set_values(&a, 0, SPOKES);
  lv_anim_set_duration(&a, TURN_MS);
  lv_anim_set_path_cb(&a, step_path);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);
  return ring;
}

lv_obj_t *ui_activity_message(lv_obj_t *parent, const char *text) {
  constexpr int SPINNER_Y = -16;
  constexpr int TEXT_Y = 24;
  lv_obj_align(activity_create(parent), LV_ALIGN_CENTER, 0, SPINNER_Y);
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(label, TEXT_OPA_SECONDARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(label, &sf_regular_16, LV_PART_MAIN);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(label, text);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, TEXT_Y);
  return label;
}
