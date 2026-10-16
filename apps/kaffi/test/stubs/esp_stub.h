#pragma once

// Only the ESP-IDF surface used by pending.cpp and sanity_api.cpp. These
// declarations are host-test doubles, not a substitute for an IDF build.
#include <cstddef>
#include <cstdint>
#include <cstdlib>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 1;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 2;
inline const char *esp_err_to_name(esp_err_t) { return "host error"; }
inline void host_log(const char *, const char *, ...) {}
#define ESP_LOGE(...) host_log(__VA_ARGS__)
#define ESP_LOGW(...) host_log(__VA_ARGS__)
#define ESP_LOGI(...) host_log(__VA_ARGS__)

using nvs_handle_t = int;
constexpr int NVS_READONLY = 0;
constexpr int NVS_READWRITE = 1;
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
void esp_fill_random(void *, size_t);

constexpr int KNOB_EVENT_LAST = 100;
#define ESP_EVENT_DECLARE_BASE(name) constexpr int name = 1
#define pdMS_TO_TICKS(ms) (ms)
inline esp_err_t esp_event_post(int, int, const void *, size_t, int) {
  return ESP_OK;
}
constexpr int MALLOC_CAP_SPIRAM = 1;
inline void *heap_caps_malloc(size_t n, int) { return std::malloc(n); }
inline void *heap_caps_calloc(size_t n, size_t size, int) {
  return std::calloc(n, size);
}
inline void heap_caps_free(void *p) { std::free(p); }
inline esp_err_t esp_crt_bundle_attach(void *) { return ESP_OK; }
inline bool art_decode_jpeg(const uint8_t *, int, unsigned char **, int *,
                            int *, int) {
  return false;
}

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
  esp_err_t (*crt_bundle_attach)(void *);
  int buffer_size;
  int buffer_size_tx;
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
