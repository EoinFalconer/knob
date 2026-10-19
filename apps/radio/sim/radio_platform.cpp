// Host stand-ins for the services the radio UI calls into.
#include "radio_sim.h"

#include "lights.h"
#include "settings.h"
#include "sonos.h"
#include "timer.h"
#include "ui/ui.h"

#include <cstdio>
#include <cstring>

// ─── Settings ───────────────────────────────────────────────────────────────

static char s_speaker_name[64] = "Living Room";
int settings_get_volume() { return 32; }
void settings_set_volume(int) {}
int settings_get_station_index() { return 2; }
void settings_set_station_index(int) {}
void settings_get_speaker_ip(char *buf, size_t len) {
  snprintf(buf, len, "10.0.0.10");
}
void settings_set_speaker_ip(const char *) {}
void settings_get_speaker_name(char *buf, size_t len) {
  snprintf(buf, len, "%s", s_speaker_name);
}
void settings_set_speaker_name(const char *name) {
  snprintf(s_speaker_name, sizeof(s_speaker_name), "%s", name);
}
bool settings_has_speaker() { return true; }

// ─── Sonos (recorded, never sent) ───────────────────────────────────────────

void sonos_play() { sim_log("sonos", "play"); }
void sonos_pause() { sim_log("sonos", "pause"); }
void sonos_next() { sim_log("sonos", "next"); }
void sonos_previous() { sim_log("sonos", "previous"); }
void sonos_set_volume(int level) { sim_log("sonos", "volume %d", level); }
void sonos_select_speaker(const char *ip, int, const char *) {
  sim_log("sonos", "select %s", ip);
}
void sonos_join_group(const char *ip, int) { sim_log("sonos", "join %s", ip); }
void sonos_leave_group(const char *ip, int) {
  sim_log("sonos", "leave %s", ip);
}

int sonos_fetch_art(const char *url, uint8_t *buf, int buf_size) {
  FILE *f = std::fopen(url, "rb");
  if (!f)
    return 0;
  int n =
      static_cast<int>(std::fread(buf, 1, static_cast<size_t>(buf_size), f));
  std::fclose(f);
  return n;
}

// ─── Timer ──────────────────────────────────────────────────────────────────

// knob_timer posts events that main.cpp forwards to the UI; here the UI hears
// back at once. Scenarios fire the timer themselves with ui_timer_fired.
bool timer_start(int seconds, const char *label) {
  ui_timer_started(seconds, static_cast<uint32_t>(seconds) * 1000, label);
  return true;
}
bool timer_cancel() {
  ui_timer_cancelled();
  return true;
}

// ─── Lights ─────────────────────────────────────────────────────────────────

// The gateway answers at once. Switched back on, the lights come back at the
// level they had.
LightsState sim_lights = {LightsStatus::Ready, true, 60, 7};
static int s_sim_lights_last = 60;

static void sim_lights_changed() {
  if (sim_lights.on)
    s_sim_lights_last = sim_lights.level;
  ui_set_lights(&sim_lights);
}

void lights_refresh() { sim_lights_changed(); }
void lights_set_on(bool on) {
  sim_lights.on = on;
  sim_lights.level = on ? s_sim_lights_last : 0;
  sim_lights_changed();
}
void lights_set_level(int level) {
  sim_lights.on = level > 0;
  sim_lights.level = level;
  sim_lights_changed();
}
LightsState lights_state() { return sim_lights; }

bool sim_alarm_sounding = false;
void sonos_start_alarm(const char *, const char *) {
  sim_alarm_sounding = true;
}
void sonos_stop_alarm() { sim_alarm_sounding = false; }
