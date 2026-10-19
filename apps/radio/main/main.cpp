#include "alarm_sound.h"
#include "app_config.h"
#include "discovery.h"
#include "encoder.h"
#include "haptic.h"
#include "lights.h"
#include "settings.h"
#include "sonos.h"
#include "timer.h"
#include "timer_events.h"
#include "weather.h"
#include "wifi_manager.h"

#include "ui/ui.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>

static constexpr const char *TAG = "main";

ESP_EVENT_DEFINE_BASE(APP_EVENT);

static int s_volume;
static int s_station_index;

static void on_station_changed(void *, esp_event_base_t, int32_t, void *data) {
  auto index = *static_cast<int32_t *>(data);
  if (index < 0 || index >= STATION_COUNT)
    return;
  s_station_index = index;
  settings_set_station_index(s_station_index);
  // Note: don't play here — on_play_requested handles playback.
  // confirm_browse() posts both STATION_CHANGED and PLAY_REQUESTED.
}

static void on_volume_changed(void *, esp_event_base_t, int32_t, void *data) {
  auto vol = *static_cast<int32_t *>(data);
  s_volume = vol;
  settings_set_volume(vol);
}

static void on_sonos_state(void *, esp_event_base_t, int32_t, void *data) {
  auto *state = static_cast<SonosState *>(data);
  ui_set_group(&state->group);
  ui_set_transport_actions(state->actions);
  ui_set_play_state(state->play_state);
  if (state->volume >= 0 && state->volume != s_volume) {
    s_volume = state->volume;
    ui_set_volume(s_volume);
  }
  if (state->station_index >= 0) {
    ui_set_station(state->station_index);
    ui_set_media_info(nullptr);
  } else if (state->media.has_media) {
    ui_set_media_info(&state->media);
  }
}

static void on_timer_started(void *, esp_event_base_t, int32_t, void *data) {
  auto *started = static_cast<TimerStarted *>(data);
  char label[64];
  timer_get_label(label, sizeof(label));
  ui_timer_started(started->total_s, started->left_ms, label);
}

// The kitchen chimes; the UI answers with Stop and Snooze. Without Wi-Fi the
// knob still shows it and buzzes.
static void on_timer_fired(void *, esp_event_base_t, int32_t, void *data) {
  auto *label = static_cast<const char *>(data);
  char url[96];
  if (alarm_sound_url(url, sizeof(url)))
    sonos_start_alarm(url, TIMER_ROOM);
  else
    ESP_LOGW(TAG, "Timer '%s' fired with no way to chime", label);
  ui_timer_fired(label);
}

static void on_timer_cancelled(void *, esp_event_base_t, int32_t, void *) {
  ui_timer_cancelled();
}

static void on_lights_changed(void *, esp_event_base_t, int32_t, void *data) {
  ui_set_lights(static_cast<LightsState *>(data));
}

// ─── Weather Events ─────────────────────────────────────────────────────────

static void on_weather_update(void *, esp_event_base_t, int32_t, void *data) {
  auto *w = static_cast<WeatherData *>(data);
  ui_set_weather(w);
}

static std::atomic<bool> s_scanning{false};

static void discover_and_connect_task(void *) {
  ui_show_scanning();

  static DiscoveryResult result;
  discovery_scan(&result);

  // First setup with one speaker: nothing to choose, just use it.
  if (result.count == 1 && !settings_has_speaker()) {
    auto &speaker = result.speakers[0];
    ESP_LOGI(TAG, "Single speaker found, auto-selecting: %s", speaker.name);
    sonos_set_speaker(speaker.ip, speaker.port);
    settings_set_speaker_name(speaker.name);
    ui_set_speaker_name(speaker.name);
    ui_show_speaker_picker(nullptr);
  } else {
    if (result.count == 0)
      ESP_LOGW(TAG, "No speakers found, showing picker with empty state");
    ui_show_speaker_picker(&result);
  }

  s_scanning = false;
  vTaskDelete(nullptr);
}

// One scan at a time: each open of the speakers sheet asks for one, and two
// at once would share the discovery buffers.
static void start_scan() {
  if (s_scanning.exchange(true))
    return;
  xTaskCreatePinnedToCore(discover_and_connect_task, "discover", 10240, nullptr,
                          NET_TASK_PRIO, nullptr, NET_TASK_CORE);
}

static void start_with_saved_speaker() {
  char ip[40] = {};
  char name[64] = {};
  settings_get_speaker_ip(ip, sizeof(ip));
  settings_get_speaker_name(name, sizeof(name));

  ESP_LOGI(TAG, "Reconnecting to saved speaker: %s (%s)", name, ip);
  sonos_set_speaker(ip);
  ui_set_speaker_name(name[0] ? name : ip);
}

static void on_wifi_connected(void *, esp_event_base_t, int32_t, void *) {
  ESP_LOGI(TAG, "WiFi connected");
  ui_set_wifi_status(true);
  weather_start();
  sonos_start();
  lights_start();

  // Set timezone and start NTP sync (idempotent — safe on reconnect)
  setenv("TZ", CONFIG_RADIO_TIMEZONE, 1);
  tzset();
  static bool sntp_started = false;
  if (!sntp_started) {
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.sync_cb = [](timeval *) { timer_restore(); };
    esp_netif_sntp_init(&sntp_cfg);
    sntp_started = true;
    ESP_LOGI(TAG, "SNTP started, TZ=%s", CONFIG_RADIO_TIMEZONE);
  }
  // After a crash or an update the clock is still set, so a saved timer can
  // come back now; after a power cut it waits for the first sync.
  timer_restore();

  if (settings_has_speaker()) {
    start_with_saved_speaker();
    // A quiet scan fills the speakers sheet, so even its first swipe up shows
    // the rooms at once instead of a spinner.
    start_scan();
  } else if (strlen(CONFIG_RADIO_SONOS_SPEAKER_IP) > 0 &&
             strcmp(CONFIG_RADIO_SONOS_SPEAKER_IP, "192.168.1.100") != 0) {
    // Use .env-configured speaker IP directly (skip discovery)
    ESP_LOGI(TAG, "Using configured speaker: %s",
             CONFIG_RADIO_SONOS_SPEAKER_IP);
    sonos_set_speaker(CONFIG_RADIO_SONOS_SPEAKER_IP);

    // Resolve speaker name from device description
    char speaker_name[64] = {};
    if (discovery_get_speaker_name(CONFIG_RADIO_SONOS_SPEAKER_IP, 1400,
                                   speaker_name, sizeof(speaker_name)) &&
        speaker_name[0]) {
      ESP_LOGI(TAG, "Speaker name: %s", speaker_name);
    } else {
      strncpy(speaker_name, CONFIG_RADIO_SONOS_SPEAKER_IP,
              sizeof(speaker_name) - 1);
    }
    settings_set_speaker_name(speaker_name);
    ui_set_speaker_name(speaker_name);
  } else {
    start_scan();
  }
}

static void on_wifi_disconnected(void *, esp_event_base_t, int32_t, void *) {
  ESP_LOGW(TAG, "WiFi disconnected");
  ui_set_wifi_status(false);
  sonos_stop();
}

static void on_play_requested(void *, esp_event_base_t, int32_t, void *) {
  ESP_LOGI(TAG, "Play requested — station: %s", STATIONS[s_station_index].name);
  sonos_play_uri(STATIONS[s_station_index].url);
}

static void on_stop_requested(void *, esp_event_base_t, int32_t, void *) {
  ESP_LOGI(TAG, "Stop requested");
  sonos_stop_playback();
}

// Polling keeps running during the scan: if the user backs out of the
// picker, the knob must still control the current room.
static void on_speaker_rescan(void *, esp_event_base_t, int32_t, void *) {
  ESP_LOGI(TAG, "Speaker rescan requested");
  start_scan();
}

static void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

static void register_events() {
  esp_event_loop_create_default();

  esp_event_handler_register(APP_EVENT, APP_EVENT_STATION_CHANGED,
                             on_station_changed, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_VOLUME_CHANGED,
                             on_volume_changed, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_PLAY_REQUESTED,
                             on_play_requested, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_STOP_REQUESTED,
                             on_stop_requested, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_SPEAKER_RESCAN,
                             on_speaker_rescan, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_WIFI_CONNECTED,
                             on_wifi_connected, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_WIFI_DISCONNECTED,
                             on_wifi_disconnected, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_SONOS_STATE_UPDATE,
                             on_sonos_state, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_TIMER_STARTED,
                             on_timer_started, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_TIMER_FIRED, on_timer_fired,
                             nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_TIMER_CANCELLED,
                             on_timer_cancelled, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_WEATHER_UPDATE,
                             on_weather_update, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_LIGHTS_CHANGED,
                             on_lights_changed, nullptr);
}

extern "C" void app_main() {
  ESP_LOGI(TAG, "Sonos Radio starting");

  init_nvs();
  settings_init();

  s_volume = settings_get_volume();
  s_station_index = settings_get_station_index();

  register_events();
  ui_init();
  haptic_init();
  encoder_init();
  discovery_init();
  sonos_init();

  // Register station URLs for URI matching
  static const char *station_urls[STATION_COUNT];
  for (int i = 0; i < STATION_COUNT; i++)
    station_urls[i] = STATIONS[i].url;
  sonos_set_stations(station_urls, STATION_COUNT);

  weather_init();
  timer_init();
  lights_init();
  wifi_manager_init();

  ESP_LOGI(TAG, "Init complete — station: %s, volume: %d",
           STATIONS[s_station_index].name, s_volume);
}
