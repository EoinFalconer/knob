// Exercise the real NVS queue and Sanity request code with host-only doubles.
#include "../main/kaffi/pending.h"
#include "../main/kaffi/sanity_api.h"
#include "stubs/esp_stub.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static std::vector<uint8_t> saved, staged;
static bool fail_open = false, fail_write = false, fail_commit = false;
static bool lose_ack = false;
static uint64_t random_calls = 0;
static time_t clock_now = 0;
static std::map<std::string, std::string> documents;
static std::vector<std::string> requests;

esp_err_t nvs_open(const char *, int mode, nvs_handle_t *h) {
  *h = mode;
  return fail_open ? ESP_FAIL : ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *out, size_t *len) {
  if (saved.empty())
    return ESP_ERR_NVS_NOT_FOUND;
  if (out && *len < saved.size())
    return ESP_ERR_NVS_INVALID_LENGTH;
  *len = saved.size();
  if (out)
    std::memcpy(out, saved.data(), saved.size());
  return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *p, size_t n) {
  if (fail_write)
    return ESP_FAIL;
  const auto *bytes = static_cast<const uint8_t *>(p);
  staged.assign(bytes, bytes + n);
  return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
  if (fail_commit)
    return ESP_FAIL;
  saved = staged;
  return ESP_OK;
}
void nvs_close(nvs_handle_t) { staged.clear(); }
void esp_fill_random(void *p, size_t n) {
  auto *bytes = static_cast<uint8_t *>(p);
  ++random_calls;
  for (size_t i = 0; i < n; i++)
    bytes[i] = static_cast<uint8_t>(random_calls >> ((i % 8) * 8));
}
int kaffi_office_get() { return 0; }
// glibc declares time() noexcept, the macOS SDK doesn't; the override has to
// match whichever declaration <ctime> brought in.
#if defined(__APPLE__)
extern "C" time_t time(time_t *out) {
#else
extern "C" time_t time(time_t *out) noexcept {
#endif
  if (out)
    *out = clock_now;
  return clock_now;
}

struct HttpClient {
  std::string body;
};
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
  CHECK(cfg->method == HTTP_METHOD_POST);
  return new HttpClient;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char *,
                                    const char *) {
  return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t h,
                                        const char *body, int n) {
  h->body.assign(body, n);
  return ESP_OK;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t h) {
  requests.push_back(h->body);
  CHECK(h->body.find("\"createIfNotExists\":{") != std::string::npos);
  auto pos = h->body.find("\"_id\":\"");
  CHECK(pos != std::string::npos);
  if (pos == std::string::npos)
    return ESP_FAIL;
  pos += 7;
  std::string id = h->body.substr(pos, h->body.find('"', pos) - pos);
  CHECK(id.size() == 38);
  // An event's identity must already be durable when HTTP starts.
  CHECK(std::search(saved.begin(), saved.end(), id.begin(), id.end()) !=
        saved.end());
  documents.emplace(id, h->body); // createIfNotExists never replaces a document
  return lose_ack ? ESP_FAIL : ESP_OK;
}
int esp_http_client_get_status_code(esp_http_client_handle_t) { return 200; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h) {
  delete h;
  return ESP_OK;
}

static PendingEvent event() {
  PendingEvent e = {};
  std::strcpy(e.person_id, "person-1");
  std::strcpy(e.person_name, "Person One");
  e.kind = 0;
  e.quantity = 1;
  std::strcpy(e.occurred_at, "2026-09-24T18:01:00Z");
  return e;
}

static void reset() {
  saved.clear();
  staged.clear();
  documents.clear();
  requests.clear();
  fail_open = fail_write = fail_commit = lose_ack = false;
  clock_now = 0;
  kaffi_pending_init();
}

static bool deliver_one() {
  const auto *e = kaffi_pending_front();
  if (!e || !sanity_api_log(e->event_id, e->person_id, e->person_name, e->kind,
                           e->quantity, e->occurred_at))
    return false;
  kaffi_pending_pop();
  return true;
}

static void lost_ack_survives_reboot() {
  reset();
  kaffi_pending_push(event());
  std::string id = kaffi_pending_front()->event_id;
  lose_ack = true;
  CHECK(!deliver_one());
  CHECK(documents.size() == 1);
  CHECK(kaffi_pending_count() == 1);
  CHECK(!deliver_one()); // another lost reply before restarting
  CHECK(documents.size() == 1);
  kaffi_pending_init(); // discard RAM and restore the committed queue
  CHECK(kaffi_pending_front()->event_id == id);
  lose_ack = false;
  CHECK(deliver_one());
  CHECK(kaffi_pending_count() == 0);
  CHECK(documents.size() == 1);
  CHECK(requests.size() == 3 && requests[0] == requests[1] &&
        requests[1] == requests[2]);
  kaffi_pending_init();
  CHECK(kaffi_pending_count() == 0);
}

static void identical_taps_remain_distinct() {
  reset();
  kaffi_pending_push(event());
  kaffi_pending_push(event()); // same person, kind, quantity and second
  kaffi_pending_init();
  CHECK(deliver_one());
  CHECK(deliver_one());
  CHECK(documents.size() == 2);
  kaffi_pending_init();
  kaffi_pending_push(event()); // a fresh tap after reboot is still distinct
  CHECK(deliver_one());
  CHECK(documents.size() == 3);
}

static void unset_clock_retries_keep_identity() {
  reset();
  auto e = event();
  e.occurred_at[0] = '\0';
  kaffi_pending_push(e);
  lose_ack = true;
  CHECK(!deliver_one());
  CHECK(requests[0].find("occurredAt") == std::string::npos);
  std::string original = documents.begin()->second;
  clock_now = 1790000000; // clock syncs before the retry
  lose_ack = false;
  CHECK(deliver_one());
  CHECK(requests[1].find("occurredAt") != std::string::npos);
  CHECK(documents.size() == 1 && documents.begin()->second == original);
}

static void failed_persistence_blocks_delivery() {
  for (bool *failure : {&fail_open, &fail_write, &fail_commit}) {
    reset();
    *failure = true;
    kaffi_pending_push(event());
    CHECK(kaffi_pending_count() == 1);
    CHECK(!deliver_one());
    CHECK(requests.empty());
    *failure = false;
    CHECK(deliver_one());
    CHECK(documents.size() == 1);
  }
}

static void failed_pop_write_replays_same_id() {
  reset();
  kaffi_pending_push(event());
  fail_commit = true;
  CHECK(deliver_one()); // server ack received; queue deletion failed to commit
  fail_commit = false;
  kaffi_pending_init();
  CHECK(kaffi_pending_count() == 1);
  CHECK(deliver_one());
  CHECK(documents.size() == 1);
}

// Construct the deployed KPN1 bytes directly, without the new C++ layout.
static void legacy_queue(int32_t head, int32_t count) {
  saved.assign(3852, 0);
  uint32_t magic = 0x314e504b;
  std::memcpy(saved.data(), &magic, 4);
  std::memcpy(saved.data() + 3844, &head, 4);
  std::memcpy(saved.data() + 3848, &count, 4);
  for (int i = 0; i < count && i < 32 && head >= 0 && head < 32; i++) {
    auto *p = saved.data() + 4 + ((head + i) % 32) * 120;
    auto e = event();
    std::snprintf(e.person_id, sizeof(e.person_id), "person-%d", i);
    e.kind = i % 2;
    e.quantity = 1 + i % 2;
    if (i % 2)
      e.occurred_at[0] = '\0';
    std::memcpy(p, e.person_id, 40);
    std::memcpy(p + 40, e.person_name, 48);
    std::memcpy(p + 88, &e.kind, 4);
    std::memcpy(p + 92, &e.quantity, 4);
    std::memcpy(p + 96, e.occurred_at, 24);
  }
}

static void migrates_legacy_queue() {
  for (int count : {0, 2, 32}) {
    reset();
    legacy_queue(31, count); // occupied entries wrap around slot zero
    kaffi_pending_init();
    CHECK(kaffi_pending_count() == count);
    auto before_ids = random_calls;
    auto *front = kaffi_pending_front();
    CHECK(random_calls == before_ids + count);
    CHECK((front != nullptr) == (count > 0));
    std::vector<std::string> ids;
    for (int i = 0; i < count; i++) {
      kaffi_pending_init(); // every ID must survive repeated restarts
      const auto *e = kaffi_pending_front();
      CHECK(e != nullptr);
      if (!e)
        break;
      CHECK(e->person_id == std::string("person-") + std::to_string(i));
      CHECK(std::strcmp(e->person_name, "Person One") == 0);
      CHECK(e->kind == i % 2 && e->quantity == 1 + i % 2);
      CHECK(std::strcmp(e->occurred_at,
                        i % 2 ? "" : "2026-09-24T18:01:00Z") == 0);
      ids.emplace_back(e->event_id);
      kaffi_pending_pop();
    }
    CHECK(random_calls == before_ids + count);
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    kaffi_pending_init();
    CHECK(kaffi_pending_count() == 0);
  }
}

static void failed_migration_commit_cannot_send_transient_ids() {
  reset();
  legacy_queue(31, 2);
  auto legacy = saved;
  kaffi_pending_init();
  fail_commit = true;
  CHECK(!deliver_one());
  CHECK(requests.empty() && saved == legacy);
  kaffi_pending_init(); // an uncommitted upgrade may safely generate new IDs
  fail_commit = false;
  CHECK(deliver_one());
  kaffi_pending_init();
  CHECK(deliver_one());
  CHECK(documents.size() == 2);
}

static void migration_preserves_new_offline_taps() {
  reset();
  legacy_queue(31, 2);
  kaffi_pending_init();
  kaffi_pending_push(event()); // persist KPN2 before legacy IDs are assigned
  kaffi_pending_init();
  CHECK(kaffi_pending_count() == 3);
  CHECK(deliver_one());
  CHECK(deliver_one());
  CHECK(deliver_one());
  CHECK(documents.size() == 3);
}

static void malformed_legacy_is_not_loaded() {
  for (auto [head, count] : {std::pair{-1, 1}, {32, 1}, {0, -1}, {0, 33}}) {
    reset();
    legacy_queue(head, count);
    kaffi_pending_init();
    CHECK(kaffi_pending_count() == 0);
  }
}

static void missing_id_never_reaches_http() {
  reset();
  CHECK(!sanity_api_log("", "person-1", "Person One", 0, 1, ""));
  CHECK(!sanity_api_log(nullptr, "person-1", "Person One", 0, 1, ""));
  CHECK(requests.empty());
}

int main() {
  lost_ack_survives_reboot();
  identical_taps_remain_distinct();
  unset_clock_retries_keep_identity();
  failed_persistence_blocks_delivery();
  failed_pop_write_replays_same_id();
  migrates_legacy_queue();
  failed_migration_commit_cannot_send_transient_ids();
  migration_preserves_new_offline_taps();
  malformed_legacy_is_not_loaded();
  missing_id_never_reaches_http();
  if (!failures)
    std::printf("delivery: all tests passed\n");
  return failures ? 1 : 0;
}
