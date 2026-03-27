#pragma once

#include "app_config.h"

/// Initialize Spotify API module. Call after auth init + WiFi connected.
void spotify_api_init();

/// Start the polling task that fetches playback state every SPOTIFY_POLL_MS.
void spotify_api_start();

/// Stop the polling task.
void spotify_api_stop();

/// Set volume on the active Spotify device (0-100).
void spotify_api_set_volume(int volume);

/// Toggle play/pause.
void spotify_api_play_pause(bool currently_playing);

/// Skip to next track.
void spotify_api_next();

/// Skip to previous track.
void spotify_api_prev();

/// Seek to position in current track (milliseconds).
void spotify_api_seek(int position_ms);

/// Play a random liked song (DJ spin feature).
void spotify_api_play_random_liked();

/// Fetch album art JPEG from a URL into buf. Returns bytes written, or 0.
int spotify_api_fetch_art(const char *url, uint8_t *buf, int buf_size);
