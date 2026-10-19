#pragma once

#include <cstdint>

static constexpr int WEATHER_SYMBOL_LEN = 40;
static constexpr int SUN_TIME_UNKNOWN = -1;

struct WeatherData {
  float temperature;
  float wind_speed; // m/s
  float humidity;   // %
  float high;       // over the next 24 h
  float low;
  char condition[WEATHER_SYMBOL_LEN]; // unknown codes show as themselves
  char symbol[WEATHER_SYMBOL_LEN];    // met.no symbol code, e.g. "rain_day"
  // Many codes ("cloudy", "rain") don't say whether the sun is up, so this
  // comes from the nearest code that does.
  bool night;
  // Minutes after local midnight, or SUN_TIME_UNKNOWN (polar day and night,
  // or the sun lookup failed).
  int sunrise = SUN_TIME_UNKNOWN;
  int sunset = SUN_TIME_UNKNOWN;
  bool valid;
};

// Weather event ID (post with WeatherData payload)
enum : int32_t {
  APP_EVENT_WEATHER_UPDATE = 400,
};

// Initialize weather module (call once at startup)
void weather_init();

// Start periodic weather fetching (call after WiFi connects)
void weather_start();
