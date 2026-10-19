#pragma once

#include <cstdint>

enum : int32_t {
  APP_EVENT_LIGHTS_CHANGED = 400, // data: LightsState
};

enum class LightsStatus : uint8_t {
  Unset,      // no gateway configured
  Connecting, // configured, not heard from yet
  Ready,
  Offline, // the gateway didn't answer or refused the knob
};

// All the lights the knob controls, taken together.
struct LightsState {
  LightsStatus status;
  bool on;   // any of them
  int level; // 0..100: how bright the ones that are on are, 0 when all are off
  int count;
};
