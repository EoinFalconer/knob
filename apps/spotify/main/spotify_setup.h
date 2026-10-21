#pragma once

/// Run Spotify sign-in: shows a QR code on the display and waits for the
/// code to arrive through the ntfy relay, then saves tokens to NVS.
/// Only needs outbound internet, so it works on client-isolated networks.
/// Blocks until sign-in completes. The argument is unused.
void spotify_setup_start(const char *device_ip);

/// The callback-page URL in the current sign-in QR code, or "" if none.
const char *spotify_setup_url();

/// Returns true if a Spotify refresh token is saved in NVS.
bool spotify_setup_has_token();
