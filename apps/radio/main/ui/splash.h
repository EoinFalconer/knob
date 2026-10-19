#pragma once

#include "lvgl.h"

#include <cstdint>

// The boot screen: KN over OB in SF Pro Rounded Black, as big as the round
// screen holds, on the night clock's sky. Each letter is a little tilted, in
// a color from the clock's day, and they rise into place one after another. It
// plays out in SPLASH_MS, about as long as the rest of the UI takes to build.
constexpr uint32_t SPLASH_MS = 1200;

lv_obj_t *splash_create();
