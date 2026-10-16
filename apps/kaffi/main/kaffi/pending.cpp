#include "pending.h"
#include "../app_config.h"

#include "esp_log.h"
#include "nvs.h"

#include <cstring>

static constexpr const char *TAG = "pending";
static constexpr const char *NS = "kaffi";
static constexpr const char *KEY = "pending";
static constexpr int CAP = 32;

static_assert(sizeof(PendingEvent::person_id) == KAFFI_ID_LEN);
static_assert(sizeof(PendingEvent::person_name) == KAFFI_NAME_LEN);

// Versioned so a future layout change can't misread an old blob.
struct Blob {
  uint32_t magic;
  PendingRing<CAP> ring;
};
static constexpr uint32_t MAGIC = 0x314e504b; // "KPN1"

static Blob s_blob = {MAGIC, {}};

static void persist() {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open failed — queue not persisted");
    return;
  }
  esp_err_t err = nvs_set_blob(h, KEY, &s_blob, sizeof(s_blob));
  if (err == ESP_OK)
    err = nvs_commit(h);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "persist failed: %s", esp_err_to_name(err));
  nvs_close(h);
}

void kaffi_pending_init() {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
    return;
  Blob loaded = {};
  size_t len = sizeof(loaded);
  esp_err_t err = nvs_get_blob(h, KEY, &loaded, &len);
  nvs_close(h);
  if (err != ESP_OK || len != sizeof(loaded) || loaded.magic != MAGIC ||
      loaded.ring.count < 0 || loaded.ring.count > CAP ||
      loaded.ring.head < 0 || loaded.ring.head >= CAP) {
    if (err != ESP_ERR_NVS_NOT_FOUND)
      ESP_LOGW(TAG, "Ignoring unreadable pending queue (%s)",
               esp_err_to_name(err));
    return;
  }
  s_blob = loaded;
  if (s_blob.ring.size() > 0)
    ESP_LOGI(TAG, "%d undelivered event(s) restored from NVS",
             s_blob.ring.size());
}

int kaffi_pending_count() { return s_blob.ring.size(); }

void kaffi_pending_push(const PendingEvent &e) {
  if (!s_blob.ring.push(e))
    ESP_LOGW(TAG, "Queue full — dropped the oldest undelivered event");
  persist();
}

const PendingEvent *kaffi_pending_front() { return s_blob.ring.front(); }

void kaffi_pending_pop() {
  s_blob.ring.pop();
  persist();
}
