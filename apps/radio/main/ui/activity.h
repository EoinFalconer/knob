#pragma once

#include "lvgl.h"

// iOS-style activity indicator: eight spokes with a bright one stepping
// clockwise. Stops by itself when its parent is deleted.
lv_obj_t *activity_create(lv_obj_t *parent);
