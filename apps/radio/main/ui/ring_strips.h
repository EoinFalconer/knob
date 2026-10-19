#pragma once

#include "lvgl.h"

// lv_area_intersect is only declared in the private header.
#include "src/misc/lv_area_private.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

// Calls `draw` with the layer clipped to each strip of the ring between
// `inner` and `outer` (radii around `center`) that the repaint touches: per
// block of rows, the stretch left and right of the hole, or one across it
// near the top and bottom. A screen-sized picture of something that only
// lives in the ring then never blits the empty middle.
template <typename Draw>
void ui_for_each_ring_strip(lv_layer_t *layer, lv_point_t center, float outer,
                            float inner, int32_t block_rows, Draw draw) {
  auto half_chord = [](float r, int32_t dy) {
    return dy >= r ? 0 : static_cast<int32_t>(std::sqrt(r * r - dy * dy));
  };
  const lv_area_t clip = layer->_clip_area;
  auto reach = static_cast<int32_t>(std::ceil(outer));
  int32_t cx = center.x, cy = center.y;
  for (int32_t y = cy - reach; y <= cy + reach; y += block_rows) {
    int32_t y2 = std::min(y + block_rows - 1, cy + reach);
    if (y2 < clip.y1 || y > clip.y2)
      continue;
    int32_t near =
        y <= cy && y2 >= cy ? 0 : std::min(std::abs(y - cy), std::abs(y2 - cy));
    int32_t far = std::max(std::abs(y - cy), std::abs(y2 + 1 - cy));
    int32_t wo = half_chord(outer, near) + 1;
    int32_t wi = std::max<int32_t>(0, half_chord(inner, far) - 1);
    lv_area_t strips[2] = {{cx - wo, y, cx - wi - 1, y2},
                           {cx + wi, y, cx + wo, y2}};
    int count = wi > 0 ? 2 : 1;
    if (count == 1)
      strips[0] = {cx - wo, y, cx + wo, y2};
    for (int i = 0; i < count; i++)
      if (lv_area_intersect(&layer->_clip_area, &clip, &strips[i]))
        draw();
  }
  layer->_clip_area = clip;
}
