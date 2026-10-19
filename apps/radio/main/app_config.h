#pragma once

#include "hal_pins.h"
#include "knob_events.h"
#include "lights_events.h"
#include "sonos_config.h"
#include "timer_events.h"
#include "weather.h"

// ─── Radio Stations ─────────────────────────────────────────────────────────

struct Station {
  const char *name;
  const char *url;
  const char *logo;
  uint32_t color;
};

constexpr Station STATIONS[] = {
    {"NRK P1 Oslo",
     "https://cdn0-47115-liveicecast0.dna.contentdelivery.net/p1_mp3_h",
     "nrk_p1.png", 0x061528},
    {"NRK P2",
     "https://cdn0-47115-liveicecast0.dna.contentdelivery.net/p2_aac_h",
     "nrk_p2.png", 0x280514},
    {"NRK P3",
     "https://cdn0-47115-liveicecast0.dna.contentdelivery.net/p3_mp3_h",
     "nrk_p3.png", 0x282305},
    {"NRK MP3",
     "https://cdn0-47115-liveicecast0.dna.contentdelivery.net/mp3_mp3_h",
     "nrk_mp3.png", 0x052819},
    {"NRK Jazz",
     "http://cdn0-47115-liveicecast0.dna.contentdelivery.net/jazz_mp3_h",
     "nrk_jazz.png", 0x150528},
    {"NRK Nyheter",
     "https://cdn0-47115-liveicecast0.dna.contentdelivery.net/nyheter_aac_h",
     "nrk_nyheter.png", 0x051428},
    {"P4 Norge", "https://p4.p4groupaudio.com/P04_MM", "p4.png", 0x280A0B},
    {"P5 Hits", "https://p5.p4groupaudio.com/P05_MM", "p5.png", 0x280506},
    {"P9 Retro", "https://p9.p4groupaudio.com/P09_MH", "p9.png", 0x062818},
    {"Radio Rock", "https://live-bauerno.sharp-stream.com/radiorock_no_mp3",
     "radio_rock.png", 0x282828},
    {"Radio Norge", "https://live-bauerno.sharp-stream.com/radionorge_no_mp3",
     "radio_norge.png", 0x280505},
    {"NRJ Norge", "https://live-bauerno.sharp-stream.com/kiss_no_mp3",
     "nrj.png", 0x280505},
};

constexpr int STATION_COUNT = sizeof(STATIONS) / sizeof(STATIONS[0]);

// The Sonos room a timer rings in, by name, whichever room the knob controls.
// Without a room by this name it rings in the controlled one.
constexpr const char *TIMER_ROOM = "Kitchen";

// Left untouched this long, the screen settles: browse, the speakers sheet and
// a timer being set give up and go back to the faces, and a woken screen dims
// again, unless a timer is running or ringing.
constexpr uint32_t IDLE_MS = 10000;

// ─── Task Config ────────────────────────────────────────────────────────────

constexpr int UI_TASK_STACK = 8192;
constexpr int UI_TASK_PRIO = 5;
constexpr int UI_TASK_CORE = 0;
