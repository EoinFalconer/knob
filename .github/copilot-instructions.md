# GitHub Copilot Instructions

See [CLAUDE.md](../CLAUDE.md) for quick reference and [AGENTS.md](../AGENTS.md) for full project context including the skills index.

This is a monorepo for ESP32-S3 knob devices. Pure C++20 on ESP-IDF v5.4, LVGL v9 UI. The main product is a Sonos radio controller (`apps/radio/`), with shared components in `components/` reusable across multiple knob apps.

Key constraints:

- RGB565 only (RGB666 causes boot loop)
- No exceptions, no RTTI
- LVGL calls only from UI task (use display_lock/unlock)
- Modules communicate via esp_event, not direct calls
- Test with `./test.sh` before pushing (defaults to radio app)

## Monorepo structure

- `components/` — Shared ESP-IDF components (knob_hal, knob_net, knob_storage, knob_ui, knob_voice, knob_sonos, knob_timer, knob_lights, knob_setup)
- `apps/radio/` — Sonos radio app (the main product)
- `apps/template/` — Starter template for new knob apps
- Hardware pin definitions live in `components/knob_hal/include/hal_pins.h`
- Shared event bus in `components/knob_net/include/knob_events.h`
- App-specific config (stations, etc.) in `apps/radio/main/app_config.h`

## Build commands

- `./test.sh` or `./test.sh radio` — build + lint the radio app
- `./test.sh --all` — build all apps
- `./flash.sh radio -m` — build, flash, and monitor the radio app
- `cd apps/radio && idf.py build` — build directly from app directory

For task-specific guidance, see `skills/`:

- `skills/hardware.md` — Display, memory, pin, flashing constraints
- `skills/ui.md` — LVGL screens, pages, images, fonts
- `skills/sonos.md` — UPnP/SOAP, discovery, speaker control
- `skills/building.md` — Build system, Kconfig, image pipeline
- `skills/voice.md` — Voice mode, OpenAI Realtime API
