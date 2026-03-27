# Spotify Knob — Specification

A Spotify remote controller for the ESP32-S3 Knob. Shows what's currently playing on your Spotify account, with physical controls for volume, track navigation, and a "DJ spin" shuffle feature.

## Core Features

### Now Playing Display
- Album art fetched as JPEG, decoded to RGB565, displayed via LVGL
- Track name, artist, album text overlaid on screen
- Polls Spotify Web API `/v1/me/player` every 3 seconds for current playback state
- Smooth progress bar interpolation between polls

### Volume Control
- Encoder turn adjusts volume on the active Spotify device
- Green volume arc overlay, fades out after 1.5s to show progress arc
- 200ms debounce before sending API call; local volume takes priority for 5s after change
- API: `PUT /v1/me/player/volume?volume_percent={vol}`

### Playback Controls
- Tap center: play/pause toggle (optimistic UI update)
- API: `PUT /v1/me/player/pause`, `PUT /v1/me/player/play`

### Seek Control (Hold Screen + Turn)
- Holding finger on screen enters seek-ready mode (art dims, progress arc turns green)
- Slow encoder turn while holding = seek within current track (3 seconds per click)
- A "+6s" / "-3s" offset label shows how far you're seeking
- Releasing finger commits the seek position to Spotify
- 300ms debounce before sending API call; seek commands are coalesced like volume
- API: `PUT /v1/me/player/seek?position_ms={ms}`

### Track Skip (Hold Screen + Flick)
- While holding screen, a fast encoder flick (3+ steps in one poll) triggers a track skip
- Direction determines next (clockwise) or previous (counter-clockwise)
- 2-second cooldown between skips
- Skip overrides any in-progress seek
- API: `POST /v1/me/player/next`, `POST /v1/me/player/previous`

### DJ Spin (Random Liked Song)
- Triggered via `APP_EVENT_SPOTIFY_DJ_SPIN` event
- Haptic buzz feedback
- Fetches total liked songs count, picks random offset, plays that track
- API: `GET /v1/me/tracks`, `PUT /v1/me/player/play` with track URI

## Authentication

Spotify OAuth 2.0 with PKCE. The ESP32 cannot run a browser, so a one-time script handles auth on your laptop.

### Setup
1. Create a Spotify app at https://developer.spotify.com/dashboard
   - Set redirect URI to `http://127.0.0.1:8888/callback`
   - Enable "Web API"
2. Run the included auth script:
   ```bash
   python3 scripts/get_token.py YOUR_CLIENT_ID
   ```
3. Copy `sdkconfig.defaults.local.template` to `sdkconfig.defaults.local` and fill in credentials.

### Runtime Token Flow
- On boot: uses Kconfig refresh token to get an access token via `POST https://accounts.spotify.com/api/token`
- Spotify may rotate the refresh token — new tokens are persisted to NVS automatically
- On subsequent boots, NVS token is preferred over the Kconfig default
- Auto-refresh 5 minutes before expiry (`SPOTIFY_TOKEN_MARGIN = 300s`)
- 401 responses trigger an immediate token refresh + single retry

## Architecture

```
apps/spotify/
  main/
    main.cpp              — boot, event wiring, cmd_task queue
    app_config.h          — event IDs, SpotifyState struct, timing constants
    wifi_picker.cpp/h     — on-device WiFi network picker (scan, list, select)
    wifi_setup.cpp/h      — captive portal for adding new WiFi networks
    spotify_setup.cpp/h   — on-device Spotify OAuth setup (PKCE, QR code)
    spotify/
      json_parse.cpp/h    — shared JSON parser and HTTP response accumulator
      spotify_api.cpp/h   — HTTP client for Spotify Web API + polling task
      spotify_auth.cpp/h  — OAuth token refresh + NVS persistence
    ui/
      ui.cpp/h            — LVGL screen layout, encoder/touch handling, animations
```

### Command Task Pattern
All Spotify API calls run on a dedicated `cmd_task` (8KB stack, pinned to core 1) via a FreeRTOS queue. This keeps HTTP+TLS off the event loop and UI task. Volume and seek commands are coalesced — only the latest value of each is sent.

### Event Flow
```
encoder/touch -> esp_event -> main.cpp handlers -> cmd_queue -> cmd_task -> spotify_api
spotify_api poll_task -> esp_event(STATE_UPDATE) -> ui_update_state
```

### Shared Components
- `knob_hal` — display, touch, encoder, haptic
- `knob_net` — WiFi, event bus
- `knob_storage` — NVS
- `knob_ui` — fonts, art decoder

## UI States

| State           | Display                                   | Encoder              | Touch                     |
|-----------------|-------------------------------------------|----------------------|---------------------------|
| Splash          | Pulsing logo + spinning ring + tagline    | —                    | —                         |
| WiFi picker     | Scrollable network list                   | Scroll list          | Tap: select network       |
| WiFi setup      | QR code for captive portal                | —                    | —                         |
| Spotify setup   | QR code for OAuth                         | —                    | —                         |
| Connecting      | Spotify logo + status text                | —                    | —                         |
| No playback     | Spotify logo + "Play something"           | —                    | —                         |
| Now playing     | Album art + track/artist + progress       | Volume adjust        | Tap: play/pause           |
| Volume overlay  | Green arc (1.5s), hides progress          | Volume adjust        | Tap: play/pause           |
| Seek mode       | Dimmed art + green progress + offset label | Seek fwd/back       | Hold: seek, release: commit |
| Paused          | Dark scrim + "| |" over art              | Volume adjust        | Tap: resume               |

Backlight dims to 8% after 15s of inactivity, restores on any input.
