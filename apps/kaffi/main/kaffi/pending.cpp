#include "pending.h"
#include "../app_config.h"

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#include <cstring>
#include <memory>
#include <new>

static constexpr const char *TAG = "pending";
static constexpr const char *NS = "kaffi";
static constexpr const char *KEY = "pending";
static constexpr int CAP = 32;

static_assert(sizeof(PendingEvent::person_id) == KAFFI_ID_LEN);
static_assert(sizeof(PendingEvent::person_name) == KAFFI_NAME_LEN);
static_assert(sizeof(PendingEvent::event_id) <= KAFFI_ID_LEN);

struct Blob {
  uint32_t magic;
  PendingRing<CAP> ring;
};
static constexpr uint32_t MAGIC = 0x324e504b; // "KPN2"

// Deployed KPN1 layout, frozen independently of PendingEvent/PendingRing.
struct LegacyEvent {
  char person_id[40];
  char person_name[48];
  int32_t kind;
  int32_t quantity;
  char occurred_at[24];
};
struct LegacyBlob {
  uint32_t magic;
  struct {
    LegacyEvent items[CAP];
    int32_t head;
    int32_t count;
  } ring;
};
static constexpr uint32_t LEGACY_MAGIC = 0x314e504b;
static_assert(sizeof(LegacyEvent) == 120);
static_assert(sizeof(LegacyBlob) == 3852);

static Blob s_blob = {MAGIC, {}};
static bool s_dirty = false;

static void assign_id(PendingEvent &e) {
  uint8_t random[16];
  esp_fill_random(random, sizeof(random));
  static constexpr char HEX[] = "0123456789abcdef";
  memcpy(e.event_id, "kaffi-", 6);
  for (size_t i = 0; i < sizeof(random); i++) {
    e.event_id[6 + 2 * i] = HEX[random[i] >> 4];
    e.event_id[7 + 2 * i] = HEX[random[i] & 0xf];
  }
  e.event_id[38] = '\0';
}

static bool persist() {
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open failed — queue not persisted");
    return false;
  }
  esp_err_t err = nvs_set_blob(h, KEY, &s_blob, sizeof(s_blob));
  if (err == ESP_OK)
    err = nvs_commit(h);
  if (err != ESP_OK)
    ESP_LOGE(TAG, "persist failed: %s", esp_err_to_name(err));
  nvs_close(h);
  if (err == ESP_OK)
    s_dirty = false;
  return err == ESP_OK;
}

static bool valid_ring(int32_t head, int32_t count) {
  return head >= 0 && head < CAP && count >= 0 && count <= CAP;
}

void kaffi_pending_init() {
  s_blob.ring.head = 0;
  s_blob.ring.count = 0;
  s_dirty = false;
  nvs_handle_t h;
  if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
    return;
  size_t len = 0;
  esp_err_t err = nvs_get_blob(h, KEY, nullptr, &len);
  bool restored = false;
  // Read staging lives on the heap: a KPN2 blob alone is over 5 KB.
  if (err == ESP_OK && len == sizeof(Blob)) {
    auto loaded = std::unique_ptr<Blob>(new (std::nothrow) Blob{});
    if (loaded) {
      err = nvs_get_blob(h, KEY, loaded.get(), &len);
      if (err == ESP_OK && len == sizeof(Blob) && loaded->magic == MAGIC &&
          valid_ring(loaded->ring.head, loaded->ring.count)) {
        s_blob = *loaded;
        restored = true;
      }
    }
  } else if (err == ESP_OK && len == sizeof(LegacyBlob)) {
    auto loaded = std::unique_ptr<LegacyBlob>(new (std::nothrow) LegacyBlob{});
    if (loaded) {
      err = nvs_get_blob(h, KEY, loaded.get(), &len);
      if (err == ESP_OK && len == sizeof(LegacyBlob) &&
          loaded->magic == LEGACY_MAGIC &&
          valid_ring(loaded->ring.head, loaded->ring.count)) {
        for (int i = 0; i < loaded->ring.count; i++) {
          const auto &old = loaded->ring.items[(loaded->ring.head + i) % CAP];
          PendingEvent e = {};
          memcpy(e.person_id, old.person_id, sizeof(e.person_id));
          memcpy(e.person_name, old.person_name, sizeof(e.person_name));
          e.kind = old.kind;
          e.quantity = old.quantity;
          memcpy(e.occurred_at, old.occurred_at, sizeof(e.occurred_at));
          s_blob.ring.push(e);
        }
        // Assign IDs after WiFi starts, then persist the upgrade before replay.
        s_dirty = true;
        restored = true;
      }
    }
  }
  nvs_close(h);
  if (!restored) {
    if (err != ESP_ERR_NVS_NOT_FOUND)
      ESP_LOGW(TAG, "Ignoring unreadable pending queue (%s)",
               esp_err_to_name(err));
    return;
  }
  if (s_blob.ring.size() > 0)
    ESP_LOGI(TAG, "%d undelivered event(s) restored from NVS",
             s_blob.ring.size());
}

int kaffi_pending_count() { return s_blob.ring.size(); }

void kaffi_pending_push(const PendingEvent &e) {
  PendingEvent identified = e;
  assign_id(identified);
  if (!s_blob.ring.push(identified))
    ESP_LOGW(TAG, "Queue full — dropped the oldest undelivered event");
  s_dirty = true;
  persist();
}

const PendingEvent *kaffi_pending_front() {
  for (int i = 0; i < s_blob.ring.count; i++) {
    auto &e = s_blob.ring.items[(s_blob.ring.head + i) % CAP];
    if (!e.event_id[0]) {
      assign_id(e);
      s_dirty = true;
    }
  }
  // Never send an ID that could be regenerated after a reboot, notably if
  // persisting a migrated KPN1 queue failed. Retry the write on the next flush.
  if (s_dirty && !persist())
    return nullptr;
  return s_blob.ring.front();
}

void kaffi_pending_pop() {
  s_blob.ring.pop();
  s_dirty = true;
  persist();
}
