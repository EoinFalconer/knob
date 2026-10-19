#include "sonos_engine.h"
#include "sonos_control.h"

#include <atomic>

// Written by whoever tracks WiFi; everything else is net-task only.
static std::atomic<bool> s_active{false};
static bool s_was_active;
static bool s_topology_stale = true;
static uint32_t s_poll_interval_ms;
static uint32_t s_next_poll_ms;
static uint32_t s_next_topology_ms;
static uint32_t s_last_volume_ms;
static bool s_volume_set;

namespace {
struct Fade {
  bool active;
  int from; // the group volume to put back once stopped
  uint32_t start_ms;
  uint32_t next_step_ms;
};
} // namespace
static Fade s_fade;
// While an alarm sounds on the controlled group, polls aren't published: the
// UI would show the chime as the music.
static uint32_t s_alarm_until_ms;
static bool s_alarm;

void engine_init(uint32_t poll_interval_ms) {
  s_poll_interval_ms = poll_interval_ms;
  s_active = false;
  s_was_active = false;
  s_topology_stale = true;
  s_next_poll_ms = 0;
  s_next_topology_ms = 0;
  s_volume_set = false;
  s_fade = {};
  s_alarm = false;
}

void engine_set_active(bool active) { s_active = active; }

bool engine_active() { return s_active; }

static bool due(uint32_t now, uint32_t at) {
  return static_cast<int32_t>(now - at) >= 0;
}

static void poll_soon(uint32_t now, uint32_t delay_ms = 0) {
  s_next_poll_ms = now + delay_ms;
}

static void apply_volume(int level, uint32_t now) {
  bool new_gesture =
      !s_volume_set || now - s_last_volume_ms >= VOLUME_GESTURE_GAP_MS;
  control_set_volume(level, new_gesture);
  s_volume_set = true;
  s_last_volume_ms = now;
}

// Stopping fades the group out, then stops it and puts the volume back, so
// the next play starts where the last one was. The snapshot taken first
// brings every member back to its own level.
static void start_fade(uint32_t now) {
  int from = control_get_volume();
  if (from <= 0) {
    control_transport("Stop");
    poll_soon(now, TRACK_CHANGE_SETTLE_MS);
    return;
  }
  control_set_volume(from, true);
  s_fade = {true, from, now, now + FADE_STEP_MS};
}

static void finish_fade(bool stop) {
  s_fade.active = false;
  if (stop)
    control_transport("Stop");
  control_set_volume(s_fade.from, false);
}

// Ease-out (quadratic). Sonos's volume scale is close to logarithmic, so a
// straight line in its units stays loud and then drops off at the end. Easing
// out spends the second half in the quiet range, where each step is the
// bigger change in loudness, and the fade sounds even.
static int fade_level(int from, uint32_t elapsed) {
  auto left = static_cast<int64_t>(FADE_OUT_MS - elapsed);
  auto total = static_cast<int64_t>(FADE_OUT_MS);
  return static_cast<int>(from * left * left / (total * total));
}

static void step_fade(uint32_t now) {
  if (!s_fade.active || !due(now, s_fade.next_step_ms))
    return;
  uint32_t elapsed = now - s_fade.start_ms;
  if (elapsed >= FADE_OUT_MS) {
    finish_fade(true);
    poll_soon(now);
    return;
  }
  control_set_volume(fade_level(s_fade.from, elapsed), false);
  s_fade.next_step_ms = now + FADE_STEP_MS;
}

static bool plays(CmdType type) {
  return type == CmdType::Play || type == CmdType::PlayUri;
}

static void end_alarm(uint32_t now) {
  s_alarm = false;
  control_alarm_restore();
  poll_soon(now, TRACK_CHANGE_SETTLE_MS);
}

void engine_handle(const Command &cmd, uint32_t now) {
  if (!s_active && cmd.type != CmdType::SetRoom)
    return;
  // Playing again calls the stop off; anything else lands after it.
  if (s_fade.active && cmd.type != CmdType::Stop)
    finish_fade(!plays(cmd.type));
  // Turning the knob sets the alarm's volume; anything else ends the alarm
  // and lands on the music it interrupted.
  if (s_alarm && cmd.type != CmdType::Volume && cmd.type != CmdType::AlarmStart)
    end_alarm(now);

  switch (cmd.type) {
  case CmdType::SetRoom:
    control_set_room(cmd.ip, cmd.port);
    engine_save_room(cmd.ip);
    s_topology_stale = true;
    poll_soon(now);
    break;
  case CmdType::Select:
    control_select(cmd.ip, cmd.port, cmd.uri);
    engine_save_room(cmd.ip);
    s_next_topology_ms = now + TOPOLOGY_REFRESH_MS;
    poll_soon(now);
    break;
  case CmdType::Join:
    control_join(cmd.ip, cmd.port);
    s_next_topology_ms = now + REGROUP_SETTLE_MS;
    poll_soon(now);
    break;
  case CmdType::Leave:
    control_leave(cmd.ip, cmd.port);
    s_next_topology_ms = now + REGROUP_SETTLE_MS;
    poll_soon(now);
    break;
  case CmdType::PlayUri:
    control_play_url(cmd.uri);
    poll_soon(now, TRACK_CHANGE_SETTLE_MS);
    break;
  case CmdType::Play:
    control_play();
    poll_soon(now, TRACK_CHANGE_SETTLE_MS);
    break;
  case CmdType::Pause:
    control_transport("Pause");
    poll_soon(now, TRACK_CHANGE_SETTLE_MS);
    break;
  case CmdType::Stop:
    if (!s_fade.active)
      start_fade(now);
    break;
  case CmdType::Volume:
    apply_volume(cmd.volume, now);
    break;
  case CmdType::Previous:
    control_transport("Previous");
    poll_soon(now, TRACK_CHANGE_SETTLE_MS);
    break;
  case CmdType::Next:
    control_transport("Next");
    poll_soon(now, TRACK_CHANGE_SETTLE_MS);
    break;
  case CmdType::SeekStart:
    control_seek_start();
    poll_soon(now, TRACK_CHANGE_SETTLE_MS);
    break;
  case CmdType::AlarmStart:
    s_alarm = control_alarm_start(cmd.uri, cmd.room, ALARM_VOLUME) || s_alarm;
    s_alarm_until_ms = now + ALARM_MAX_MS;
    break;
  case CmdType::AlarmStop:
    break; // ended above
  }
}

void engine_tick(uint32_t now) {
  if (!s_active) {
    s_was_active = false;
    return;
  }
  if (!s_was_active) {
    // Back online: the household may have regrouped while we were away.
    s_was_active = true;
    s_topology_stale = true;
    s_next_poll_ms = now;
  }

  if (s_topology_stale || due(now, s_next_topology_ms)) {
    bool ok = control_refresh_topology();
    s_topology_stale = false;
    s_next_topology_ms = now + (ok ? TOPOLOGY_REFRESH_MS : TOPOLOGY_RETRY_MS);
    poll_soon(now); // a regroup should reach the UI now, not next poll
  }
  step_fade(now);
  if (s_alarm && due(now, s_alarm_until_ms))
    end_alarm(now);
  if (due(now, s_next_poll_ms)) {
    SonosState state;
    bool hidden = s_alarm && control_alarm_shares_group();
    if (!hidden && control_poll(&state)) {
      // The stop is decided: don't show the fading volume or the music that
      // is still trailing off.
      if (s_fade.active) {
        state.volume = -1;
        state.play_state = PlayState::Stopped;
      }
      engine_publish(state);
    }
    s_next_poll_ms = now + s_poll_interval_ms;
  }
}
