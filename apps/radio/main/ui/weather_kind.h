#pragma once

// What a met.no symbol code ("lightrainshowers_day") looks like, which is all
// the faces need to pick a sky and an icon.
enum class WeatherKind {
  Clear,
  Fair,
  PartlyCloudy,
  Cloudy,
  Fog,
  Drizzle,
  Rain,
  Showers,
  Thunder,
  Snow,
  SnowShowers,
  Sleet,
};

WeatherKind weather_kind(const char *symbol);

// True for "_night" codes. Codes without a suffix ("rain") say nothing about
// the sun, so they get the fallback.
bool weather_is_night(const char *symbol, bool fallback);
