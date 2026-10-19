#pragma once

#include <cstdint>

// ─── Sonos ──────────────────────────────────────────────────────────────────

constexpr int SONOS_PORT = 1400;
constexpr int SONOS_HTTP_TIMEOUT_MS = 4000;

// ─── Volume ─────────────────────────────────────────────────────────────────

constexpr int VOLUME_MIN = 0;
constexpr int VOLUME_MAX = 100;
constexpr int VOLUME_STEP = 2;
constexpr int VOLUME_OVERLAY_TIMEOUT_MS = 1500;

// ─── Task Config ────────────────────────────────────────────────────────────

constexpr int NET_TASK_STACK = 8192;
constexpr int NET_TASK_PRIO = 4;
constexpr int NET_TASK_CORE = 1;

// ─── Sonos State ────────────────────────────────────────────────────────────

enum class PlayState : uint8_t {
  Stopped,
  Playing,
  Paused,
  Transitioning,
  Unknown,
};

struct MediaInfo {
  char title[128];
  char artist[128];
  char source[32]; // "Spotify", "TV", "Line-In", "AirPlay", ...
  char art_url[256];
  bool has_media;
};

// What the current source lets us do (TV and line-in can't pause or skip).
enum TransportAction : uint8_t {
  ACTION_PLAY = 1 << 0,
  ACTION_PAUSE = 1 << 1,
  ACTION_STOP = 1 << 2,
  ACTION_NEXT = 1 << 3,
  ACTION_PREVIOUS = 1 << 4,
};

constexpr int SONOS_GROUP_MAX = 8;

// The group the controlled room is in, as the speakers report it. Visible
// members only (no stereo-pair halves), controlled room first.
struct SonosGroup {
  char room_uuid[40];
  char coordinator_uuid[40];
  char member_uuids[SONOS_GROUP_MAX][40];
  char member_names[SONOS_GROUP_MAX][64];
  int count;
};

struct SonosState {
  PlayState play_state;
  int volume;        // group volume
  int station_index; // -1 if URI doesn't match any known station
  MediaInfo media;
  uint8_t actions; // TransportAction bits
  SonosGroup group;
};

// ─── Sonos Events (offset to avoid collision with knob + voice events) ──────

enum : int32_t {
  APP_EVENT_STATION_CHANGED = 200,
  APP_EVENT_VOLUME_CHANGED,
  APP_EVENT_PLAY_REQUESTED,
  APP_EVENT_STOP_REQUESTED,
  APP_EVENT_SONOS_STATE_UPDATE,
  APP_EVENT_SPEAKER_RESCAN,
};
