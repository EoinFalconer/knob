#pragma once

#include "sonos_config.h"

#include <cstdint>

// The net task's brain, minus FreeRTOS: decides what each command does and
// when to refresh topology and poll. Time is passed in so tests can drive it.

enum class CmdType : uint8_t {
  SetRoom,
  Select,
  Join,
  Leave,
  PlayUri,
  Play,
  Pause,
  Stop,
  Volume,
  Previous,
  Next,
  SeekStart,
  AlarmStart,
  AlarmStop,
};

struct Command {
  CmdType type;
  uint16_t port;
  int volume;
  char ip[40];
  char uri[256];
  char room[40]; // AlarmStart: where it rings
};

constexpr uint32_t TOPOLOGY_REFRESH_MS = 15000;
// An unreachable room costs a full HTTP timeout per try; don't spin on it.
constexpr uint32_t TOPOLOGY_RETRY_MS = 5000;
// Per Sonos: snapshot the group's volume ratios at the start of each drag.
constexpr uint32_t VOLUME_GESTURE_GAP_MS = 2000;
// Track changes take a moment to show up in GetPositionInfo.
constexpr uint32_t TRACK_CHANGE_SETTLE_MS = 500;
// Sonos answers a join or leave before the topology shows it, so the read
// right after the command often still has the old group.
constexpr uint32_t REGROUP_SETTLE_MS = 1000;
// A stop fades the group out over this long, a volume step at a time.
constexpr uint32_t FADE_OUT_MS = 1500;
constexpr uint32_t FADE_STEP_MS = 100;
// An alarm always sounds at this group volume, and gives up after this long.
constexpr int ALARM_VOLUME = 30;
constexpr uint32_t ALARM_MAX_MS = 60000;

void engine_init(uint32_t poll_interval_ms);

// Inactive (WiFi down): no polling, and every command but SetRoom is dropped.
void engine_set_active(bool active);
bool engine_active();

void engine_handle(const Command &cmd, uint32_t now_ms);
void engine_tick(uint32_t now_ms);

// Provided by the host: firmware posts an esp_event, tests record it.
void engine_publish(const SonosState &state);
void engine_save_room(const char *ip);
