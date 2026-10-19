#pragma once

#include "lights_events.h"
#include "lvgl.h"

// All the lights as one, on the top sheet's second page: a tall slider that
// fills with warm light as they get brighter, like the iOS Home app's. A tap
// anywhere switches them all on or off; the knob dims them.
lv_obj_t *lights_page_create(lv_obj_t *parent);
// Coming into view: ask the gateway how things are.
void lights_page_appear();
void lights_page_show(const LightsState &state);
void lights_page_turn(int steps);
