#include "app_config.h"
#include "spotify/spotify_auth.h"
#include "spotify/spotify_api.h"
#include "spotify_setup.h"
#include "ui/ui.h"
#include "wifi_setup.h"

#include "hal_pins.h"
#include "knob_events.h"
#include "display.h"
#include "encoder.h"
#include "haptic.h"
#include "wifi_manager.h"
#include "settings.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "nvs.h"

#include <cstdlib>
#include <cstring>

static constexpr const char *TAG = "spotify";
static constexpr int WIFI_MAX_DISCONNECTS = 5;
static int s_wifi_disconnect_count = 0;
static bool s_portal_active = false;

ESP_EVENT_DEFINE_BASE(APP_EVENT);

// ─── Kconfig accessors (from sdkconfig), with NVS fallback for on-device setup

static char s_nvs_client_id[128] = {};

extern "C" const char *spotify_client_id(void) {
    // Try NVS first (set by on-device OAuth setup)
    if (s_nvs_client_id[0] != '\0') return s_nvs_client_id;

    nvs_handle_t h;
    if (nvs_open("spotify", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_nvs_client_id);
        if (nvs_get_str(h, "client_id", s_nvs_client_id, &len) == ESP_OK &&
            s_nvs_client_id[0] != '\0') {
            nvs_close(h);
            return s_nvs_client_id;
        }
        nvs_close(h);
    }

    // Fall back to compiled-in value
#ifdef CONFIG_SPOTIFY_CLIENT_ID
    return CONFIG_SPOTIFY_CLIENT_ID;
#else
    return "";
#endif
}

extern "C" const char *spotify_refresh_token(void) {
#ifdef CONFIG_SPOTIFY_REFRESH_TOKEN
    return CONFIG_SPOTIFY_REFRESH_TOKEN;
#else
    return "";
#endif
}

// ─── Command queue for API calls (runs on a dedicated task with enough stack)

enum class CmdType : uint8_t { SetVolume, Play, Pause, Next, Prev, DjSpin };

struct Cmd {
    CmdType type;
    int32_t value; // for SetVolume
};

static QueueHandle_t s_cmd_queue = nullptr;

static void cmd_task(void *) {
    Cmd cmd;
    while (true) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE)
            continue;

        // Drain any queued volume changes — only send the latest
        if (cmd.type == CmdType::SetVolume) {
            Cmd newer;
            while (xQueueReceive(s_cmd_queue, &newer, 0) == pdTRUE) {
                if (newer.type == CmdType::SetVolume) {
                    cmd.value = newer.value; // keep latest volume
                } else {
                    // Non-volume command — process volume first, then this
                    spotify_api_set_volume(cmd.value);
                    cmd = newer;
                    break;
                }
            }
        }

        switch (cmd.type) {
        case CmdType::SetVolume:
            spotify_api_set_volume(cmd.value);
            break;
        case CmdType::Play:
            spotify_api_play_pause(false); // not playing -> play
            break;
        case CmdType::Pause:
            spotify_api_play_pause(true); // playing -> pause
            break;
        case CmdType::Next:
            spotify_api_next();
            break;
        case CmdType::Prev:
            spotify_api_prev();
            break;
        case CmdType::DjSpin:
            haptic_buzz();
            spotify_api_play_random_liked();
            break;
        }
    }
}

static void enqueue_cmd(CmdType type, int32_t value = 0) {
    Cmd cmd = {type, value};
    xQueueSend(s_cmd_queue, &cmd, 0);
}

// ─── Volume debounce
static int s_pending_volume = -1;
static int64_t s_volume_send_at = 0;

// ─── Event handlers

static char s_device_ip[32] = {};

static void get_device_ip(char *buf, size_t buf_size) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        strncpy(buf, "0.0.0.0", buf_size);
        return;
    }
    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(buf, buf_size, IPSTR, IP2STR(&ip_info.ip));
    } else {
        strncpy(buf, "0.0.0.0", buf_size);
    }
}

static void on_wifi_connected(void *, esp_event_base_t, int32_t, void *) {
    if (s_portal_active) return;
    ESP_LOGI(TAG, "WiFi connected");
    s_wifi_disconnect_count = 0;

    get_device_ip(s_device_ip, sizeof(s_device_ip));
    ESP_LOGI(TAG, "Device IP: %s", s_device_ip);

    // Check if we have a refresh token (NVS or compiled-in)
    bool has_token = spotify_setup_has_token() ||
                     (spotify_refresh_token()[0] != '\0');

    if (!has_token) {
        // No token — start on-device OAuth setup
        ESP_LOGW(TAG, "No Spotify refresh token — starting setup flow");
        ui_show_spotify_setup(s_device_ip);
        spotify_setup_start(s_device_ip); // blocks until auth completes

        // After setup completes, continue with normal init
        ESP_LOGI(TAG, "Spotify setup complete — initializing auth");
    }

    ui_set_status("Connecting to Spotify...\nThis can take a moment");
    spotify_auth_init();

    const char *token = spotify_auth_get_token();
    if (token) {
        ui_set_status("Connected\nWaiting for playback...");
        spotify_api_init();
        spotify_api_start();
    } else {
        // Token failed — fall back to on-device OAuth setup
        ESP_LOGW(TAG, "Token failed — starting Spotify setup flow");
        get_device_ip(s_device_ip, sizeof(s_device_ip));
        ui_show_spotify_setup(s_device_ip);
        spotify_setup_start(s_device_ip); // blocks until auth completes

        ESP_LOGI(TAG, "Spotify setup complete — retrying auth");
        spotify_auth_init();
        token = spotify_auth_get_token();
        if (token) {
            ui_set_status("Connected\nWaiting for playback...");
            spotify_api_init();
            spotify_api_start();
        } else {
            ui_set_status("Auth failed\nRestart and try again");
        }
    }
}

static void on_wifi_disconnected(void *, esp_event_base_t, int32_t, void *) {
    if (s_portal_active) return; // portal owns WiFi now, ignore events

    ESP_LOGW(TAG, "WiFi disconnected");
    spotify_api_stop();
    s_wifi_disconnect_count++;

    if (s_wifi_disconnect_count >= WIFI_MAX_DISCONNECTS) {
        ESP_LOGW(TAG, "Too many disconnects (%d), starting captive portal",
                 s_wifi_disconnect_count);
        s_portal_active = true;
        ui_show_wifi_setup("knob");
        wifi_setup_start(); // blocks forever, device restarts after setup
    } else {
        ui_set_status("WiFi disconnected\nReconnecting...");
    }
}

static void on_spotify_state(void *, esp_event_base_t, int32_t, void *data) {
    auto *state = static_cast<SpotifyState *>(data);
    ui_update_state(state);
}

static void on_volume_set(void *, esp_event_base_t, int32_t, void *data) {
    auto vol = *static_cast<int32_t *>(data);
    s_pending_volume = vol;
    s_volume_send_at = esp_timer_get_time() + 200000;
}

static void on_play(void *, esp_event_base_t, int32_t, void *) {
    enqueue_cmd(CmdType::Play);
}

static void on_pause(void *, esp_event_base_t, int32_t, void *) {
    enqueue_cmd(CmdType::Pause);
}

static void on_next(void *, esp_event_base_t, int32_t, void *) {
    enqueue_cmd(CmdType::Next);
}

static void on_prev(void *, esp_event_base_t, int32_t, void *) {
    enqueue_cmd(CmdType::Prev);
}

static void on_dj_spin(void *, esp_event_base_t, int32_t, void *) {
    ESP_LOGI(TAG, "DJ SPIN!");
    enqueue_cmd(CmdType::DjSpin);
}

// ─── Volume debounce timer — just enqueues, no HTTP here
static void volume_timer_cb(void *) {
    if (s_pending_volume >= 0 &&
        esp_timer_get_time() >= s_volume_send_at) {
        enqueue_cmd(CmdType::SetVolume, s_pending_volume);
        s_pending_volume = -1;
    }
}

// ─── NVS init

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
    ESP_LOGI(TAG, "Spotify Knob starting");

    init_nvs();
    settings_init();

    // Command queue + task for API calls (needs big stack for HTTP+TLS)
    s_cmd_queue = xQueueCreate(8, sizeof(Cmd));
    xTaskCreatePinnedToCore(cmd_task, "spotify_cmd", 8192, nullptr, 5,
                            nullptr, 1);

    // Event loop
    esp_event_loop_create_default();

    esp_event_handler_register(APP_EVENT, APP_EVENT_WIFI_CONNECTED,
                               on_wifi_connected, nullptr);
    esp_event_handler_register(APP_EVENT, APP_EVENT_WIFI_DISCONNECTED,
                               on_wifi_disconnected, nullptr);
    esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_STATE_UPDATE,
                               on_spotify_state, nullptr);
    esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_VOLUME_SET,
                               on_volume_set, nullptr);
    esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_PLAY,
                               on_play, nullptr);
    esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_PAUSE,
                               on_pause, nullptr);
    esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_NEXT,
                               on_next, nullptr);
    esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_PREV,
                               on_prev, nullptr);
    esp_event_handler_register(APP_EVENT, APP_EVENT_SPOTIFY_DJ_SPIN,
                               on_dj_spin, nullptr);

    // Hardware + UI
    ui_init();
    haptic_init();
    encoder_init();

    // Volume debounce timer (100ms periodic) — only enqueues, doesn't do HTTP
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = volume_timer_cb;
    timer_args.name = "vol_debounce";
    esp_timer_handle_t vol_timer = nullptr;
    esp_timer_create(&timer_args, &vol_timer);
    esp_timer_start_periodic(vol_timer, 100000); // 100ms

    // Check for WiFi credentials — start captive portal if none saved
    if (!wifi_setup_has_credentials()) {
        ESP_LOGW(TAG, "No WiFi credentials — starting captive portal");
        ui_show_wifi_setup("knob");
        wifi_setup_start(); // blocks forever, device restarts after setup
        return; // unreachable
    }

    // Start WiFi
    wifi_manager_init();

    ESP_LOGI(TAG, "Init complete");
}
