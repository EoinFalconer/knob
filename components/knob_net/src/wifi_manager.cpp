#include "wifi_manager.h"
#include "knob_events.h"
#include "settings.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include <algorithm>
#include <cstring>

static constexpr const char *TAG = "wifi";
static constexpr int BASE_RETRY_MS = 1000;
// Knobs run on mains power, so a short cap costs nothing and gets us back
// online quickly when the signal is marginal.
static constexpr int MAX_RETRY_MS = 10000;
// Associated but no DHCP lease — kick the association and start over.
static constexpr int IP_WAIT_MS = 15000;
// Offline this long despite retries: restart the WiFi driver from scratch,
// which clears whatever state a reboot would have cleared.
static constexpr int64_t STALL_RESET_MS = 5 * 60 * 1000;

static int s_retry_count;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_ip_watchdog;
static esp_timer_handle_t s_stall_timer;

static char s_ssid[33];
static char s_pass[65];
static bool s_scanning;    // a directed scan we started is in flight
static int s_scan_misses;  // consecutive directed scans that heard nothing
static bool s_resetting;   // driver stop/start in progress; ignore its events

static void schedule_retry() {
  int delay_ms = std::min(BASE_RETRY_MS * (1 << s_retry_count), MAX_RETRY_MS);
  s_retry_count++;
  ESP_LOGW(TAG, "Retry in %d ms (attempt %d)", delay_ms, s_retry_count);
  esp_timer_stop(s_retry_timer); // re-arm cleanly if already pending
  esp_timer_start_once(s_retry_timer, delay_ms * 1000LL);
}

// Station config for the saved network. With a BSSID the driver skips its
// own scan and goes straight to that AP on that channel.
static void apply_sta_config(const uint8_t *bssid, uint8_t channel) {
  wifi_config_t cfg = {};
  std::memcpy(cfg.sta.ssid, s_ssid,
              std::min(strlen(s_ssid), sizeof(cfg.sta.ssid)));
  std::memcpy(cfg.sta.password, s_pass,
              std::min(strlen(s_pass), sizeof(cfg.sta.password)));
  cfg.sta.threshold.authmode =
      s_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  if (bssid) {
    std::memcpy(cfg.sta.bssid, bssid, sizeof(cfg.sta.bssid));
    cfg.sta.bssid_set = true;
    cfg.sta.channel = channel;
  }
  // At the edge of range one missed auth/assoc frame shouldn't cost a whole
  // scan-and-backoff cycle — let the driver retry the same AP a few times.
  cfg.sta.failure_retry_cnt = 3;
  esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

static void connect_now() {
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK) {
    // A synchronous failure never produces a DISCONNECTED event, so the
    // retry chain would die here without this.
    ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    schedule_retry();
  }
}

// A hidden SSID never beacons its name, and the driver's own connect-time
// scan gives each channel so little time that it routinely misses the probe
// response — reason 201 and a long backoff, over and over. Ask for the
// network by name with a generous dwell, then connect to the exact BSSID and
// channel we heard. Works the same for visible networks.
static void attempt_connect(void *) {
  ESP_LOGI(TAG, "Connecting (attempt %d)...", s_retry_count + 1);

  wifi_scan_config_t sc = {};
  sc.ssid = reinterpret_cast<uint8_t *>(s_ssid);
  sc.show_hidden = true;
  sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  sc.scan_time.active.min = 100;
  sc.scan_time.active.max = 400;

  esp_err_t err = esp_wifi_scan_start(&sc, false);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Directed scan failed (%s) — connecting blind",
             esp_err_to_name(err));
    apply_sta_config(nullptr, 0);
    connect_now();
    return;
  }
  s_scanning = true;
}

static void on_scan_done() {
  if (!s_scanning)
    return; // not our scan
  s_scanning = false;

  // The scan was filtered by SSID, so every record is our network — pick the
  // strongest. Hidden APs may report an empty name; that's still a match.
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  wifi_ap_record_t recs[8];
  if (n > 8)
    n = 8;
  if (n == 0)
    esp_wifi_clear_ap_list(); // get_ap_records would have freed it
  else
    esp_wifi_scan_get_ap_records(&n, recs);

  int best = -1;
  for (int i = 0; i < n; i++)
    if (best < 0 || recs[i].rssi > recs[best].rssi)
      best = i;

  if (best >= 0) {
    s_scan_misses = 0;
    ESP_LOGI(TAG, "Found %s on ch %d (rssi %d)", s_ssid, recs[best].primary,
             recs[best].rssi);
    apply_sta_config(recs[best].bssid, recs[best].primary);
    connect_now();
  } else if (++s_scan_misses % 3 == 0) {
    // Not hearing the AP isn't proof it's gone — let the driver try on its
    // own every third time.
    ESP_LOGW(TAG, "%s not found by scan — trying a blind connect", s_ssid);
    apply_sta_config(nullptr, 0);
    connect_now();
  } else {
    ESP_LOGW(TAG, "%s not found by scan", s_ssid);
    schedule_retry();
  }
}

// Getting associated is not enough — without an IP nothing works, and no
// further event will ever fire. Force a fresh cycle.
static void ip_watchdog_cb(void *) {
  ESP_LOGW(TAG, "Associated but no IP after %d ms — reconnecting", IP_WAIT_MS);
  esp_wifi_disconnect(); // triggers DISCONNECTED → retry
}

static void arm_stall_timer() {
  if (!esp_timer_is_active(s_stall_timer))
    esp_timer_start_once(s_stall_timer, STALL_RESET_MS * 1000LL);
}

static void stall_reset_cb(void *) {
  ESP_LOGW(TAG, "Offline for %lld s — restarting WiFi driver (free heap %lu)",
           STALL_RESET_MS / 1000,
           static_cast<unsigned long>(esp_get_free_heap_size()));
  s_resetting = true;
  s_scanning = false;
  esp_timer_stop(s_retry_timer);
  esp_timer_stop(s_ip_watchdog);
  esp_wifi_stop();
  s_retry_count = 0;
  s_scan_misses = 0;
  apply_sta_config(nullptr, 0);
  esp_wifi_start(); // STA_START → attempt_connect
  arm_stall_timer();
}

static void on_wifi_event(void *, esp_event_base_t, int32_t id, void *data) {
  if (id == WIFI_EVENT_STA_START) {
    s_resetting = false;
    // Mains powered: keep the radio awake so weak beacons aren't missed.
    esp_wifi_set_ps(WIFI_PS_NONE);
    attempt_connect(nullptr);
  } else if (id == WIFI_EVENT_SCAN_DONE) {
    on_scan_done();
  } else if (id == WIFI_EVENT_STA_CONNECTED) {
    esp_timer_stop(s_ip_watchdog);
    esp_timer_start_once(s_ip_watchdog, IP_WAIT_MS * 1000LL);
  } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
    auto *info = static_cast<wifi_event_sta_disconnected_t *>(data);
    ESP_LOGW(TAG, "Disconnected (reason %d, rssi %d, free heap %lu)",
             info->reason, info->rssi,
             static_cast<unsigned long>(esp_get_free_heap_size()));
    if (s_resetting)
      return; // fallout from our own esp_wifi_stop()
    s_scanning = false;
    esp_timer_stop(s_ip_watchdog);
    arm_stall_timer();
    esp_event_post(APP_EVENT, APP_EVENT_WIFI_DISCONNECTED, nullptr, 0, 0);

    if (s_retry_count >= CONFIG_RADIO_WIFI_MAX_RETRIES) {
      ESP_LOGE(TAG, "Max retries reached, backing off");
      s_retry_count = CONFIG_RADIO_WIFI_MAX_RETRIES - 2;
    }
    schedule_retry();
  }
}

static void on_ip_event(void *, esp_event_base_t, int32_t id, void *data) {
  if (id == IP_EVENT_STA_GOT_IP) {
    auto *info = static_cast<ip_event_got_ip_t *>(data);
    ESP_LOGI(TAG, "Connected — IP: " IPSTR, IP2STR(&info->ip_info.ip));
    esp_timer_stop(s_ip_watchdog);
    esp_timer_stop(s_stall_timer);
    s_retry_count = 0;
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

  const esp_timer_create_args_t stall_args = {
      .callback = stall_reset_cb,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "wifi_stall",
      .skip_unhandled_events = true,
  };
  ESP_ERROR_CHECK(esp_timer_create(&stall_args, &s_stall_timer));

  settings_get_wifi_ssid(s_ssid, sizeof(s_ssid));
  settings_get_wifi_pass(s_pass, sizeof(s_pass));

  if (s_ssid[0] == '\0') {
    ESP_LOGW(TAG, "No SSID configured — set via menuconfig or NVS");
    return;
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  apply_sta_config(nullptr, 0);
  ESP_ERROR_CHECK(esp_wifi_start());
  arm_stall_timer(); // never connecting at all counts as a stall too

  ESP_LOGI(TAG, "STA init — SSID: %s", s_ssid);
}
