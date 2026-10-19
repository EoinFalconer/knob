#pragma once

#include "lvgl.h"

// cubic-bezier(0.23, 1, 0.32, 1): moves immediately, settles softly. The
// built-in ease-in-out starts slow, which reads as lag on a physical knob.
inline void motion_ease_out(lv_anim_t *a) {
  lv_anim_set_path_cb(a, lv_anim_path_custom_bezier3);
  lv_anim_set_bezier3_param(a, LV_BEZIER_VAL_FLOAT(0.23),
                            LV_BEZIER_VAL_FLOAT(1), LV_BEZIER_VAL_FLOAT(0.32),
                            LV_BEZIER_VAL_FLOAT(1));
}

// cubic-bezier(0.77, 0, 0.175, 1), for something changing in place on screen.
inline void motion_ease_in_out(lv_anim_t *a) {
  lv_anim_set_path_cb(a, lv_anim_path_custom_bezier3);
  lv_anim_set_bezier3_param(a, LV_BEZIER_VAL_FLOAT(0.77),
                            LV_BEZIER_VAL_FLOAT(0), LV_BEZIER_VAL_FLOAT(0.175),
                            LV_BEZIER_VAL_FLOAT(1));
}
