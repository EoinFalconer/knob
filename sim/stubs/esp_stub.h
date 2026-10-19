#pragma once

// Host doubles for the ESP-IDF surface the radio UI touches.
#include <cstddef>
#include <cstdint>
#include <cstdlib>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
inline const char *esp_err_to_name(esp_err_t) { return "sim"; }

void sim_log(const char *tag, const char *fmt, ...);
#define ESP_LOGE(tag, ...) sim_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) sim_log(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) sim_log(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ((void)0)

constexpr int MALLOC_CAP_SPIRAM = 1;
constexpr int MALLOC_CAP_8BIT = 2;
constexpr int MALLOC_CAP_INTERNAL = 4;
inline void *heap_caps_malloc(size_t n, int) { return std::malloc(n); }
inline void *heap_caps_calloc(size_t n, size_t s, int) {
  return std::calloc(n, s);
}
inline void heap_caps_free(void *p) { std::free(p); }

using esp_event_base_t = const char *;
#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t const id
#define ESP_EVENT_DEFINE_BASE(id) esp_event_base_t const id = #id
esp_err_t esp_event_post(esp_event_base_t base, int32_t id, const void *data,
                         size_t size, int ticks);
