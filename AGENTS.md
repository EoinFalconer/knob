# AGENTS.md — ESP32-S3 Knob Monorepo

Multiple ESP32-S3 knobs, each running a different app. Same hardware, different firmware.

Shared drivers and libraries live in `components/`. Each knob's firmware is a standalone ESP-IDF project under `apps/`. The original product is `apps/radio/` (Sonos internet radio). Use `apps/template/` as a starter when adding a new knob.

**Hardware:** ESP32-S3R8 (8 MB PSRAM, 16 MB flash), ST77916 360×360 round QSPI LCD, CST816S touch, bidirectional rotary switch, I2S mic/DAC. Pure C++20 on ESP-IDF v5.4, LVGL v9 UI.

---

## Skills

Read the relevant skill file when working on a specific task. Each is self-contained.

| Skill        | File                                       | Use when…                                                                     |
| ------------ | ------------------------------------------ | ----------------------------------------------------------------------------- |
| **Hardware** | [`skills/hardware.md`](skills/hardware.md) | Touching display driver, memory allocation, pin assignments, DMA, or flashing |
| **UI**       | [`skills/ui.md`](skills/ui.md)             | Creating or modifying LVGL screens, pages, images, fonts, or animations       |
| **Sonos**    | [`skills/sonos.md`](skills/sonos.md)       | Modifying playback, volume, speaker discovery, or SOAP commands               |
| **Building** | [`skills/building.md`](skills/building.md) | Build system, Kconfig, flash workflow, image pipeline, CI                     |
| **Voice**    | [`skills/voice.md`](skills/voice.md)       | Voice mode, OpenAI Realtime API, mic/DAC audio pipeline                       |

---

## Stack

| Component | Detail                                                                           |
| --------- | -------------------------------------------------------------------------------- |
| MCU       | ESP32-S3R8 (8 MB PSRAM, 16 MB flash)                                             |
| Framework | ESP-IDF v5.4+ (C++20, `-fno-exceptions -fno-rtti`)                               |
| UI        | LVGL v9 via `esp_lvgl_port`                                                      |
| Display   | ST77916 360×360 QSPI @ 50 MHz, RGB565, swap_bytes=true                           |
| Touch     | CST816S I2C @ 400 kHz                                                            |
| Input     | Bidirectional rotary switch (NOT quadrature — pin A=CW, pin B=CCW, GPIO polling) |
| Audio     | I2S DAC (playback), PDM mic (voice capture)                                      |
| Build     | `idf.py build` / `./flash.sh` / `./test.sh`                                      |

## Project Layout

```
components/                    Shared ESP-IDF components
  knob_hal/                    Display, touch, encoder, haptic drivers
    include/                   hal_pins.h, display.h, encoder.h, haptic.h
    src/                       display_idf.cpp, encoder.cpp, haptic.cpp
  knob_net/                    WiFi manager, shared event bus
    include/                   wifi_manager.h, knob_events.h
    src/                       wifi_manager.cpp
  knob_storage/                NVS persistence (multi-network WiFi, up to 5 saved)
    include/                   settings.h (WifiEntry, settings_wifi_save/get/remove)
    src/                       settings.cpp
  knob_ui/                     LVGL page system, fonts, squircle, art decoder
    include/                   ui_pages.h, fonts.h, squircle.h, art_decoder.h
    src/                       ui_pages.cpp, squircle.cpp, art_decoder.cpp, tjpgd/
  knob_voice/                  OpenAI Realtime API voice pipeline
    include/                   voice_task.h, voice_tools.h, voice_config.h, ...
    src/                       voice_task.cpp, voice_audio.cpp, voice_mic.cpp, ...
  knob_sonos/                  Sonos UPnP/SOAP, SSDP discovery
    include/                   sonos.h, discovery.h, sonos_config.h
    src/                       sonos.cpp, discovery.cpp
  knob_timer/                  Countdown timer with voice tool integration
    include/                   timer.h, timer_events.h
    src/                       timer.cpp
apps/
  radio/                       Sonos radio app (main project)
    main/
      main.cpp                 Entry point, event wiring
      app_config.h             Radio-specific: stations, UI task config
      ui/                      Radio-specific screens
      voice/                   Radio-specific voice tools
      fonts/                   LVGL font .c files
    sdkconfig.defaults         Build configuration
    stations.json              Station definitions
  spotify/                     Spotify remote control
    main/
      main.cpp                 Entry point, event wiring, command queue
      app_config.h             Spotify events, SpotifyState struct
      wifi_picker.cpp/h        On-device WiFi network picker UI
      wifi_setup.cpp/h         Captive portal for adding new networks
      spotify_setup.cpp/h      On-device Spotify OAuth (PKCE)
      spotify/                 API client, auth, JSON parsing
      ui/                      Now-playing, seek, splash screen
    sdkconfig.defaults         Build configuration
  template/                    Starter template for new knob apps
scripts/                       Shared tooling
docs/                          Hardware specs, architecture, protocol refs
skills/                        Agent skill files
```

### Component / App Split

Shared hardware drivers and reusable logic live in `components/`. Each component has its own `CMakeLists.txt`, `include/` (public headers), and `src/` (implementation).

App-specific code lives in `apps/<name>/`. Each app is a standalone ESP-IDF project with its own `CMakeLists.txt`, `main/`, `sdkconfig.defaults`, and `partitions.csv`. Apps pull in shared components via `EXTRA_COMPONENT_DIRS` pointing to `../../components`.

`app_config.h` is app-specific — it lives in each app's `main/` directory (e.g., radio-specific station lists, UI task config). Shared configuration lives in component public headers (e.g., `hal_pins.h`, `knob_events.h`).

---

## Conventions

- **Language**: C++20 with ESP-IDF. No exceptions, no RTTI. Use `constexpr`, `std::clamp`, `std::string_view`, RAII.
- **Module communication**: `esp_event` only. No direct cross-module calls except through public headers.
- **LVGL thread safety**: All LVGL calls from the UI task only. Use `display_lock()`/`display_unlock()` from elsewhere.
- **Large buffers**: PSRAM via `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`.
- **ISRs**: Minimal — post to FreeRTOS queues, no direct logic.
- **Naming**: Prefix public symbols with module name: `wifi_manager_init()`, `sonos_play_uri()`, `settings_get_volume()`.
- **No barrel files. No unnecessary comments. Self-documenting code.**

## Build & Flash

All top-level commands accept an app name. If omitted, they default to `radio`.

```bash
# Build a specific app:
./test.sh              # build radio (default) + lint
./test.sh radio        # same, explicit
./test.sh homekit      # build a different app
./test.sh --all        # build every app under apps/

# Flash a specific app to a knob:
./flash.sh radio -m    # build + flash + serial monitor
./flash.sh homekit     # flash a different app

# Or work directly from an app directory:
cd apps/radio && idf.py build
cd apps/radio && ./flash.sh -m
```

Each app has its own `build/`, `sdkconfig`, and `managed_components/`. Building one app doesn't affect another.

CI runs on every PR (`.github/workflows/build.yml`). See `skills/building.md` for full build reference.

## Secrets & Configuration

Each app has its own `sdkconfig.defaults.local` (gitignored) for WiFi credentials, speaker IP, API keys, etc. Copy from `sdkconfig.defaults.local.template` in the app directory. Never commit secrets.

## Adding a New Knob

1. `cp -r apps/template apps/my_knob`
2. Edit `apps/my_knob/CMakeLists.txt` — change `project(knob_app)` to `project(my_knob)`
3. Edit `apps/my_knob/main/CMakeLists.txt` — add shared components to `REQUIRES` as needed (e.g. `knob_sonos`, `knob_voice`)
4. `cp apps/my_knob/sdkconfig.defaults.local.template apps/my_knob/sdkconfig.defaults.local` — fill in WiFi credentials
5. Write your `main.cpp`, app-specific UI, and config
6. `./test.sh my_knob` to build, `./flash.sh my_knob -m` to flash

---

## Boundaries

- ✅ **Always:** Run `./test.sh <app>` before commits, strict C++20, use RAII
- ⚠️ **Ask first:** Pin assignment changes, new ESP-IDF components, Kconfig choice changes
- 🚫 **Never:** Commit secrets, use RGB666 display mode, call LVGL from non-UI tasks, use exceptions/RTTI

---

## Deep Reference

| Document                                             | Topic                                                  |
| ---------------------------------------------------- | ------------------------------------------------------ |
| [`docs/hardware.md`](docs/hardware.md)               | Full pin map, GPIO tables, peripheral specs            |
| [`docs/architecture.md`](docs/architecture.md)       | Module layout, task model, event system, boot sequence |
| [`docs/sonos-upnp.md`](docs/sonos-upnp.md)           | SOAP envelope templates for every Sonos command        |
| [`docs/setup.md`](docs/setup.md)                     | First-time hardware setup, troubleshooting             |
| [`docs/voice-mode-plan.md`](docs/voice-mode-plan.md) | Voice mode implementation plan, phased rollout         |

---

## Maintaining These Docs

**Keep agent docs up to date.** When you change architecture, add/remove modules, or discover new constraints — update the relevant skill file (or this file) in the same PR. Stale docs are worse than no docs.
