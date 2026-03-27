#include "wifi_picker.h"
#include "ui/ui.h"
#include "display.h"
#include "encoder.h"
#include "haptic.h"
#include "fonts.h"
#include "settings.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <algorithm>
#include <cstring>

static constexpr const char *TAG = "wifi_picker";

// ─── Colors (match the Spotify app UI)
static constexpr lv_color_t COL_BG    = {.blue = 0x10, .green = 0x10, .red = 0x10};
static constexpr lv_color_t COL_GREEN = {.blue = 0x54, .green = 0xB9, .red = 0x1D};
static constexpr lv_color_t COL_WHITE = {.blue = 0xFF, .green = 0xFF, .red = 0xFF};
static constexpr lv_color_t COL_GREY  = {.blue = 0x88, .green = 0x88, .red = 0x88};
static constexpr lv_color_t COL_DIM   = {.blue = 0x30, .green = 0x30, .red = 0x30};
static constexpr lv_color_t COL_SEL   = {.blue = 0x2A, .green = 0x2A, .red = 0x2A};

static constexpr int MAX_VISIBLE_NETWORKS = 16;
static constexpr int DISPLAY_SIZE = 360;

// ─── Scanned network entry
struct PickerNetwork {
    char ssid[33];
    int8_t rssi;
    bool saved;   // exists in NVS
};

// ─── Picker state
static PickerNetwork s_networks[MAX_VISIBLE_NETWORKS];
static int s_network_count = 0;
static int s_selected = 0;        // currently highlighted index
static bool s_add_new_selected = false; // true when "Add new" item is highlighted
static volatile bool s_confirmed = false;

// LVGL objects
static lv_obj_t *s_picker_bg = nullptr;
static lv_obj_t *s_title_lbl = nullptr;
static lv_obj_t *s_list_cont = nullptr;
static lv_obj_t *s_items[MAX_VISIBLE_NETWORKS + 1] = {}; // +1 for "Add new"
static lv_obj_t *s_status_lbl = nullptr;
static int s_total_items = 0; // networks + 1 for "Add new"

// ─── WiFi scan (blocking, runs before LVGL list is built)

static void wifi_scan(bool init_wifi) {
    if (init_wifi) {
        esp_netif_init();
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        // APSTA mode — scan is much more reliable than STA-only on ESP32-S3
        esp_netif_create_default_wifi_ap();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

        wifi_config_t ap_cfg = {};
        strcpy((char *)ap_cfg.ap.ssid, "knob-scan");
        ap_cfg.ap.ssid_len = 9;
        ap_cfg.ap.channel = 1;
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        ap_cfg.ap.max_connection = 0; // no clients needed
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "Radio started (APSTA mode), settling...");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 200;
    scan_cfg.scan_time.active.max = 1500;
    scan_cfg.channel = 0; // all channels

    uint16_t ap_count = 0;
    wifi_ap_record_t ap_list[20] = {};

    // Retry scan up to 3 times if no results
    for (int attempt = 0; attempt < 3; attempt++) {
        esp_err_t err = esp_wifi_scan_start(&scan_cfg, true); // blocking
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Scan attempt %d failed: %s", attempt, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 20) ap_count = 20;
        esp_wifi_scan_get_ap_records(&ap_count, ap_list);

        if (ap_count > 0) {
            ESP_LOGI(TAG, "Scan attempt %d: found %d APs", attempt, ap_count);
            break;
        }

        ESP_LOGW(TAG, "Scan attempt %d: 0 results, retrying...", attempt);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Load saved networks for comparison
    int saved_count = settings_wifi_count();
    WifiEntry saved[WIFI_MAX_SAVED] = {};
    for (int i = 0; i < saved_count; i++) {
        settings_wifi_get(i, &saved[i]);
    }

    // Build deduplicated list, saved networks first (sorted by RSSI)
    s_network_count = 0;

    for (int i = 0; i < ap_count && s_network_count < MAX_VISIBLE_NETWORKS; i++) {
        if (ap_list[i].ssid[0] == '\0') continue;

        // Skip duplicates
        bool dup = false;
        for (int j = 0; j < s_network_count; j++) {
            if (strcmp(s_networks[j].ssid, (char *)ap_list[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        auto &net = s_networks[s_network_count];
        strncpy(net.ssid, (char *)ap_list[i].ssid, sizeof(net.ssid) - 1);
        net.ssid[sizeof(net.ssid) - 1] = '\0';
        net.rssi = ap_list[i].rssi;
        net.saved = false;

        for (int s = 0; s < saved_count; s++) {
            if (strcmp(net.ssid, saved[s].ssid) == 0) {
                net.saved = true;
                break;
            }
        }
        s_network_count++;
    }

    // Sort: saved networks first, then by RSSI descending
    std::sort(s_networks, s_networks + s_network_count,
              [](const PickerNetwork &a, const PickerNetwork &b) {
                  if (a.saved != b.saved) return a.saved > b.saved;
                  return a.rssi > b.rssi;
              });

    ESP_LOGI(TAG, "Scan found %d unique networks", s_network_count);
}

// ─── Signal strength label

static const char *rssi_label(int8_t rssi) {
    if (rssi >= -50) return "Strong";
    if (rssi >= -70) return "Good";
    return "Weak";
}

// ─── Build the picker LVGL UI

static void update_selection();
static void make_item_tappable(lv_obj_t *row);

static void build_picker_ui() {
    s_picker_bg = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_picker_bg);
    lv_obj_set_size(s_picker_bg, DISPLAY_SIZE, DISPLAY_SIZE);
    lv_obj_set_style_bg_color(s_picker_bg, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_picker_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_picker_bg, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_picker_bg, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    s_title_lbl = lv_label_create(s_picker_bg);
    lv_label_set_text(s_title_lbl, "Select WiFi");
    lv_obj_set_style_text_color(s_title_lbl, COL_WHITE, 0);
    lv_obj_set_style_text_font(s_title_lbl, &geist_regular_22, 0);
    lv_obj_set_style_text_align(s_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title_lbl, LV_ALIGN_TOP_MID, 0, 40);

    // Scrollable list container
    s_list_cont = lv_obj_create(s_picker_bg);
    lv_obj_remove_style_all(s_list_cont);
    lv_obj_set_size(s_list_cont, 320, 240);
    lv_obj_align(s_list_cont, LV_ALIGN_CENTER, 0, 15);
    lv_obj_set_style_clip_corner(s_list_cont, true, 0);
    lv_obj_set_flex_flow(s_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_list_cont, 6, 0);
    lv_obj_set_style_pad_top(s_list_cont, 6, 0);
    lv_obj_add_flag(s_list_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_snap_y(s_list_cont, LV_SCROLL_SNAP_CENTER);
    lv_obj_clear_flag(s_list_cont, LV_OBJ_FLAG_SCROLL_ELASTIC);

    // Build list items for each network
    s_total_items = s_network_count + 1; // +1 for "Add new"

    for (int i = 0; i < s_network_count; i++) {
        lv_obj_t *row = lv_obj_create(s_list_cont);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 300, 56);
        lv_obj_set_style_bg_color(row, COL_SEL, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(row, 16, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        // SSID label
        lv_obj_t *ssid_lbl = lv_label_create(row);
        lv_label_set_text(ssid_lbl, s_networks[i].ssid);
        lv_obj_set_width(ssid_lbl, 200);
        lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(ssid_lbl, COL_WHITE, 0);
        lv_obj_set_style_text_font(ssid_lbl, &geist_regular_16, 0);
        lv_obj_align(ssid_lbl, LV_ALIGN_LEFT_MID, 16, 0);

        // Right side: saved indicator or signal
        lv_obj_t *info_lbl = lv_label_create(row);
        if (s_networks[i].saved) {
            lv_label_set_text(info_lbl, "Saved");
            lv_obj_set_style_text_color(info_lbl, COL_GREEN, 0);
        } else {
            lv_label_set_text(info_lbl, rssi_label(s_networks[i].rssi));
            lv_obj_set_style_text_color(info_lbl, COL_GREY, 0);
        }
        lv_obj_set_style_text_font(info_lbl, &geist_regular_16, 0);
        lv_obj_align(info_lbl, LV_ALIGN_RIGHT_MID, -16, 0);

        make_item_tappable(row);
        s_items[i] = row;
    }

    // "Add new network" item
    {
        lv_obj_t *row = lv_obj_create(s_list_cont);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 300, 56);
        lv_obj_set_style_bg_color(row, COL_SEL, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(row, 16, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, "+ Add new network");
        lv_obj_set_style_text_color(lbl, COL_GREEN, 0);
        lv_obj_set_style_text_font(lbl, &geist_regular_16, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 16, 0);

        make_item_tappable(row);
        s_items[s_network_count] = row;
    }

    // Status label (hidden, shown during connection)
    s_status_lbl = lv_label_create(s_picker_bg);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_width(s_status_lbl, 260);
    lv_obj_set_style_text_color(s_status_lbl, COL_GREY, 0);
    lv_obj_set_style_text_font(s_status_lbl, &geist_regular_16, 0);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -35);
    lv_obj_add_flag(s_status_lbl, LV_OBJ_FLAG_HIDDEN);

    // Set initial selection
    s_selected = 0;
    s_add_new_selected = (s_total_items == 1); // only "Add new" if no networks
    update_selection();
}

static void update_selection() {
    for (int i = 0; i < s_total_items; i++) {
        if (!s_items[i]) continue;
        bool selected = (i == s_selected);
        lv_obj_set_style_bg_opa(s_items[i], selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }

    s_add_new_selected = (s_selected == s_network_count);

    // Scroll to keep selected item visible
    if (s_items[s_selected]) {
        lv_obj_scroll_to_view(s_items[s_selected], LV_ANIM_ON);
    }
}

// ─── Connect to a network (blocking)

static EventGroupHandle_t s_connect_events = nullptr;
static constexpr int CONNECT_OK_BIT = BIT0;
static constexpr int CONNECT_FAIL_BIT = BIT1;
static esp_event_handler_instance_t s_evt_wifi = nullptr;
static esp_event_handler_instance_t s_evt_ip = nullptr;

static void connect_event_handler(void *, esp_event_base_t base, int32_t id, void *) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_connect_events, CONNECT_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_connect_events, CONNECT_OK_BIT);
    }
}

static bool try_connect(const char *ssid, const char *pass) {
    ESP_LOGI(TAG, "Connecting to %s...", ssid);

    s_connect_events = xEventGroupCreate();

    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                        connect_event_handler, nullptr, &s_evt_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        connect_event_handler, nullptr, &s_evt_ip);

    // Configure and connect
    wifi_config_t cfg = {};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_connect_events,
                                            CONNECT_OK_BIT | CONNECT_FAIL_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(10000));

    esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_evt_wifi);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_evt_ip);
    vEventGroupDelete(s_connect_events);
    s_connect_events = nullptr;

    bool ok = (bits & CONNECT_OK_BIT) != 0;
    ESP_LOGI(TAG, "Connection to %s: %s", ssid, ok ? "OK" : "FAILED");
    return ok;
}

// ─── Cleanup picker UI

static void cleanup_picker() {
    if (s_picker_bg) {
        lv_obj_delete(s_picker_bg);
        s_picker_bg = nullptr;
    }
    s_list_cont = nullptr;
    s_title_lbl = nullptr;
    s_status_lbl = nullptr;
    for (auto &item : s_items) item = nullptr;
}

// ─── Encoder polling timer for the picker

static lv_timer_t *s_picker_timer = nullptr;

static void picker_tick(lv_timer_t *) {
    int32_t steps = encoder_take_steps();
    if (steps == 0) return;

    haptic_buzz();

    int new_sel = s_selected + (steps > 0 ? 1 : -1);
    if (new_sel < 0) new_sel = 0;
    if (new_sel >= s_total_items) new_sel = s_total_items - 1;

    if (new_sel != s_selected) {
        s_selected = new_sel;
        update_selection();
    }
}

// ─── Touch handlers for list items

static void on_item_pressed(lv_event_t *e) {
    auto *row = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, COL_GREEN, 0);
}

static void on_item_released(lv_event_t *) {
    update_selection();
}

static void on_item_clicked(lv_event_t *e) {
    haptic_buzz();
    auto *row = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
    for (int i = 0; i < s_total_items; i++) {
        if (s_items[i] == row) {
            s_selected = i;
            s_add_new_selected = (i == s_network_count);
            update_selection();
            s_confirmed = true;
            return;
        }
    }
}

static void make_item_tappable(lv_obj_t *row) {
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_item_pressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(row, on_item_released, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(row, on_item_released, LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_add_event_cb(row, on_item_clicked, LV_EVENT_SHORT_CLICKED, nullptr);
}

// ─── Show "Connecting..." status on the picker

static void show_connecting(const char *ssid) {
    if (!display_lock(200)) return;
    char msg[64];
    snprintf(msg, sizeof(msg), "Connecting to %s...", ssid);
    lv_label_set_text(s_status_lbl, msg);
    lv_obj_clear_flag(s_status_lbl, LV_OBJ_FLAG_HIDDEN);
    display_unlock();
}

static void show_failed(const char *ssid) {
    if (!display_lock(200)) return;
    char msg[64];
    snprintf(msg, sizeof(msg), "Failed to connect to %s", ssid);
    lv_label_set_text(s_status_lbl, msg);
    display_unlock();
}

// ─── Show scanning status on the splash screen

static void show_scanning() {
    ui_splash_set_status("scanning for networks...");
}

// ─── Main entry point

WifiPickerResult wifi_picker_run() {
    // ─── Check if we can auto-connect (single saved network in range)
    // First, scan to see what's available
    show_scanning();

    wifi_scan(true);

    // Always show the picker — let the user choose

    // ─── Show the picker UI
    ui_dismiss_splash();
    if (display_lock(500)) {
        cleanup_picker();
        build_picker_ui();

        // Start encoder polling
        s_picker_timer = lv_timer_create(picker_tick, 20, nullptr);

        s_confirmed = false;
        display_unlock();
    }

    // ─── Main loop: wait for user to confirm a selection
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(50));

        if (!s_confirmed) continue;
        s_confirmed = false;

        // "Add new network" selected
        if (s_add_new_selected) {
            if (display_lock(200)) {
                if (s_picker_timer) {
                    lv_timer_delete(s_picker_timer);
                    s_picker_timer = nullptr;
                }
                cleanup_picker();
                display_unlock();
            }
            // Don't deinit WiFi — wifi_setup_start() will handle teardown.
            // We already have netifs created, so just stop.
            esp_wifi_stop();
            return WifiPickerResult::AddNew;
        }

        // Network selected — try to connect
        int idx = s_selected;
        if (idx < 0 || idx >= s_network_count) continue;

        const char *ssid = s_networks[idx].ssid;

        // Find password from saved networks
        char pass[65] = {};
        if (s_networks[idx].saved) {
            int cnt = settings_wifi_count();
            for (int i = 0; i < cnt; i++) {
                WifiEntry e;
                if (settings_wifi_get(i, &e) && strcmp(e.ssid, ssid) == 0) {
                    strncpy(pass, e.pass, sizeof(pass) - 1);
                    break;
                }
            }
        }

        // If not a saved network, we don't have the password — send to portal
        if (!s_networks[idx].saved) {
            if (display_lock(200)) {
                if (s_picker_timer) {
                    lv_timer_delete(s_picker_timer);
                    s_picker_timer = nullptr;
                }
                cleanup_picker();
                display_unlock();
            }
            esp_wifi_stop();
            esp_wifi_deinit();
            return WifiPickerResult::AddNew;
        }

        show_connecting(ssid);

        if (try_connect(ssid, pass)) {
            settings_wifi_save(ssid, pass); // move to front as most recent

            if (display_lock(200)) {
                if (s_picker_timer) {
                    lv_timer_delete(s_picker_timer);
                    s_picker_timer = nullptr;
                }
                cleanup_picker();
                display_unlock();
            }
            esp_wifi_stop();
            return WifiPickerResult::Connected;
        }

        // Connection failed — show error, let user try again
        show_failed(ssid);
        vTaskDelay(pdMS_TO_TICKS(2000));

        if (display_lock(200)) {
            lv_obj_add_flag(s_status_lbl, LV_OBJ_FLAG_HIDDEN);
            display_unlock();
        }
    }
}
