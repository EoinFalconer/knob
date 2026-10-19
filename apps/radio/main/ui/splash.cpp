#include "ui/splash.h"
#include "fonts.h"
#include "hal_pins.h"
#include "ui/clock_art.h"
#include "ui/face_palette.h"
#include "ui/motion.h"

#include <algorithm>

namespace {

const lv_font_t &FONT = sf_rounded_black_200;
constexpr const char *LINES[] = {"KN", "OB"};
constexpr int TRACKING = -6;
constexpr int LINE_GAP = 10;
// Each letter takes one part of the clock's day, like fridge magnets: a
// little tilted and off its line, in its own color.
struct Look {
  DayPart part;
  bool ink;     // the palette's ink instead of its sky
  int16_t tilt; // 0.1 degrees, counterclockwise
  int8_t dy;
};
constexpr Look LOOKS[] = {
    {DayPart::Morning, false, 50, -2},
    {DayPart::Day, false, -40, 3},
    {DayPart::Evening, false, -50, -3},
    {DayPart::Night, true, 60, 2},
};
constexpr int32_t RISE = 18;
constexpr uint32_t RISE_MS = 520;
constexpr uint32_t STAGGER_MS = 70;

// Ink-cropped letter bitmaps, kept after boot (about 70 KB of PSRAM).
ClockArt s_letters[4];

struct Glyph {
  char c;
  lv_point_t at; // ink top-left, relative to the line's ink box
};

struct Line {
  Glyph glyphs[2];
  int32_t w, h;
};

Line layout(const char *text) {
  Line line = {};
  int32_t baseline = lv_font_get_line_height(&FONT) - FONT.base_line;
  int32_t pen = 0;
  lv_area_t ink = {INT16_MAX, INT16_MAX, INT16_MIN, INT16_MIN};
  for (int i = 0; i < 2; i++) {
    lv_font_glyph_dsc_t g;
    lv_font_get_glyph_dsc(&FONT, &g, static_cast<uint8_t>(text[i]), 0);
    lv_point_t at = {pen + g.ofs_x, baseline - g.ofs_y - g.box_h};
    line.glyphs[i] = {text[i], at};
    ink.x1 = std::min<int32_t>(ink.x1, at.x);
    ink.y1 = std::min<int32_t>(ink.y1, at.y);
    ink.x2 = std::max<int32_t>(ink.x2, at.x + g.box_w);
    ink.y2 = std::max<int32_t>(ink.y2, at.y + g.box_h);
    pen += g.adv_w + TRACKING;
  }
  for (Glyph &g : line.glyphs) {
    g.at.x -= ink.x1;
    g.at.y -= ink.y1;
  }
  line.w = ink.x2 - ink.x1;
  line.h = ink.y2 - ink.y1;
  return line;
}

void anim_rise_cb(void *letter, int32_t v) {
  auto *obj = static_cast<lv_obj_t *>(letter);
  lv_obj_set_style_opa(obj, static_cast<lv_opa_t>(v * LV_OPA_COVER / 256),
                       LV_PART_MAIN);
  lv_obj_set_style_translate_y(obj, RISE * (256 - v) / 256, LV_PART_MAIN);
}

void rise(lv_obj_t *letter, uint32_t delay) {
  anim_rise_cb(letter, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, letter);
  lv_anim_set_exec_cb(&a, anim_rise_cb);
  lv_anim_set_values(&a, 0, 256);
  lv_anim_set_duration(&a, RISE_MS);
  lv_anim_set_delay(&a, delay);
  motion_ease_out(&a);
  lv_anim_start(&a);
}

lv_color_t color_of(const Look &look) {
  FacePalette p = face_palette(look.part);
  return lv_color_hex(look.ink ? p.ink : p.sky);
}

} // namespace

lv_obj_t *splash_create() {
  FacePalette night = face_palette(DayPart::Night);
  lv_obj_t *screen = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(screen, lv_color_hex(night.sky), LV_PART_MAIN);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  Line lines[2] = {layout(LINES[0]), layout(LINES[1])};
  int32_t y = (LCD_V_RES - lines[0].h - LINE_GAP - lines[1].h) / 2;
  int k = 0;
  for (const Line &line : lines) {
    int32_t x = (LCD_H_RES - line.w) / 2;
    for (const Glyph &g : line.glyphs) {
      char text[2] = {g.c, '\0'};
      ClockArt &art = s_letters[k];
      clock_art_text(&art, &FONT, text, 0);
      lv_obj_t *img = lv_image_create(screen);
      const Look &look = LOOKS[k];
      lv_obj_set_style_image_recolor(img, color_of(look), LV_PART_MAIN);
      lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, LV_PART_MAIN);
      if (art.pixels) {
        lv_image_set_src(img, &art.dsc);
        lv_image_set_pivot(img, static_cast<int32_t>(art.dsc.header.w) / 2,
                           static_cast<int32_t>(art.dsc.header.h) / 2);
        lv_image_set_rotation(img, -look.tilt);
      }
      lv_obj_set_pos(img, x + g.at.x, y + g.at.y + look.dy);
      rise(img, static_cast<uint32_t>(k) * STAGGER_MS);
      k++;
    }
    y += line.h + LINE_GAP;
  }
  return screen;
}
