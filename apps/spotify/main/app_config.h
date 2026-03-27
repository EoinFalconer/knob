#pragma once

#include "knob_events.h"

// ─── Spotify app event IDs (offset 400+)
enum : int32_t {
    APP_EVENT_SPOTIFY_STATE_UPDATE = 400, // data: SpotifyState*
    APP_EVENT_SPOTIFY_VOLUME_SET   = 401, // data: int32_t (0-100)
    APP_EVENT_SPOTIFY_PLAY         = 402, // no data — request play
    APP_EVENT_SPOTIFY_PAUSE        = 403, // no data — request pause
    APP_EVENT_SPOTIFY_NEXT         = 404, // no data
    APP_EVENT_SPOTIFY_PREV         = 405, // no data
    APP_EVENT_SPOTIFY_DJ_SPIN      = 406, // no data — random liked song
    APP_EVENT_SPOTIFY_SEEK         = 407, // data: int32_t (position_ms)
};

// ─── Playback state from Spotify API
struct SpotifyState {
    char track[128];
    char artist[128];
    char album[128];
    char art_url[256];
    int  volume;          // 0-100
    bool is_playing;
    int  progress_ms;
    int  duration_ms;
};

// ─── Task config
static constexpr int SPOTIFY_POLL_MS       = 3000;
static constexpr int SPOTIFY_TOKEN_MARGIN  = 300; // refresh 5 min before expiry
static constexpr int SPOTIFY_HTTP_TIMEOUT  = 5000;

// ─── UI timing
static constexpr int VOL_DISPLAY_MS        = 1500;
static constexpr int ENCODER_POLL_MS       = 20;
static constexpr int ANIM_FADE_MS          = 200;
static constexpr int BACKLIGHT_NORMAL      = 80;
static constexpr int BACKLIGHT_DIM         = 8;
static constexpr int BACKLIGHT_INACTIVITY_MS = 15000;
