#pragma once

#include "lvgl.h"

// iOS-style activity indicator: eight spokes with a bright one stepping
// clockwise. Stops by itself when its parent is deleted.
lv_obj_t *activity_create(lv_obj_t *parent);

// The spinner a little above center with a line under it, for a screen
// that waits: "Scanning for speakers…", "Loading people…". Returns the
// line, in SF 16 at secondary opacity.
lv_obj_t *ui_activity_message(lv_obj_t *parent, const char *text);
