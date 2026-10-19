#pragma once

#include "lvgl.h"
#include "weather.h"

// Idle watch faces: a big-numeral analog clock and a weather face that take
// turns while nothing plays.
void faces_create(lv_obj_t *parent);
void faces_show(bool visible);
// The color things drawn over the current face should use, like the volume
// arc: the hands' color on the clock, white on the weather.
lv_color_t faces_accent();
// The knob turned while the faces show: the clock's numerals wobble along.
void faces_nudge(int steps);
// A swipe sideways (-1 left, +1 right) brings up the other face now, rather
// than when it's due; they keep taking turns from there.
void faces_swipe(int direction);
// While a timer runs, the faces show its countdown instead and hold still.
// Without `animate` the face swaps at once, for when something covers it.
void faces_set_timer(bool running, bool animate);
// Decodes the sky picture for this weather if it changed. It takes a while, so
// call it before taking the display lock, then faces_set_weather under it.
void faces_prepare_weather(const WeatherData *data);
void faces_set_weather(const WeatherData *data);
