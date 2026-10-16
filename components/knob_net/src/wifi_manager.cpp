#include "wifi_manager.h"
#include "knob_events.h"
#include "settings.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include <algorithm>
#include <cstring>

static constexpr const char *TAG = "wifi";
static constexpr int BASE_RETRY_MS = 1000;
static constexpr int MAX_RETRY_MS = 30000;
// Associated but no DHCP lease — kick the association and start over.
static constexpr int IP_WAIT_MS = 15000;

static int s_retry_count;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_ip_watchdog;

static void schedule_retry() {
  int delay_ms = std::min(BASE_RETRY_MS * (1 << s_retry_count), MAX_RETRY_MS);
  s_retry_count++;
  ESP_LOGW(TAG, "Retry in %d ms (attempt %d)", delay_ms, s_retry_count);
  esp_timer_stop(s_retry_timer); // re-arm cleanly if already pending
  esp_timer_start_once(s_retry_timer, delay_ms * 1000LL);
}

static void attempt_connect(void *) {
  ESP_LOGI(TAG, "Connecting (attempt %d)...", s_retry_count + 1);
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    // A synchronous failure never produces a DISCONNECTED event, so the
    // retry chain would die here without this.
    ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    schedule_retry();
  }
}

// Getting associated is not enough — without an IP nothing works, and no
// further event will ever fire. Force a fresh cycle.
static void ip_watchdog_cb(void *) {
  ESP_LOGW(TAG, "Associated but no IP after %d ms — reconnecting", IP_WAIT_MS);
  esp_wifi_disconnect(); // triggers DISCONNECTED → retry
}

static void on_wifi_event(void *, esp_event_base_t, int32_t id, void *data) {
  if (id == WIFI_EVENT_STA_START) {
    attempt_connect(nullptr);
  } else if (id == WIFI_EVENT_STA_CONNECTED) {
    esp_timer_stop(s_ip_watchdog);
    esp_timer_start_once(s_ip_watchdog, IP_WAIT_MS * 1000LL);
  } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
    auto *info = static_cast<wifi_event_sta_disconnected_t *>(data);
    ESP_LOGW(TAG, "Disconnected (reason %d)", info->reason);
    esp_timer_stop(s_ip_watchdog);
    esp_event_post(APP_EVENT, APP_EVENT_WIFI_DISCONNECTED, nullptr, 0, 0);

    if (s_retry_count >= CONFIG_RADIO_WIFI_MAX_RETRIES) {
      ESP_LOGE(TAG, "Max retries reached, backing off");
      s_retry_count = CONFIG_RADIO_WIFI_MAX_RETRIES - 2;
    }
    schedule_retry();
  }
}

// Compiled-in (Kconfig) credentials are only a bootstrap: they live in the
// binary, not in flash, so an OTA to a generic build would send the knob
// back to the captive portal. Once they have produced a working connection,
// save them like the portal would. Idempotent — no NVS write on reconnects.
static void persist_credentials_if_new() {
  char ssid[33] = {};
  char pass[65] = {};
  settings_get_wifi_ssid(ssid, sizeof(ssid));
  settings_get_wifi_pass(pass, sizeof(pass));
  if (ssid[0] == '\0')
    return;

  for (int i = 0; i < settings_wifi_count(); i++) {
    WifiEntry e;
    if (settings_wifi_get(i, &e) && strcmp(e.ssid, ssid) == 0)
      return;
  }
  ESP_LOGI(TAG, "Saving credentials for %s to NVS", ssid);
  settings_wifi_save(ssid, pass);
}

static void on_ip_event(void *, esp_event_base_t, int32_t id, void *data) {
  if (id == IP_EVENT_STA_GOT_IP) {
    auto *info = static_cast<ip_event_got_ip_t *>(data);
    ESP_LOGI(TAG, "Connected — IP: " IPSTR, IP2STR(&info->ip_info.ip));
    esp_timer_stop(s_ip_watchdog);
    s_retry_count = 0;
    persist_credentials_if_new();
    esp_event_post(APP_EVENT, APP_EVENT_WIFI_CONNECTED, nullptr, 0, 0);
  } else if (id == IP_EVENT_STA_LOST_IP) {
    ESP_LOGW(TAG, "Lost IP — reconnecting");
    esp_wifi_disconnect(); // triggers DISCONNECTED → retry
  }
}

void wifi_manager_init() {
  ESP_ERROR_CHECK(esp_netif_init());
  // Only create STA netif if it doesn't already exist (wifi_picker may have)
  if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
    esp_netif_create_default_wifi_sta();
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event,
                             nullptr);
  esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, nullptr);

  const esp_timer_create_args_t timer_args = {
      .callback = attempt_connect,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_retry",
      .skip_unhandled_events = true,
  };
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

  const esp_timer_create_args_t wd_args = {
      .callback = ip_watchdog_cb,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_ip_wd",
      .skip_unhandled_events = true,
  };
  ESP_ERROR_CHECK(esp_timer_create(&wd_args, &s_ip_watchdog));

  char ssid[33] = {};
  char pass[65] = {};
  settings_get_wifi_ssid(ssid, sizeof(ssid));
  settings_get_wifi_pass(pass, sizeof(pass));

  if (ssid[0] == '\0') {
    ESP_LOGW(TAG, "No SSID configured — set via menuconfig or NVS");
    return;
  }

  wifi_config_t wifi_cfg = {};
  std::memcpy(wifi_cfg.sta.ssid, ssid,
              std::min(strlen(ssid), sizeof(wifi_cfg.sta.ssid)));
  std::memcpy(wifi_cfg.sta.password, pass,
              std::min(strlen(pass), sizeof(wifi_cfg.sta.password)));
  wifi_cfg.sta.threshold.authmode =
      pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "STA init — SSID: %s", ssid);
}
