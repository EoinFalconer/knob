# ESP32-S3 Knob Monorepo

Multiple firmware apps for the [Waveshare ESP32-S3-Knob-Touch-LCD-1.8](https://www.waveshare.com/esp32-s3-knob-touch-lcd-1.8.htm) — a round 360×360 QSPI display with touch, rotary encoder, and haptic feedback. Same hardware, different firmware per knob.

The original app is a **Sonos internet radio controller** with voice assistant support. The repo is set up so you can add new apps that reuse the shared drivers and UI framework.

## Hardware

|             |                                                      |
| ----------- | ---------------------------------------------------- |
| **Board**   | Waveshare ESP32-S3-Knob-Touch-LCD-1.8 (CNC case)     |
| **MCU**     | ESP32-S3R8 — 8 MB PSRAM, 16 MB flash                 |
| **Display** | ST77916 360×360 round QSPI LCD, RGB565               |
| **Touch**   | CST816S I2C capacitive                               |
| **Input**   | Bidirectional rotary switch (CW/CCW, no push button) |
| **Haptics** | DRV2605 motor driver on shared I2C bus               |
| **Audio**   | I2S DAC (PCM5100A) + PDM microphone                  |
| **Storage** | micro SD card slot (SDMMC 4-bit)                     |

## Repo Structure

```
components/              shared ESP-IDF components (drivers, libraries)
  knob_hal/              display, touch, encoder, haptic
  knob_net/              WiFi manager, shared event bus
  knob_storage/          NVS settings, SD card config
  knob_ui/               LVGL page system, fonts, art decoder
  knob_voice/            OpenAI Realtime API voice pipeline
  knob_sonos/            Sonos UPnP/SOAP control + discovery
  knob_timer/            countdown timer with voice integration

apps/                    one directory per knob
  radio/                 Sonos radio controller (the original app)
  spotify/               Spotify remote control with seek and album art
  template/              starter template — copy this for new apps

scripts/                 shared tooling (lint, image conversion)
docs/                    hardware specs, architecture, protocols
skills/                  task-specific guides for AI agents
```

Each app under `apps/` is a fully standalone ESP-IDF project. It has its own `CMakeLists.txt`, `sdkconfig`, partition table, and flash script. Shared components are pulled in via `EXTRA_COMPONENT_DIRS`.

## Prerequisites

- **ESP-IDF v5.4+** — [install guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
- USB-C cable (orientation matters — see [docs/setup.md](docs/setup.md))

```bash
# Install ESP-IDF (one-time):
git clone -b v5.4 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf && ./install.sh esp32s3

# Source it (every new terminal):
source ~/esp/esp-idf/export.sh
```

## Quick Start (Radio App)

```bash
# 1. Set up WiFi credentials:
cp apps/radio/sdkconfig.defaults.local.template apps/radio/sdkconfig.defaults.local
# Edit the file — fill in WIFI_SSID, WIFI_PASSWORD, and optionally SPEAKER_IP

# 2. Build:
./test.sh

# 3. Flash + monitor:
./flash.sh -m
```

Or work directly from the app directory:

```bash
cd apps/radio
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

## Build & Flash Any App

All top-level scripts accept an app name as the first argument. If omitted, they default to `radio`.

```bash
./test.sh              # build + lint radio (default)
./test.sh my_knob      # build + lint a different app
./test.sh --all        # build every app

./flash.sh -m          # flash radio + open serial monitor
./flash.sh my_knob -m  # flash a different app
```

Each app gets its own `build/` directory and `sdkconfig`. Building one app doesn't affect another.

## Creating a New App

### 1. Copy the template

```bash
cp -r apps/template apps/my_knob
```

### 2. Name your project

Edit `apps/my_knob/CMakeLists.txt`:

```cmake
project(my_knob)  # was: project(knob_app)
```

### 3. Pick your components

Edit `apps/my_knob/main/CMakeLists.txt` and add the shared components you need to `REQUIRES`:

```cmake
REQUIRES
    knob_hal        # always — display, touch, encoder, haptic
    knob_net        # always — WiFi, event bus
    knob_storage    # always — NVS settings, SD card
    knob_ui         # if you want the LVGL page system and fonts
    knob_voice      # if you want OpenAI voice assistant
    knob_sonos      # if you want Sonos speaker control
    knob_timer      # if you want countdown timers
    esp_event
    esp_timer
    nvs_flash
```

### 4. Write your firmware

The template gives you a working `main.cpp` with display, encoder, haptic, and WiFi already initialized. Add your logic:

```cpp
#include "hal_pins.h"       // pin definitions (from knob_hal)
#include "knob_events.h"    // shared event bus (from knob_net)
#include "display.h"        // LVGL display init (from knob_hal)
#include "encoder.h"        // rotary input (from knob_hal)
#include "settings.h"       // NVS persistence (from knob_storage)
#include "ui_pages.h"       // page navigation (from knob_ui)

// Components communicate via esp_event — post events, don't call directly
esp_event_post(APP_EVENT, APP_EVENT_ENCODER_ROTATE, &steps, sizeof(steps), 0);
```

### 5. Set up WiFi and build

```bash
cp apps/my_knob/sdkconfig.defaults.local.template apps/my_knob/sdkconfig.defaults.local
# Edit: add your WIFI_SSID and WIFI_PASSWORD

./test.sh my_knob       # build
./flash.sh my_knob -m   # flash + monitor
```

## Shared Components Reference

| Component        | Include                                             | What it gives you                                                                                                                    |
| ---------------- | --------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| **knob_hal**     | `hal_pins.h` `display.h` `encoder.h` `haptic.h`     | Hardware pin map, ST77916 QSPI display + CST816S touch init, LVGL mutex, rotary encoder polling, DRV2605 haptic buzz, shared I2C bus |
| **knob_net**     | `wifi_manager.h` `knob_events.h`                    | WiFi STA with auto-reconnect, `APP_EVENT` bus with `WIFI_CONNECTED`/`DISCONNECTED`/`ENCODER_ROTATE` events                           |
| **knob_storage** | `settings.h`                                        | NVS get/set for volume, station, speaker, WiFi creds (multi-network, up to 5 saved), API keys                                        |
| **knob_ui**      | `ui_pages.h` `fonts.h` `squircle.h` `art_decoder.h` | Horizontal page system with slide animations, Geist font declarations, iOS-style squircle mask, JPEG-to-RGB565 decoder               |
| **knob_voice**   | `voice_task.h` `voice_tools.h` `voice_config.h`     | Full OpenAI Realtime API pipeline: WebSocket, PDM mic capture, I2S DAC playback, tool registry with `REGISTER_TOOL()` macro          |
| **knob_sonos**   | `sonos.h` `discovery.h` `sonos_config.h`            | Sonos UPnP/SOAP: play/pause/volume/next/prev, SSDP speaker discovery, album art fetch, `PlayState`/`SonosState` types                |
| **knob_timer**   | `timer.h` `timer_events.h`                          | Labeled countdown timer with voice tool integration (`set_timer`, `cancel_timer`, `get_timer_status`)                                |

### Event ID Ranges

Components define their own event IDs at fixed offsets to avoid collisions:

| Range   | Source           | Events                                                                                        |
| ------- | ---------------- | --------------------------------------------------------------------------------------------- |
| 0–9     | `knob_events.h`  | `WIFI_CONNECTED`, `WIFI_DISCONNECTED`, `ENCODER_ROTATE`                                       |
| 100–103 | `voice_config.h` | `VOICE_ACTIVATE`, `VOICE_DEACTIVATE`, `VOICE_STATE`, `VOICE_TRANSCRIPT`                       |
| 200–204 | `sonos_config.h` | `STATION_CHANGED`, `VOLUME_CHANGED`, `PLAY_REQUESTED`, `STOP_REQUESTED`, `SONOS_STATE_UPDATE` |
| 300–301 | `timer_events.h` | `TIMER_STARTED`, `TIMER_FIRED`                                                                |
| 400–407 | `app_config.h` (spotify) | `SPOTIFY_STATE_UPDATE`, `VOLUME_SET`, `PLAY`, `PAUSE`, `NEXT`, `PREV`, `DJ_SPIN`, `SEEK` |

Your app can define additional events starting at any unused offset.

## Key Constraints

- **RGB565 only.** RGB666 causes an unrecoverable boot loop on this display.
- **No exceptions, no RTTI.** C++20 with `-fno-exceptions -fno-rtti`.
- **LVGL calls only from the UI task.** Use `display_lock()`/`display_unlock()` from other tasks.
- **Modules communicate via `esp_event`.** No direct cross-module function calls.
- **USB-C cable orientation matters.** The board has two MCUs sharing one port — flip the cable if you see the wrong chip.

## Further Reading

| Document                                     | When to read it                                         |
| -------------------------------------------- | ------------------------------------------------------- |
| [docs/setup.md](docs/setup.md)               | First-time hardware setup, USB quirks, troubleshooting  |
| [docs/architecture.md](docs/architecture.md) | Task model, event flow, boot sequence, memory strategy  |
| [docs/hardware.md](docs/hardware.md)         | Full pin map, GPIO tables, peripheral specs             |
| [docs/sonos-upnp.md](docs/sonos-upnp.md)     | SOAP envelope templates for every Sonos command         |
| [skills/building.md](skills/building.md)     | Build system deep dive, Kconfig gotchas, image pipeline |
| [skills/ui.md](skills/ui.md)                 | LVGL screens, page system, image/font pipeline          |
| [skills/voice.md](skills/voice.md)           | OpenAI Realtime API integration, audio pipeline         |

## License

MIT
