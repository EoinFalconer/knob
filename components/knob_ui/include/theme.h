#pragma once

#include "lvgl.h"

#include <cstdint>

// The knobs' colors, in one place so apps can't drift apart: Apple's
// dark-mode system colors, and how white sits on color. Secondary text is
// white at reduced opacity rather than a fixed gray, so it picks up the color
// behind it, like vibrancy on iOS.

constexpr uint32_t COL_GREEN = 0x30D158;
constexpr uint32_t COL_ORANGE = 0xFF9F0A;
constexpr uint32_t COL_RED = 0xFF453A;
constexpr uint32_t COL_YELLOW = 0xFFD60A;
constexpr uint32_t COL_GRAY = 0x8E8E93; // text that stays gray on any color
// A tile with nothing to show yet, like Apple Music's empty cover.
constexpr uint32_t COL_PLACEHOLDER = 0x2C2C2E;

constexpr lv_opa_t TEXT_OPA_SOFT =
    LV_OPA_80; // pills; a subtitle while browsing
constexpr lv_opa_t TEXT_OPA_SECONDARY = LV_OPA_60;
constexpr lv_opa_t TEXT_OPA_TERTIARY = LV_OPA_50;

// White veils: a pill holding a line, and a capsule button.
constexpr lv_opa_t PILL_OPA = LV_OPA_10;
constexpr lv_opa_t PILL_PRESSED_OPA = LV_OPA_20;
constexpr lv_opa_t CAPSULE_OPA = 38;
constexpr lv_opa_t CAPSULE_PRESSED_OPA = 70;
