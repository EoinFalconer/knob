#pragma once

#include "app_config.h"
#include "discovery.h"
#include "weather.h"

void ui_init();

void ui_set_volume(int level);
void ui_set_play_state(PlayState state);
void ui_set_transport_actions(uint8_t actions);
void ui_set_group(const SonosGroup *group);
void ui_set_media_info(const MediaInfo *info);
void ui_set_station(int index);
void ui_set_wifi_status(bool connected);
void ui_set_speaker_name(const char *name);
void ui_set_weather(const WeatherData *data);

// knob_timer's events, from any task.
// `left_ms` is under the full `total_s` for a timer restored after a reboot.
void ui_timer_started(int total_s, uint32_t left_ms, const char *label);
void ui_timer_fired(const char *label);
void ui_timer_cancelled();
// knob_lights' state, from any task.
void ui_set_lights(const LightsState *state);

void ui_show_speaker_picker(const DiscoveryResult *speakers);
void ui_show_scanning();

void ui_on_encoder_rotate(int32_t steps);
void ui_on_touch_tap();
void ui_on_touch_long_press();
