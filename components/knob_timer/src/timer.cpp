#include "timer.h"
#include "knob_events.h"
#include "timer_events.h"
#include "voice_tools.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static constexpr const char *TAG = "timer";
static constexpr int MAX_SECONDS = 3600;
// Run out while the knob was off by less than this, a timer still rings when
// it's back: the pasta needs draining either way. Any later, it's dropped.
static constexpr int64_t RING_LATE_MS = 5 * 60 * 1000;
// Before SNTP has set it, the clock reads 1970.
static constexpr time_t CLOCK_SET_AFTER = 1'700'000'000;

// ─── State
// ──────────────────────────────────────────────────────────────────────────

static esp_timer_handle_t s_alarm;
static std::atomic<int64_t> s_deadline_us{0}; // 0 when none is running
static int32_t s_total_s;
static char s_label[64] = {};
static char s_fired_label[64] = {};
static std::atomic<bool> s_restored{false};

// ─── Persistence ────────────────────────────────────────────────────────────

struct Saved {
  int64_t deadline_ms; // wall clock
  int32_t total_s;
  char label[64];
};

static int64_t wall_ms() {
  timeval tv = {};
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < CLOCK_SET_AFTER)
    return 0;
  return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

static bool load(Saved *out) {
  nvs_handle_t h;
  if (nvs_open("timer", NVS_READONLY, &h) != ESP_OK)
    return false;
  size_t len = sizeof(*out);
  bool ok =
      nvs_get_blob(h, "saved", out, &len) == ESP_OK && len == sizeof(*out);
  nvs_close(h);
  out->label[sizeof(out->label) - 1] = '\0';
  return ok;
}

static void store(const Saved *saved) {
  nvs_handle_t h;
  if (nvs_open("timer", NVS_READWRITE, &h) != ESP_OK)
    return;
  if (saved)
    nvs_set_blob(h, "saved", saved, sizeof(*saved));
  else
    nvs_erase_key(h, "saved");
  nvs_commit(h);
  nvs_close(h);
}

// Without the wall clock there's nothing to save against; timer_restore saves
// it once the clock is set.
static void save_running() {
  int64_t now = wall_ms();
  if (!now || !timer_is_active())
    return;
  Saved saved = {now + timer_remaining_ms(), s_total_s, {}};
  snprintf(saved.label, sizeof(saved.label), "%s", s_label);
  store(&saved);
}

// Saving follows the timer's own events, so it runs on the event loop rather
// than holding up whoever started the timer.
static void on_own_event(void *, esp_event_base_t, int32_t id, void *) {
  if (id == APP_EVENT_TIMER_STARTED)
    save_running();
  else
    store(nullptr);
}

// ─── Format Helper ──────────────────────────────────────────────────────────

static int fmt_duration(char *buf, size_t len, int seconds) {
  int m = seconds / 60;
  int s = seconds % 60;
  if (m > 0)
    return snprintf(buf, len, "%d min %d sec", m, s);
  return snprintf(buf, len, "%d seconds", s);
}

// ─── Public API ─────────────────────────────────────────────────────────────

// Runs on the esp_timer task.
static void on_alarm(void *) {
  if (s_deadline_us.exchange(0) == 0)
    return; // cancelled as it went off
  snprintf(s_fired_label, sizeof(s_fired_label), "%s", s_label);
  ESP_LOGI(TAG, "Timer '%s' fired!", s_fired_label);
  esp_event_post(APP_EVENT, APP_EVENT_TIMER_FIRED, s_fired_label,
                 strlen(s_fired_label) + 1, 0);
}

static void arm(int total_s, int64_t left_ms, const char *label) {
  esp_timer_stop(s_alarm);
  snprintf(s_label, sizeof(s_label), "%s", label && label[0] ? label : "Timer");
  s_total_s = total_s;
  int64_t left_us = left_ms * 1000;
  s_deadline_us = esp_timer_get_time() + left_us;
  esp_timer_start_once(s_alarm, static_cast<uint64_t>(left_us));
  TimerStarted started = {total_s, static_cast<uint32_t>(left_ms)};
  esp_event_post(APP_EVENT, APP_EVENT_TIMER_STARTED, &started, sizeof(started),
                 0);
}

bool timer_start(int seconds, const char *label) {
  if (!s_alarm)
    return false;
  seconds = std::clamp(seconds, 1, MAX_SECONDS);
  arm(seconds, static_cast<int64_t>(seconds) * 1000, label);
  ESP_LOGI(TAG, "Started: %ds label='%s'", seconds, s_label);
  return true;
}

// A timer started since boot is newer than the saved one, and replaces it.
void timer_restore() {
  int64_t now = wall_ms();
  if (!now || !s_alarm || s_restored.exchange(true))
    return;
  if (timer_is_active()) {
    save_running();
    return;
  }
  Saved saved;
  if (!load(&saved))
    return;
  int64_t left = saved.deadline_ms - now;
  if (left <= -RING_LATE_MS) {
    ESP_LOGI(TAG, "'%s' ran out while off; dropped", saved.label);
    store(nullptr);
  } else if (left <= 0) {
    ESP_LOGI(TAG, "'%s' ran out while off; ringing now", saved.label);
    snprintf(s_label, sizeof(s_label), "%s", saved.label);
    snprintf(s_fired_label, sizeof(s_fired_label), "%s", saved.label);
    esp_event_post(APP_EVENT, APP_EVENT_TIMER_FIRED, s_fired_label,
                   strlen(s_fired_label) + 1, 0);
  } else {
    int total = std::clamp<int>(saved.total_s, 1, MAX_SECONDS);
    arm(total, std::min<int64_t>(left, total * 1000LL), saved.label);
    ESP_LOGI(TAG, "Restored '%s': %lld s left", s_label,
             static_cast<long long>(left / 1000));
  }
}

bool timer_cancel() {
  if (s_alarm)
    esp_timer_stop(s_alarm);
  if (s_deadline_us.exchange(0) == 0)
    return false;
  ESP_LOGI(TAG, "Cancelled");
  esp_event_post(APP_EVENT, APP_EVENT_TIMER_CANCELLED, nullptr, 0, 0);
  return true;
}

bool timer_is_active() { return s_deadline_us.load() != 0; }

uint32_t timer_remaining_ms() {
  int64_t deadline = s_deadline_us.load();
  if (deadline == 0)
    return 0;
  int64_t left = std::max<int64_t>(0, deadline - esp_timer_get_time());
  return static_cast<uint32_t>((left + 999) / 1000);
}

int timer_remaining_sec() {
  return static_cast<int>((timer_remaining_ms() + 999) / 1000);
}

void timer_get_label(char *buf, int buf_len) {
  if (buf_len <= 0)
    return;
  strncpy(buf, s_label, buf_len - 1);
  buf[buf_len - 1] = '\0';
}

void timer_init() {
  if (!s_alarm) {
    esp_timer_create_args_t args = {};
    args.callback = on_alarm;
    args.name = "timer";
    esp_timer_create(&args, &s_alarm);
    for (int32_t id : {APP_EVENT_TIMER_STARTED, APP_EVENT_TIMER_FIRED,
                       APP_EVENT_TIMER_CANCELLED})
      esp_event_handler_register(APP_EVENT, id, on_own_event, nullptr);
  }
  s_deadline_us = 0;
  s_label[0] = '\0';
  ESP_LOGI(TAG, "Initialized");
}

// ─── Voice Tools ────────────────────────────────────────────────────────────

static bool handle_set_timer(const char *args, ToolResult *r) {
  int seconds = 0;
  if (!tool_json_get_int(args, "seconds", &seconds) || seconds < 1) {
    r->success = false;
    snprintf(r->output, sizeof(r->output),
             "Invalid duration. Provide seconds between 1 and 3600.");
    return true;
  }

  char label[64] = "Timer";
  tool_json_get_string(args, "label", label, sizeof(label));

  timer_start(seconds, label);

  r->success = true;
  char dur[32];
  fmt_duration(dur, sizeof(dur), std::clamp(seconds, 1, MAX_SECONDS));
  snprintf(r->output, sizeof(r->output), "Timer '%s' set for %s.", label, dur);
  return true;
}

static bool handle_cancel_timer(const char *, ToolResult *r) {
  r->success = true;
  if (timer_cancel())
    snprintf(r->output, sizeof(r->output), "Timer cancelled.");
  else
    snprintf(r->output, sizeof(r->output), "No timer was running.");
  return true;
}

static bool handle_get_timer_status(const char *, ToolResult *r) {
  int rem = timer_remaining_sec();
  r->success = true;
  if (rem <= 0) {
    snprintf(r->output, sizeof(r->output), "No timer is currently running.");
  } else {
    char label[64];
    timer_get_label(label, sizeof(label));
    char dur[32];
    fmt_duration(dur, sizeof(dur), rem);
    snprintf(r->output, sizeof(r->output), "Timer '%s': %s remaining.", label,
             dur);
  }
  return true;
}

REGISTER_TOOL(
    set_timer, "set_timer",
    "Set a countdown timer. When it expires the speaker chimes and the knob "
    "offers Stop and Snooze. Only one timer at a time.",
    R"J({"type":"object","properties":{"seconds":{"type":"integer","description":"Duration in seconds (1-3600)"},"label":{"type":"string","description":"Optional short label, e.g. 'eggs' or 'pizza'"}},"required":["seconds"]})J",
    handle_set_timer);

REGISTER_TOOL(cancel_timer, "cancel_timer",
              "Cancel the currently running timer, if any.",
              R"J({"type":"object","properties":{}})J", handle_cancel_timer);

REGISTER_TOOL(get_timer_status, "get_timer_status",
              "Check how much time is left on the current timer.",
              R"J({"type":"object","properties":{}})J",
              handle_get_timer_status);
