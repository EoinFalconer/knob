#include "ui/face_palette.h"

namespace {

constexpr int HOUR = 60;
constexpr int MORNING_LENGTH = 3 * HOUR; // after sunrise
constexpr int GOLDEN_HOURS = 2 * HOUR;   // before sunset
// Late enough that the face goes dark even when a northern summer keeps the
// sun up.
constexpr int BEDTIME = 23 * HOUR;
constexpr int WAKE = 5 * HOUR;
// Rough sun times for when the real ones are unknown.
constexpr int SUNRISE_GUESS = 6 * HOUR;
constexpr int SUNSET_GUESS = 21 * HOUR;

} // namespace

DayPart day_part(int minute, const SunClock &sun) {
  if (minute >= BEDTIME || minute < WAKE)
    return DayPart::Night;

  bool known = sun.sunrise >= 0 && sun.sunset > sun.sunrise;
  int sunrise = known ? sun.sunrise : SUNRISE_GUESS;
  int sunset = known ? sun.sunset : SUNSET_GUESS;
  bool night = (!known && sun.sun_down)
                   ? *sun.sun_down
                   : (minute < sunrise || minute >= sunset);
  if (night)
    return DayPart::Night;
  if (minute < sunrise + MORNING_LENGTH)
    return DayPart::Morning;
  if (minute >= sunset - GOLDEN_HOURS)
    return DayPart::Evening;
  return DayPart::Day;
}

FacePalette face_palette(DayPart part) {
  switch (part) {
  case DayPart::Morning:
    return {0xF0AE62, 0x2E1C0A, 0xFFFFFF};
  case DayPart::Day:
    // Apple's muted sage, not a saturated mint.
    return {0x58935F, 0x0A0A0A, 0xFFFFFF};
  case DayPart::Evening:
    return {0xD99A9E, 0x4A1E26, 0x5E1F2C};
  case DayPart::Night:
    // Dim on purpose: low contrast reads fine at 200 px and stays dark.
    return {0x141A2C, 0x4E5B82, 0xAEB8D6};
  }
  return face_palette(DayPart::Day);
}
