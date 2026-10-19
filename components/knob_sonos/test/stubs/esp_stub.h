#pragma once

// Only the ESP-IDF surface the knob_sonos control code uses. Host-test
// doubles, not a substitute for an IDF build.
#include <cstddef>
#include <cstdint>
#include <cstdlib>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
inline const char *esp_err_to_name(esp_err_t) { return "host error"; }
inline void host_log(const char *, const char *, ...) {}
#define ESP_LOGE(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)
#define ESP_LOGI(...) host_log(__VA_ARGS__)
#define ESP_LOGD(...) host_log(__VA_ARGS__)

constexpr int MALLOC_CAP_SPIRAM = 1;
inline void *heap_caps_malloc(size_t n, int) { return std::malloc(n); }
inline void heap_caps_free(void *p) { std::free(p); }

enum esp_http_client_method_t { HTTP_METHOD_GET, HTTP_METHOD_POST };
constexpr int HTTP_EVENT_ON_DATA = 1;
struct esp_http_client_event_t {
  int event_id;
  void *user_data;
  int data_len;
  void *data;
};
struct esp_http_client_config_t {
  const char *url;
  esp_http_client_method_t method;
  int timeout_ms;
  esp_err_t (*event_handler)(esp_http_client_event_t *);
  void *user_data;
};
struct HttpClient;
using esp_http_client_handle_t = HttpClient *;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char *,
                                     const char *);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t, const char *,
                                         int);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
