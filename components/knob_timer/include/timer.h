#pragma once

#include <cstdint>

// One countdown at a time, set by voice (`set_timer`) or by the app. It runs
// to a deadline on a one-shot esp_timer, so it fires on the millisecond and
// needs no ticking. Starting, cancelling and firing each post an APP_EVENT
// (timer_events.h).

void timer_init();

// The running timer is kept in NVS against the wall clock, so it outlives a
// reboot or a power cut. Call this whenever the clock may have become known
// (on Wi-Fi up, on an SNTP sync); the first call with a set clock brings a
// saved timer back, or rings it if it ran out in the last few minutes.
void timer_restore();

// Replaces any running timer. `seconds` is clamped to 1..3600.
bool timer_start(int seconds, const char *label = "Timer");
bool timer_cancel();

bool timer_is_active();
int timer_remaining_sec(); // rounded up, so it reads 0 only once fired
uint32_t timer_remaining_ms();
void timer_get_label(char *buf, int buf_len);
