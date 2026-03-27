# UI — LVGL Screens, Pages & Display

## When to read this

You're creating or modifying UI screens, adding pages, working with LVGL widgets, or changing the display driver.

---

## Key Constraint

**All LVGL calls must happen from the UI task.** If you need to touch LVGL from another task (e.g., updating a label from a network callback), use the lock:

```cpp
if (esp_lvgl_port_lock(100)) {
    // safe LVGL calls here
    esp_lvgl_port_unlock();
}
```

Prefer posting an `esp_event` and letting the UI task handle it on the next tick.

---

## Display Driver

The display driver lives in `components/knob_hal/src/display_idf.cpp`. It initializes the ST77916 QSPI panel and CST816S touch controller, then sets up `esp_lvgl_port`.

### Critical rules

- **RGB565 only.** DMA buffers only support RGB565. RGB666 causes an unrecoverable boot loop.
- **Draw buffer: 36 rows max** (52 KB double-buffered in internal DMA SRAM). 54+ rows causes OOM.
- **`swap_bytes=true`** required for correct colors.
- **180° rotation** via `mirror_x=true, mirror_y=true` (not `sw_rotate`).
- **Field-by-field struct assignment** — C++20 designated initializers cause order mismatch with `esp_lvgl_port` structs. Assign each field individually.
- **`LV_COLOR_FORMAT_RGB565_SWAPPED`** is set after display init so LVGL renders byte-swapped natively, eliminating software byte-swap in the flush callback.

See `skills/hardware.md` for deeper display and memory constraints.

---

## Page System

The horizontal pager in `components/knob_ui/src/ui_pages.cpp` manages multiple full-screen pages with slide animations.

> **Shared component:** `ui_pages.cpp` lives in the `knob_ui` component, so the page system is reusable by any app built on this hardware. App-specific pages (timer, voice, home screen) live in the app's own `main/ui/` directory (e.g., `apps/radio/main/ui/`).

### Adding a new page

1. Create `apps/radio/main/ui/ui_mypage.cpp` + `apps/radio/main/ui/ui_mypage.h`
2. Define a `PageDef` with build/destroy/tick callbacks:

```cpp
static void my_page_build(lv_obj_t *parent) { /* create widgets */ }
static void my_page_destroy(void) { /* cleanup */ }
static void my_page_tick(void) { /* periodic updates */ }

static const PageDef my_page_def = {
    .name = "mypage",
    .build = my_page_build,
    .destroy = my_page_destroy,
    .tick = my_page_tick,
};
```

3. Register with `pages_add(&my_page_def, priority)` — priority determines slide position relative to home
4. Add the `.cpp` file to `apps/radio/main/CMakeLists.txt` SRCS

Pages auto-slide via the pager. The home screen is the default; other pages are to the left or right based on priority.

### Existing pages (radio app)

| Page  | File                              | Position               |
| ----- | --------------------------------- | ---------------------- |
| Home  | `apps/radio/main/ui/ui.cpp`       | Center (default)       |
| Timer | `apps/radio/main/ui/ui_timer.cpp` | Right of home          |
| Voice | `apps/radio/main/ui/ui_voice.cpp` | Overlay (not in pager) |

---

## Home Screen Modes

The home screen (`apps/radio/main/ui/ui.cpp`) is a single-screen state machine with two primary modes:

```
VOLUME MODE (default)
  encoder turn → volume overlay (auto-hides 1.5s)
  screen tap   → enter BROWSE mode

BROWSE MODE
  encoder turn → cycle stations (wraps)
  screen tap   → confirm station, start stream, return to VOLUME
  7s inactivity → cancel, revert to VOLUME
```

Voice mode is a separate overlay triggered by double-tap (see `skills/voice.md`).

### Spotify App: Touch + Encoder Interaction

The Spotify app (`apps/spotify/main/ui/ui.cpp`) uses touch as a modifier for encoder input:

```
NO TOUCH + encoder turn → volume adjust (green arc overlay)

TOUCH DOWN (finger on screen):
  + slow encoder turn → seek within track (3s per click, green progress arc, "+6s" label)
  + fast encoder flick (3+ steps) → skip to next/prev track
  + release without turning → play/pause toggle (SHORT_CLICKED)

TOUCH DOWN visual state:
  - album art dims to 60% opacity
  - progress arc thickens and turns green
  - seek offset label shown centered on art
```

Touch state is tracked via LVGL `PRESSED`/`RELEASED`/`PRESS_LOST` events on `s_bg`. The `s_encoder_while_touched` counter prevents play/pause from firing after a seek gesture.

---

## Images & Logos

Images are compiled into firmware as C arrays. No filesystem, no file I/O — everything is memory-mapped from flash.

### Image specs

| Type          | Size                              | Format       | Compression |
| ------------- | --------------------------------- | ------------ | ----------- |
| Station logos | 120×120                           | RGB565A8     | LZ4         |
| Backgrounds   | Solid color from `Station::color` | Runtime fill | None        |

### Image pipeline

```
apps/radio/stations.json
  → bun apps/radio/scripts/gen_stations.ts
    → downloads logos → apps/radio/assets/logos/*.png (120×120)
    → patches STATIONS[] in apps/radio/main/app_config.h

apps/radio/assets/logos/*.png
  → ./apps/radio/scripts/convert_images.sh
    → LVGLImage.py (--cf RGB565A8 --compress LZ4 --premultiply --ofmt C)
    → apps/radio/main/ui/images/*.c (~4 KB each compressed)
```

### Adding a station logo

1. Add entry to `apps/radio/stations.json`
2. Run `bun apps/radio/scripts/gen_stations.ts` — downloads logo, patches `app_config.h`
3. Run `./apps/radio/scripts/convert_images.sh` — generates C array
4. Add the new `.c` file to `apps/radio/main/CMakeLists.txt` SRCS
5. Add `LV_IMAGE_DECLARE(station_id);` to `apps/radio/main/ui/images/images.h`
6. Add `&station_id` to the `s_logos[]` array in `apps/radio/main/ui/ui.cpp` (must match `STATIONS[]` order)

### Image rendering details

- **Premultiplied alpha** saves one multiply-per-pixel during blending.
- **Image cache**: 1 MB in PSRAM for decoded logo images.
- **Background crossfade**: two-layer opacity animation on station confirm (250ms).
- To re-download all logos: `bun apps/radio/scripts/gen_stations.ts --force`
- To skip patching app_config.h: `bun apps/radio/scripts/gen_stations.ts --no-cpp`

---

## Fonts

LVGL font `.c` files live in `apps/radio/main/fonts/` (app-specific assets). Font declarations shared across apps are in `components/knob_ui/include/fonts.h`. Fonts are generated via `lv_font_conv` and compiled into firmware like images.

Font files >50 KB cannot be pushed via some worker tools — use sandbox git if needed.

---

## Style Patterns

- Use `lv_obj_set_style_*` functions, not direct struct access.
- Round display: 360×360, so center everything with `lv_obj_center()` or `lv_obj_align(parent, LV_ALIGN_CENTER, 0, 0)`.
- For overlays (volume, voice), create on `lv_layer_top()` so they draw above the pager.
- Animations: use `lv_anim_t` for smooth transitions. Keep durations short (150–300ms) on this hardware.

---

## Key Files

### Shared (components)

| File                                      | Purpose                                      |
| ----------------------------------------- | -------------------------------------------- |
| `components/knob_hal/src/display_idf.cpp` | ST77916 QSPI + CST816S init, LVGL port setup |
| `components/knob_hal/include/hal_pins.h`  | Pin definitions, LCD constants               |
| `components/knob_ui/src/ui_pages.cpp`     | Horizontal pager, slide animations           |
| `components/knob_ui/include/fonts.h`      | Shared font declarations                     |

### Radio-specific (apps/radio)

| File                                 | Purpose                                      |
| ------------------------------------ | -------------------------------------------- |
| `apps/radio/main/ui/ui.cpp`          | Home screen, mode state machine, logo array  |
| `apps/radio/main/ui/ui_timer.cpp`    | Timer page                                   |
| `apps/radio/main/ui/ui_voice.cpp`    | Voice mode overlay                           |
| `apps/radio/main/ui/images/`         | Station logo C arrays                        |
| `apps/radio/main/ui/images/images.h` | `LV_IMAGE_DECLARE` for all logos             |
| `apps/radio/main/fonts/`             | LVGL font .c files                           |
| `apps/radio/main/app_config.h`       | Station list, colors                         |
| `apps/radio/main/CMakeLists.txt`     | App build config, SRCS list                  |
| `apps/radio/stations.json`           | Station definitions (name, URL, logo, color) |

---

> **Keep this alive:** If you discover undocumented UI patterns or gotchas — update this file in the same PR.
