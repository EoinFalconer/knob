#include "settings.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

static constexpr const char *TAG = "settings";
static constexpr const char *NVS_NAMESPACE = "radio";

static constexpr int VOLUME_MIN = 0;
static constexpr int VOLUME_MAX = 100;

static constexpr const char *KEY_VOLUME = "volume";
static constexpr const char *KEY_STATION = "station";
static constexpr const char *KEY_SPEAKER_IP = "speaker_ip";
static constexpr const char *KEY_SPEAKER_NAME = "speaker_nm";
static constexpr const char *KEY_WIFI_SSID = "wifi_ssid";
static constexpr const char *KEY_WIFI_PASS = "wifi_pass";
static constexpr const char *KEY_OPENAI_KEY = "openai_key";

static nvs_handle_t s_nvs;

static void wifi_migrate_legacy();

static int32_t read_i32(const char *key, int32_t fallback) {
  int32_t val = fallback;
  if (nvs_get_i32(s_nvs, key, &val) != ESP_OK)
    return fallback;
  return val;
}

static void write_i32(const char *key, int32_t val) {
  nvs_set_i32(s_nvs, key, val);
  nvs_commit(s_nvs);
}

static void read_str(const char *key, char *buf, size_t len,
                     const char *fallback) {
  size_t required = len;
  if (nvs_get_str(s_nvs, key, buf, &required) != ESP_OK) {
    strncpy(buf, fallback, len);
    buf[len - 1] = '\0';
  }
}

static void write_str(const char *key, const char *val) {
  nvs_set_str(s_nvs, key, val);
  nvs_commit(s_nvs);
}

void settings_init() {
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    return;
  }
  ESP_LOGI(TAG, "NVS initialized");
  wifi_migrate_legacy();
}

int settings_get_volume() {
  return std::clamp(
      static_cast<int>(read_i32(KEY_VOLUME, CONFIG_RADIO_VOLUME_DEFAULT)),
      VOLUME_MIN, VOLUME_MAX);
}

void settings_set_volume(int volume) {
  write_i32(KEY_VOLUME, std::clamp(volume, VOLUME_MIN, VOLUME_MAX));
}

int settings_get_station_index() {
  return std::clamp(static_cast<int>(read_i32(KEY_STATION, 0)), 0, 99);
}

void settings_set_station_index(int index) {
  write_i32(KEY_STATION, std::clamp(index, 0, 99));
}

void settings_get_speaker_ip(char *buf, size_t len) {
  read_str(KEY_SPEAKER_IP, buf, len, CONFIG_RADIO_SONOS_SPEAKER_IP);
}

void settings_set_speaker_ip(const char *ip) { write_str(KEY_SPEAKER_IP, ip); }

void settings_get_speaker_name(char *buf, size_t len) {
  read_str(KEY_SPEAKER_NAME, buf, len, "");
}

void settings_set_speaker_name(const char *name) {
  write_str(KEY_SPEAKER_NAME, name);
}

bool settings_has_speaker() {
  char ip[40] = {};
  settings_get_speaker_ip(ip, sizeof(ip));
  return ip[0] != '\0' && strcmp(ip, CONFIG_RADIO_SONOS_SPEAKER_IP) != 0;
}

void settings_get_wifi_ssid(char *buf, size_t len) {
  read_str(KEY_WIFI_SSID, buf, len, CONFIG_RADIO_WIFI_SSID);
}

void settings_set_wifi_ssid(const char *ssid) {
  write_str(KEY_WIFI_SSID, ssid);
}

void settings_get_wifi_pass(char *buf, size_t len) {
  read_str(KEY_WIFI_PASS, buf, len, CONFIG_RADIO_WIFI_PASSWORD);
}

void settings_set_wifi_pass(const char *pass) {
  write_str(KEY_WIFI_PASS, pass);
}

// ─── Multi-network WiFi storage ───────────────────────────────────────────

int settings_wifi_count() {
  return std::clamp(static_cast<int>(read_i32("wifi_cnt", 0)), 0, WIFI_MAX_SAVED);
}

static void wifi_write_count(int n) {
  write_i32("wifi_cnt", std::clamp(n, 0, WIFI_MAX_SAVED));
}

static void wifi_key(char *buf, const char *prefix, int index) {
  // e.g. "ws0", "wp0" — short keys for NVS 15-char limit
  snprintf(buf, 16, "%s%d", prefix, index);
}

bool settings_wifi_get(int index, WifiEntry *entry) {
  if (index < 0 || index >= settings_wifi_count()) return false;
  char key[16];
  wifi_key(key, "ws", index);
  read_str(key, entry->ssid, sizeof(entry->ssid), "");
  wifi_key(key, "wp", index);
  read_str(key, entry->pass, sizeof(entry->pass), "");
  return entry->ssid[0] != '\0';
}

void settings_wifi_save(const char *ssid, const char *pass) {
  int count = settings_wifi_count();

  // Check if SSID already exists — find its index
  int existing = -1;
  for (int i = 0; i < count; i++) {
    WifiEntry e;
    if (settings_wifi_get(i, &e) && strcmp(e.ssid, ssid) == 0) {
      existing = i;
      break;
    }
  }

  // Shift entries down to make room at index 0
  int start = (existing >= 0) ? existing : std::min(count, WIFI_MAX_SAVED - 1);
  for (int i = start; i > 0; i--) {
    WifiEntry e;
    if (settings_wifi_get(i - 1, &e)) {
      char key[16];
      wifi_key(key, "ws", i);
      write_str(key, e.ssid);
      wifi_key(key, "wp", i);
      write_str(key, e.pass);
    }
  }

  // Write new entry at index 0
  char key[16];
  wifi_key(key, "ws", 0);
  write_str(key, ssid);
  wifi_key(key, "wp", 0);
  write_str(key, pass);

  if (existing < 0 && count < WIFI_MAX_SAVED) {
    wifi_write_count(count + 1);
  }

  // Also update the legacy single-network keys for wifi_manager compatibility
  settings_set_wifi_ssid(ssid);
  settings_set_wifi_pass(pass);
}

bool settings_wifi_remove(const char *ssid) {
  int count = settings_wifi_count();
  int found = -1;
  for (int i = 0; i < count; i++) {
    WifiEntry e;
    if (settings_wifi_get(i, &e) && strcmp(e.ssid, ssid) == 0) {
      found = i;
      break;
    }
  }
  if (found < 0) return false;

  // Shift entries up
  for (int i = found; i < count - 1; i++) {
    WifiEntry e;
    if (settings_wifi_get(i + 1, &e)) {
      char key[16];
      wifi_key(key, "ws", i);
      write_str(key, e.ssid);
      wifi_key(key, "wp", i);
      write_str(key, e.pass);
    }
  }
  wifi_write_count(count - 1);
  return true;
}

// ─── Migrate legacy single-network credentials into multi-network storage
// Called once from settings_init(). If legacy keys exist but wifi_cnt == 0,
// import them as saved network 0.
static void wifi_migrate_legacy() {
  int count = settings_wifi_count();
  if (count > 0) return; // already have multi-network data

  // Check if legacy NVS keys have a real SSID (not just Kconfig default)
  char ssid[33] = {};
  size_t len = sizeof(ssid);
  if (nvs_get_str(s_nvs, KEY_WIFI_SSID, ssid, &len) != ESP_OK || ssid[0] == '\0') {
    return; // no legacy credentials
  }

  char pass[65] = {};
  len = sizeof(pass);
  nvs_get_str(s_nvs, KEY_WIFI_PASS, pass, &len); // OK if missing

  ESP_LOGI(TAG, "Migrating legacy WiFi credentials: %s", ssid);
  settings_wifi_save(ssid, pass);
}

void settings_get_openai_api_key(char *buf, size_t len) {
  const char *fallback = "";
#ifdef CONFIG_RADIO_OPENAI_API_KEY
  fallback = CONFIG_RADIO_OPENAI_API_KEY;
#endif
  read_str(KEY_OPENAI_KEY, buf, len, fallback);
}

void settings_set_openai_api_key(const char *key) {
  write_str(KEY_OPENAI_KEY, key);
}
