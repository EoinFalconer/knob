#pragma once

#include <cstdint>

// Colors for the clock face. They follow the sun: warm after sunrise, green
// in daytime, rose in the hours before sunset, and dark at night so the face
// doesn't light up a bedroom.
struct FacePalette {
  uint32_t sky;
  uint32_t ink;  // numerals and date
  uint32_t hand; // hands and center cap
};

enum class DayPart { Morning, Day, Evening, Night };

struct SunClock {
  int sunrise = -1; // minutes after local midnight, -1 if unknown
  int sunset = -1;
  const bool *sun_down = nullptr; // from the weather symbols, if any
};

DayPart day_part(int minute_of_day, const SunClock &sun);
FacePalette face_palette(DayPart part);
