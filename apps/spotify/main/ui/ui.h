#pragma once

#include "app_config.h"

/// Initialize the UI (call after display_init).
void ui_init();

/// Update the status message shown before playback starts.
void ui_set_status(const char *msg);

/// Update UI with new Spotify state (call from event handler).
void ui_update_state(const SpotifyState *state);

/// Show volume overlay for a brief moment.
void ui_show_volume(int volume);

/// Show WiFi setup screen with QR code to join the AP.
void ui_show_wifi_setup(const char *ap_name);

/// Show Spotify setup screen with QR code pointing to the setup URL.
void ui_show_spotify_setup(const char *device_ip);
