#pragma once

#include <cstddef>

// The timer's chime, served to the speaker over HTTP from the knob itself.
// Writes the URL a Sonos player can fetch it from, or returns false without
// Wi-Fi or memory. The server starts on first use and stays up.
bool alarm_sound_url(char *out, size_t out_len);
