#pragma once

#include "app_config.h"

/// Initialize the display + UI. Call once at boot.
void ui_init();

/// Show the splash ("kaffi") screen.
void ui_show_splash();

/// Show the captive-portal QR screen (join instructions for `ap_name`).
void ui_show_wifi_setup(const char *ap_name);

/// First-boot office picker. Blocks the calling task until the user taps an
/// office; returns its index into KAFFI_OFFICES. Not for the LVGL task.
int ui_pick_office();

/// Show a status message (connecting, errors, …).
void ui_set_status(const char *msg);

/// Network pill on the list screen: shown while offline and/or while taps are
/// queued for delivery; hidden when online with nothing pending.
/// Safe to call from any task.
void ui_set_offline(bool offline, int pending);

/// Replace the cached leaderboard and refresh the view.
/// Safe to call from any task (locks the display internally).
void ui_update_people(const KaffiState *state);

/// Hand a decoded avatar (RGB565_SWAPPED) to the UI, matched to a person by
/// image URL (stable across reordering). pixels=null marks a failed fetch.
/// The UI takes ownership of `pixels` and frees it. Safe to call from any task.
void ui_set_avatar(const char *url, unsigned char *pixels, int w, int h);
