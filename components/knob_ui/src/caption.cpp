#include "caption.h"
#include "fonts.h"
#include "hal_pins.h"
#include "theme.h"

#include <cstdlib>
#include <cstring>

namespace {

constexpr int MARQUEE_DELAY_MS = 2500;
constexpr int MARQUEE_PX_PER_S = 30;
constexpr int TITLE_W = LCD_H_RES - 80;
// Widest a line can be at `row` (its centre, below the screen's centre)
// and still clear the ring by CAPTION_MARGIN at its lower corners.
constexpr int CAPTION_MARGIN = 24; // ring width plus breathing room
constexpr int line_w(int row, int half_height) {
  int r = LCD_H_RES / 2 - CAPTION_MARGIN;
  int y = row + half_height;
  int w = 0;
  while ((w + 1) * (w + 1) + y * y <= r * r)
    w++;
  return 2 * w;
}

void rest_truncated(lv_anim_t *a) {
  auto *label = static_cast<lv_obj_t *>(a->var);
  // Dot mode wraps first and only cuts at the height, so pin it to one line.
  const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
  lv_obj_set_height(label, lv_font_get_line_height(font));
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

void scroll_once(lv_obj_t *label) {
  static lv_anim_t marquee;
  static bool ready;
  if (!ready) {
    lv_anim_init(&marquee);
    marquee.act_time = -MARQUEE_DELAY_MS;
    lv_anim_set_repeat_count(&marquee, 1);
    lv_anim_set_completed_cb(&marquee, rest_truncated);
    ready = true;
  }
  lv_obj_set_style_anim(label, &marquee, LV_PART_MAIN);
  lv_obj_set_style_anim_duration(
      label, lv_anim_speed_clamped(MARQUEE_PX_PER_S, 1000, 15000),
      LV_PART_MAIN);
}

lv_obj_t *make_line(lv_obj_t *parent, const lv_font_t *font, int y) {
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_label_set_text(label, "");
  lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
  return label;
}

} // namespace

Caption ui_caption(lv_obj_t *parent) {
  Caption c;
  c.title = make_line(parent, &sf_semibold_28, CAPTION_TITLE_Y);
  lv_obj_set_width(c.title, TITLE_W);
  lv_label_set_long_mode(c.title, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_align(c.title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  scroll_once(c.title);

  c.subtitle = make_line(parent, &sf_regular_16, CAPTION_SUBTITLE_Y);
  lv_obj_set_width(c.subtitle, line_w(CAPTION_SUBTITLE_Y, 10));
  lv_obj_set_style_text_align(c.subtitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_opa(c.subtitle, TEXT_OPA_SECONDARY, LV_PART_MAIN);
  scroll_once(c.subtitle);

  c.detail = make_line(parent, &sf_regular_16, CAPTION_DETAIL_Y);
  lv_obj_set_width(c.detail, line_w(CAPTION_DETAIL_Y, 10));
  lv_obj_set_style_text_align(c.detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(c.detail, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(c.detail, lv_color_hex(COL_CAPTION_DETAIL),
                              LV_PART_MAIN);
  return c;
}

void ui_marquee_text(lv_obj_t *label, const char *text) {
  if (strcmp(lv_label_get_text(label), text) == 0 &&
      lv_label_get_long_mode(label) != LV_LABEL_LONG_WRAP)
    return;
  lv_obj_set_height(label, LV_SIZE_CONTENT);
  lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_label_set_text(label, text);
}

lv_opa_t ui_caption_fade(int32_t carousel_offset) {
  return static_cast<lv_opa_t>(LV_OPA_COVER *
                               (500 - std::abs(carousel_offset)) / 500);
}
