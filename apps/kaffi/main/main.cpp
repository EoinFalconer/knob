#include "app_config.h"
#include "kaffi/ota.h"
#include "kaffi/pending.h"
#include "kaffi/prefs.h"
#include "kaffi/sanity_api.h"
#include "ui/ui.h"

#include "encoder.h"
#include "haptic.h"
#include "knob_events.h"
#include "settings.h"
#include "wifi_manager.h"
#include "wifi_setup.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <cstdlib>
#include <cstring>
#include <ctime>

static constexpr const char *TAG = "kaffi";

#ifndef CONFIG_RADIO_WIFI_SSID
#define CONFIG_RADIO_WIFI_SSID ""
#endif

static constexpr int KAFFI_OTA_CHECK_MS = 60 * 60 * 1000; // hourly
// No successful Sanity fetch for this long means something is wedged that
// the WiFi manager couldn't heal (stuck stack, leaked heap, zombie link).
// A reboot is what fixed it by hand; queued taps survive it in NVS.
static constexpr uint32_t KAFFI_UNHEALTHY_REBOOT_S = 20 * 60;

ESP_EVENT_DEFINE_BASE(APP_EVENT);

// ─── Command queue: all HTTP runs on one task with a big stack (TLS heavy)

struct Cmd {
  enum class Type { Log, Flush, Refresh, OtaCheck } type;
  KaffiLogCmd log;
};

static QueueHandle_t s_cmd_queue = nullptr;
static QueueHandle_t s_avatar_queue = nullptr;
static esp_timer_handle_t s_refresh_timer = nullptr;
static bool s_net_ready = false;          // one-time network init done
static bool s_have_people = false;        // at least one successful fetch
static bool s_wifi_up = false;            // associated with an IP right now
static volatile uint32_t s_last_ok_s = 0; // uptime of last successful fetch

static uint32_t uptime_s() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000000);
}

static void enqueue(Cmd cmd);

// Offline pill on the list screen: what the network is doing and how many
// taps are waiting for it.
static void publish_link_state() {
  ui_set_offline(!s_wifi_up, kaffi_pending_count());
}

// Replay undelivered taps, oldest first, stopping at the first failure so
// order is preserved. Returns how many landed.
static int flush_pending() {
  int sent = 0;
  while (const PendingEvent *e = kaffi_pending_front()) {
    if (!sanity_api_log(e->event_id, e->person_id, e->person_name, e->kind,
                        e->quantity, e->occurred_at))
      break;
    kaffi_pending_pop();
    sent++;
  }
  if (sent)
    ESP_LOGI(TAG, "Replayed %d queued event(s), %d left", sent,
             kaffi_pending_count());
  publish_link_state();
  return sent;
}

// Persist every tap and its ID before HTTP, including while online. A reset
// or lost response can then only replay the same event, never create a second.
static void log_or_queue(const KaffiLogCmd &log) {
  PendingEvent e = {};
  strncpy(e.person_id, log.person_id, sizeof(e.person_id) - 1);
  strncpy(e.person_name, log.person_name, sizeof(e.person_name) - 1);
  e.kind = log.kind;
  e.quantity = log.quantity;
  sanity_api_iso_now(e.occurred_at, sizeof(e.occurred_at));
  kaffi_pending_push(e);
  ESP_LOGW(TAG, "Queued %s for %s (%d pending)",
           log.kind == KAFFI_BREW ? "brew" : "cup", log.person_name,
           kaffi_pending_count());
  publish_link_state();
}

static void cmd_task(void *) {
  Cmd cmd;
  while (true) {
    if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE)
      continue;

    switch (cmd.type) {
    case Cmd::Type::Log:
      log_or_queue(cmd.log);
      if (s_wifi_up) {
        flush_pending();           // older taps go out first
        sanity_api_fetch_people(); // reflect the new total
      }
      break;
    case Cmd::Type::Flush:
      if (s_wifi_up && flush_pending() > 0)
        sanity_api_fetch_people();
      break;
    case Cmd::Type::Refresh:
      if (sanity_api_fetch_people()) {
        s_last_ok_s = uptime_s();
      } else if (!s_have_people) {
        // Nothing on screen yet — keep trying rather than parking on an
        // error. Once people are loaded, the periodic refresh heals things
        // quietly instead.
        ui_set_status("Can't reach Sanity.\nTrying again\u2026");
        vTaskDelay(pdMS_TO_TICKS(5000));
        Cmd retry;
        retry.type = Cmd::Type::Refresh;
        enqueue(retry);
      }
      break;
    case Cmd::Type::OtaCheck:
      kaffi_ota_check(); // spawns its own task; returns immediately
      break;
    }
  }
}

// Avatars run on their own task so a burst of preloads never delays a log
// write. Failures post a null-pixel AvatarReady so the UI can retry later.
static void avatar_task(void *) {
  AvatarReq req;
  while (true) {
    if (xQueueReceive(s_avatar_queue, &req, portMAX_DELAY) != pdTRUE)
      continue;
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    bool ok = sanity_api_fetch_avatar(req.url, &px, &w, &h);
    AvatarReady ar = {};
    strncpy(ar.url, req.url, sizeof(ar.url) - 1);
    ar.pixels = ok ? px : nullptr;
    ar.w = w;
    ar.h = h;
    if (esp_event_post(APP_EVENT, APP_EVENT_KAFFI_AVATAR_READY, &ar, sizeof(ar),
                       pdMS_TO_TICKS(100)) != ESP_OK &&
        ar.pixels) {
      heap_caps_free(ar.pixels); // nobody will take ownership
    }
  }
}

static void enqueue(Cmd cmd) {
  if (s_cmd_queue)
    xQueueSend(s_cmd_queue, &cmd, 0);
}

// ─── Event handlers

// Runs on the event loop task, so keep it quick — the actual fetch (TLS,
// slow) is queued onto the command task.
static void on_wifi_connected(void *, esp_event_base_t, int32_t, void *) {
  ESP_LOGI(TAG, "WiFi connected");
  s_wifi_up = true;
  publish_link_state();
  if (!s_net_ready) {
    s_net_ready = true;
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_cfg);
    sanity_api_init();
    if (s_refresh_timer) {
      esp_timer_start_periodic(s_refresh_timer,
                               static_cast<uint64_t>(KAFFI_REFRESH_MS) * 1000);
    }
  }

  if (!s_have_people)
    ui_set_status("Loading people\u2026");

  // Anything queued while offline goes out before the refresh so the new
  // totals include it.
  Cmd flush;
  flush.type = Cmd::Type::Flush;
  enqueue(flush);
  Cmd cmd;
  cmd.type = Cmd::Type::Refresh;
  enqueue(cmd);
}

static void on_wifi_disconnected(void *, esp_event_base_t, int32_t, void *) {
  ESP_LOGW(TAG, "WiFi disconnected");
  s_wifi_up = false;
  publish_link_state();
  // With people already loaded the knob keeps working from memory — a blip
  // shouldn't yank the screen away. Reconnect refetches in the background.
  if (!s_have_people)
    ui_set_status("Wi-Fi dropped.\nReconnecting\u2026");
}

static void on_people_update(void *, esp_event_base_t, int32_t, void *data) {
  bool first = !s_have_people;
  s_have_people = true;
  ui_update_people(static_cast<KaffiState *>(data));

  // A working fetch is the health check for a fresh OTA image — confirm it
  // so the bootloader stops considering a rollback, then look for updates.
  kaffi_ota_mark_valid();
  if (first) {
    Cmd cmd;
    cmd.type = Cmd::Type::OtaCheck;
    enqueue(cmd);
  }
}

static void on_log(void *, esp_event_base_t, int32_t, void *data) {
  Cmd cmd;
  cmd.type = Cmd::Type::Log;
  cmd.log = *static_cast<KaffiLogCmd *>(data);
  enqueue(cmd);
}

static void on_refresh(void *, esp_event_base_t, int32_t, void *) {
  // The periodic refresh doubles as a retry tick for the queue.
  if (kaffi_pending_count() > 0) {
    Cmd flush;
    flush.type = Cmd::Type::Flush;
    enqueue(flush);
  }
  Cmd cmd;
  cmd.type = Cmd::Type::Refresh;
  enqueue(cmd);
}

// Called directly by the UI. We bypass the esp_event loop here because its
// queue is small (~32) and a burst of ~35 preload requests would overflow it,
// silently dropping the last few people's avatars.
void kaffi_request_avatar(const char *url) {
  if (!s_avatar_queue || !url || !url[0])
    return;
  AvatarReq req = {};
  strncpy(req.url, url, sizeof(req.url) - 1);
  xQueueSend(s_avatar_queue, &req, 0);
}

static void on_avatar_ready(void *, esp_event_base_t, int32_t, void *data) {
  auto *ar = static_cast<AvatarReady *>(data);
  ui_set_avatar(ar->url, ar->pixels, ar->w, ar->h);
}

static void refresh_timer_cb(void *) {
  esp_event_post(APP_EVENT, APP_EVENT_KAFFI_REFRESH, nullptr, 0, 0);
}

static void health_timer_cb(void *) {
  uint32_t stale = uptime_s() - s_last_ok_s;
  if (stale < KAFFI_UNHEALTHY_REBOOT_S)
    return;
  ESP_LOGE(TAG,
           "No Sanity fetch for %lu s (wifi %s, free heap %lu) — rebooting",
           static_cast<unsigned long>(stale), s_wifi_up ? "up" : "down",
           static_cast<unsigned long>(esp_get_free_heap_size()));
  esp_restart();
}

static void ota_timer_cb(void *) {
  Cmd cmd;
  cmd.type = Cmd::Type::OtaCheck;
  enqueue(cmd);
}

// ─── NVS

static void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

// ─── Entry point

extern "C" void app_main() {
  ESP_LOGI(TAG, "Kaffi starting");

  init_nvs();
  settings_init();
  kaffi_pending_init();

  s_cmd_queue = xQueueCreate(8, sizeof(Cmd));
  // Fetching, parsing and decoding stay off core 1, where LVGL draws: there
  // they took turns with it, and the strip stuttered while photos loaded.
  xTaskCreatePinnedToCore(cmd_task, "kaffi_cmd", 8192, nullptr, 5, nullptr, 0);

  // Avatar preloads run at lower priority so logging stays snappy.
  s_avatar_queue = xQueueCreate(KAFFI_MAX_PEOPLE + 4, sizeof(AvatarReq));
  xTaskCreatePinnedToCore(avatar_task, "kaffi_avatar", 8192, nullptr, 3,
                          nullptr, 0);

  esp_event_loop_create_default();
  esp_event_handler_register(APP_EVENT, APP_EVENT_WIFI_CONNECTED,
                             on_wifi_connected, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_WIFI_DISCONNECTED,
                             on_wifi_disconnected, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_KAFFI_PEOPLE_UPDATE,
                             on_people_update, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_KAFFI_LOG, on_log, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_KAFFI_REFRESH, on_refresh,
                             nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_KAFFI_AVATAR_READY,
                             on_avatar_ready, nullptr);

  esp_timer_create_args_t timer_args = {};
  timer_args.callback = refresh_timer_cb;
  timer_args.name = "kaffi_refresh";
  esp_timer_create(&timer_args, &s_refresh_timer);

  ui_init();
  ui_show_splash();
  publish_link_state(); // show any backlog restored from NVS right away
  haptic_init();
  encoder_init();

  // First boot: pick the office on the knob (blocks until tapped). The
  // office drives the directory filter, event stamping, and timezone.
  int office = kaffi_office_get();
  if (office < 0) {
    office = ui_pick_office();
    kaffi_office_set(office);
  }
  setenv("TZ", KAFFI_OFFICES[office].tz, 1);
  tzset();

  // No WiFi anywhere (no NVS credentials, nothing compiled in): captive
  // portal — scan the QR with a phone, pick a network, done. This is how a
  // CI-built generic binary gets provisioned in a new office.
  if (!wifi_setup_has_credentials() && CONFIG_RADIO_WIFI_SSID[0] == '\0') {
    ui_show_wifi_setup("kaffi");
    wifi_setup_start("kaffi"); // blocks until credentials verified
  }

  ui_set_status("Connecting to Wi-Fi\u2026");
  wifi_manager_init();

  esp_timer_create_args_t ota_args = {};
  ota_args.callback = ota_timer_cb;
  ota_args.name = "kaffi_ota_tick";
  esp_timer_handle_t ota_timer = nullptr;
  esp_timer_create(&ota_args, &ota_timer);
  esp_timer_start_periodic(ota_timer,
                           static_cast<uint64_t>(KAFFI_OTA_CHECK_MS) * 1000);

  esp_timer_create_args_t health_args = {};
  health_args.callback = health_timer_cb;
  health_args.name = "kaffi_health";
  esp_timer_handle_t health_timer = nullptr;
  esp_timer_create(&health_args, &health_timer);
  esp_timer_start_periodic(health_timer, 60ULL * 1000 * 1000);

  ESP_LOGI(TAG, "Init complete");
}
