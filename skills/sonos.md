# Sonos — UPnP/SOAP Control & Speaker Discovery

## When to read this

You're modifying Sonos playback, volume control, speaker discovery, or the polling loop. Read `docs/sonos-upnp.md` alongside this for full SOAP envelope templates.

---

## Architecture

Sonos functionality lives in the **`knob_sonos`** shared component (`components/knob_sonos/`). It is app-agnostic — any app in the monorepo can link against it. The radio app registers its station list at init via `sonos_set_stations()`.

---

## Key Files

| File                                           | Purpose                                                                                    |
| ---------------------------------------------- | ------------------------------------------------------------------------------------------ |
| `components/knob_sonos/include/sonos.h`        | Public API. Every call queues work for the net task and returns immediately                |
| `components/knob_sonos/src/sonos.cpp`          | FreeRTOS glue: the one long-lived net task, command queue, volume coalescing               |
| `components/knob_sonos/src/sonos_engine.cpp`   | What each command does and when to refresh topology and poll (no FreeRTOS, time passed in) |
| `components/knob_sonos/src/sonos_control.cpp`  | Room → coordinator routing, select/join/leave, group volume, polling                       |
| `components/knob_sonos/src/zone_topology.cpp`  | `GetZoneGroupState` fetch + parser, shared by control and discovery                        |
| `components/knob_sonos/src/track_info.cpp`     | Transport state/actions, track metadata, source names, station matching                    |
| `components/knob_sonos/src/soap.cpp`           | SOAP call (fails on UPnP faults), XML helpers                                              |
| `components/knob_sonos/include/sonos_group.h`  | Group label ("Kitchen + 2") and long-press join/leave decision                             |
| `components/knob_sonos/src/discovery.cpp`      | SSDP multicast speaker discovery                                                           |
| `components/knob_sonos/include/sonos_config.h` | Constants, `SonosState` / `SonosGroup`, event IDs                                          |
| `apps/radio/main/ui/now_playing.cpp`           | LVGL-free home-screen decisions (controls per source, subtitle, speaker line)              |
| `docs/sonos-upnp.md`                           | SOAP envelope templates                                                                    |

### What moved

| Old path                   | New path                                  |
| -------------------------- | ----------------------------------------- |
| `main/sonos/sonos.cpp`     | `components/knob_sonos/src/sonos.cpp`     |
| `main/sonos/sonos.h`       | `components/knob_sonos/include/sonos.h`   |
| `main/sonos/discovery.cpp` | `components/knob_sonos/src/discovery.cpp` |
| `stations.json`            | `apps/radio/stations.json`                |

### What moved between headers

- Sonos event IDs (`APP_EVENT_STATION_CHANGED`, etc.) now live in `sonos_config.h`, **not** `app_config.h`.
- Sonos constants (`SONOS_PORT`, etc.) also in `sonos_config.h`.

---

## Using knob_sonos from an App

Apps depend on `knob_sonos` like any ESP-IDF component. At init, the app registers its station list:

```c
#include "sonos.h"

// App startup
sonos_set_stations(my_stations, my_station_count);
sonos_init();
```

`sonos_init()` starts the net task idle; `sonos_start()` / `sonos_stop()` follow WiFi up/down. Register stations before the first poll. The component does not own or embed any station data — the app provides it.

---

## Interaction Model

The knob controls one **room**. Sonos only honours transport (Play, Pause, Next...) and
group volume on a group's **coordinator**, and a `SetAVTransportURI` sent to a group
member pulls it out of the group into its own, unsynced copy of the stream (the
"echo" bug). So `sonos_control.cpp` resolves the room's coordinator from
`GetZoneGroupState` and sends every command there. Topology is refreshed on start,
after every select/join/leave, and every 15 s (5 s retry when unreachable), so
regroups made in the Sonos app are picked up.

| Action                                             | What happens                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| -------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Station selected                                   | `SetAVTransportURI` + `Play` on the coordinator. Members follow in sync                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| Volume knob                                        | `SnapshotGroupVolume` at the start of each turn, then `SetGroupVolume` on the coordinator. Keeps the rooms' relative balance                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| Tap a room in the picker                           | Control that room, **on its own**: the list is flat, so a tap means that one room and groups are only made by holding. A member leaves its group; a coordinator sends the other rooms away (and keeps playing). Not done when the room's group is playing something the knob isn't moving. If our radio was playing, the old group gets `Stop` and the room plays the station                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| Long-press a room                                  | Not in our group: `SetAVTransportURI x-rincon:<coordinator UUID>` sent **to that room** (join). In our group: `BecomeCoordinatorOfStandaloneGroup` sent to it (leave). Long-press our own room while grouped: it leaves, the rest keep playing                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| Hold to stop                                       | Fades out: `SnapshotGroupVolume`, then `SetGroupVolume` easing out to 0 over `FADE_OUT_MS` (1.5 s, quadratic, every 100 ms; a straight line in Sonos's near-logarithmic units holds loud and then cuts off), then `Stop`, then the volume goes back so the next play starts where it was. Polls during the fade report stopped and no volume. Play calls the stop off; any other command finishes it first                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| Timer rings                                        | `sonos_start_alarm(url, "Kitchen")`: finds the room by name in the topology and works on its group's coordinator (rooms grouped with it hear it too; no such room rings the controlled one). Notes `GetTransportInfo`, `GetGroupVolume`, `GetMediaInfo` (URI and metadata, kept escaped) and `GetPositionInfo` (track, time), then plays Apple's "Breaking" ringtone, which the knob serves itself (`alarm_sound.cpp`, port 8080, looped to 72 s) at group volume 30, whatever it was before. Stop, Snooze, any other command or a minute puts it all back: `SetAVTransportURI` with the old URI and metadata, `Seek` to the queue track and time, the old volume, and `Play` if it was playing. A room with nothing loaded goes back to its own queue so Play can't replay the chime. While it rings on the controlled group, polls aren't published |
| Poll (every `CONFIG_RADIO_SONOS_POLL_INTERVAL_MS`) | `GetTransportInfo`, `GetGroupVolume`, `GetPositionInfo`, `GetCurrentTransportActions` on the coordinator → `APP_EVENT_SONOS_STATE_UPDATE`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |

### Other sources (Spotify, AirPlay, TV, line-in, Apple Music...)

`detect_source()` names the source from the track URI: `x-sonos-vli:...,spotify:` and
`,airplay:` (Spotify Connect / AirPlay), `x-sonos-htastream:` (TV), `x-rincon-stream:`
(line-in), and the `sid=` music-service ID for queue tracks (204 Apple Music, 201 Amazon,
174 TIDAL, ...). Which controls the UI shows comes from `GetCurrentTransportActions`,
not from the source name: a TV or line-in can't pause or skip, so it gets no buttons.
Tapping the artwork pauses/plays when the source allows it.

### Tests

```bash
apps/radio/test/run.sh            # UI decisions + knob_sonos
components/knob_sonos/test/run.sh # knob_sonos only
```

They run the real control, engine and parsing code against a fake household that
behaves like Sonos (members refuse transport commands, a member given a stream leaves
its group, a departing coordinator hands its group off). Assertions are about what
you'd hear (`audible()`), not just which requests were sent. `./test.sh` and CI run them.

---

## Constraints

### URI prefix required

Since Sonos firmware v6.4.2+, internet radio streams must use the `x-rincon-mp3radio://` URI prefix. Without it, Sonos silently refuses to play.

Stream URLs in `apps/radio/stations.json` use `https://`. The firmware prepends the prefix before sending to Sonos.

### Stereo pairs

In a stereo pair, the right channel has `Invisible='1'` in SSDP responses. All commands must be sent to the **coordinator** (left channel) IP. Sending to the right channel is silently ignored.

### HTTP event queue

`CONFIG_ESP_SYSTEM_EVENT_TASK_QUEUE_SIZE=64` (default 32 overflows from Sonos HTTP polling combined with WiFi events). If you see events being dropped, check this config value. The `sdkconfig` lives per-app under `apps/radio/`.

---

## Speaker Discovery

SSDP discovery runs **every time WiFi connects** — not just on first boot. This handles Sonos speakers changing IP addresses (common on most home networks). The flow:

1. WiFi connects → discovery task starts, scanning overlay shown
2. SSDP multicast finds at least one speaker on the network
3. `GetZoneGroupState` SOAP call resolves all zone coordinators (handles stereo pairs)
4. If a saved speaker name matches a discovered coordinator → IP is silently updated, auto-connect
5. If no match (or no saved speaker) → picker UI is shown

| Scenario                         | Action                                                             |
| -------------------------------- | ------------------------------------------------------------------ |
| Saved speaker found (same IP)    | Auto-connect, dismiss scanning overlay                             |
| Saved speaker found (IP changed) | Update IP in NVS, auto-connect, log the move                       |
| Saved speaker not found          | Show speaker picker (speaker may be off or renamed)                |
| One speaker, first boot          | Auto-selected, saved to NVS                                        |
| Multiple speakers                | User picks from card-style list (encoder scroll, touch/tap select) |
| No speakers                      | Show picker with "No speakers found" + Rescan button               |

Speaker matching uses the **room name** (stable across reboots) rather than IP (which changes). The name is persisted in NVS alongside the IP via `settings_set_speaker_name()`.

### Rescan from Home Screen

Swiping up on the home screen (iOS-style drawer sheet gesture) triggers `APP_EVENT_SPEAKER_RESCAN`, which re-runs discovery with the picker shown. Polling keeps running during the scan, so backing out of the picker leaves the knob controlling the current room.

The gesture is detected in `on_screen_released` (`ui.cpp`): vertical travel ≥ 40px upward and greater than horizontal travel. The handler lives in `main.cpp` and spawns `discover_and_connect_task` on the net core.

### Speaker Picker UI

The picker is a separate LVGL screen (`s_scr_picker`) with:

- **Scanning state**: animated spinner arc + "Scanning for speakers…" text (overlay on main screen)
- **List state**: "Speakers" title, scrollable card-based list, Rescan button
- **Cards**: dark rounded rectangles (240×50, radius 16) with room name; green dot marks the controlled room, green ring marks other rooms in its group
- **Navigation**: encoder scrolls through cards (blue highlight), tap to select, long-press to add/remove from the group
- **Back arrow**: shown when already connected (allows dismissing without changing speaker). Its touch target runs from below the list to the bezel
- **Closes by itself** after 10 s (`IDLE_MS` in `app_config.h`) with no touch or turn, once a room is chosen

---

### Events

| Event                      | Posted by | Purpose                                      |
| -------------------------- | --------- | -------------------------------------------- |
| `APP_EVENT_SPEAKER_RESCAN` | UI        | User swiped up on home → trigger rediscovery |

### Key Functions

| Function                   | File            | Purpose                                       |
| -------------------------- | --------------- | --------------------------------------------- |
| `discovery_scan()`         | `discovery.cpp` | SSDP + ZoneGroupState coordinator resolution  |
| `discovery_find_by_name()` | `discovery.cpp` | Match saved speaker name in discovery results |
| `select_speaker()`         | `ui.cpp`        | Switch room (moves our radio along), persist  |
| `toggle_speaker_group()`   | `ui.cpp`        | Long-press join/leave                         |
| `rebuild_speaker_list()`   | `ui.cpp`        | Build the card-based picker screen            |

---

## Testing Sonos Changes

Always test SOAP commands with `curl` from your dev machine first to isolate firmware vs. service issues:

```bash
curl -X POST http://SPEAKER_IP:1400/MediaRenderer/AVTransport/Control \
  -H 'Content-Type: text/xml; charset="utf-8"' \
  -H 'SOAPAction: "urn:schemas-upnp-org:service:AVTransport:1#GetTransportInfo"' \
  -d '<?xml version="1.0" encoding="utf-8"?>
<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"
  s:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">
  <s:Body>
    <u:GetTransportInfo xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">
      <InstanceID>0</InstanceID>
    </u:GetTransportInfo>
  </s:Body>
</s:Envelope>'
```

See `docs/sonos-upnp.md` for the complete set of SOAP envelopes (SetAVTransportURI, Play, Stop, GetVolume, SetVolume, GetPositionInfo).

---

## Common Issues

**Sonos stops playing after a few minutes**: Check if the stream URL returns a redirect. Sonos doesn't follow HTTP redirects from `x-rincon-mp3radio://` URIs. Use the final URL.

**Transport or volume commands ignored (UPnP error 800)**: The target isn't the group coordinator. Everything should go through `control_*`, which routes to the coordinator.

**Same station playing slightly out of sync in two rooms**: Something sent `SetAVTransportURI` with a stream to a group member instead of the coordinator. Rooms in a group must only ever get `x-rincon:<coordinator UUID>`.

**Home theater room missing from the picker**: Satellites (Sub, surrounds) are nested `<Satellite Invisible="1">` children. Attribute lookups must stay inside the member's own tag.

**Event queue overflow** (`ESP_ERR_TIMEOUT` from `esp_event_post`): Increase `CONFIG_ESP_SYSTEM_EVENT_TASK_QUEUE_SIZE` in the app's sdkconfig (`apps/radio/sdkconfig`). The Sonos polling loop + WiFi events + UI events can exceed 32 slots.

---

> **Keep this alive:** If you discover undocumented Sonos behavior or new constraints while working, update this file in the same PR.
