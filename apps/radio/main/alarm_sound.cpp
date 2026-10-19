#include "alarm_sound.h"

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

static constexpr const char *TAG = "alarm";
static constexpr uint16_t PORT = 8080;
static constexpr const char *PATH = "/alarm.mp3";
// Apple's "Breaking" ringtone (24 s), taken from the Mac's ToneLibrary and
// encoded without a bit reservoir or an Info header, so copies of it play
// back to back as one longer file. Three outlast the speaker's own minute
// limit (ALARM_MAX_MS in knob_sonos).
static constexpr int REPEATS = 3;

extern const uint8_t chime_start[] asm("_binary_alarm_mp3_start");
extern const uint8_t chime_end[] asm("_binary_alarm_mp3_end");

static httpd_handle_t s_server;

// Sonos wants a length up front, so the whole file is built for each request
// in PSRAM rather than streamed chunked.
static esp_err_t serve(httpd_req_t *req) {
  auto one = static_cast<size_t>(chime_end - chime_start);
  size_t total = one * REPEATS;
  auto *body = static_cast<char *>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM));
  if (!body)
    return httpd_resp_send_500(req);
  for (int i = 0; i < REPEATS; i++)
    memcpy(body + i * one, chime_start, one);
  httpd_resp_set_type(req, "audio/mpeg");
  esp_err_t err = httpd_resp_send(req, body, static_cast<ssize_t>(total));
  heap_caps_free(body);
  return err;
}

static bool start_server() {
  if (s_server)
    return true;
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = PORT;
  cfg.ctrl_port = PORT + 1;
  cfg.max_open_sockets = 2;
  cfg.lru_purge_enable = true;
  if (httpd_start(&s_server, &cfg) != ESP_OK) {
    ESP_LOGW(TAG, "Couldn't start the chime server");
    s_server = nullptr;
    return false;
  }
  httpd_uri_t uri = {};
  uri.uri = PATH;
  uri.method = HTTP_GET;
  uri.handler = serve;
  httpd_register_uri_handler(s_server, &uri);
  return true;
}

bool alarm_sound_url(char *out, size_t out_len) {
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip = {};
  if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.ip.addr == 0)
    return false;
  if (!start_server())
    return false;
  snprintf(out, out_len, "http://" IPSTR ":%u%s", IP2STR(&ip.ip), PORT, PATH);
  return true;
}
