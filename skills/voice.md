# Voice — OpenAI Realtime API & Audio Pipeline

> This describes the current voice mode. A rework that plays answers on the Sonos is planned in [`docs/voice-assistant-plan.md`](../docs/voice-assistant-plan.md).

## When to read this

You're working on voice mode: the WebSocket connection to OpenAI, PDM mic capture, I2S DAC playback, tool-call handling, or the voice UI overlay.

---

## Overview

Voice mode lets the user talk to an AI assistant that can control the device. Double-tap the screen to enter voice mode, speak naturally, and the assistant responds with audio while executing tool calls (change station, set volume, set timer, etc.).

Voice is a **shared component** (`components/knob_voice/`) — any knob app can use voice mode by defining its own tools and UI. The radio app provides radio-specific tool definitions and a voice overlay screen.

**Protocol:** OpenAI Realtime API over WebSocket (wss://api.openai.com/v1/realtime)
**Audio in:** PDM mic → PCM16 mono 24 kHz → base64 → WebSocket
**Audio out:** WebSocket → base64 → PCM16 24 kHz → I2S DAC
**UI:** Animated orb overlay on top of the current page

---

## Module Structure

Voice is split between shared infrastructure and app-specific code:

```
components/knob_voice/              ← shared component, reusable across apps
  include/
    voice_config.h                  VoiceState enum, task constants, event IDs
  src/
    voice_task.cpp                  WebSocket connection, event loop, session management
    voice_audio.cpp                 PDM mic capture → ring buffer → base64 encoding
    voice_tools.cpp                 Tool-call dispatch framework
    voice_mic.cpp                   Mic peripheral setup and capture task
    voice_protocol.cpp              Realtime API message serialization/parsing
    voice_session.cpp               Session lifecycle (connect, configure, teardown)

apps/radio/main/                    ← app-specific voice code
  voice/
    tools_radio.cpp                 Radio-specific tool definitions (station, volume, timer)
  ui/
    ui_voice.cpp                    Voice overlay UI (orb animation, state indicators)
```

Other knob apps reuse `knob_voice` but provide their own `tools_*.cpp` and `ui_voice.cpp`.

### Key Events

| Event                 | Direction                | Purpose                                                  |
| --------------------- | ------------------------ | -------------------------------------------------------- |
| `VOICE_START`         | UI → voice_task          | Double-tap detected, start session                       |
| `VOICE_STOP`          | UI → voice_task          | User dismissed voice mode                                |
| `VOICE_STATE_CHANGED` | voice_task → UI          | State transition (connecting, listening, speaking, etc.) |
| `VOICE_TOOL_RESULT`   | voice_tools → voice_task | Tool call completed, send result back                    |

Event IDs and `VoiceState` are defined in `components/knob_voice/include/voice_config.h`.

---

## WebSocket Session

### Connection flow

1. Open WebSocket to `wss://api.openai.com/v1/realtime?model=gpt-4o-realtime-preview`
2. Set headers: `Authorization: Bearer <key>`, `OpenAI-Beta: realtime=v1`
3. Send `session.update` with session config (voice, tools, instructions)
4. Wait for `session.updated` confirmation
5. Begin streaming mic audio via `input_audio_buffer.append`

### Session config

```json
{
  "type": "session.update",
  "session": {
    "voice": "ash",
    "instructions": "You are a radio assistant...",
    "input_audio_format": "pcm16",
    "output_audio_format": "pcm16",
    "input_audio_transcription": { "model": "gpt-4o-mini-transcription" },
    "tools": [...]
  }
}
```

### Key server events to handle

| Event                                   | Action                                   |
| --------------------------------------- | ---------------------------------------- |
| `response.audio.delta`                  | Decode base64, write PCM16 to I2S DAC    |
| `response.audio.done`                   | Flush audio buffer                       |
| `response.function_call_arguments.done` | Parse tool call, dispatch to voice_tools |
| `input_audio_buffer.speech_started`     | Cancel any playing response audio        |
| `input_audio_buffer.speech_stopped`     | VAD detected end of speech               |
| `error`                                 | Log and potentially reconnect            |

### Tool calls

Tools are defined in the session config and executed locally. The flow:

1. Server sends `response.function_call_arguments.done` with tool name + args JSON
2. `voice_tools.cpp` parses and dispatches to the app-specific tool handler (e.g., `tools_radio.cpp`)
3. Execute the action via esp_event (same as encoder/touch would)
4. Send `conversation.item.create` with tool result
5. Send `response.create` to prompt the assistant to continue

**Never call Sonos or UI directly from tool handlers.** Post events and let the existing handlers do their job.

---

## Audio Pipeline

### Mic capture (PDM → PCM16)

- PDM mic on I2S peripheral, configured for 24 kHz mono
- Capture runs in a dedicated FreeRTOS task when voice mode is active (`voice_mic.cpp`)
- Raw samples written to a ring buffer (`xRingbufferSend`)
- A separate encoding loop reads from the ring buffer, base64-encodes chunks, and sends via WebSocket (`voice_audio.cpp`)
- Chunk size: ~4800 bytes (100ms of audio at 24 kHz, 16-bit)

### DAC playback (PCM16 → I2S)

- Incoming base64 audio decoded to PCM16 in-place
- Written to I2S DMA buffer for playback
- Buffer in PSRAM — playback audio can be large

### Volume ducking

When voice mode activates:

1. Store current Sonos volume
2. Duck Sonos to ~20% (or mute)
3. On voice mode exit, restore original volume

This happens via the existing `sonos_set_volume()` path.

---

## Voice UI (ui_voice.cpp)

The voice overlay renders on top of whatever page is active. It does NOT replace the current screen. This file is app-specific — each app provides its own voice UI in `apps/<app>/main/ui/ui_voice.cpp`.

### States

| State        | Visual                  | Meaning                        |
| ------------ | ----------------------- | ------------------------------ |
| `CONNECTING` | Pulsing dim orb         | WebSocket connecting           |
| `LISTENING`  | Calm breathing orb      | Mic active, waiting for speech |
| `THINKING`   | Spinning orb            | Processing user input          |
| `SPEAKING`   | Pulsing bright orb      | Assistant audio playing        |
| `ERROR`      | Red flash, then dismiss | Connection or API error        |

### LVGL implementation

- Overlay is an `lv_obj` with `LV_OBJ_FLAG_FLOATING` on top of the screen
- Orb is an arc or canvas animation driven by `voice_state`
- **All LVGL calls in ui_voice.cpp run from the UI task** — voice_task posts `VOICE_STATE_CHANGED` events, ui processes them on next tick

---

## Constraints

- **API key** lives in `apps/radio/sdkconfig.defaults.local` as a Kconfig option. Never hardcode.
- **Memory**: WebSocket + audio buffers can consume 50-80 KB. Audio buffers go in PSRAM.
- **ESP WebSocket client** (`esp_websocket_client`) handles TLS and reconnection. Configure with sufficient buffer size (8 KB+ for Realtime API frames).
- **No simultaneous mic + DAC on same I2S peripheral.** Use separate I2S peripherals or time-multiplex. Current design uses two I2S peripherals.
- **Voice mode is ephemeral** — no conversation history persists between sessions. Each double-tap starts fresh.

---

## Deep Reference

- [`docs/voice-mode-plan.md`](../docs/voice-mode-plan.md) — Full implementation plan with phased rollout, memory budget, risk analysis
- [`docs/hardware.md`](../docs/hardware.md) — Mic and DAC pin assignments, I2S peripheral config
- [OpenAI Realtime API docs](https://platform.openai.com/docs/guides/realtime) — Protocol reference

---

> **Keep this alive:** If you change the voice pipeline, tool definitions, or audio config — update this file in the same PR.
