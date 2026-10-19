#include "weather.h"
#include "app_config.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>

static constexpr const char *TAG = "weather";
static constexpr int FETCH_INTERVAL_MS = 5 * 60 * 1000;
static constexpr int RESP_BUF_SIZE = 48 * 1024;
static constexpr int TASK_STACK = 8192;
static constexpr const char *USER_AGENT =
    "knob-radio/1.0 github.com/rostimelk/knob";

static WeatherData s_data = {};
static TaskHandle_t s_task = nullptr;
static bool s_started = false;

// ─── Symbol Codes ───────────────────────────────────────────────────────────

enum class TimeOfDay { Day, Night, Unknown };

struct SymbolText {
  const char *code;
  const char *text;
};

static constexpr SymbolText SYMBOLS[] = {
    {"clearsky", "Clear"},
    {"fair", "Fair"},
    {"partlycloudy", "Partly cloudy"},
    {"cloudy", "Cloudy"},
    {"fog", "Fog"},
    {"lightrain", "Light rain"},
    {"rain", "Rain"},
    {"heavyrain", "Heavy rain"},
    {"lightrainshowers", "Showers"},
    {"rainshowers", "Showers"},
    {"heavyrainshowers", "Heavy showers"},
    {"lightrainandthunder", "Thunder"},
    {"rainandthunder", "Thunder"},
    {"heavyrainandthunder", "Thunder"},
    {"lightsnow", "Light snow"},
    {"snow", "Snow"},
    {"heavysnow", "Heavy snow"},
    {"lightsnowshowers", "Snow showers"},
    {"snowshowers", "Snow showers"},
    {"heavysnowshowers", "Heavy snow"},
    {"snowandthunder", "Snow & thunder"},
    {"lightsleet", "Sleet"},
    {"sleet", "Sleet"},
    {"heavysleet", "Heavy sleet"},
    {"lightsleetshowers", "Sleet"},
    {"sleetshowers", "Sleet"},
    {"heavysleetshowers", "Heavy sleet"},
    {"sleetandthunder", "Sleet & thunder"},
    {"lightssnowshowersandthunder", "Snow & thunder"},
    {"snowshowersandthunder", "Snow & thunder"},
    {"heavysnowshowersandthunder", "Snow & thunder"},
    {"lightrainshowersandthunder", "Thunder"},
    {"rainshowersandthunder", "Thunder"},
    {"heavyrainshowersandthunder", "Thunder"},
    {"lightssleetshowersandthunder", "Sleet & thunder"},
    {"sleetshowersandthunder", "Sleet & thunder"},
    {"heavysleetshowersandthunder", "Sleet & thunder"},
    {"lightsleetandthunder", "Sleet & thunder"},
    {"heavysleetandthunder", "Sleet & thunder"},
    {"lightsnowandthunder", "Snow & thunder"},
    {"heavysnowandthunder", "Snow & thunder"},
};

// "rain_day" → "rain". Writes the code without its day/night/polar-twilight
// suffix and says which one it had.
static TimeOfDay split_symbol(const char *symbol, char *base, size_t base_len) {
  snprintf(base, base_len, "%s", symbol);
  if (char *sep = strchr(base, '_')) {
    *sep = '\0';
    return strcmp(sep + 1, "night") == 0 ? TimeOfDay::Night : TimeOfDay::Day;
  }
  return TimeOfDay::Unknown;
}

static const char *symbol_text(const char *base) {
  for (const auto &entry : SYMBOLS) {
    if (strcmp(base, entry.code) == 0)
      return entry.text;
  }
  return base;
}

// ─── HTTP ───────────────────────────────────────────────────────────────────

struct HttpBuf {
  uint8_t *data;
  int len;
  int capacity;
};

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
  auto *buf = static_cast<HttpBuf *>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && buf && evt->data_len > 0) {
    int needed = buf->len + evt->data_len;
    if (needed < buf->capacity) {
      memcpy(buf->data + buf->len, evt->data, evt->data_len);
      buf->len += evt->data_len;
      buf->data[buf->len] = '\0';
    }
  }
  return ESP_OK;
}

// ─── Fetch & Parse ──────────────────────────────────────────────────────────

// Symbol code from the shortest forecast period that has one.
static const char *entry_symbol(cJSON *data) {
  for (const char *period : {"next_1_hours", "next_6_hours", "next_12_hours"}) {
    cJSON *next = cJSON_GetObjectItem(data, period);
    cJSON *summary = next ? cJSON_GetObjectItem(next, "summary") : nullptr;
    cJSON *sym =
        summary ? cJSON_GetObjectItem(summary, "symbol_code") : nullptr;
    if (sym && cJSON_IsString(sym))
      return sym->valuestring;
  }
  return nullptr;
}

// Codes like "cloudy" don't say whether the sun is up; the next code that
// does ("partlycloudy_night") answers for now too.
static bool is_night(cJSON *series) {
  int count = cJSON_GetArraySize(series);
  for (int i = 0; i < count; i++) {
    cJSON *entry = cJSON_GetArrayItem(series, i);
    cJSON *data = entry ? cJSON_GetObjectItem(entry, "data") : nullptr;
    const char *symbol = data ? entry_symbol(data) : nullptr;
    char base[WEATHER_SYMBOL_LEN];
    if (!symbol)
      continue;
    TimeOfDay t = split_symbol(symbol, base, sizeof(base));
    if (t != TimeOfDay::Unknown)
      return t == TimeOfDay::Night;
  }
  return false;
}

// Days since 1970-01-01 for a civil date
// (howardhinnant.github.io/date_algorithms).
static int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static cJSON *fetch_json(const char *url) {
  auto *raw = static_cast<uint8_t *>(
      heap_caps_malloc(RESP_BUF_SIZE, MALLOC_CAP_SPIRAM));
  if (!raw) {
    ESP_LOGE(TAG, "Failed to allocate response buffer");
    return nullptr;
  }
  HttpBuf resp = {raw, 0, RESP_BUF_SIZE};

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = 10000;
  cfg.event_handler = on_http_event;
  cfg.user_data = &resp;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 512;

  auto *client = esp_http_client_init(&cfg);
  if (!client) {
    heap_caps_free(raw);
    return nullptr;
  }
  esp_http_client_set_header(client, "User-Agent", USER_AGENT);
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  cJSON *root = nullptr;
  if (err == ESP_OK && status == 200)
    root = cJSON_Parse(reinterpret_cast<char *>(raw));
  else
    ESP_LOGW(TAG, "Fetch failed: err=%s status=%d", esp_err_to_name(err),
             status);
  heap_caps_free(raw);
  return root;
}

static bool number(cJSON *obj, const char *key, float *out) {
  cJSON *item = obj ? cJSON_GetObjectItem(obj, key) : nullptr;
  if (!item || !cJSON_IsNumber(item))
    return false;
  *out = static_cast<float>(item->valuedouble);
  return true;
}

static cJSON *instant_details(cJSON *entry) {
  cJSON *data = entry ? cJSON_GetObjectItem(entry, "data") : nullptr;
  cJSON *instant = data ? cJSON_GetObjectItem(data, "instant") : nullptr;
  return instant ? cJSON_GetObjectItem(instant, "details") : nullptr;
}

// High and low over the next 24 hourly entries.
static void day_range(cJSON *series) {
  constexpr int HOURS = 24;
  s_data.high = s_data.low = s_data.temperature;
  int count = std::min(cJSON_GetArraySize(series), HOURS);
  for (int i = 0; i < count; i++) {
    float t;
    if (!number(instant_details(cJSON_GetArrayItem(series, i)),
                "air_temperature", &t))
      continue;
    s_data.high = std::max(s_data.high, t);
    s_data.low = std::min(s_data.low, t);
  }
}

static bool fetch_forecast() {
  char url[128];
  snprintf(url, sizeof(url),
           "https://api.met.no/weatherapi/locationforecast/2.0/"
           "compact?lat=%s&lon=%s",
           CONFIG_RADIO_WEATHER_LAT, CONFIG_RADIO_WEATHER_LON);
  cJSON *root = fetch_json(url);
  if (!root)
    return false;

  cJSON *props = cJSON_GetObjectItem(root, "properties");
  cJSON *series = props ? cJSON_GetObjectItem(props, "timeseries") : nullptr;
  cJSON *first = series ? cJSON_GetArrayItem(series, 0) : nullptr;
  cJSON *details = instant_details(first);
  bool ok = number(details, "air_temperature", &s_data.temperature);
  if (ok) {
    number(details, "wind_speed", &s_data.wind_speed);
    number(details, "relative_humidity", &s_data.humidity);
    day_range(series);
    cJSON *data = cJSON_GetObjectItem(first, "data");
    if (const char *symbol = entry_symbol(data)) {
      snprintf(s_data.symbol, sizeof(s_data.symbol), "%s", symbol);
      char base[WEATHER_SYMBOL_LEN];
      split_symbol(symbol, base, sizeof(base));
      snprintf(s_data.condition, sizeof(s_data.condition), "%s",
               symbol_text(base));
    }
    s_data.night = is_night(series);
  }
  cJSON_Delete(root);
  return ok;
}

// "2026-10-17T18:02+02:00" → minutes after local midnight. The request asks
// for local time, so the clock part is already ours.
static int sun_minutes(cJSON *props, const char *event) {
  cJSON *e = cJSON_GetObjectItem(props, event);
  cJSON *t = e ? cJSON_GetObjectItem(e, "time") : nullptr;
  int h, m;
  if (!t || !cJSON_IsString(t) ||
      sscanf(t->valuestring, "%*d-%*d-%*dT%d:%d", &h, &m) != 2)
    return SUN_TIME_UNKNOWN;
  return h * 60 + m;
}

// Sunrise and sunset change slowly, so they're looked up once a day.
static void fetch_sun() {
  static int fetched_yday = -1;
  time_t now = time(nullptr);
  tm local = {};
  localtime_r(&now, &local);
  if (local.tm_yday == fetched_yday)
    return;

  int64_t local_s =
      days_from_civil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday) *
          86400 +
      int64_t{local.tm_hour} * 3600 + int64_t{local.tm_min} * 60 + local.tm_sec;
  int offset_min = static_cast<int>((local_s - now) / 60);
  char url[192];
  snprintf(url, sizeof(url),
           "https://api.met.no/weatherapi/sunrise/3.0/"
           "sun?lat=%s&lon=%s&date=%04d-%02d-%02d&offset=%c%02d:%02d",
           CONFIG_RADIO_WEATHER_LAT, CONFIG_RADIO_WEATHER_LON,
           local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
           offset_min < 0 ? '-' : '+', std::abs(offset_min) / 60,
           std::abs(offset_min) % 60);
  cJSON *root = fetch_json(url);
  if (!root)
    return;
  cJSON *props = cJSON_GetObjectItem(root, "properties");
  s_data.sunrise = props ? sun_minutes(props, "sunrise") : SUN_TIME_UNKNOWN;
  s_data.sunset = props ? sun_minutes(props, "sunset") : SUN_TIME_UNKNOWN;
  cJSON_Delete(root);
  fetched_yday = local.tm_yday;
}

static bool fetch_and_parse() {
  if (!fetch_forecast())
    return false;
  fetch_sun();
  s_data.valid = true;
  ESP_LOGI(TAG, "Weather: %.1f\xC2\xB0 C (%.0f..%.0f), %s (%s), sun %d..%d",
           s_data.temperature, s_data.low, s_data.high, s_data.condition,
           s_data.symbol, s_data.sunrise, s_data.sunset);
  esp_event_post(APP_EVENT, APP_EVENT_WEATHER_UPDATE, &s_data, sizeof(s_data),
                 pdMS_TO_TICKS(100));
  return true;
}

// ─── Task ───────────────────────────────────────────────────────────────────

static bool time_synced() { return time(nullptr) > 1'700'000'000; }

static void weather_task(void *) {
  // Wait for NTP — TLS cert validation requires a valid system clock
  for (int i = 0; i < 300 && !time_synced(); i++)
    vTaskDelay(pdMS_TO_TICKS(100));

  while (true) {
    if (fetch_and_parse()) {
      vTaskDelay(pdMS_TO_TICKS(FETCH_INTERVAL_MS));
    } else {
      vTaskDelay(pdMS_TO_TICKS(10000)); // retry sooner on failure
    }
  }
}

void weather_init() {}

void weather_start() {
  if (s_started)
    return;
  s_started = true;

  xTaskCreatePinnedToCore(weather_task, "weather", TASK_STACK, nullptr,
                          NET_TASK_PRIO - 1, &s_task, NET_TASK_CORE);
  ESP_LOGI(TAG, "Started (lat=%s, lon=%s)", CONFIG_RADIO_WEATHER_LAT,
           CONFIG_RADIO_WEATHER_LON);
}
