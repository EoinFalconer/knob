#pragma once

/// Start local HTTP server for Spotify OAuth setup.
/// Shows QR code on display. Blocks until auth completes.
/// device_ip is the device's IP on the local network.
void spotify_setup_start(const char *device_ip);

/// Returns true if a Spotify refresh token is saved in NVS.
bool spotify_setup_has_token();
