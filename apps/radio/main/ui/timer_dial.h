#pragma once

#include "lvgl.h"

// The timer's face, after Apple's: 60 thick ticks around the bezel, a big
// countdown in the middle and a short orange caption. A full-screen object;
// place nothing else under it.

// Where the digits sit unless moved: the idle face and the sheet must agree,
// so the sheet can fade away over the face without the numbers shifting.
constexpr int TIMER_DIAL_DIGITS_Y = -8;

lv_obj_t *timer_dial_create(lv_obj_t *parent);

// Ticks from `from` to `to` glow orange, counted in ticks from 12 o'clock
// clockwise (0..60); the rest stay dim orange. The ends light partly, so a
// draining timer slides smoothly instead of stepping a tick at a time. Only
// ticks that change are repainted.
void timer_dial_set_ticks(lv_obj_t *dial, float from, float to);

// m:ss, re-centered as the figures change width.
void timer_dial_set_seconds(lv_obj_t *dial, int seconds);

// Vertical position of the digits' ink center, relative to the screen center.
void timer_dial_set_digits_y(lv_obj_t *dial, int y);

// The caption sits under the digits, or over them with `above`. Orange caps
// by default; `muted` makes it a quiet white hint instead.
void timer_dial_set_caption(lv_obj_t *dial, const char *text, bool above,
                            bool muted);
