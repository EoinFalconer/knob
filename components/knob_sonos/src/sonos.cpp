#include "sonos.h"
#include "knob_events.h"
#include "settings.h"
#include "sonos_config.h"
#include "sonos_control.h"
#include "sonos_engine.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

static constexpr const char *TAG = "sonos";
static constexpr int TICK_MS = 100;

static QueueHandle_t s_cmd_queue;
static std::atomic<int> s_pending_volume{-1};
static std::atomic<bool> s_volume_queued{false};

void engine_publish(const SonosState &state) {
  esp_event_post(APP_EVENT, APP_EVENT_SONOS_STATE_UPDATE, &state, sizeof(state),
                 0);
}

void engine_save_room(const char *ip) { settings_set_speaker_ip(ip); }

static uint32_t now_ms() {
  return static_cast<uint32_t>(pdTICKS_TO_MS(xTaskGetTickCount()));
}

// The single owner of all speaker HTTP: commands and polls never overlap.
static void net_task(void *) {
  while (true) {
    Command cmd;
    if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
      if (cmd.type == CmdType::Volume) {
        s_volume_queued = false;
        cmd.volume = s_pending_volume.load();
      }
      engine_handle(cmd, now_ms());
    }
    engine_tick(now_ms());
  }
}

static bool enqueue(CmdType type, const char *ip = nullptr, int port = 0,
                    const char *uri = nullptr) {
  if (!s_cmd_queue)
    return false;
  Command cmd = {};
  cmd.type = type;
  cmd.port = static_cast<uint16_t>(port);
  if (ip)
    snprintf(cmd.ip, sizeof(cmd.ip), "%s", ip);
  if (uri)
    snprintf(cmd.uri, sizeof(cmd.uri), "%s", uri);
  if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE)
    return true;
  ESP_LOGW(TAG, "Command queue full, dropped command %d",
           static_cast<int>(type));
  return false;
}

void sonos_init() {
  if (s_cmd_queue)
    return;
  engine_init(CONFIG_RADIO_SONOS_POLL_INTERVAL_MS);
  s_cmd_queue = xQueueCreate(8, sizeof(Command));
  char ip[40] = {};
  settings_get_speaker_ip(ip, sizeof(ip));
  if (ip[0])
    control_set_room(ip, SONOS_PORT);
  xTaskCreatePinnedToCore(net_task, "sonos", NET_TASK_STACK, nullptr,
                          NET_TASK_PRIO, nullptr, NET_TASK_CORE);
  ESP_LOGI(TAG, "Initialized, saved speaker: %s", ip[0] ? ip : "(none)");
}

void sonos_start() { engine_set_active(true); }
void sonos_stop() { engine_set_active(false); }

void sonos_set_stations(const char *const *urls, int count) {
  control_set_stations(urls, count);
}

void sonos_set_speaker(const char *ip, int port) {
  enqueue(CmdType::SetRoom, ip, port);
}

void sonos_select_speaker(const char *ip, int port, const char *resume_url) {
  enqueue(CmdType::Select, ip, port, resume_url);
}

void sonos_join_group(const char *ip, int port) {
  enqueue(CmdType::Join, ip, port);
}

void sonos_leave_group(const char *ip, int port) {
  enqueue(CmdType::Leave, ip, port);
}

void sonos_play_uri(const char *uri) {
  enqueue(CmdType::PlayUri, nullptr, 0, uri);
}

void sonos_play() { enqueue(CmdType::Play); }
void sonos_pause() { enqueue(CmdType::Pause); }
void sonos_stop_playback() { enqueue(CmdType::Stop); }
void sonos_previous() { enqueue(CmdType::Previous); }
void sonos_next() { enqueue(CmdType::Next); }
void sonos_seek_start() { enqueue(CmdType::SeekStart); }
void sonos_start_alarm(const char *url, const char *room) {
  if (!s_cmd_queue)
    return;
  Command cmd = {};
  cmd.type = CmdType::AlarmStart;
  snprintf(cmd.uri, sizeof(cmd.uri), "%s", url);
  snprintf(cmd.room, sizeof(cmd.room), "%s", room ? room : "");
  if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50)) != pdTRUE)
    ESP_LOGW(TAG, "Command queue full, dropped the alarm");
}
void sonos_stop_alarm() { enqueue(CmdType::AlarmStop); }

void sonos_set_volume(int level) {
  s_pending_volume = std::clamp(level, VOLUME_MIN, VOLUME_MAX);
  if (!s_volume_queued.exchange(true) && !enqueue(CmdType::Volume))
    s_volume_queued = false;
}
int sonos_fetch_art(const char *url, uint8_t *buf, int buf_size) {
  if (!url || !url[0] || !buf || buf_size <= 0)
    return 0;

  ESP_LOGI(TAG, "Fetching art: %.120s", url);

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = 8000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 1024;

  auto *client = esp_http_client_init(&cfg);
  if (!client)
    return 0;

  // Use open/read instead of perform to avoid event loop spam
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Art open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return 0;
  }

  int content_len = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);

  if (status != 200) {
    ESP_LOGW(TAG, "Art HTTP %d (content_len=%d)", status, content_len);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return 0;
  }

  // Read response body into buffer
  int total = 0;
  while (total < buf_size) {
    int read_len = esp_http_client_read(
        client, reinterpret_cast<char *>(buf + total), buf_size - total);
    if (read_len <= 0)
      break;
    total += read_len;
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (total <= 0) {
    ESP_LOGW(TAG, "Art: no data received");
    return 0;
  }

  ESP_LOGI(TAG, "Album art downloaded: %d bytes (JPEG: %s)", total,
           (total > 2 && buf[0] == 0xFF && buf[1] == 0xD8) ? "yes" : "no");
  return total;
}
