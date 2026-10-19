#pragma once

#include "lvgl.h"

#include <cstdint>

// The volume gauge around the bezel, shared by every home surface. Callers
// describe how it should look; the ring animates there on its own.
struct RingLook {
  lv_color_t color;
  lv_opa_t indicator_opa;
  lv_opa_t track_opa; // the track is `color` at this opacity
  uint8_t width;
  bool shown; // a hidden ring retracts into 12 o'clock
};

// How it looks at rest: thin, the level soft and the track faint.
constexpr uint8_t RING_W_REST = 4;
constexpr lv_opa_t RING_LEVEL_OPA_REST = 110;
constexpr lv_opa_t RING_TRACK_OPA = 28;
constexpr RingLook ring_rest_look(lv_color_t color) {
  return {color, RING_LEVEL_OPA_REST, RING_TRACK_OPA, RING_W_REST, true};
}

void volume_ring_create(lv_obj_t *parent, int32_t min, int32_t max,
                        int32_t level);
void volume_ring_set_level(int32_t level);
void volume_ring_set_look(const RingLook &look, uint32_t duration_ms);
lv_obj_t *volume_ring_obj();
