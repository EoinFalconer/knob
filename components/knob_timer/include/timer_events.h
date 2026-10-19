#pragma once

#include <cstdint>

enum : int32_t {
  APP_EVENT_TIMER_STARTED = 300,   // data: TimerStarted
  APP_EVENT_TIMER_FIRED = 301,     // data: null-terminated label string
  APP_EVENT_TIMER_CANCELLED = 302, // no data
};

// A timer restored after a reboot has less left than it was set for.
struct TimerStarted {
  int32_t total_s;
  uint32_t left_ms;
};
