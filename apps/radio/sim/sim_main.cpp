// Headless simulator: runs the real radio UI against LVGL with the firmware's
// configuration, writes screenshots, and reports how much of the panel each
// interaction repaints (the cost that matters on the ESP32-S3).
#include "sim.h"

#include "hal_pins.h"
#include "sonos_config.h"
#include "ui/ui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Same partial buffer as the device (display_idf.cpp: LCD_DRAW_ROWS rows).
static constexpr int DRAW_ROWS = 36;
static constexpr uint32_t STEP_MS = 5;

static uint32_t s_now;
static lv_display_t *s_disp;
static lv_indev_t *s_touch;
static uint16_t
    s_frame[LCD_H_RES * LCD_V_RES]; // RGB565, swapped like the panel
static const char *s_out_dir = "out";

struct Touch {
  bool down;
  lv_point_t point;
};
static Touch s_touch_state;

struct Cost {
  uint64_t pixels;
  uint32_t frames;
  uint32_t flushes;
  double render_ms;
};
static Cost s_cost;
static bool s_in_frame;
static std::chrono::steady_clock::time_point s_frame_start;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
  auto *src = reinterpret_cast<uint16_t *>(px);
  int32_t w = lv_area_get_width(area);
  for (int32_t y = area->y1; y <= area->y2; y++)
    memcpy(&s_frame[y * LCD_H_RES + area->x1], src + (y - area->y1) * w,
           static_cast<size_t>(w) * 2);
  s_cost.pixels += static_cast<uint64_t>(lv_area_get_size(area));
  s_cost.flushes++;
  lv_display_flush_ready(disp);
}

static void render_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
    s_in_frame = true;
    s_frame_start = std::chrono::steady_clock::now();
  } else if (lv_event_get_code(e) == LV_EVENT_RENDER_READY && s_in_frame) {
    s_in_frame = false;
    s_cost.frames++;
    s_cost.render_ms += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - s_frame_start)
                            .count();
  }
}

static void touch_read_cb(lv_indev_t *, lv_indev_data_t *data) {
  data->point = s_touch_state.point;
  data->state =
      s_touch_state.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

lv_display_t *sim_display() { return s_disp; }
lv_indev_t *sim_touch() { return s_touch; }

void sim_run(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += STEP_MS) {
    s_now += STEP_MS;
    lv_timer_handler();
  }
}

void sim_tap(int32_t x, int32_t y, uint32_t hold_ms) {
  if (sim_verbose) {
    lv_point_t p = {x, y};
    lv_obj_t *hit = lv_indev_search_obj(lv_screen_active(), &p);
    lv_area_t a = {};
    if (hit)
      lv_obj_get_coords(hit, &a);
    std::printf("tap (%d,%d) lands on %p (%d,%d %dx%d) screen=%p events=%u\n",
                static_cast<int>(x), static_cast<int>(y),
                static_cast<void *>(hit), static_cast<int>(a.x1),
                static_cast<int>(a.y1), static_cast<int>(lv_area_get_width(&a)),
                static_cast<int>(lv_area_get_height(&a)),
                static_cast<void *>(lv_screen_active()),
                hit ? lv_obj_get_event_count(hit) : 0u);
  }
  s_touch_state = {true, {x, y}};
  sim_run(hold_ms);
  s_touch_state.down = false;
  sim_run(40);
}

void sim_swipe(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  s_touch_state = {true, {x0, y0}};
  sim_run(30);
  for (int i = 1; i <= 6; i++) {
    s_touch_state.point = {x0 + (x1 - x0) * i / 6, y0 + (y1 - y0) * i / 6};
    sim_run(15);
  }
  s_touch_state.down = false;
  sim_run(40);
}

void sim_touch_down(int32_t x, int32_t y) {
  s_touch_state = {true, {x, y}};
  sim_run(20);
}

void sim_touch_move(int32_t x, int32_t y, uint32_t ms) {
  constexpr uint32_t STEP = 10;
  lv_point_t from = s_touch_state.point;
  uint32_t steps = std::max<uint32_t>(1, ms / STEP);
  for (uint32_t i = 1; i <= steps; i++) {
    auto at = [&](int32_t a, int32_t b) {
      return a +
             (b - a) * static_cast<int32_t>(i) / static_cast<int32_t>(steps);
    };
    s_touch_state.point = {at(from.x, x), at(from.y, y)};
    sim_run(STEP);
  }
}

void sim_touch_up() {
  s_touch_state.down = false;
  sim_run(20);
}

// The panel is round: anything outside the circle is never seen.
// A clickable object with no handler that doesn't pass events up swallows
// every tap that lands on it: the gesture never reaches the screen's router.
// LVGL makes every lv_obj_create() clickable, so this catches the slip.
static int s_dead_targets;

static void find_dead_targets(lv_obj_t *obj, const char *shot, bool is_root) {
  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN))
    return;
  if (!is_root && lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) &&
      !lv_obj_has_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE) &&
      lv_obj_get_event_count(obj) == 0) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    std::printf("FAIL %s: dead tap target at (%d,%d %dx%d)\n", shot,
                static_cast<int>(a.x1), static_cast<int>(a.y1),
                static_cast<int>(lv_area_get_width(&a)),
                static_cast<int>(lv_area_get_height(&a)));
    s_dead_targets++;
  }
  for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
    find_dead_targets(lv_obj_get_child(obj, static_cast<int32_t>(i)), shot,
                      false);
}

void sim_screenshot(const char *name) {
  find_dead_targets(lv_screen_active(), name, true);
  std::string path = std::string(s_out_dir) + "/" + name + ".ppm";
  FILE *f = std::fopen(path.c_str(), "wb");
  if (!f)
    return;
  std::fprintf(f, "P6\n%d %d\n255\n", LCD_H_RES, LCD_V_RES);
  const int r = LCD_H_RES / 2;
  for (int y = 0; y < LCD_V_RES; y++) {
    for (int x = 0; x < LCD_H_RES; x++) {
      uint16_t v = s_frame[y * LCD_H_RES + x];
      v = static_cast<uint16_t>((v >> 8) | (v << 8));
      uint8_t rgb[3] = {static_cast<uint8_t>((v >> 11) << 3),
                        static_cast<uint8_t>(((v >> 5) & 0x3F) << 2),
                        static_cast<uint8_t>((v & 0x1F) << 3)};
      int dx = x - r, dy = y - r;
      if (dx * dx + dy * dy > r * r)
        rgb[0] = rgb[1] = rgb[2] = 40;
      std::fwrite(rgb, 1, 3, f);
    }
  }
  std::fclose(f);
}

static std::string s_mark_label;
static constexpr double FULL_FRAME = double(LCD_H_RES) * LCD_V_RES;

static void report(const char *label, const Cost &c) {
  std::printf("  %-28s %4u frames  %6.2f screens  %5.1f%% peak-avg/frame  "
              "%7.2f ms render\n",
              label, c.frames, double(c.pixels) / FULL_FRAME,
              c.frames ? 100.0 * double(c.pixels) / c.frames / FULL_FRAME : 0.0,
              c.render_ms);
}

void sim_mark(const char *label) {
  if (!s_mark_label.empty())
    report(s_mark_label.c_str(), s_cost);
  s_cost = {};
  s_mark_label = label ? label : "";
}

static uint32_t tick_cb() { return s_now; }

static void init_lvgl() {
  lv_init();
  lv_tick_set_cb(tick_cb);

  s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
  lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
  constexpr size_t BUF_BYTES = LCD_H_RES * DRAW_ROWS * 2;
  alignas(64) static uint8_t buf1[BUF_BYTES];
  alignas(64) static uint8_t buf2[BUF_BYTES];
  lv_display_set_buffers(s_disp, buf1, buf2, BUF_BYTES,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(s_disp, flush_cb);
  lv_display_add_event_cb(s_disp, render_event_cb, LV_EVENT_RENDER_START,
                          nullptr);
  lv_display_add_event_cb(s_disp, render_event_cb, LV_EVENT_RENDER_READY,
                          nullptr);

  s_touch = lv_indev_create();
  lv_indev_set_type(s_touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(s_touch, touch_read_cb);
}

void sim_scenarios(const char *art_path);

int main(int argc, char **argv) {
  const char *art = argc > 1 ? argv[1] : "art.jpg";
  if (argc > 2)
    s_out_dir = argv[2];
  sim_verbose = std::getenv("SIM_VERBOSE") != nullptr;

  init_lvgl();
  sim_scenarios(art);
  if (s_dead_targets) {
    std::printf("%d dead tap target(s)\n", s_dead_targets);
    return 1;
  }
  sim_mark(nullptr);
  return 0;
}
