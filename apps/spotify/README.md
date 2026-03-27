# Spotify Knob

A Spotify remote control for the ESP32-S3 Knob hardware. Displays album art and track info from your active Spotify session, with physical controls for volume, playback, and track skipping.

This is a **remote control only** — it does not play audio locally. It controls whatever Spotify client is currently active (phone, laptop, speaker, etc.) via the Spotify Web API.

## Setup

### 1. Spotify Developer App

1. Go to https://developer.spotify.com/dashboard and create an app.
2. Set the redirect URI to `http://127.0.0.1:8888/callback`.
3. Enable "Web API" under the app settings.
4. Note your **Client ID** (no client secret needed — uses PKCE).

### 2. Get a Refresh Token

```bash
python3 scripts/get_token.py YOUR_CLIENT_ID
```

This opens your browser for Spotify login and prints a refresh token. The token is long-lived; the device will auto-refresh access tokens from it.

### 3. Configure Credentials

```bash
cp sdkconfig.defaults.local.template sdkconfig.defaults.local
```

Edit `sdkconfig.defaults.local` with your WiFi and Spotify credentials. This file is gitignored — never commit it.

## Build & Flash

From the repo root:

```bash
./test.sh spotify          # build + lint
./flash.sh spotify -m      # flash + serial monitor
```

Or from this directory:

```bash
idf.py build
idf.py flash monitor
```

## Controls

| Input                          | Action                        |
|--------------------------------|-------------------------------|
| Turn encoder                   | Adjust volume                 |
| Tap screen                     | Play / pause                  |
| Hold screen + turn slowly      | Seek forward/backward (3s/click) |
| Hold screen + flick encoder    | Skip to next/previous track   |

Volume changes show a green arc overlay for 1.5 seconds. A white progress arc shows song position when volume is hidden.

When holding the screen, the album art dims and the progress arc turns green to indicate seek mode. A "+6s" / "-3s" label shows the seek offset. Releasing your finger commits the seek position.

The backlight dims after 15 seconds of inactivity and restores on any input.

## Boot Flow

1. **Splash screen** — Spotify logo zooms in with a spinning green ring and "every knob deserves music" tagline
2. **WiFi picker** — scans for networks, shows a scrollable list. Saved networks are marked green. Tap to select, or tap "+ Add new network" to open the captive portal
3. **Spotify auth** — if no token is stored, shows a QR code for on-device OAuth setup
4. **Now playing** — connects to Spotify and shows current playback

## Known Limitations

- **Remote control only.** Requires an active Spotify session on another device. If nothing is playing, the knob shows "No active playback."
- **Spotify Premium required** for playback control (play, pause, skip, volume). Free accounts can only view what's playing.
- **Token bootstrap.** A refresh token can be obtained on a computer via `get_token.py`, or configured on-device via the QR code OAuth flow at first boot.
- **No playlist browsing.** The knob shows and controls whatever is currently playing — there is no UI for browsing playlists or search.
- **Album art download.** Fetching album art takes ~0.5-1s over WiFi. A pulsating placeholder is shown during download.
- **Single device.** Controls the currently active Spotify device. If multiple devices are active, Spotify chooses which one responds.
