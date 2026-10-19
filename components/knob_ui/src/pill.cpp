// Apart from primitives.cpp: it draws in app fonts, which only apps that use
// it compile.
#include "fonts.h"
#include "primitives.h"
#include "theme.h"

#include <initializer_list>

void ui_pill_tint(const IconPill &p, lv_color_t color, lv_opa_t opa) {
  for (lv_obj_t *o : {p.icon, p.label}) {
    lv_obj_set_style_text_color(o, color, LV_PART_MAIN);
    lv_obj_set_style_text_opa(o, opa, LV_PART_MAIN);
  }
}

IconPill ui_pill(lv_obj_t *parent, const char *icon) {
  IconPill p = ui_icon_pill(parent, PILL_H, &lucide_18, icon, 0, &sf_medium_15);
  lv_obj_set_style_bg_color(p.pill, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(p.pill, PILL_OPA, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(
      p.pill, PILL_PRESSED_OPA,
      static_cast<lv_style_selector_t>(LV_PART_MAIN | LV_STATE_PRESSED));
  ui_pill_tint(p, lv_color_white(), TEXT_OPA_SOFT);
  lv_obj_set_style_max_width(p.label, PILL_TEXT_MAX_W, LV_PART_MAIN);
  lv_label_set_long_mode(p.label, LV_LABEL_LONG_DOT);
  return p;
}
