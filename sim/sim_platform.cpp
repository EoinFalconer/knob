// Host stand-ins for the hardware every knob app's UI calls into.
#include "sim.h"

#include "display.h"
#include "encoder.h"
#include "esp_stub.h"
#include "haptic.h"
#include "knob_events.h"

#include <cstdarg>
#include <cstdio>

ESP_EVENT_DEFINE_BASE(APP_EVENT);

bool sim_verbose = false;

void sim_log(const char *tag, const char *fmt, ...) {
  if (!sim_verbose)
    return;
  va_list args;
  va_start(args, fmt);
  std::fprintf(stderr, "[%s] ", tag);
  std::vfprintf(stderr, fmt, args);
  std::fprintf(stderr, "\n");
  va_end(args);
}

static SimEventHook s_event_hook;
void sim_on_event(SimEventHook hook) { s_event_hook = hook; }

esp_err_t esp_event_post(esp_event_base_t, int32_t id, const void *data,
                         size_t size, int) {
  sim_log("event", "posted %d", static_cast<int>(id));
  if (s_event_hook)
    s_event_hook(id, data, size);
  return ESP_OK;
}

void display_init(lv_display_t **disp, lv_indev_t **touch) {
  *disp = sim_display();
  *touch = sim_touch();
}
static uint8_t s_backlight;
void display_set_backlight(uint8_t level) { s_backlight = level; }
uint8_t sim_backlight() { return s_backlight; }
bool display_lock(int) { return true; }
void display_unlock() {}

static int32_t s_steps;
void sim_turn(int32_t steps) { s_steps += steps; }
void encoder_init() {}
int32_t encoder_take_steps() {
  int32_t s = s_steps;
  s_steps = 0;
  return s;
}

void haptic_init() {}
void haptic_buzz() {}
void haptic_buzz_double() {}
void haptic_buzz_triple() {}
