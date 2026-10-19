#include "primitives.h"

#include <cstring>
#include <initializer_list>

lv_obj_t *ui_box(lv_obj_t *parent) {
  lv_obj_t *box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  return box;
}

static void absorb(lv_event_t *) {}

void ui_absorb_taps(lv_obj_t *obj) {
  lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_event_cb(obj, absorb, LV_EVENT_CLICKED, nullptr);
}

static void mirror_press(lv_event_t *e) {
  auto *visual = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
  lv_obj_set_state(visual, LV_STATE_PRESSED,
                   lv_event_get_code(e) == LV_EVENT_PRESSED);
}

lv_obj_t *ui_hit_area(lv_obj_t *parent, lv_obj_t *visual,
                      lv_event_cb_t on_tap) {
  lv_obj_t *hit = ui_box(parent);
  lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(visual, LV_OBJ_FLAG_CLICKABLE);
  // A sheet that takes over a drag resets the press rather than releasing it.
  for (auto code : {LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST,
                    LV_EVENT_INDEV_RESET})
    lv_obj_add_event_cb(hit, mirror_press, code, visual);
  lv_obj_add_event_cb(hit, on_tap, LV_EVENT_SHORT_CLICKED, nullptr);
  return hit;
}

// ─── Icon pills

static constexpr int PILL_INK_GAP = 6;
// The rounded ends take visual room away, so the ink sits this much further
// from them than from the top and bottom.
static constexpr int PILL_END_EXTRA = 5;

static uint32_t decode_utf8(const char *s) {
  auto b = reinterpret_cast<const uint8_t *>(s);
  if (b[0] < 0x80)
    return b[0];
  if ((b[0] & 0xE0) == 0xC0)
    return ((b[0] & 0x1F) << 6) | (b[1] & 0x3F);
  if ((b[0] & 0xF0) == 0xE0)
    return ((b[0] & 0x0F) << 12) | ((b[1] & 0x3F) << 6) | (b[2] & 0x3F);
  return ((b[0] & 0x07) << 18) | ((b[1] & 0x3F) << 12) | ((b[2] & 0x3F) << 6) |
         (b[3] & 0x3F);
}

static uint32_t last_letter(const char *s) {
  size_t i = strlen(s);
  if (i == 0)
    return 0;
  do {
    i--;
  } while (i > 0 && (static_cast<uint8_t>(s[i]) & 0xC0) == 0x80);
  return decode_utf8(s + i);
}

namespace {
struct Ink {
  int left;  // blank columns before the ink
  int right; // blank columns after it
  int height;
  int center_dy; // moves a label's ink center onto its box's center
};
} // namespace

static Ink ink_of(const lv_font_t *font, uint32_t letter) {
  lv_font_glyph_dsc_t g = {};
  if (!letter || !lv_font_get_glyph_dsc(font, &g, letter, 0))
    return {};
  int line_h = lv_font_get_line_height(font);
  // LVGL draws a glyph's ink from (line_h - base_line - ofs_y - box_h) down.
  int ink_mid_x2 = 2 * (line_h - font->base_line - g.ofs_y) - g.box_h;
  return {g.ofs_x, g.adv_w - g.ofs_x - g.box_w, g.box_h,
          (line_h - ink_mid_x2) / 2};
}

static uint32_t icon_letter(const IconPill &pill) {
  return decode_utf8(lv_label_get_text(pill.icon));
}

IconPill ui_icon_pill(lv_obj_t *parent, int height, const lv_font_t *icon_font,
                      const char *icon, int icon_nudge_y,
                      const lv_font_t *text_font) {
  IconPill p = {};
  p.pill = ui_box(parent);
  lv_obj_set_size(p.pill, LV_SIZE_CONTENT, height);
  lv_obj_set_style_radius(p.pill, height / 2, LV_PART_MAIN);
  lv_obj_set_flex_flow(p.pill, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(p.pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  p.icon = lv_label_create(p.pill);
  lv_obj_set_style_text_font(p.icon, icon_font, LV_PART_MAIN);
  p.label = lv_label_create(p.pill);
  lv_obj_set_style_text_font(p.label, text_font, LV_PART_MAIN);
  lv_obj_set_style_translate_y(p.label, ink_of(text_font, 'H').center_dy,
                               LV_PART_MAIN);
  lv_label_set_text(p.label, "");
  ui_icon_pill_set_icon(p, icon, icon_nudge_y);
  return p;
}

// Spaces the text off the icon and the pill's end by their ink.
static void refit(const IconPill &p) {
  const char *text = lv_label_get_text(p.label);
  const lv_font_t *icon_font = lv_obj_get_style_text_font(p.icon, LV_PART_MAIN);
  const lv_font_t *text_font =
      lv_obj_get_style_text_font(p.label, LV_PART_MAIN);
  Ink icon = ink_of(icon_font, icon_letter(p));
  Ink first = ink_of(text_font, decode_utf8(text));
  Ink last = ink_of(text_font, last_letter(text));
  int inset = (lv_obj_get_style_height(p.pill, LV_PART_MAIN) - icon.height) / 2;
  lv_obj_set_style_pad_column(p.pill, PILL_INK_GAP - icon.right - first.left,
                              LV_PART_MAIN);
  lv_obj_set_style_pad_right(p.pill, inset + PILL_END_EXTRA - last.right,
                             LV_PART_MAIN);
}

void ui_icon_pill_set_icon(const IconPill &p, const char *icon,
                           int icon_nudge_y) {
  lv_label_set_text(p.icon, icon);
  const lv_font_t *icon_font = lv_obj_get_style_text_font(p.icon, LV_PART_MAIN);
  Ink glyph = ink_of(icon_font, icon_letter(p));
  int height = lv_obj_get_style_height(p.pill, LV_PART_MAIN);
  lv_obj_set_style_translate_y(p.icon, glyph.center_dy + icon_nudge_y,
                               LV_PART_MAIN);
  lv_obj_set_style_pad_left(
      p.pill, (height - glyph.height) / 2 + PILL_END_EXTRA - glyph.left,
      LV_PART_MAIN);
  refit(p);
}

void ui_icon_pill_set_text(const IconPill &p, const char *text) {
  lv_label_set_text(p.label, text);
  refit(p);
}

