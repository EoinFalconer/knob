#pragma once

#include "lvgl.h"

// iOS's dark material, from a snapshot of `screen`: blurred hard, a little more
// saturated, and darkened so white text reads on any face.
//
// Frosting is split so the slow part overlaps whatever comes next: the
// snapshot is taken by frost_begin, on the UI task, and the blur runs on the
// other core. frost_take waits for whatever is left of it. The image is the
// size of the screen; two are kept, so a sheet still showing the last one
// isn't drawn over while the next is made.

// Returns false without memory.
bool frost_begin(lv_obj_t *screen);
// The image from the last frost_begin, or nullptr if there is none.
const lv_image_dsc_t *frost_take();
