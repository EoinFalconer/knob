# Voice Mode — Implementation Plan

> Superseded by [`voice-assistant-plan.md`](voice-assistant-plan.md).

## Summary

Add an AI voice assistant mode to the radio. Double-tap the touchscreen to enter voice mode: Sonos ducks to 1% volume, the display shows a Siri-style animated orb, the onboard PDM microphone captures speech, and OpenAI's Realtime API (`gpt-realtime-1.5`) provides speech-to-speech responses played through the onboard I2S DAC (3.5mm jack). Voice mode exits automatically after 8 seconds of inactivity — no manual dismissal required. Sonos volume restores on exit.

---

## 1. Trigger Gesture — Double-Tap

### Current input map

| Gesture            | VOLUME mode   | BROWSE mode          |
| ------------------ | ------------- | -------------------- |
| Encoder turn       | Volume ±      | Cycle stations       |
| Single tap         | Enter BROWSE  | Confirm station      |
| Long press (>0.5s) | Stop playback | Cancel browse + stop |

### New gesture

| Gesture                     | Any mode         |
| --------------------------- | ---------------- |
| **Double-tap** (<300ms gap) | Enter voice mode |

Double-tap does not conflict with any existing interaction. Implementation: track `last_tap_time` in the touch handler. On release (non-long-press), record timestamp. If a second tap arrives within 300ms of the previous, fire `APP_EVENT_VOICE_ACTIVATE` instead of the normal tap action. Use a 300ms delay on the first tap before committing it as a real single-tap — this is the standard iOS double-tap disambiguation pattern.

### Exit voice mode (fully automatic)

The conversation ends on its own — no manual tap required. OpenAI's Semantic VAD detects when the user stops speaking and triggers a response. After the AI finishes speaking, an 8-second idle timer starts. If no new speech is detected, voice mode exits automatically and Sonos volume restores.

| Trigger             | Behavior                  |
| ------------------- | ------------------------- |
| **8s idle timeout** | Auto-exit (primary path)  |
| Tap screen          | Force-exit (escape hatch) |
| Long press          | Force-exit (escape hatch) |

The idle timer resets every time the user starts speaking. Multi-turn conversations work naturally — just keep talking. Walk away and the radio resumes on its own.

---

## 2. OpenAI Realtime API

### Model

**`gpt-realtime-1.5`** — OpenAI's best voice model for audio-in, audio-out. Supports native speech-to-speech (no intermediate STT/TTS hop).

### Connection

**WebSocket** at `wss://api.openai.com/v1/realtime?model=gpt-realtime-1.5`

WebSocket is the correct choice for a server-side / embedded device (not a browser). The ESP32-S3 connects directly over TLS using `esp_websocket_client`.

### Audio format

| Direction | Format                            |
| --------- | --------------------------------- |
| Input     | PCM16, 24kHz, mono, little-endian |
| Output    | PCM16, 24kHz, mono, little-endian |

The PDM mic captures at a configurable rate. We resample to 24kHz mono PCM16 before sending. The I2S DAC accepts PCM16 natively.

### Session configuration

```json
{
  "type": "session.update",
  "session": {
    "type": "realtime",
    "model": "gpt-realtime-1.5",
    "instructions": "You are a helpful voice assistant built into a Sonos radio controller. Be concise — answers should be 1-3 sentences. Speak naturally and warmly. The user is controlling a Sonos speaker playing Norwegian radio stations. You can set timers, switch stations, adjust volume, and check what's playing. When the user asks to play a station, use the play_station tool. When the user asks about time remaining on a timer, use get_timer_status. Always confirm actions briefly after executing a tool.",
    "audio": {
      "input": {
        "format": { "type": "audio/pcm", "rate": 24000 },
        "noise_reduction": { "type": "far_field" },
        "transcription": { "model": "gpt-4o-mini-transcribe" },
        "turn_detection": {
          "type": "semantic_vad",
          "eagerness": "high",
          "create_response": true,
          "interrupt_response": true
        }
      },
      "output": {
        "format": { "type": "audio/pcm", "rate": 24000 },
        "voice": "cedar"
      }
    },
    "tools": [
      {
        "type": "function",
        "name": "set_timer",
        "description": "Set a countdown timer. When it expires the device will beep and show a notification. Only one timer at a time.",
        "parameters": {
          "type": "object",
          "properties": {
            "seconds": {
              "type": "integer",
              "description": "Duration in seconds (1-3600)"
            },
            "label": {
              "type": "string",
              "description": "Optional short label, e.g. 'eggs' or 'pizza'"
            }
          },
          "required": ["seconds"]
        }
      },
      {
        "type": "function",
        "name": "cancel_timer",
        "description": "Cancel the currently running timer, if any.",
        "parameters": { "type": "object", "properties": {} }
      },
      {
        "type": "function",
        "name": "get_timer_status",
        "description": "Check how much time is left on the current timer.",
        "parameters": { "type": "object", "properties": {} }
      },
      {
        "type": "function",
        "name": "play_station",
        "description": "Switch to a radio station by name. Available: NRK P1 Oslo, NRK P2, NRK P3, NRK MP3, NRK Jazz, P4 Norge, P5 Hits, P9 Retro, Radio Rock, Radio Norge, NRJ Norge.",
        "parameters": {
          "type": "object",
          "properties": {
            "station_name": {
              "type": "string",
              "description": "Name of the station (case-insensitive partial match)"
            }
          },
          "required": ["station_name"]
        }
      },
      {
        "type": "function",
        "name": "set_volume",
        "description": "Set the Sonos speaker volume.",
        "parameters": {
          "type": "object",
          "properties": {
            "level": { "type": "integer", "description": "Volume level 0-100" }
          },
          "required": ["level"]
        }
      },
      {
        "type": "function",
        "name": "get_now_playing",
        "description": "Get the currently playing station name, play state, and volume.",
        "parameters": { "type": "object", "properties": {} }
      }
    ],
    "tool_choice": "auto",
    "output_modalities": ["audio"],
    "max_output_tokens": 1024
  }
}
```

Key choices:

- **`semantic_vad`** with `eagerness: high` — the model uses AI to detect when the user is done speaking. "High" eagerness means faster response (2s max silence), good for a radio assistant where queries are short. This is what makes the "no manual exit" UX work — the model knows when to respond and when to keep listening.
- **`noise_reduction: far_field`** — the PDM mic is embedded in the device, not a headset. Far-field filtering helps with room noise and speaker bleed from the ducked Sonos.
- **`interrupt_response: true`** — if the user starts talking while the AI is responding, it cancels and listens. Natural conversational behavior.
- **`voice: cedar`** — OpenAI recommends `cedar` or `marin` for best quality.
- **Input transcription enabled** — we get text transcripts of what the user said (for displaying on screen), running on a separate ASR model.
- **`tools`** — six function-calling tools let the model control the device: set/cancel/query timers, switch stations, adjust volume, and check now-playing state. Implemented in `components/knob_voice/src/voice_tools.cpp` (framework) and `apps/radio/main/voice/tools_radio.cpp` (radio-specific tools).
- **`tool_choice: auto`** — the model decides when to call tools vs. respond with speech. For direct commands ("play jazz", "set a 5 minute timer") it calls the tool; for questions it responds normally.

### Event flow (happy path — speech response)

```
ESP32                                    OpenAI
  │                                        │
  │──── WebSocket connect ────────────────►│
  │◄──── session.created ─────────────────│
  │──── session.update (config+tools) ───►│
  │◄──── session.updated ─────────────────│
  │                                        │
  │  [user speaks into PDM mic]            │
  │──── input_audio_buffer.append ───────►│  (every 100ms, ~4.8KB base64)
  │──── input_audio_buffer.append ───────►│
  │◄──── input_audio_buffer.speech_started│  → UI: orb starts pulsing
  │──── input_audio_buffer.append ───────►│     idle timer paused
  │◄──── input_audio_buffer.speech_stopped│  → UI: "Thinking..."
  │                                        │
  │◄──── response.created ────────────────│
  │◄──── response.output_audio.delta ─────│  → I2S DAC playback
  │◄──── response.output_audio.delta ─────│  → UI: orb flows
  │◄──── response.output_audio_transcript │  → UI: show text
  │◄──── response.done ───────────────────│  → UI: orb settles
  │                                        │     idle timer starts (8s)
  │                                        │
  │  [user speaks again → timer resets]    │
  │  [8s silence → auto-exit voice mode]   │
```

### Event flow (tool call — e.g. "set a 5 minute timer")

When the model decides to call a tool instead of (or before) producing audio, the flow includes a function-call round-trip. The ESP32 executes the tool locally and feeds the result back so the model can speak a confirmation.

```
ESP32                                    OpenAI
  │                                        │
  │  [user: "set a 5 minute timer"]        │
  │──── input_audio_buffer.append ───────►│
  │◄──── speech_started ──────────────────│  → UI: Listening
  │◄──── speech_stopped ──────────────────│  → UI: Thinking
  │                                        │
  │◄──── response.created ────────────────│
  │◄──── response.function_call_arguments │  (streamed incrementally)
  │      .done                             │
  │      name: "set_timer"                 │
  │      arguments: {"seconds":300,        │
  │                   "label":"timer"}      │
  │      call_id: "call_abc123"            │
  │                                        │
  │  [voice_tools_execute("set_timer",...)]│  → starts 300s countdown
  │                                        │
  │──── conversation.item.create ────────►│  tool result:
  │      type: function_call_output        │  "Timer 'timer' set for 5 min 0 sec."
  │      call_id: "call_abc123"            │
  │──── response.create ─────────────────►│  (ask model to continue)
  │                                        │
  │◄──── response.created ────────────────│
  │◄──── response.output_audio.delta ─────│  "Done! 5 minute timer started."
  │◄──── response.output_audio.delta ─────│  → I2S DAC playback
  │◄──── response.audio_transcript.done ──│  → UI: show text
  │◄──── response.done ───────────────────│  → UI: orb settles
  │                                        │     idle timer starts (8s)
```

Key details:

- **Tool execution is local** — `voice_tools_execute()` runs on the ESP32 and returns immediately. No network round-trip beyond the WebSocket.
- **Two frames sent back** — first `conversation.item.create` with the tool output, then `response.create` to trigger the model's spoken confirmation. Both are built by `voice_protocol_handle_tool_call()`.
- **Model speaks after tool** — the `response.create` causes the model to generate a new audio response incorporating the tool result (e.g. "Done! 5 minute timer started.").
- **Multiple tools per turn** — if the model calls several tools (e.g. "play jazz and set volume to 30"), each `response.function_call_arguments.done` is handled independently, and the final `response.create` triggers the spoken summary.

### Authentication

The API key (`OPENAI_API_KEY`) is stored in NVS, configured via `CONFIG_RADIO_OPENAI_API_KEY` in `sdkconfig.defaults.local` (same as WiFi credentials and speaker IP). Sent as `Authorization: Bearer <key>` in the WebSocket handshake headers.

```
# sdkconfig.defaults.local
CONFIG_RADIO_OPENAI_API_KEY="sk-..."
```

---

## 3. Audio Pipeline

### Capture: PDM Microphone → PCM16 24kHz

```
PDM Mic (GPIO45 CLK, GPIO46 DATA)
    │
    ▼
ESP-IDF I2S PDM RX driver
    │  (configured for 24kHz sample rate, 16-bit, mono)
    ▼
Ring buffer (PSRAM, 48KB = 1s of audio)
    │
    ▼
Voice task reads 2400 samples (100ms chunks)
    │  base64-encode
    ▼
WebSocket send: input_audio_buffer.append
```

ESP-IDF's I2S driver supports PDM receive natively (`i2s_pdm_rx_config_t`). The PDM-to-PCM decimation happens in hardware. We configure for 24kHz output directly — no software resampling needed.

### Playback: PCM16 24kHz → I2S DAC

```
WebSocket recv: response.output_audio.delta
    │  base64-decode
    ▼
Playback ring buffer (PSRAM, 96KB = 2s)
    │
    ▼
I2S TX driver → PCM5100A DAC → 3.5mm jack
    (24kHz, 16-bit stereo — duplicate mono to both channels)
```

The PCM5100A is a hardware I2S DAC. It runs continuously while voice mode is active. Audio deltas arrive in chunks and are written to the ring buffer. The I2S TX DMA reads from the buffer automatically.

### Buffer sizing

| Buffer         | Size | Duration | Location |
| -------------- | ---- | -------- | -------- |
| Mic ring       | 48KB | 1.0s     | PSRAM    |
| Playback ring  | 96KB | 2.0s     | PSRAM    |
| WebSocket TX   | 8KB  | —        | PSRAM    |
| Base64 scratch | 8KB  | —        | PSRAM    |

Total PSRAM for voice: ~160KB — well within the 8MB budget.

---

## 4. Sonos Volume Ducking

When voice mode activates:

1. Read current Sonos volume → save as `s_pre_voice_volume`
2. Call `sonos_set_volume(1)` (duck to 1%)
3. Voice interaction happens
4. On voice mode exit: `sonos_set_volume(s_pre_voice_volume)` (restore)

If the user was not playing anything, skip ducking — just enter voice mode silently.

### Future: Sonos as voice output

For a later iteration, the ESP32 could host a tiny HTTP server (`httpd`) that streams the AI response audio as a WAV/PCM file. Sonos would play `http://<esp32-ip>:8080/voice.wav` via `SetAVTransportURI`. This lets the AI voice come through the Sonos speakers instead of the 3.5mm jack. Complexity is high (chunked HTTP streaming, WAV header synthesis, Sonos buffering latency) so this is deferred.

---

## 5. UI — Voice Mode Screen

### Design language

CarPlay / iOS inspired. Dark, minimal, focused. The 360×360 round display is perfect for a centered orb animation.

### Layout

```
┌─────────────────────────────────────┐
│                                     │
│          ╭─ status text ─╮          │   "Listening..." / "Thinking..."
│          │               │          │   lv_label, COL_TEXT_SEC, 14px
│          ╰───────────────╯          │
│                                     │
│                                     │
│             ╭───────╮               │
│           ╭─┤       ├─╮             │
│          ╭┤ │  ORB  │ ├╮            │   Animated circle/ring
│          ╰┤ │       │ ├╯            │   80×80px core, glow ring to 140px
│           ╰─┤       ├─╯             │   Siri-like gradient animation
│             ╰───────╯               │
│                                     │
│                                     │
│     ╭── transcript text ──╮         │   User speech + AI response
│     │  "What's the..."    │         │   lv_label, word-wrap, max 3 lines
│     ╰─────────────────────╯         │   Fades in with each new segment
│                                     │
│          ┌────────────┐             │
│          │  tap to end │             │   Hint label, COL_TEXT_SEC, 12px
│          └────────────┘             │   Fades out after 3s
│                                     │
└─────────────────────────────────────┘
```

### Orb states

| State          | Animation                                          | Duration          |
| -------------- | -------------------------------------------------- | ----------------- |
| **Connecting** | Small dot (24px), gray glow, gentle pulse          | ~1s               |
| **Listening**  | Orb expands to 80px, blue glow, slow breathe pulse | While user speaks |
| **Thinking**   | Orb contracts to 60px, indigo glow, fast shimmer   | 0.5-3s            |
| **Speaking**   | Orb expands to 100px, purple glow, rapid pulse     | While AI speaks   |
| **Exiting**    | Overlay fades out (250ms)                          | 250ms             |

Implementation (already built): The orb is a single `lv_obj` with `LV_RADIUS_CIRCLE` and LVGL shadow properties for the glow effect. State transitions use `lv_anim` to interpolate:

- `width` / `height` via custom `anim_orb_size_cb` (size transitions between states)
- `shadow_spread` + `shadow_opa` via looping playback animations (breathing pulse)
- `bg_color` / `shadow_color` set directly per state (blue → indigo → purple)

The hint text "Exits automatically" appears on entry and fades out after 4 seconds, teaching the user that no manual action is needed.

For mic-reactive sizing (Phase B), sample the RMS amplitude of the current mic buffer every 50ms and map it to the orb scale (1.0–1.4×).

### Color palette (voice mode)

```
COL_V_BG       0x000000   Pure black
COL_V_BLUE     0x0A84FF   iOS blue (listening)
COL_V_INDIGO   0x5E5CE6   Indigo (thinking)
COL_V_PURPLE   0xBF5AF2   Purple (AI speaking)
COL_V_GRAY     0x8E8E93   Gray (connecting, user transcript)
COL_V_TEXT     0xFFFFFF   White (AI transcript)
COL_V_DIM      0x48484A   Dim hint text
```

### Transitions

- **Enter**: Voice overlay fades in over the main screen (300ms), orb starts as small gray dot, "Connecting..." status, hint "Exits automatically" appears at bottom
- **Exit**: Overlay fades out (250ms), main screen is immediately visible underneath, Sonos volume restores. Triggered automatically by idle timeout — user does nothing.

---

## 6. Module Structure

### New files

```
components/knob_voice/
  include/voice_task.h       Public API: voice_task_init(), voice_task_start(), voice_task_stop()
  include/voice_config.h     VoiceState enum, task constants, event IDs
  include/voice_tools.h      Tool registry framework, REGISTER_TOOL macro
  src/voice_task.cpp         State machine, WebSocket client, audio orchestration
  src/voice_audio.cpp        I2S DAC playback, ring buffer, base64 decode
  src/voice_mic.cpp          PDM mic capture, ring buffer
  src/voice_protocol.cpp     OpenAI Realtime API JSON parser
  src/voice_session.cpp      Session config builder

apps/radio/main/
  voice/tools_radio.cpp      Radio-specific voice tools (play_station, set_volume, etc.)
  ui/ui_voice.cpp            Voice mode LVGL screen (orb, transcript, animations)
```

### New events (implemented in `app_config.h`)

```cpp
APP_EVENT_VOICE_ACTIVATE,     // double-tap detected → enter voice mode
APP_EVENT_VOICE_DEACTIVATE,   // idle timeout or force-exit → leave voice mode
APP_EVENT_VOICE_STATE,        // data: VoiceState enum
APP_EVENT_VOICE_TRANSCRIPT,   // data: null-terminated string (user or AI text)
```

### VoiceState enum (implemented in `app_config.h`)

```cpp
enum class VoiceState : uint8_t {
  Inactive,
  Connecting,
  Listening,
  Thinking,
  Speaking,
};
```

### New constants (implemented in `app_config.h`)

```cpp
constexpr int VOICE_TASK_STACK = 12288;
constexpr int VOICE_TASK_PRIO = 5;
constexpr int VOICE_TASK_CORE = 1;

constexpr int VOICE_DUCKED_VOLUME = 1;
constexpr int VOICE_IDLE_TIMEOUT_MS = 8000;   // auto-exit after 8s silence
constexpr int DOUBLE_TAP_WINDOW_MS = 300;

constexpr const char* OPENAI_REALTIME_URL = "wss://api.openai.com/v1/realtime?model=gpt-realtime-1.5";
constexpr const char* OPENAI_VOICE = "cedar";
```

### New Kconfig / NVS key

```
# In sdkconfig.defaults.local
CONFIG_RADIO_OPENAI_API_KEY="sk-..."
```

---

## 7. State Machine

```
                    ┌──────────┐
          ┌────────►│  RADIO   │◄────────────────┐
          │         │ (normal) │                  │
          │         └────┬─────┘                  │
          │              │ double-tap              │
          │              ▼                         │
          │     ┌────────────────┐                │
          │     │  CONNECTING    │                │
          │     │ (WebSocket TLS)│──── fail ──────┘
          │     └───────┬────────┘
          │             │ session.created
          │             ▼
          │     ┌────────────────┐
          │     │  LISTENING     │◄──── response.done
          │     │ (mic → API)    │        (idle timer starts 8s)
          │     │                │               ▲
          │     └───────┬────────┘               │
          │             │ speech_stopped          │
          │             │ (idle timer paused)     │
          │             ▼                         │
          │     ┌────────────────┐               │
          │     │  THINKING      │               │
          │     │ (waiting)      │               │
          │     └───────┬────────┘               │
          │             │ response.output_audio   │
          │             ▼                         │
          │     ┌────────────────┐               │
          │     │  SPEAKING      │───────────────┘
          │     │ (play audio)   │
          │     └────────────────┘
          │
          │   8s idle timeout (automatic)
          │   or tap/long-press (escape hatch)
          │   or WebSocket error
          └─────────────┘
```

The idle timer is the primary exit mechanism. It starts in the LISTENING state after a response finishes. If the user speaks again, the timer resets. If 8 seconds pass with no speech, voice mode exits automatically and Sonos volume restores.

---

## 8. Dependencies

### ESP-IDF components (already available)

| Component              | Use                       |
| ---------------------- | ------------------------- |
| `driver/i2s_pdm`       | PDM microphone capture    |
| `driver/i2s_std`       | I2S DAC playback          |
| `esp_websocket_client` | WebSocket to OpenAI       |
| `esp_tls`              | TLS for WSS connection    |
| `mbedtls`              | Base64 encode/decode      |
| `cJSON`                | Parse Realtime API events |

### idf_component.yml addition

```yaml
dependencies:
  espressif/esp_websocket_client: "^1.2.0"
  espressif/cjson: "^1.7.0"
```

`esp_websocket_client` supports WSS (TLS), custom headers (for auth), and binary/text frames. It handles reconnection, ping/pong, and fragmentation.

---

## 9. Implementation Order

### Phase A — Double-tap detection + UI shell + simulator ✅ DONE

1. ✅ Double-tap detection in `ui.cpp` with 300ms window (delays single-tap to disambiguate)
2. ✅ `APP_EVENT_VOICE_ACTIVATE` / `DEACTIVATE` / `STATE` / `TRANSCRIPT` events in `app_config.h`
3. ✅ `VoiceState` enum: Inactive, Connecting, Listening, Thinking, Speaking
4. ✅ `ui_voice.cpp`: full voice overlay with animated orb, transcript, status text, hint
5. ✅ Orb animates between states (size, color, shadow pulse rate)
6. ✅ 8-second idle timer auto-exits voice mode (no manual tap needed)
7. ✅ Sonos volume ducks to 1% on enter, restores on exit
8. ✅ Simulator: 'V'/'D' key toggles voice mode, auto-demo cycles through all states
9. ✅ Double-click works in simulator via mouse (same double-tap detection)
10. ✅ `Mode::Voice` added to existing mode enum, encoder ignored in voice mode

### Phase B — Audio I/O

1. Initialize PDM RX driver (mic capture) in `voice_audio.cpp`
2. Initialize I2S TX driver (DAC playback) in `voice_audio.cpp`
3. Ring buffer implementation (PSRAM-backed)
4. Test: capture mic → loopback to DAC (echo test)
5. Base64 encode/decode helpers

### Phase C — OpenAI WebSocket integration

1. Add `OPENAI_API_KEY` to settings/NVS/Kconfig (configured via `sdkconfig.defaults.local`)
2. WebSocket client: connect, send session.update, handle events
3. Voice task: mic ring buffer → base64 → `input_audio_buffer.append` every 100ms
4. Parse `response.output_audio.delta` → base64 decode → playback ring buffer
5. Parse `response.output_audio_transcript.delta` → post to UI
6. Handle `input_audio_buffer.speech_started` / `speech_stopped` → UI state updates

### Phase D — Sonos ducking + polish

1. Save/restore Sonos volume on voice mode enter/exit
2. Orb amplitude reactivity (RMS from mic buffer)
3. Transcript display with fade animations
4. Error handling: WebSocket disconnect, API errors, timeout
5. Graceful degradation if no API key configured

### Phase E — Simulator enhancements

1. ✅ Voice UI works in simulator (Phase A)
2. ✅ Auto-demo cycles: Connecting → Listening → Thinking → Speaking → Listening → idle exit
3. Add keyboard controls to manually trigger specific states (for testing individual transitions)
4. Mock WebSocket with canned JSON events for integration testing
5. Simulated mic input via desktop audio capture (optional, for end-to-end demo)

---

## 10. Memory & Performance Budget

| Resource      | Budget  | Notes                                    |
| ------------- | ------- | ---------------------------------------- |
| PSRAM         | ~200KB  | Buffers + WebSocket + JSON parsing       |
| Internal SRAM | ~2KB    | Task stack overhead                      |
| Flash         | ~30KB   | Voice module code                        |
| CPU (Core 1)  | ~15%    | Audio capture/encode + WebSocket I/O     |
| Network       | ~50KB/s | 24kHz × 16-bit = 48KB/s raw, +33% base64 |

WebSocket frames are sent every 100ms (~6.4KB base64 per frame). At typical WiFi speeds this is negligible. The ESP32-S3 has hardware AES for TLS, so the crypto overhead is low.

---

## 11. Risk & Mitigation

| Risk                                      | Mitigation                                                               |
| ----------------------------------------- | ------------------------------------------------------------------------ |
| WebSocket TLS handshake is slow           | Pre-connect on first double-tap, show "Connecting..." UI state           |
| Audio latency >500ms                      | Use 100ms capture chunks, stream immediately, no batching                |
| PSRAM bandwidth contention                | Voice buffers use separate PSRAM region, LVGL draw buffers in DMA        |
| API key exposure on device                | Key is in NVS (encrypted flash partition), never logged                  |
| Sonos volume restore fails                | Cache pre-voice volume in NVS, restore on next boot if needed            |
| User has no 3.5mm speaker                 | Future: HTTP stream to Sonos (Phase E2), for now require 3.5mm           |
| OpenAI rate limits                        | Limit to one active session, debounce rapid double-taps                  |
| Double-tap adds 300ms delay to single-tap | Acceptable — 300ms is imperceptible for mode switching                   |
| User doesn't know how to exit             | Hint "Exits automatically" shown on entry; auto-exit is the primary path |
| User walks away mid-conversation          | 8s idle timer handles this — voice mode exits, Sonos resumes             |

---

## 12. Configuration Update

Add to `sdkconfig.defaults.local`:

```
CONFIG_RADIO_OPENAI_API_KEY="sk-proj-..."
```

---

## 13. Simulator

The voice UI is fully testable in the simulator without hardware or an API key.

```
cmake -B build -S sim && cmake --build build -j$(nproc)
pkill -f "./build/sim" 2>/dev/null; sleep 0.3; ./build/sim &
```

| Key          | Action                              |
| ------------ | ----------------------------------- |
| V / D        | Toggle voice mode on/off            |
| Double-click | Enter voice mode (same as hardware) |
| Click        | Exit voice mode (when active)       |
| Up/Down      | Volume / browse (normal modes)      |

When voice mode activates in the simulator, a demo sequence auto-plays:

1. **Connecting** (0.8s) — small gray dot
2. **Listening** (3s) — blue orb, breathing pulse
3. **Thinking** (1.5s) — indigo orb, fast shimmer, user transcript appears
4. **Speaking** (3s) — purple orb, AI transcript appears
5. **Listening** — returns to listening, idle timer starts
6. **Auto-exit** (8s) — voice mode exits automatically

The demo sequence runs via `s_sim_timer` in `ui_voice.cpp`, gated behind `#ifdef SIMULATOR`.

---

## 14. Non-Goals (this iteration)

- Streaming AI audio to Sonos (requires HTTP server on ESP32)
- Wake word detection ("Hey radio")
- Conversation history persistence across sessions
- Using the co-processor ESP32-U4WDH for audio offload

These are future enhancements out of scope for the initial voice mode implementation.

### ~~Function calling~~ → DONE

Function calling (tool use) is implemented in `components/knob_voice/` (shared framework) and `apps/radio/main/voice/tools_radio.cpp` (radio-specific tools). The LLM can call tools to set timers, switch stations, adjust volume, and query now-playing state. See `voice_tools.h`, `voice_protocol.h`, and `voice_session.h`.
