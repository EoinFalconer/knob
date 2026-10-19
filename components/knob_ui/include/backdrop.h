#pragma once

#include "lvgl.h"

// The screen's color behind everything, changed with a crossfade. It's two
// solid layers, the front fading in with the new color: a solid fill costs
// next to nothing, and a small opacity step per frame hides any tear from
// the partial draw buffer. One per app; creating it again replaces it.
void backdrop_create(lv_obj_t *parent, lv_color_t color);
void backdrop_set(lv_color_t color, bool animate);
// Ends a crossfade under way at its color, for when something covers it.
void backdrop_finish();
