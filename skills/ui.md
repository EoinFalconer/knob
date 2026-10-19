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
| Voice | `apps/radio/main/ui/ui_voice.cpp` | Overlay (not in pager) |

The timer is a sheet, not a page (see Timer below).

---

## Home Screen Modes

The home screen (`apps/radio/main/ui/ui.cpp`) is a single-screen state machine with two primary modes:

```
VOLUME MODE (default)
  encoder turn → volume overlay (auto-hides 1.5s)
  screen tap   → enter BROWSE mode

BROWSE MODE
  encoder turn → the station strip glides one tile per detent (wraps)
  screen tap   → confirm station, start stream, return to VOLUME
  IDLE_MS (10 s) untouched → cancel, revert to VOLUME
```

Browse is a strip of tiles (`ui/carousel.cpp`), like iOS paging through cards: the chosen station sits where the artwork was, with its neighbors peeking in at the sides (160 px apart, dimmed to about 30%). Opening it fades the neighbors in around the artwork; each detent glides the strip one tile (280 ms, ease-out), and a detent mid-glide retargets it from where it is, so a fast spin keeps it moving instead of restarting. The title follows the strip: it fades as a tile leaves the middle and changes at the halfway point, so it always names the tile nearest the middle. Leaving, the strip glides back to what plays (the station just picked, or the one browsing started from) as the neighbors fade, and hands over to the artwork, which looks the same. With nothing playing, leaving fades the clock in over the strip instead, which stays put underneath and hides once it's covered (`carousel_hide_after`). The artwork sits in its own slot (`s_art_slot`), hidden while browsing, so the code that shows and fades artwork can't pop it up over the strip.

Keeping it smooth on the device (each detent costs about 26 ms a frame):

- Tiles move by translation and opacity only, no scale. The image rounds its own corners (`radius` on the `lv_image`); `clip_corner` on the tile rendered the corner rows through a layer every frame.
- The ring dims for browse only once the strip has opened, and comes back only once it has closed (`browse_opened`, `browse_closed`): both repaint a lot, and in the same frames they stuttered.

Voice mode is a separate overlay triggered by double-tap (see `skills/voice.md`).

### Input: surfaces and the router

Gestures are recognized once, on the screen object (`on_screen_pressed` / `on_screen_released`, the long-press timer, the encoder poll), and routed by `surface()` in `ui.cpp`: `Faces`, `Playing`, `Browse`, `Page` (another pager page) or `Picker`. Each handler switches on the surface instead of testing the speakers sheet, `s_mode`, `s_idle_active` and `pages_is_home()` itself. The table above `surface()` is the source of truth for what each gesture does where.

- **Only controls take presses.** A widget gets `LV_OBJ_FLAG_CLICKABLE` only if it has its own handler (skip buttons, picker rows, Back). Everything else must let presses fall through to the screen.
- **Use `ui_box()` (`ui/primitives.h`) for layout and decoration.** `lv_obj_create()` is clickable by default, and a clickable box with no handler silently eats every tap on it. That is how the weather face once ignored taps.
- **The simulator enforces this.** Every screenshot walks the screen and fails the run on a "dead tap target": a visible, clickable object with no event handler that doesn't bubble.
- **Vertical swipes belong to sheets**, not the router; see Sheets below.
- **Small controls get a bigger target with `ui_hit_area()`.** It's an invisible box on top of the drawn control that takes the tap and mirrors its pressed state onto the drawing. The picker's Back capsule uses one that runs from below the list to the bezel.

### Sheets

`ui/sheet.{h,cpp}` makes drawers that behave like iOS sheets. The speakers sheet comes up from the bottom; the timer comes down from the top.

- **It follows the finger.** A drag that is clearly vertical after 10 px is taken by the sheet, which then moves 1:1 with the finger, rubber-banding past fully open. The panel runs on 120 px past the screen-sized body on the side it opened from (as far as the band stretches), so a stretched sheet never shows a gap; `sheet_create` returns the body to fill.
- **Release keeps the speed.** The sheet projects where the finger was heading (180 ms ahead) to choose open or closed, so a flick works even after a short move. The settle animation starts at the finger's speed: `motion_ease_out` starts with a slope of 1/0.23, so its duration is that times distance over speed (160 to 440 ms).
- **Animations can be caught.** Touching a moving sheet stops it under the finger, and the drag carries on from there.
- **Drag down to close**, anywhere except over a list that isn't scrolled to its top (`may_close_from`).
- **Gestures are recognized on raw touch.** `sheets_track_touch` wraps the touch input's read callback, ahead of LVGL's own handling. When a sheet takes a drag it resets the input (`lv_indev_reset` plus `lv_indev_wait_release`), so the pressed row, button or screen gets `LV_EVENT_INDEV_RESET` and never a click. Anything that tracks presses should listen for it (`ui_hit_area` and the screen router do). Touch is read every 10 ms instead of LVGL's 30 so a dragged sheet doesn't step.
- **A sheet is a child of the screen, not a screen.** LVGL skips drawing whatever an opaque object fully covers, so a sliding sheet only costs the strip of home it uncovers plus itself. Screen-load animations draw both screens in full every frame, and they block input while they run.
- **Each sheet has a policy:** `may_open`, `may_close_from`, `on_appear` (fill it in; it runs before the first frame of a drag), `on_opened` and `on_closed`.
- **Taps on a sheet's background stop there** (`ui_absorb_taps`).
- **Drags lead the finger.** A frame shows about 40 ms after the touch it draws, so everything dragged would trail. `sheets_track_touch` hands LVGL a point 28 ms ahead along the finger's speed over the last 40 ms (at most 24 px), as iOS predicts touches; sheets, pages and lists all follow it. Release reports the last shown point so nothing jumps back. A sheet claimed after its slop starts from where the finger came down, and anything LVGL scrolls catches up the 10 px scroll limit it otherwise drops (`sheets_scroll_with_finger` on `LV_EVENT_SCROLL_BEGIN`). It must use `lv_obj_scroll_by_raw`: `lv_obj_scroll_by` sends `LV_EVENT_SCROLL_BEGIN` again, and a slow drag recursed until the LVGL task's stack corrupted and the device rebooted.
- **`sheet_dissolve` fades an open sheet where it is**, for when the screen underneath already shows the sheet's content and a slide would show it twice (the timer over its own idle face). It fades `opa_layered`, so the sheet fades as one picture; plain `opa` fades each part on its own, and the half-faded frost then veils whatever is under the sheet's text. Taking the sheet back up (a drag or `sheet_open`) makes it whole again.
- **Sheets are frosted, like iOS's dark material.** When a sheet appears, `ui/frost.cpp` snapshots the screen behind it (LVGL's snapshot, so `CONFIG_LV_USE_SNAPSHOT=y`), blurs a quarter-size copy (box blur, three passes each way, about a 14 px Gaussian at full size), pushes the colors a little more saturated, darkens them, and scales back up with an ordered dither so RGB565 doesn't band. The picture stays where the screen is while the sheet moves over it, so every point keeps showing what is right behind it. It's taken once per appearance, so it doesn't follow changes behind an open sheet.
  - Only the snapshot (20-40 ms) runs on the UI task. The blur runs on a FreeRTOS worker on core 0 (`frost_begin`), and `frost_take` waits for whatever is left. It starts the moment a finger lands where a drag could open a sheet, so by the time a drag clears the slop it waits 0-25 ms; a drag recognized within a second reuses it. Opening a sheet without a finger (a pill, the knob) waits for the whole thing, about 80 ms.
  - Two output buffers, so a sheet still showing the last picture isn't drawn over. They're `RGB565_SWAPPED`, the panel's byte order (the display renders swapped natively), so drawing one is a plain copy.
- **The sheet draws the frost itself, rounded by the image's clip radius.** `on_draw_frost` draws the screen-sized picture with `clip_radius` and `image_area` set to the sheet's own box: LVGL rounds an image to `image_area`, not to where the picture is placed, so the corners follow the sheet while the picture stays put. Don't use `clip_corner`: it drew every row near the sheet's edge, frost and content, through a layer, 7-12 ms a frame. The sheet's background is transparent while it has frost, so a `LV_EVENT_COVER_CHECK` handler tells LVGL the solid middle covers what's under it; otherwise it would draw the whole home screen under a sliding sheet. LVGL's own `blur_backdrop` style re-blurs on every frame, far too slow for a sliding panel.
- **`sheet_draw_material` paints the sheet's material back over part of it**, at an opacity, from a draw event of something on the sheet. Anything drawn before it fades into the sheet, exactly as if it were that transparent, without a layer.
- **Content on a frosted sheet is white veils, not gray fills:** rows are white at 10% (20% focused, 28% pressed), and the Back capsule at 15%, so the color behind shows through.
- **Scrolling lists fade at their edges** like iOS's scroll edge effect, instead of cutting rows off. After the list draws (`LV_EVENT_DRAW_POST`), `sheet_draw_material` paints the frost back over its edges in 2 px strips of rising opacity. Nothing about it switches abruptly: the fade is 44 px deep and eased (smoothstep, so it has no visible start or end), and each edge fades only as far as rows have scrolled under it, so it grows in with the scroll instead of popping on after one pixel. It used to be a `bitmap_mask_src`, which drew the list through a layer: too slow to keep while the sheet slid, so it faded out and back in, and the fade-in after landing hitched for a fifth of a second. A true progressive blur would need a blur pass per frame under the list, which the device can't afford.

### Timer

The timer sheet (`ui/timer_sheet.cpp`) drops from the top on a swipe down from the faces, now playing or browse. Its face (`ui/timer_dial.cpp`) follows Apple's timer: 60 ticks around the bezel (23 px long, 6.5 px wide, about 3.5:1 like Apple's, each a capsule bitmap drawn once at its exact angle because LVGL's lines snap their ends to whole pixels, then copied at its opacity into one A8 picture per dial that's drawn in ring strips like the volume ring: 60 image draws per band of the screen cost about 10 ms a frame), a big countdown in SF Pro Rounded Semibold with proportional figures re-centered as they count and a colon on the figures' middle, and an orange caps caption.

- **Set:** it opens on 5 minutes. The knob lights a tick per minute (1-60, each tick sliding in over 180 ms), and a tap anywhere starts it. Starting winds the ring up to all 60 ticks (420 ms), then the sheet dissolves (240 ms). Over the faces, the timer face underneath is the same dial with its digits in the same place (`TIMER_DIAL_DIGITS_Y`), so only the frost fades; over now playing the dial gives way to the pill. Snooze winds up the same way, from 5 ticks. Untouched for `IDLE_MS` (10 s) while setting, the sheet slides away.
- **Running:** the countdown lives on as an orange pill at the bottom of now playing and browse (a tap reopens the sheet; in browse it takes the station count's place) and as its own idle face, which replaces the clock and the weather and doesn't rotate. There the ticks drain from 12 o'clock (spent ticks stay dim orange, the draining one dims smoothly) and the caption names the timer: a voice timer's label, or its length ("12 MIN"). Swiped back down, the sheet is a cancel button: one big red (Apple's system red) "Stop timer" capsule, cut like the ringing sheet's Stop, with the time left as the orange caption above it. A tap anywhere on the sheet takes it. The dial is hidden: the ring and countdown already show on the screen underneath, and a second ring sliding over them doubled up. The knob is on volume, shown briefly in the caption. The screen stays fully lit while a timer runs or rings (`backlight_rest`), and dims again `IDLE_MS` after it's over. While the sheet is on screen at all, the face underneath swaps instantly, not animated, so no face change plays out half-covered as the sheet comes or goes.
- **Leaving keeps the last look.** Stopped, cancelled or answered, the sheet slides away still showing what was tapped (`s_stale`), and shows the new phase only when it next appears. Caught on its way out and pulled back open, it refreshes then.
- **Ringing:** the sheet drops down over anything (the speakers sheet closes, the pager goes home), can't be pushed away, buzzes every 2 s and keeps the screen lit. The Kitchen chimes (`TIMER_ROOM` in `app_config.h`; see skills/sonos.md). Stop or Snooze (5 min) answer it; after a minute it stops itself, as the speaker does.
- **One timer, from anywhere.** knob_timer owns it and posts started, cancelled and fired events; voice's `set_timer` goes through the same path. The countdown runs to a deadline on a one-shot `esp_timer`, so it fires on time without ticking.
- **It outlives a reboot.** knob_timer saves the deadline in NVS as wall-clock time, with the length and label, and clears it when the timer is cancelled or fires (from its own events, on the event loop). There's no battery-backed clock, so `timer_restore()` waits until the clock is set: at Wi-Fi up after a crash or update (the ESP32-S3's RTC keeps the time through soft resets), or at the first SNTP sync after a power cut. A timer with time left comes back with the ring drained as far as it should be (`TimerStarted::left_ms`); one that ran out less than 5 minutes ago rings at once; older ones are dropped. A timer started before the clock is set is saved once it is.

### Lights

Swiped sideways, the timer sheet's second page is the lights (`ui/lights_page.cpp`), with two page dots near the bottom. The pages sit side by side in a horizontally snapping scroller inside the sheet, so a sideways drag pages and a vertical one still moves the sheet. A ringing timer holds the sheet on its own page, without dots. Swiped down afresh, the sheet always opens on the timer; caught on its way out and pulled back, it stays on the page it was leaving.

- **All lights as one, after the iOS Home app's light control.** No title: a hint sits above the slider only when there's nothing to show (not set up, connecting, unreachable). The page is one tall rounded slider (124×220, 40 px corners) that fills from the bottom in warm white, with the percent (or "Off") near its top and a lightbulb at its foot (`lucide_34`, which holds only the two bulbs). The fill is drawn as the slider's own rounded shape clipped at the level, so its bottom corners follow the slider's without a layer. The percent and the bulb are drawn twice, clipped to each side of the fill's edge: white on the track, dark ink on the fill, so a figure the edge crosses is split between the two like iOS's sliders. A tap anywhere switches them all on (each back at its own level) or off; each detent dims them 1%. It deliberately looks nothing like the timer next to it.
- **It's optimistic.** The page shows what the knob or a tap asked for at once, and keeps showing it until the hand has been off for 1.5 s; a gateway report that comes in meanwhile is applied then. knob_lights only reports once the lights have caught up with the latest request, and then reports exactly the level asked for, not the bulbs' average read back (rounding between percent and 0..254 made the number step by 1). A read replaces it only when it's news: opening the page, or bulbs that still missed after every correction.
- **Outlets count as lights** that only switch (TRADFRI control plugs, `3312`). The plant light is left alone by name (`CONFIG_LIGHTS_IGNORE`), on a schedule of its own, and the bedroom by room (`CONFIG_LIGHTS_IGNORE_ROOMS`): its lights are dropped before rooms are mapped, so a room holding them is switched light by light.
- **knob_lights talks to an IKEA TRADFRI gateway** directly over the network: CoAP over DTLS 1.2 with a pre-shared key (`TLS_PSK_WITH_AES_128_CCM_8`, the gateway's only suite), so `CONFIG_MBEDTLS_SSL_PROTO_DTLS` and `CONFIG_MBEDTLS_KEY_EXCHANGE_PSK` are on. The gateway doesn't do secure renegotiation, which OpenSSL 3 refuses by default: test it from a Mac with `openssl s_client -dtls1_2 -legacy_server_connect -psk_identity Client_identity -psk <hex code> -cipher 'PSK-AES128-CCM8:@SECLEVEL=0'`; libcoap's `coap-client` fails the handshake. The knob can't resolve `.local` names (no mDNS component), so `CONFIG_LIGHTS_TRADFRI_HOST` is an IP for now. AES runs in software (`CONFIG_MBEDTLS_HARDWARE_AES` off): the hardware engine bounces PSRAM buffers through small internal DMA allocations that failed now and then, and the handshake with them. `coaps.cpp` is a small CoAP client: one confirmable request at a time, empty acknowledgements followed by separate responses, and blockwise answers put back together.
- **Pairing happens once.** With the code from the gateway's label (`CONFIG_LIGHTS_TRADFRI_CODE`, in the gitignored `sdkconfig.defaults.local`), the knob asks the gateway for a key under its own identity and keeps both in NVS (`lights` namespace). A gateway that was reset refuses that key, and the knob pairs again.
- **Big rooms get one command, everything else one per light.** A room command is a Zigbee broadcast, so its lights change together. A room of 3 lights or fewer (`SMALL_ROOM`), a room holding an ignored light (`CONFIG_LIGHTS_IGNORE`, "plant" by default, matched in the light's name) and lights in no room get a command each.
- **No fades.** An IKEA bulb ignores every command that arrives while it's still fading (zigbee2mqtt #18574 calls it freezing), so a 1 s fade swallowed the next second of detents and the lights only moved on the next turn. Dims are sent with `5712: 0`.
- **Broadcasts get lost, quietly.** Zigbee allows about 8 broadcasts in any 9 s across the whole network and drops the rest without telling anyone; the gateway still answers `2.04`. So the first 2 detents of a turn go out at once and after that a room command goes at most every 1.5 s, always the latest level (`BURST`, `COMMAND_GAP_MS`; Hue's guidance for groups is 1 a second). 1 s after the knob rests every bulb is read back, and any that missed get the level again on their own (unicast, which the bulb acknowledges), 100 ms apart, up to twice. Not as a room command: by then the broadcast budget is spent. Measured: every turn lands within 1 to 2 s of the knob stopping. Sending `5850: 1` with a dim made room commands land even less often; a brightness alone switches a bulb on. Per-light commands faster than about 10 a second get `5.03`.
- **The UI never waits.** Calls queue work for the lights task on core 0; what was asked for shows at once, and the gateway's answer follows as `APP_EVENT_LIGHTS_CHANGED`. The DTLS session is kept for a minute after the last request.

### Boot screen

`ui/splash.cpp` fills the round screen with KN over OB in SF Pro Rounded Black, in the night clock's blues, the block's rounded corners just meeting the bezel. Each line is an ink-cropped bitmap (clock_art) and rises 18 px into place, the second 110 ms after the first, then the home screen fades in at 1.2 s.

The speakers sheet opens on the rooms it already knows, so the slide never waits on the network, and rescans once it has settled. A scan also runs at boot, so even the first swipe shows rooms. A rescan only rebuilds the list when the rooms changed; otherwise it just refreshes the group checks. A scan that finds nothing keeps the known rooms. Scans never overlap (`start_scan` in `main.cpp`).

### Now playing

From the top: the route pill (rooms that play, like AirPlay's), the artwork, the title, then the subtitle (and the browse position). Without the pill at the top the block sank toward the bezel. The block sits slightly above center, and the pill's top corners clear the volume ring by about 12 px. Tapping the pill opens the speakers sheet, as swipe up does.

### Icon pills

Build capsules with an icon and text through `ui_icon_pill()` (`ui/primitives.h`), which spaces them on drawn ink the way the Dynamic Island insets its art. The icon's ink sits 5 px further from the leading end than from the top and bottom (a capsule's rounded ends take visual room away, so matching the vertical inset reads cramped), the text's last letter ends that same distance from the trailing end, and icon and text are 6 px apart. Both are centered on ink, the text on its capitals. The glyph metrics come from the font, so changing the text refits the padding.

- **Size the icon to the pill.** The ink should fill a bit over half the height (the route pill: 18 px Lucide in 30 px). The 22 px Lucide was too big and left uneven insets.
- **Center asymmetric icons optically.** Blur the rendered icon into a blob and center the blob, not the ink box; pass the difference as `icon_nudge_y`. `apps/radio/sim/measure_pill.py <png> <pill top> <pill height>` prints the insets and the blob center from a sim screenshot.
- **Match icon and text weight.** Thin Lucide strokes look lighter than text at the same opacity, so don't dim the icon further.

### Volume ring

`ui/volume_ring.cpp` owns the gauge around the bezel. Callers never style it. `ring_look()` in `ui.cpp` derives one `RingLook` (color, level and track opacity, width, shown) from the surface and whether the knob is turning, and `sync_volume_ring()` is the only place that applies it. Call it after any state change.

| Surface | At rest                                      | While turning                               |
| ------- | -------------------------------------------- | ------------------------------------------- |
| Playing | 4 px, white level at 43%, track white at 11% | swells to 8 px, full white                  |
| Faces   | hidden                                       | 6 px in `faces_accent()` over a faint track |
| Browse  | 4 px, dim (the knob picks stations)          | same                                        |

A turn swells the ring in 140 ms; letting go settles it over 420 ms. On the faces the ring sweeps out from 12 o'clock in 240 ms, and retracts back into it over 560 ms, thinning to half width and fading over the last stretch.

The ring is not an `lv_arc`. LVGL draws an arc by masking every row of the area being repainted, angle by angle, so on the device anything moving inside the ring (the browse strip, a fading title) paid about 22 ms a frame for it, and the ring's own animations as much again. Instead `ui/volume_ring.cpp` rasterizes the track and the level into two A8 bitmaps in PSRAM and draws them as tinted images:

- The pixels the ring can ever cover (a 13 px band) are listed once at boot, sorted by angle. When only the ends move (opening, retracting, a volume step) only the pixels around the old and new ends are redrawn, and only those stretches are invalidated. A width change redraws the whole band (about 7 ms).
- Color and opacity are applied when drawing, so dimming the ring for browse or fading it on the faces rasterizes nothing.
- It's drawn in strips that hug the band (`ui_for_each_ring_strip`, shared with the timer dial), so a repaint inside the ring never blits the empty middle.
- `volume_ring_set_look` ignores a look it already has; the UI asks for one after every state change.

### Grouping in the picker

Holding a row toggles it in the group at once. Sonos acknowledges a join before its topology shows it, so the next reports still carry the old group. The picker keeps the local guess until a report matches it, or for at most 6 s (`GROUP_CONFIRM_TIMEOUT_MS`). The engine also re-reads the topology 1 s after a join or leave (`REGROUP_SETTLE_MS`) instead of waiting for its 15 s refresh.

### Idle watch faces

When nothing plays, `ui/faces.cpp` covers the home page with two faces that take turns: a big-numeral analog clock (20 s) and a weather face (10 s). Every idle spell opens on the clock, and the weather face is skipped until weather data arrives.

- **Face changes** are a stack, not a crossfade: the old face's elements leave upward and the new one's rise from below, staggered top to bottom by their height on screen (`wave_delay`). Each spot on screen only ever shows one face, so there's no double exposure. The background color morphs in place with `motion_ease_in_out`.
- **A swipe sideways brings up the other face** at once (`faces_swipe`), the same change turned on its side: the old face leaves the way the finger went, the new one comes in behind it, leading edge first. They keep taking turns from there. The face changes as soon as the finger has gone 28 px clearly sideways (`LV_EVENT_PRESSING`), not when it lifts.
- **Weather art** comes from `scripts/gen_weather_art.py`, which writes `ui/weather_assets.{h,cpp}`. It paints 11 skies (360×360 baseline JPEG, ~10 KB each) and renders Meteocons "fill" icons (MIT) at 44 and 30 px as RGB565A8. `ui/weather_kind.cpp` maps met.no symbol codes to a sky and an icon.
- **Sky decoding** is slow (a JPEG), so `ui_set_weather` calls `faces_prepare_weather` before taking the display lock and swaps the result in under it. A sky is only decoded when the look changes.
- **The clock follows Apple's big-numeral face.** The numerals are SF Pro Rounded at 200 px (`sf_rounded_regular_200` holds only 1, 2, 3, 6 and 9), as big as the round face allows: each sits so its furthest ink is 12 px inside the edge, so 12 and 6 nearly meet in the middle and 9 and 3 overlap the 12 (`NUMERAL_SPOTS`). Hands are 11 px with a thin 3 px stem by the center, the second hand is 2 px with a short tail, and a ring cap with a sky-colored pin sits on top.
- **Numerals and hands are bitmaps** (`ui/clock_art.cpp`): A8 images tinted with `image_recolor`, LVGL's cheapest draw. A numeral is rendered once, cropped to its ink. As a label, a 200 px glyph repaints a quarter of its font height around itself and its ink spills past the box, so a moving numeral was slow and clipped. A hand is two capsules (stem and bar) drawn with a distance field: true round ends, anti-aliased, at any angle. A rounded `lv_line` stamps a circle on each end that sits visibly off the body at 11 px, and rotating objects with `transform_rotation` renders them through a layer. No hand shadows: see-through shapes darken where they overlap, and an opaque fake drew a halo around the tips. The bitmaps live in PSRAM (see hardware.md).
- **The hands sweep.** A 33 ms timer reads the time to the millisecond. All three hands are drawn sub-pixel; an `lv_line` snaps its ends to whole pixels, so a thin second hand visibly wobbled about the center. The second hand is redrawn every tick, the minute and hour hands whenever they've turned 0.1 degrees (once a second for the minute hand, a quarter pixel at its tip), so they glide.
- **Turning the knob jostles the numerals.** Each detent pushes a numeral's lean 9 px along the dial in the turn's direction, one numeral after another around the face (45 ms apart); the lean fades back (100 ms), and the numeral follows it on an underdamped spring (`faces_nudge`, about 2.2 Hz, damping 0.28), so a click overshoots and settles. A fast spin tops the lean up to its 14 px limit and holds it there, so the numerals lean into the turn and stay put (no repaint) instead of shaking; let go and they spring home. A numeral keeps its first queued push during a spin, so the later ones still follow 45 ms apart instead of waiting for the spin to end.
- **Clock colors follow the day** (`ui/face_palette.cpp`): apricot in the morning, Apple's muted sage by day, rose in the evening, dim navy at night. Night comes from the weather's `_night` symbols when there is weather, else from the clock, and always from 23:00 to 05:00. A change eases over 1.2 s, about four times a day.
- **The volume ring** is hidden on the faces and sweeps out in the face's own color when the knob turns (see Volume ring below). `faces_accent()` gives the color: the hands' color on the clock, white on the weather.
- **Weather face type follows HomeView.** Weight runs opposite to size: the 132 px temperature is SF Pro Rounded, set tight (-5 px tracking), and everything small is SF Pro Semibold or Medium at full white. Hierarchy comes from size and weight, not from dimming text over the sky. The temperature is "glass": translucent white over a soft offset shadow. Only the digits are centered (horizontally and vertically on their ink); the minus and the degree sign hang off either side, so the number doesn't move as they come and go.
- **Weather facts** are the next sunrise or sunset, wind and humidity, each as icon, value and label. High and low cover the next 24 hours. Sunrise and sunset come from met.no's sunrise API once a day and also decide the clock's colors.

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
| Weather skies | 360×360                           | JPEG         | JPEG        |
| Weather icons | 44×44 and 30×30                   | RGB565A8     | None        |

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
- **JPEG decoding dithers.** TJpgDec outputs RGB888 and `art_decoder.cpp` quantizes to RGB565 through a 4×4 Bayer matrix, so gradients in skies and covers don't band.
- **Artwork placeholder.** While a cover downloads, or when a source has none (TV, line-in), the tile shows a still grey square with the source's glyph (`lucide_48`), like Apple Music. The cover fades in over it. Don't pulse it: that repaints the tile every frame for the whole download.

---

## Fonts

LVGL font `.c` files live in each app's `main/fonts/`. Font declarations shared across apps are in `components/knob_ui/include/fonts.h`.

Text is Apple's SF Pro, taken from the Mac's system fonts by `scripts/gen_fonts.py`. The generated `sf_*.c` files are gitignored; `scripts/gen_assets.sh` makes any that are missing before every `test.sh` and `flash.sh` build (`--force` remakes them all). The script lists every app's fonts; add a size there, never by hand. Each size is cut from the variable font at its own optical size, so 13-22 px text gets SF Pro Text's spacing and 28 px and up SF Pro Display's, as on Apple's platforms. Big type uses SF Pro Rounded: Semibold for the timer, Regular for the clock numerals and the temperature, Black for the boot screen. A font can have OpenType features baked into its character map (lv_font_conv ignores them): `tnum` for tabular figures where a number must hold still (the calendar's clock), `case` for the colon that sits at the figures' middle. The fonts are licensed for Apple platforms only: fine for a private build, never committed or released.

Font files >50 KB cannot be pushed via some worker tools — use sandbox git if needed.

---

## Style Patterns

- Use `lv_obj_set_style_*` functions, not direct struct access.
- Round display: 360×360, so center everything with `lv_obj_center()` or `lv_obj_align(parent, LV_ALIGN_CENTER, 0, 0)`.
- For overlays (volume, voice), create on `lv_layer_top()` so they draw above the pager.
- Animations: use `lv_anim_t` for smooth transitions. Keep durations short (150–300ms) on this hardware.

### Alignment

Measure the drawn pixels, not the object boxes. The simulator's PNGs plus `magick ... -threshold 50% -trim -format "%wx%h%O" info:` give the real ink bounds.

- **Concentric radii.** A shape nested inside a rounded parent uses `inner radius = outer radius - gap`. Capsules use `radius = height / 2`. A circle inside a capsule row sits `row_h / 2 - circle / 2` from the top, bottom and end edges (`ROW_INSET` in `ui.cpp`).
- **Set `pad_all(0)` on `lv_obj` containers.** The default theme pads cards, and that padding gets added on top of every align offset.
- **Pad against glyph ink.** Lucide icons sit about 4 px inside their 22 px box, and SF leaves about 1 px either side of a word. Subtract these bearings from paddings and gaps, as the picker's Back pill does.
- **Center text on cap height.** Icons and dots line up with the middle of the capitals, not the line box. With SF and `LV_ALIGN_*_MID` these currently agree to within 0.5 px.
- **Optical nudges.** A play triangle is moved 1 px toward its tip so its centroid is centered. Pointed icons can sit 1 px closer to an edge than a text stem.

### Screens

- **Never delete a screen that may still be loading.** `lv_screen_load_anim` keeps a pointer to the screen sliding in, and freeing it mid-slide leaves LVGL animating freed memory. On the device that doesn't crash, it freezes. The speakers picker is now a sheet that lives as long as the device; `reset_picker_content()` cleans and refills its content.
- **Layers need stack.** Rotated clock hands once rendered through a transform layer, which nested deep enough that the LVGL task overflowed esp_lvgl_port's default 7 KB stack and the device boot-looped. The radio still peaks around 7 KB with sheets, so the task gets 16 KB (`display_idf.cpp`), and `CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK` makes an overflow panic with the task's name instead of corrupting the scheduler.
- **The sim runs under AddressSanitizer**, so a use-after-free like that fails the run with a stack trace instead of hanging hardware. The scenario includes a scan that answers mid-slide (`12d-speakers-fast-scan`).

### Redraw budget

- Never restyle a full-screen object per frame: any style change invalidates the whole 360×360 area. Animate the volume ring through `volume_ring_set_look`, which only redraws the band at the edge.
- Hiding or showing a full-screen object repaints the whole screen, even if it's empty. Size containers to their content (the artwork's slot is 120 px for this reason).
- Touch is interrupt-driven on the device (esp_lvgl_port puts the indev in `LV_INDEV_MODE_EVENT`): LVGL reads it only when the CST816S signals, so an indev read timer's period does nothing there. A probe that fakes touches in a wrapped read callback must call `lv_indev_read` itself to have them read.
- Measure on the device, not in the sim: the sim's render times say nothing about the ESP32-S3. A temporary probe that drives the UI from an `lv_timer` and logs `LV_EVENT_RENDER_START`/`READY` times per phase (plus `LV_EVENT_INVALIDATE_AREA` to see what repaints) finds the cost quickly. To see which draw tasks cost what, time `execute_drawing` in `lv_draw_sw.c` per task type with `esp_cpu_get_cycle_count()`, and restore the file from `~/Library/Caches/Espressif/ComponentManager` afterwards (a formatter may reflow the whole file).
- A full-screen slide can't beat about 16 ms a frame: the QSPI panel takes 10 ms for a full frame at 50 MHz, and reading the 259 KB frost from PSRAM takes 6-8 ms. On top of that, a sliding sheet pays for what's on it: the speakers sheet about 25-40 ms a frame in all (translucent rounded rows cost about 1 ms each; labels about 0.5 ms), the timer about 32 ms.
- No layers in anything that moves every frame: `clip_corner`, `bitmap_mask_src`, `opa_layered` and transforms each render through an ARGB layer in PSRAM. A sheet's `sheet_dissolve` is the one deliberate exception.
- Many small image draws cost more than one big one: each draw task opens the image decoder, and the screen is drawn in 36-row bands, so a draw that spans the screen becomes ten tasks. Skip what's outside `layer->_clip_area` in custom draws, or merge small pictures into one.
- A `LV_LABEL_LONG_SCROLL` marquee invalidates every refresh, so titles scroll once (`scroll_once`) and then rest truncated with an ellipsis. Set text through `set_marquee_text`: Sonos resends the same title on every poll, and re-setting it would restart the pass before it moves.
- Spinners step rather than spin. `ui/activity.cpp` is an iOS-style eight-spoke indicator that repaints only its own 42 px box, ten times a second.
- Keep runtime-drawn backgrounds solid: RGB565 gradients band. Gradients belong in pre-rendered JPEGs, which are dithered on decode.

### Simulator

`apps/radio/sim/run.sh` renders the real `ui.cpp` headless. Run one `idf.py build` first so it has the generated `sdkconfig.h`. It prints redraw cost per interaction and writes `apps/radio/build/sim/out/*.png`. `SIM_VERBOSE=1` adds an object geometry dump. Check redraw numbers before and after any UI change.

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

| File                                  | Purpose                                      |
| ------------------------------------- | -------------------------------------------- |
| `apps/radio/main/ui/ui.cpp`           | Home screen, mode state machine, logo array  |
| `apps/radio/main/ui/faces.cpp`        | Idle clock and weather faces                 |
| `apps/radio/main/ui/weather_assets.*` | Generated skies and icons (don't edit)       |
| `apps/radio/main/ui/activity.cpp`     | iOS-style activity indicator                 |
| `apps/radio/main/ui/motion.h`         | Shared easing curves                         |
| `apps/radio/main/ui/timer_sheet.cpp`  | Timer sheet, countdown pill                  |
| `apps/radio/main/ui/timer_dial.cpp`   | Timer face: ticks, countdown, caption        |
| `apps/radio/main/ui/sheet.cpp`        | Drawers: drag, settle, frost, cover check    |
| `apps/radio/main/ui/frost.cpp`        | Blurred material behind sheets               |
| `apps/radio/main/ui/ring_strips.h`    | Draws a ring-shaped picture in strips        |
| `apps/radio/main/ui/splash.cpp`       | Boot screen                                  |
| `apps/radio/main/ui/ui_voice.cpp`     | Voice mode overlay                           |
| `apps/radio/main/ui/images/`          | Station logo C arrays                        |
| `apps/radio/main/ui/images/images.h`  | `LV_IMAGE_DECLARE` for all logos             |
| `apps/radio/main/fonts/`              | LVGL font .c files                           |
| `apps/radio/main/app_config.h`        | Station list, colors                         |
| `apps/radio/main/CMakeLists.txt`      | App build config, SRCS list                  |
| `apps/radio/stations.json`            | Station definitions (name, URL, logo, color) |

---

> **Keep this alive:** If you discover undocumented UI patterns or gotchas — update this file in the same PR.
