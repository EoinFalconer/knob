#pragma once

#include "lights_events.h"
#include "lvgl.h"

#include <cstdint>

// The timer, in a sheet that drops from the top edge (swipe down):
// - set: the knob picks the minutes, lighting a tick per minute; a tap
//   anywhere starts it, winds the ring up and sends the sheet away;
// - running: the countdown stays on the now playing screen as a small pill,
//   and on the idle clock as its own face, where the ticks drain from 12
//   o'clock. Swiped back down, the whole sheet is a cancel button; the knob
//   is on volume;
// - ringing: the sheet drops back down on its own, the speaker chimes, and
//   Stop or Snooze answer it.
//
// The countdown itself lives in knob_timer, so voice timers show up here the
// same way: the app forwards its events to the timer_sheet_* calls below.
//
// Swiped sideways, the sheet's second page is the lights (ui/lights_page.h),
// except while the timer rings.

struct TimerSheetHooks {
  bool (*may_open)();   // may a swipe down open it right now?
  bool (*pill_shown)(); // is the now playing screen up, browsing or not?
  void (*wake)();       // light the screen and hold off idle
  // A timer started, ended or rings. Also called when nothing changed, so
  // it must be cheap to repeat.
  void (*changed)();
};

void timer_sheet_create(lv_obj_t *screen, const TimerSheetHooks &hooks);

bool timer_sheet_is_open();
// On screen at all, opening, closing or fading.
bool timer_sheet_is_visible();
void timer_sheet_open();

// The knob while the sheet is open: minutes, or the lights' brightness on
// their page. Returns false when the turn should change the volume as usual.
bool timer_sheet_turn(int steps);
void timer_sheet_set_lights(const LightsState &state);
// Shows the volume briefly while the knob changes it over the sheet.
void timer_sheet_show_volume(int level);

void timer_sheet_started(int total_s, uint32_t left_ms, const char *label);
void timer_sheet_fired(const char *label);
void timer_sheet_cancelled();

bool timer_running();
bool timer_ringing();
uint32_t timer_left_ms();
uint32_t timer_total_ms();
// What the running timer is called: its label, or how long it was set for.
const char *timer_caption();

// The countdown pill for the now playing screen; a tap opens the sheet. It
// shows and updates itself.
void timer_pill_create(lv_obj_t *parent);
