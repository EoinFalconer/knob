#include "ui/weather_kind.h"

#include <cstring>

static bool has(const char *s, const char *part) {
  return strstr(s, part) != nullptr;
}

static bool starts(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

// Most specific first: "rainshowersandthunder" is thunder before it is rain,
// and "partlycloudy" contains "cloudy".
WeatherKind weather_kind(const char *s) {
  if (has(s, "thunder"))
    return WeatherKind::Thunder;
  if (has(s, "sleet"))
    return WeatherKind::Sleet;
  if (has(s, "snowshowers"))
    return WeatherKind::SnowShowers;
  if (has(s, "snow"))
    return WeatherKind::Snow;
  if (has(s, "rainshowers"))
    return WeatherKind::Showers;
  if (starts(s, "lightrain"))
    return WeatherKind::Drizzle;
  if (has(s, "rain"))
    return WeatherKind::Rain;
  if (starts(s, "fog"))
    return WeatherKind::Fog;
  if (starts(s, "partlycloudy"))
    return WeatherKind::PartlyCloudy;
  if (starts(s, "cloudy"))
    return WeatherKind::Cloudy;
  if (starts(s, "fair"))
    return WeatherKind::Fair;
  return WeatherKind::Clear;
}

bool weather_is_night(const char *symbol, bool fallback) {
  const char *sep = strchr(symbol, '_');
  if (!sep)
    return fallback;
  return strcmp(sep + 1, "night") == 0;
}
