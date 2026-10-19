#pragma once

// A headless knob: runs an app's real UI against LVGL with the firmware's
// configuration, writes screenshots, and reports how much of the panel each
// interaction repaints. Each app supplies sim_scenarios() and stand-ins for
// its own services.

#include "lvgl.h"

#include <cstddef>
#include <cstdint>

// Called with the app's argv[1], usually a picture to stand in for artwork.
void sim_scenarios(const char *asset_path);

lv_display_t *sim_display();
lv_indev_t *sim_touch();

// Advance simulated time, running LVGL timers and rendering as on device.
void sim_run(uint32_t ms);

// Touch at (x, y) in display coordinates; `hold_ms` before release.
void sim_tap(int32_t x, int32_t y, uint32_t hold_ms = 60);
void sim_swipe(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
// A finger in pieces, for drags that stop, hold or get screenshotted midway.
// `sim_touch_move` glides from the current point at the touch read rate.
void sim_touch_down(int32_t x, int32_t y);
void sim_touch_move(int32_t x, int32_t y, uint32_t ms);
void sim_touch_up();
void sim_turn(int32_t steps);

void sim_screenshot(const char *name);
// The level the UI last gave the panel's backlight.
uint8_t sim_backlight();

// Redraw accounting between two marks: how much the panel had to repaint.
void sim_mark(const char *label);

extern bool sim_verbose;

// Hears every esp_event_post, for apps whose UI asks for work by event.
using SimEventHook = void (*)(int32_t id, const void *data, size_t size);
void sim_on_event(SimEventHook hook);
