#include "sheet_list.h"
#include "fonts.h"
#include "primitives.h"
#include "theme.h"

#include <algorithm>
#include <cstdint>

namespace {

constexpr lv_opa_t ROW_OPA = 26;
constexpr lv_opa_t ROW_FOCUS_OPA = 50;
constexpr lv_opa_t ROW_PRESSED_OPA = 72;

constexpr int TITLE_Y = 46;
constexpr int TITLE_ALONE_Y = 54;
constexpr int HINT_Y = 72;

constexpr int LIST_W = 260;
constexpr int ROW_GAP = 8;
constexpr int TRAILING_GAP = 12; // between a row's text and its value

// Under a title alone a list starts about 90 px down. Above the Back capsule it
// ends at 276, where three large rows fill it; a tall list has no Back and
// runs on to 326, fading into the bezel.
// A row's text sits in a box this tall, centered: tighter than the font's
// line, whose accents and descenders would push the letters off center.
struct Geometry {
  int list_h, list_y; // list_y from the center
  int row_w, row_h;
  const lv_font_t *font;
  int text_h;
};
constexpr Geometry REGULAR = {170, 6, LIST_ROW_W, LIST_ROW_H, &sf_regular_18, 22};
constexpr Geometry TALL = {240, 26, LIST_ROW_W, LIST_ROW_H, &sf_regular_18, 22};
constexpr Geometry LARGE = {3 * 56 + 2 * ROW_GAP, 4, 256, 56, &sf_regular_22,
                            27};

// The list fades out at an edge where rows carry on past it, instead of
// cutting them off, like iOS's scroll edge effect. Nothing about it switches
// abruptly:
// - the fade is deep, most of a row, and eased (smoothstep), so it has no
//   visible start or end;
// - an edge's fade grows with how far the rows have scrolled under it, so it
//   comes in as the list moves instead of popping on after one pixel.
// It is the sheet's material painted back over the rows, in thin strips of
// rising opacity. Masking the list instead would draw it through a layer, too
// slow for a sheet that slides.
constexpr int FADE_H = 44;
constexpr int FADE_STEP = 2;
constexpr int32_t FADE_FULL = 256;

// The list remembers its sheet, its rows' size and its lifted row.
struct ListState {
  lv_obj_t *sheet;
  const Geometry *geometry;
  int focus;
};

// Text without circles starts where a rounded end stops curving much.
int text_inset(int row_h) { return row_h / 2 - 6; }

ListState *state_of(lv_obj_t *list) {
  return static_cast<ListState *>(lv_obj_get_user_data(list));
}

// How much of the content shows at `from_edge` px inside an edge at full fade.
int32_t fade_alpha(int from_edge) {
  if (from_edge >= FADE_H)
    return LV_OPA_COVER;
  int32_t t = from_edge * FADE_FULL / FADE_H;
  return t * t * (3 * FADE_FULL - 2 * t) / (FADE_FULL * FADE_FULL) *
         LV_OPA_COVER / FADE_FULL;
}

// 0..FADE_FULL: how strongly an edge with `hidden` px of rows beyond it fades.
int32_t edge_strength(int32_t hidden) {
  return std::clamp<int32_t>(hidden, 0, FADE_H) * FADE_FULL / FADE_H;
}

void veil_edge(lv_obj_t *sheet, lv_layer_t *layer, const lv_area_t &list,
               int32_t strength, bool top) {
  for (int y = 0; strength > 0 && y < FADE_H; y += FADE_STEP) {
    int32_t hidden = LV_OPA_COVER - fade_alpha(y + FADE_STEP / 2);
    auto veil = static_cast<lv_opa_t>(strength * hidden / FADE_FULL);
    lv_area_t strip = list;
    strip.y1 = top ? list.y1 + y : list.y2 - y - FADE_STEP + 1;
    strip.y2 = strip.y1 + FADE_STEP - 1;
    sheet_draw_material(sheet, layer, &strip, veil);
  }
}

void on_list_drawn(lv_event_t *e) {
  lv_obj_t *list = lv_event_get_current_target_obj(e);
  lv_area_t area;
  lv_obj_get_coords(list, &area);
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_obj_t *sheet = state_of(list)->sheet;
  if (!sheet)
    return;
  veil_edge(sheet, layer, area, edge_strength(lv_obj_get_scroll_top(list)),
            true);
  veil_edge(sheet, layer, area, edge_strength(lv_obj_get_scroll_bottom(list)),
            false);
}

void on_list_deleted(lv_event_t *e) {
  delete state_of(lv_event_get_current_target_obj(e));
}

lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, const char *text,
                     int y) {
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_label_set_text(label, text);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
  return label;
}

} // namespace

void ui_list_title(lv_obj_t *parent, const char *title, const char *hint) {
  make_label(parent, &sf_regular_22, title, hint ? TITLE_Y : TITLE_ALONE_Y);
  if (!hint)
    return;
  lv_obj_t *line = make_label(parent, &sf_regular_16, hint, HINT_Y);
  lv_obj_set_style_text_opa(line, TEXT_OPA_TERTIARY, LV_PART_MAIN);
}

lv_obj_t *ui_list_create(lv_obj_t *parent, lv_obj_t *sheet, ListSize size) {
  const Geometry &g = size == ListSize::Large  ? LARGE
                      : size == ListSize::Tall ? TALL
                                               : REGULAR;
  lv_obj_t *list = lv_obj_create(parent);
  lv_obj_set_size(list, LIST_W, g.list_h);
  lv_obj_align(list, LV_ALIGN_CENTER, 0, g.list_y);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(list, ROW_GAP, LV_PART_MAIN);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  // Rows scroll it from their own presses; the gaps between them must reach
  // the screen, or a swipe down to dismiss dies there.
  lv_obj_remove_flag(list, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(list, new ListState{sheet, &g, -1});
  lv_obj_add_event_cb(list, on_list_deleted, LV_EVENT_DELETE, nullptr);
  lv_obj_add_event_cb(list, on_list_drawn, LV_EVENT_DRAW_POST, nullptr);
  lv_obj_add_event_cb(list, sheets_scroll_with_finger, LV_EVENT_SCROLL_BEGIN,
                      nullptr);
  return list;
}

lv_obj_t *ui_list_row(lv_obj_t *list, const char *text, bool circles) {
  const Geometry &g = *state_of(list)->geometry;
  lv_obj_t *row = lv_obj_create(list);
  lv_obj_set_size(row, g.row_w, g.row_h);
  lv_obj_set_style_bg_color(row, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(row, ROW_OPA, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(row, g.row_h / 2, LV_PART_MAIN);
  // The theme's card padding would push the circles off their concentric
  // spots; every inset is measured from the row's real edge.
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(
      row, ROW_PRESSED_OPA,
      static_cast<lv_style_selector_t>(LV_PART_MAIN | LV_STATE_PRESSED));

  lv_obj_t *label = lv_label_create(row);
  lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, g.font, LV_PART_MAIN);
  lv_label_set_text(label, text);
  // Fixed size + DOT: long text must ellipsize on one line, or the label
  // wraps and bursts out of the row.
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  int x = circles ? LIST_ROW_TEXT_X : text_inset(g.row_h);
  lv_obj_set_size(label, g.row_w - 2 * x, g.text_h);
  lv_obj_align(label, LV_ALIGN_LEFT_MID, x, 0);
  return row;
}

lv_obj_t *ui_list_trailing(lv_obj_t *row, const char *text) {
  const Geometry &g = *state_of(lv_obj_get_parent(row))->geometry;
  lv_obj_t *label = lv_label_create(row);
  lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, g.font, LV_PART_MAIN);
  lv_label_set_text(label, text);
  int inset = text_inset(g.row_h);
  lv_obj_align(label, LV_ALIGN_RIGHT_MID, -inset, 0);
  // The row's text gives way to the value as wide as it really is, so it
  // only shortens when the two can't both fit.
  lv_obj_update_layout(label);
  lv_obj_t *name = lv_obj_get_child(row, 0);
  int32_t room = g.row_w - inset - lv_obj_get_width(label) - TRAILING_GAP -
                 lv_obj_get_x(name);
  lv_obj_set_width(name, std::min(lv_obj_get_width(name), room));
  return label;
}

int ui_list_count(lv_obj_t *list) {
  return list ? static_cast<int>(lv_obj_get_child_count(list)) : 0;
}

// Focus lifts the row instead of outlining it: one fill change, no border.
// Only the two rows a detent touches are restyled, not the whole list.
void ui_list_focus(lv_obj_t *list, int index) {
  if (!list)
    return;
  ListState *s = state_of(list);
  int count = ui_list_count(list);
  if (s->focus >= 0 && s->focus < count && s->focus != index)
    lv_obj_set_style_bg_opa(lv_obj_get_child(list, s->focus), ROW_OPA,
                            LV_PART_MAIN);
  if (index >= 0 && index < count) {
    lv_obj_t *row = lv_obj_get_child(list, index);
    lv_obj_set_style_bg_opa(row, ROW_FOCUS_OPA, LV_PART_MAIN);
    lv_obj_scroll_to_view(row, LV_ANIM_ON);
  }
  s->focus = index;
}

// Pads are set against the visible ink, not the glyph boxes: Lucide draws
// arrow-left and x 4 px inside their 22 px box, SF 16 leaves about 1 px on
// either side of the text. Ink margins equal the capsule radius, with the
// pointed icon 1 px closer since a tip reads lighter than a stem.
void ui_list_back(lv_obj_t *parent, const char *icon, const char *text,
                  lv_event_cb_t on_tap) {
  constexpr int BACK_H = 40;
  constexpr int BACK_BOTTOM = 37;
  constexpr int ICON_BEARING = 4;
  constexpr int TEXT_LEAD = 1;
  constexpr int TEXT_TRAIL = 1;
  constexpr int ICON_GAP = 6;
  constexpr int HIT_W = 180;
  constexpr int HIT_H = 85; // stops just short of the list's last row
  lv_obj_t *button = lv_obj_create(parent);
  lv_obj_set_size(button, LV_SIZE_CONTENT, BACK_H);
  lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -BACK_BOTTOM);
  lv_obj_set_style_bg_color(button, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, CAPSULE_OPA, LV_PART_MAIN);
  lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(button, BACK_H / 2, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(button, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_left(button, BACK_H / 2 - 1 - ICON_BEARING,
                            LV_PART_MAIN);
  lv_obj_set_style_pad_right(button, BACK_H / 2 - TEXT_TRAIL, LV_PART_MAIN);
  lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(
      button, CAPSULE_PRESSED_OPA,
      static_cast<lv_style_selector_t>(LV_PART_MAIN | LV_STATE_PRESSED));
  lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(button, ICON_GAP - ICON_BEARING - TEXT_LEAD,
                              LV_PART_MAIN);

  lv_obj_t *glyph = lv_label_create(button);
  lv_obj_set_style_text_color(glyph, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(glyph, &lucide_22, LV_PART_MAIN);
  lv_label_set_text(glyph, icon);

  lv_obj_t *label = lv_label_create(button);
  lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, &sf_regular_16, LV_PART_MAIN);
  lv_label_set_text(label, text);

  // The capsule is a small target down by the bezel; take everything below
  // the list instead.
  lv_obj_t *hit = ui_hit_area(parent, button, on_tap);
  lv_obj_set_size(hit, HIT_W, HIT_H);
  lv_obj_align(hit, LV_ALIGN_BOTTOM_MID, 0, 0);
}

bool ui_list_may_close_from(lv_obj_t *list, lv_point_t start, SheetEdge edge) {
  if (!list)
    return true;
  int32_t left = edge == SheetEdge::Bottom ? lv_obj_get_scroll_top(list)
                                           : lv_obj_get_scroll_bottom(list);
  if (left <= 0)
    return true;
  lv_area_t a;
  lv_obj_get_coords(list, &a);
  return !(start.x >= a.x1 && start.x <= a.x2 && start.y >= a.y1 &&
           start.y <= a.y2);
}
