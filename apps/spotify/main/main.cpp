#include "app_config.h"
#include "spotify/spotify_api.h"
#include "spotify/spotify_auth.h"
#include "spotify_setup.h"
#include "ui/ui.h"
#include "wifi_setup.h"

#include "display.h"
#include "encoder.h"
#include "hal_pins.h"
#include "haptic.h"
#include "knob_events.h"
#include "settings.h"
#include "wifi_manager.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "nvs.h"

#include "esp_system.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

static constexpr const char *TAG = "spotify";

ESP_EVENT_DEFINE_BASE(APP_EVENT);

// ─── Kconfig accessors (from sdkconfig), with NVS fallback for on-device setup

static char s_nvs_client_id[128] = {};

extern "C" const char *spotify_client_id(void) {
  // Try NVS first (set by on-device OAuth setup)
  if (s_nvs_client_id[0] != '\0')
    return s_nvs_client_id;

  nvs_handle_t h;
  if (nvs_open("spotify", NVS_READONLY, &h) == ESP_OK) {
    size_t len = sizeof(s_nvs_client_id);
    if (nvs_get_str(h, "client_id", s_nvs_client_id, &len) == ESP_OK &&
        s_nvs_client_id[0] != '\0') {
      nvs_close(h);
      return s_nvs_client_id;
    }
    nvs_close(h);
  }

  // Fall back to compiled-in value
#ifdef CONFIG_SPOTIFY_CLIENT_ID
  return CONFIG_SPOTIFY_CLIENT_ID;
#else
  return "";
#endif
}

extern "C" const char *spotify_refresh_token(void) {
#ifdef CONFIG_SPOTIFY_REFRESH_TOKEN
  return CONFIG_SPOTIFY_REFRESH_TOKEN;
#else
  return "";
#endif
}

// ─── Command queue for API calls (runs on a dedicated task with enough stack)

enum class CmdType : uint8_t {
  SetVolume,
  Play,
  Pause,
  Next,
  Prev,
  DjSpin,
  Seek,
  Transfer
};

struct Cmd {
  CmdType type;
  int32_t value; // for SetVolume
  SpotifyTransfer transfer;
};

static void post_control_result(CmdType type, int status,
                                const char *device_name = "") {
  SpotifyControlResult result = {};
  result.event_id = static_cast<int32_t>(type);
  result.status = status;
  snprintf(result.device_name, sizeof(result.device_name), "%s", device_name);
  esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_CONTROL_RESULT, &result,
                 sizeof(result), 0);
}

static QueueHandle_t s_cmd_queue = nullptr;

static void cmd_task(void *) {
  Cmd cmd;
  while (true) {
    if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE)
      continue;

    // Drain queued volume/seek changes — only send the latest of each
    if (cmd.type == CmdType::SetVolume || cmd.type == CmdType::Seek) {
      CmdType drain_type = cmd.type;
      Cmd newer;
      while (xQueueReceive(s_cmd_queue, &newer, 0) == pdTRUE) {
        if (newer.type == drain_type) {
          cmd.value = newer.value; // keep latest
        } else {
          // Different command — process current first, then this
          if (drain_type == CmdType::SetVolume)
            spotify_api_set_volume(cmd.value);
          else
            spotify_api_seek(cmd.value);
          cmd = newer;
          break;
        }
      }
    }

    int status = 0;
    switch (cmd.type) {
    case CmdType::SetVolume:
      status = spotify_api_set_volume(cmd.value);
      break;
    case CmdType::Play:
      status = spotify_api_play_pause(false); // not playing -> play
      break;
    case CmdType::Pause:
      status = spotify_api_play_pause(true); // playing -> pause
      break;
    case CmdType::Next:
      status = spotify_api_next();
      break;
    case CmdType::Prev:
      status = spotify_api_prev();
      break;
    case CmdType::DjSpin:
      haptic_buzz();
      spotify_api_play_random_liked();
      break;
    case CmdType::Seek:
      status = spotify_api_seek(cmd.value);
      break;
    case CmdType::Transfer:
      status = spotify_api_transfer(cmd.transfer.device_id);
      post_control_result(cmd.type, status, cmd.transfer.device_name);
      break;
    }
    if (cmd.type != CmdType::DjSpin && cmd.type != CmdType::Transfer)
      post_control_result(cmd.type, status);
  }
}

static void enqueue_cmd(CmdType type, int32_t value = 0) {
  Cmd cmd = {};
  cmd.type = type;
  cmd.value = value;
  xQueueSend(s_cmd_queue, &cmd, 0);
}

// ─── Volume debounce
static int s_pending_volume = -1;
static int64_t s_volume_send_at = 0;

// ─── Seek debounce
static int s_pending_seek = -1;
static int64_t s_seek_send_at = 0;

// ─── Event handlers

static char s_device_ip[32] = {};

static void get_device_ip(char *buf, size_t buf_size) {
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!netif) {
    strncpy(buf, "0.0.0.0", buf_size);
    return;
  }
  esp_netif_ip_info_t ip_info = {};
  if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
    snprintf(buf, buf_size, IPSTR, IP2STR(&ip_info.ip));
  } else {
    strncpy(buf, "0.0.0.0", buf_size);
  }
}

static TaskHandle_t s_wifi_conn_task = nullptr;
// True while the Spotify sign-in QR is up, so a Wi-Fi drop can restore it.
static volatile bool s_spotify_setup_active = false;

// ─── Wi-Fi setup fallback
//
// A saved network that has gone away would leave the knob retrying forever
// on "Connecting to Wi-Fi…" with no way to choose another. If nothing has
// connected WIFI_FALLBACK_MS after boot, flag the next boot to open the
// setup portal and restart. The portal owns the radio, so it runs on a clean
// boot before wifi_manager_init() rather than alongside the retry loop.

static constexpr int64_t WIFI_FALLBACK_MS = 60 * 1000;
static constexpr const char *SETUP_NS = "spotify";
static constexpr const char *SETUP_KEY = "wifi_setup";
static esp_timer_handle_t s_wifi_fallback_timer = nullptr;

static bool take_wifi_setup_request() {
  nvs_handle_t h;
  if (nvs_open(SETUP_NS, NVS_READWRITE, &h) != ESP_OK)
    return false;
  uint8_t requested = 0;
  nvs_get_u8(h, SETUP_KEY, &requested);
  if (requested) {
    nvs_erase_key(h, SETUP_KEY);
    nvs_commit(h);
  }
  nvs_close(h);
  return requested != 0;
}

static void wifi_fallback_cb(void *) {
  ESP_LOGW(TAG, "No Wi-Fi after %lld s, restarting into setup",
           WIFI_FALLBACK_MS / 1000);
  nvs_handle_t h;
  if (nvs_open(SETUP_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, SETUP_KEY, 1);
    nvs_commit(h);
    nvs_close(h);
  }
  esp_restart();
}

// WiFi connected logic runs on a dedicated task so it doesn't block
// the system event loop (spotify_setup_start blocks, and the token
// exchange needs the event loop for HTTP/TLS).
static void wifi_connected_task(void *) {
  get_device_ip(s_device_ip, sizeof(s_device_ip));
  ESP_LOGI(TAG, "Device IP: %s", s_device_ip);

  // Check if we have a refresh token (NVS or compiled-in)
  bool has_token =
      spotify_setup_has_token() || (spotify_refresh_token()[0] != '\0');

  if (!has_token) {
    ESP_LOGW(TAG, "No Spotify refresh token — starting setup flow");
    s_spotify_setup_active = true;
    spotify_setup_start(s_device_ip); // blocks until auth completes
    s_spotify_setup_active = false;
    ESP_LOGI(TAG, "Spotify setup complete — initializing auth");
  }

  ui_set_status("Connecting to Spotify...\nThis can take a moment");
  spotify_auth_init();

  const char *token = spotify_auth_get_token();
  if (token) {
    ui_set_status("Connected\nWaiting for playback...");
    spotify_api_init();
    spotify_api_start();
  } else {
    // Token failed — fall back to on-device OAuth setup
    ESP_LOGW(TAG, "Token failed — starting Spotify setup flow");
    get_device_ip(s_device_ip, sizeof(s_device_ip));
    s_spotify_setup_active = true;
    spotify_setup_start(s_device_ip); // blocks until auth completes
    s_spotify_setup_active = false;

    ESP_LOGI(TAG, "Spotify setup complete — retrying auth");
    spotify_auth_init();
    token = spotify_auth_get_token();
    if (token) {
      ui_set_status("Connected\nWaiting for playback...");
      spotify_api_init();
      spotify_api_start();
    } else {
      ui_set_status("Auth failed\nRestart and try again");
    }
  }

  s_wifi_conn_task = nullptr;
  vTaskDelete(nullptr);
}

static void on_wifi_connected(void *, esp_event_base_t, int32_t, void *) {
  ESP_LOGI(TAG, "WiFi connected");
  ui_set_offline(false);
  if (s_wifi_fallback_timer)
    esp_timer_stop(s_wifi_fallback_timer);

  // Only spawn one task — ignore reconnects while setup is in progress
  if (s_wifi_conn_task != nullptr) {
    ESP_LOGW(TAG, "WiFi connect task already running, skipping");
    if (s_spotify_setup_active && spotify_setup_url()[0]) {
      ui_show_spotify_setup(spotify_setup_url());
    }
    return;
  }

  xTaskCreatePinnedToCore(wifi_connected_task, "wifi_conn", 8192, nullptr, 5,
                          &s_wifi_conn_task, 1);
}

static void on_wifi_disconnected(void *, esp_event_base_t, int32_t, void *) {
  ESP_LOGW(TAG, "WiFi disconnected");
  spotify_api_stop();
  ui_set_offline(true);
}

static void on_spotify_state(void *, esp_event_base_t, int32_t, void *data) {
  auto *state = static_cast<SpotifyState *>(data);
  ui_update_state(state);
}

static void on_control_result(void *, esp_event_base_t, int32_t, void *data) {
  ui_control_result(static_cast<SpotifyControlResult *>(data));
}

static void on_poll_status(void *, esp_event_base_t, int32_t, void *data) {
  ui_poll_status(*static_cast<int32_t *>(data));
}

static void on_volume_set(void *, esp_event_base_t, int32_t, void *data) {
  auto vol = *static_cast<int32_t *>(data);
  s_pending_volume = vol;
  s_volume_send_at = esp_timer_get_time() + 200000;
}

static void on_play(void *, esp_event_base_t, int32_t, void *) {
  enqueue_cmd(CmdType::Play);
}

static void on_pause(void *, esp_event_base_t, int32_t, void *) {
  enqueue_cmd(CmdType::Pause);
}

static void on_next(void *, esp_event_base_t, int32_t, void *) {
  enqueue_cmd(CmdType::Next);
}

static void on_prev(void *, esp_event_base_t, int32_t, void *) {
  enqueue_cmd(CmdType::Prev);
}

static void on_seek(void *, esp_event_base_t, int32_t, void *data) {
  auto pos = *static_cast<int32_t *>(data);
  s_pending_seek = pos;
  s_seek_send_at = esp_timer_get_time() + 300000; // 300ms debounce
}

static void on_transfer(void *, esp_event_base_t, int32_t, void *data) {
  Cmd cmd = {};
  cmd.type = CmdType::Transfer;
  cmd.transfer = *static_cast<SpotifyTransfer *>(data);
  xQueueSend(s_cmd_queue, &cmd, 0);
}

static void on_dj_spin(void *, esp_event_base_t, int32_t, void *) {
  ESP_LOGI(TAG, "DJ SPIN!");
  enqueue_cmd(CmdType::DjSpin);
}

// ─── Debounce timer (volume + seek) — just enqueues, no HTTP here
static void debounce_timer_cb(void *) {
  // Periodic heap monitoring (every 60s) to catch leaks
  static int s_tick_count = 0;
  if (++s_tick_count >= 600) { // 600 × 100ms = 60s
    s_tick_count = 0;
    ESP_LOGI(
        TAG, "Heap: free=%lu internal=%lu min_ever=%lu",
        (unsigned long)esp_get_free_heap_size(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  }
  int64_t now = esp_timer_get_time();
  if (s_pending_volume >= 0 && now >= s_volume_send_at) {
    enqueue_cmd(CmdType::SetVolume, s_pending_volume);
    s_pending_volume = -1;
  }
  if (s_pending_seek >= 0 && now >= s_seek_send_at) {
    enqueue_cmd(CmdType::Seek, s_pending_seek);
    s_pending_seek = -1;
  }
}

// ─── NVS init

static void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

// ─── Entry point

extern "C" void app_main() {
  ESP_LOGI(TAG, "Spotify Knob starting");

  init_nvs();
  settings_init();

  // Command queue + task for API calls (needs big stack for HTTP+TLS)
  s_cmd_queue = xQueueCreate(8, sizeof(Cmd));
  xTaskCreatePinnedToCore(cmd_task, "spotify_cmd", 8192, nullptr, 5, nullptr,
                          1);

  // Event loop
  esp_event_loop_create_default();

  esp_event_handler_register(APP_EVENT, APP_EVENT_WIFI_CONNECTED,
                             on_wifi_connected, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_WIFI_DISCONNECTED,
                             on_wifi_disconnected, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_STATE_UPDATE,
                             on_spotify_state, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_CONTROL_RESULT,
                             on_control_result, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_POLL_STATUS,
                             on_poll_status, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_VOLUME_SET,
                             on_volume_set, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_PLAY, on_play,
                             nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_PAUSE, on_pause,
                             nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_NEXT, on_next,
                             nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_PREV, on_prev,
                             nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_DJ_SPIN, on_dj_spin,
                             nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_SEEK, on_seek,
                             nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_TRANSFER,
                             on_transfer, nullptr);

  // Hardware + UI
  ui_init();
  ui_show_splash();
  haptic_init();
  encoder_init();

  // Volume debounce timer (100ms periodic) — only enqueues, doesn't do HTTP
  esp_timer_create_args_t timer_args = {};
  timer_args.callback = debounce_timer_cb;
  timer_args.name = "debounce";
  esp_timer_handle_t vol_timer = nullptr;
  esp_timer_create(&timer_args, &vol_timer);
  esp_timer_start_periodic(vol_timer, 100000); // 100ms

  // A provisioned knob joins its saved network immediately. A generic build
  // with no saved or compiled-in credentials falls back to the shared portal,
  // as does a knob whose saved network never came up last boot.
  bool no_credentials =
      !wifi_setup_has_credentials() && CONFIG_RADIO_WIFI_SSID[0] == '\0';
  if (no_credentials || take_wifi_setup_request()) {
    ui_show_wifi_setup("spotify-knob");
    wifi_setup_start("spotify-knob");
  }

  ui_set_status("Connecting to Wi-Fi…");
  wifi_manager_init();

  esp_timer_create_args_t fallback_args = {};
  fallback_args.callback = wifi_fallback_cb;
  fallback_args.name = "wifi_fallback";
  esp_timer_create(&fallback_args, &s_wifi_fallback_timer);
  esp_timer_start_once(s_wifi_fallback_timer, WIFI_FALLBACK_MS * 1000);

  ESP_LOGI(TAG, "Init complete");
}
