# Hardware — Display, Memory, Input & Flashing Constraints

## When to read this

You're modifying display driver code, changing pin assignments, adjusting memory allocation, touching DMA buffers, or flashing the device.

---

## Display

The ST77916 is a 360×360 round QSPI LCD. These constraints have caused boot loops and hours of debugging.

### RGB565 only

`esp_lvgl_port` DMA buffers only support RGB565. **RGB666 causes an unrecoverable boot loop.** Never attempt RGB666 without an alternative DMA approach.

### Draw buffers

- **36 rows max** (52 KB double-buffered in internal DMA SRAM)
- 54+ rows causes OOM — there is no wiggle room
- This means 10 SPI flushes per frame — visible top-down scanline wipe is a known performance tradeoff
- **ESP32-S3 SPI DMA cannot read from PSRAM** — setting `buff_spiram=true` falls back to CPU memcpy. Larger PSRAM buffers reduce visible banding but may not improve throughput

### Color and orientation

- `swap_bytes=true` required for correct colors on this panel
- 180° rotation via `mirror_x=true, mirror_y=true` (not `sw_rotate`)
- `LV_COLOR_FORMAT_RGB565_SWAPPED` set after display init — LVGL renders byte-swapped natively, eliminating the software byte-swap in the flush callback

### Initialization

`components/knob_hal/src/display_idf.cpp` uses **field-by-field assignment** for `esp_lvgl_port` structs. C++20 designated initializers cause order mismatch with these structs — do not refactor to use them.

---

## Memory

### Internal SRAM

- Task stacks and DMA-capable LCD transfer buffers live here
- `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y` eats ~38 KB — it was removed, do not re-enable
- sys_evt stack: **8192 bytes minimum** (default 2048 causes stack overflow with HTTP+TLS+XML handlers)
- LVGL task stack: **16 KB** (`display_idf.cpp`). The radio peaks around 7 KB (layers for the sheet's rounded corners and masked lists, screen snapshots), right at esp_lvgl_port's default, and an overflow boot-loops the device with a corrupted backtrace in `vTaskSwitchContext`.
- **`malloc` under 16 KB lands here** (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`), and `lv_malloc` is `malloc`. A handful of 12-15 KB UI buffers starved Wi-Fi of its RX buffers at boot (`wifi:Expected to init 10 rx buffer, actual is 3`, then an abort in `wifi_manager_init`). Put UI bitmaps in PSRAM explicitly with `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`. `CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y` turns that into a readable "Stack canary watchpoint triggered (taskLVGL)" panic.

### PSRAM

- All allocations >4 KB go in PSRAM via `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`
- Image cache: 1 MB in PSRAM for decoded logo images
- HTTP response bodies live in PSRAM

### Kconfig memory choices

`CONFIG_LV_USE_CLIB_MALLOC=y` requires special handling because Kconfig "choice" blocks are fragile:

1. Add explicit unset: `# CONFIG_LV_USE_BUILTIN_MALLOC is not set`
2. Add new choice: `CONFIG_LV_USE_CLIB_MALLOC=y`
3. Delete sdkconfig: `rm sdkconfig`
4. Full clean: `idf.py fullclean`
5. Rebuild: `idf.py build`
6. **Verify**: `grep LV_USE_CLIB_MALLOC apps/<app>/build/config/sdkconfig.h`

Always verify Kconfig changes actually took effect — choices silently revert. The build output lives under `apps/<app>/build/` (e.g. `apps/radio/build/config/sdkconfig.h`).

---

## Rotary Input

**NOT a quadrature encoder.** Pin A pulses on CW rotation, pin B pulses on CCW. Uses GPIO polling with software debounce, not PCNT hardware decoder. Do not attempt to use ESP32's PCNT peripheral for this.

---

## Haptic Feedback

DRV2605 motor driver on the shared I2C bus (400 kHz). Used for tactile feedback on encoder rotation and touch events.

---

## USB & Flashing

- **USB-C cable orientation matters.** CH445P analog switch routes to different chips depending on orientation. Flip cable for ESP32-S3 on `/dev/cu.usbmodem2101`
- **No reset button** — power slider only. Bad firmware = power cycle + reflash
- **Each flash cycle costs ~30s.** Batch fixes aggressively. Never push changes that crash without a known-good fallback

### Serial log capture (no interactive terminal)

`idf.py monitor` requires an interactive TTY. From an agent or script, run from the app directory (e.g. `apps/radio/`) and capture serial output to `serial.log`:

```python
python3 -c "
import serial, time
s = serial.Serial('/dev/cu.usbmodem2101', 115200, timeout=1)
end = time.time() + 10
with open('serial.log', 'w') as f:
    while time.time() < end:
        data = s.read(s.in_waiting or 1)
        if data:
            f.write(data.decode('utf-8', errors='replace'))
s.close()
"
```

**Always capture serial output after flashing changes that touch memory allocation, Kconfig, or display init.**

---

## Pin Assignments

All hardware pin definitions live in `components/knob_hal/include/hal_pins.h` — this is the single source of truth. Before changing any pin:

1. Verify against `docs/hardware.md` (full pin map with GPIO tables)
2. Check for I2C bus conflicts (touch + haptic share the bus)
3. Test on hardware — pin conflicts cause silent failures or boot loops

---

## Sonos Hardware Constraints

- `CONFIG_ESP_SYSTEM_EVENT_TASK_QUEUE_SIZE=64` (default 32 overflows from Sonos HTTP polling)
- Stereo pairs: right channel has `Invisible='1'` — must send commands to coordinator IP

---

## Deep Reference

- [`docs/hardware.md`](../docs/hardware.md) — Full pin map, GPIO tables, peripheral specs
- [`docs/setup.md`](../docs/setup.md) — First-time hardware setup, troubleshooting table

---

> **Keep this alive:** If you discover a new hardware constraint or gotcha while working — update this file in the same PR.
