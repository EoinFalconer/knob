#pragma once

#include "lvgl.h"
#include "sheet.h"

// A list on a sheet, the way the speaker picker draws one: a title with a
// hint under it, a column of pill rows, and a Back capsule by the bezel.
//
// Rows sit on the frosted sheet as white veils, like iOS cells on a material,
// so the color behind shows through. The knob moves a focus that lifts one
// row; a tap on a row is the row's own handler.

// A circle nested in a row sits concentric only when its gap to the edge is
// the same on every side: gap = row radius - circle radius
// (cloudfour.com/thinks/the-math-behind-nesting-rounded-corners). Rows with
// circles keep a circle's slot at each end, so text shares one left edge
// whether or not a row has one.
constexpr int LIST_ROW_W = 240;
constexpr int LIST_ROW_H = 50;
constexpr int LIST_ROW_CIRCLE = 28;
constexpr int LIST_ROW_INSET = LIST_ROW_H / 2 - LIST_ROW_CIRCLE / 2;
constexpr int LIST_ROW_TEXT_X = LIST_ROW_INSET + LIST_ROW_CIRCLE + 8;
constexpr lv_opa_t LIST_RING_OPA = 110;

// The title near the top of the sheet, and a quieter line under it. Pass no
// hint for a title alone, sitting a little lower.
void ui_list_title(lv_obj_t *parent, const char *title, const char *hint);

// Regular rows hold a name between circles, like the speaker picker's, under
// a title and a hint, above Back. Under a title alone a list can use the
// hint's room. Large has rows for a few actions, taller, wider and in bigger
// type, above Back. Tall has regular rows and no Back, running on to the
// bezel: it's for a top sheet, which is pushed back up to close, like the
// radio's timer.
enum class ListSize : uint8_t { Regular, Tall, Large };

// The scrolling column the rows go in, centered a touch below the middle. On
// a sheet it fades out at an edge where rows carry on past it, in the sheet's
// material; pass no sheet for a list on a plain screen.
lv_obj_t *ui_list_create(lv_obj_t *parent, lv_obj_t *sheet,
                         ListSize size = ListSize::Regular);

// A pill row with `text`. With `circles`, the text sits between a leading and
// a trailing circle's slot; without, it starts at the pill's own inset. Add a
// handler for LV_EVENT_SHORT_CLICKED; the row's label is its first child.
lv_obj_t *ui_list_row(lv_obj_t *list, const char *text, bool circles = true);
// A value at the row's end, as far in from it as text without circles.
lv_obj_t *ui_list_trailing(lv_obj_t *row, const char *text);

// Lifts the row at `index` and scrolls it into view; -1 lifts none.
void ui_list_focus(lv_obj_t *list, int index);
int ui_list_count(lv_obj_t *list);

// The capsule at the bottom that closes the sheet, with everything below the
// list as its touch target.
void ui_list_back(lv_obj_t *parent, const char *icon, const char *text,
                  lv_event_cb_t on_tap);

// For SheetPolicy::may_close_from: a drag toward the sheet's edge over a list
// that can still scroll that way scrolls it instead of closing the sheet.
bool ui_list_may_close_from(lv_obj_t *list, lv_point_t start,
                            SheetEdge edge = SheetEdge::Bottom);
