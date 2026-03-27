#include "spotify/spotify_api.h"
#include "spotify/spotify_auth.h"
#include "spotify/json_parse.h"
#include "app_config.h"

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

static constexpr const char *TAG = "spotify_api";
static constexpr int RESP_BUF_SIZE = 4096;
static constexpr int ART_BUF_SIZE  = 128 * 1024;

ESP_EVENT_DECLARE_BASE(APP_EVENT);

static TaskHandle_t s_poll_task = nullptr;
static bool s_running = false;

// ─── HTTP helpers

static void set_auth_header(esp_http_client_handle_t client) {
    const char *token = spotify_auth_get_token();
    if (!token) return;
    char header[600];
    snprintf(header, sizeof(header), "Bearer %s", token);
    esp_http_client_set_header(client, "Authorization", header);
}

// ─── Parse playback state JSON

static bool parse_player_state(const char *json, SpotifyState *state) {
    memset(state, 0, sizeof(*state));

    json_bool(json, "is_playing", &state->is_playing);
    json_int(json, "progress_ms", &state->progress_ms);

    // Volume from device object
    const char *device = strstr(json, "\"device\"");
    if (device) {
        json_int(device, "volume_percent", &state->volume);
    }

    // Track info from item object
    const char *item = strstr(json, "\"item\"");
    if (!item) return false;

    json_int(item, "duration_ms", &state->duration_ms);

    // Artist name — from first entry in "artists" array
    const char *artists = strstr(item, "\"artists\"");
    if (artists) {
        json_str(artists, "name", state->artist, sizeof(state->artist));
    }

    // Album name and art
    const char *album = strstr(item, "\"album\"");
    if (album) {
        json_str(album, "name", state->album, sizeof(state->album));
        // Get 300px image (second in array) — fits in buffer, fast download
        // Images: [{640px}, {300px}, {64px}]
        const char *images = strstr(album, "\"images\"");
        if (images) {
            const char *first_url = strstr(images, "\"url\"");
            const char *second_url = first_url ? strstr(first_url + 5, "\"url\"") : nullptr;
            if (second_url) {
                json_str(second_url, "url", state->art_url, sizeof(state->art_url));
            } else if (first_url) {
                json_str(first_url, "url", state->art_url, sizeof(state->art_url));
            }
        }
    }

    // Track name — the item's own "name" field.
    // Spotify JSON: "item": { "album":{...}, "artists":[...], ..., "name":"Track" }
    // The track "name" is the LAST "name" key in the item object, after all
    // nested album/artist names. Use json_str_last to find it.
    json_str_last(item, "name", state->track, sizeof(state->track));

    return state->track[0] != '\0';
}

// ─── API calls

/// Create an authenticated HTTP client, set body/headers, ready to perform.
static esp_http_client_handle_t make_client(
        const esp_http_client_config_t *cfg,
        esp_http_client_method_t method,
        const char *body) {
    auto *client = esp_http_client_init(cfg);
    set_auth_header(client);
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    } else if (method == HTTP_METHOD_PUT || method == HTTP_METHOD_POST) {
        // Spotify requires Content-Length: 0 for empty PUT/POST
        esp_http_client_set_post_field(client, "", 0);
    }
    return client;
}

static int api_request(const char *url, esp_http_client_method_t method,
                       char *resp_buf, int resp_cap,
                       const char *body = nullptr) {
    Response resp = {resp_buf, 0, resp_cap};
    if (resp_buf) resp_buf[0] = '\0';

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = method;
    cfg.timeout_ms = SPOTIFY_HTTP_TIMEOUT;
    cfg.event_handler = on_http_event;
    cfg.user_data = resp_buf ? &resp : nullptr;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    auto *client = make_client(&cfg, method, body);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP error: %d for %s", err, url);
        return -1;
    }

    // 401 = token expired, try refresh + retry once
    if (status == 401) {
        ESP_LOGW(TAG, "401 — refreshing token");
        if (spotify_auth_refresh()) {
            resp.len = 0;
            if (resp_buf) resp_buf[0] = '\0';
            auto *c2 = make_client(&cfg, method, body);
            esp_http_client_perform(c2);
            status = esp_http_client_get_status_code(c2);
            esp_http_client_cleanup(c2);
        }
    }

    return status;
}

// ─── Poll task

static void poll_task(void *) {
    auto *buf = static_cast<char *>(heap_caps_malloc(RESP_BUF_SIZE, MALLOC_CAP_SPIRAM));
    if (!buf) {
        ESP_LOGE(TAG, "Failed to alloc response buffer");
        vTaskDelete(nullptr);
        return;
    }

    while (s_running) {
        int status = api_request(
            "https://api.spotify.com/v1/me/player",
            HTTP_METHOD_GET, buf, RESP_BUF_SIZE);

        if (status == 200 && buf[0] == '{') {
            SpotifyState state;
            if (parse_player_state(buf, &state)) {
                esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_STATE_UPDATE,
                               &state, sizeof(state), 0);
            }
        } else if (status == 204) {
            // No active device — send empty state
            SpotifyState state = {};
            esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_STATE_UPDATE,
                           &state, sizeof(state), 0);
        } else if (status > 0) {
            ESP_LOGW(TAG, "Player poll: HTTP %d", status);
        }

        vTaskDelay(pdMS_TO_TICKS(SPOTIFY_POLL_MS));
    }

    heap_caps_free(buf);
    vTaskDelete(nullptr);
}

// ─── Public API

void spotify_api_init() {
    ESP_LOGI(TAG, "Spotify API initialized");
}

void spotify_api_start() {
    if (s_poll_task) return;
    s_running = true;
    xTaskCreatePinnedToCore(poll_task, "spotify_poll", 8192, nullptr, 5,
                            &s_poll_task, 1);
    ESP_LOGI(TAG, "Polling started");
}

void spotify_api_stop() {
    s_running = false;
    s_poll_task = nullptr;
}

void spotify_api_set_volume(int volume) {
    char url[128];
    snprintf(url, sizeof(url),
             "https://api.spotify.com/v1/me/player/volume?volume_percent=%d",
             volume);
    api_request(url, HTTP_METHOD_PUT, nullptr, 0);
}

void spotify_api_play_pause(bool currently_playing) {
    const char *url = currently_playing
        ? "https://api.spotify.com/v1/me/player/pause"
        : "https://api.spotify.com/v1/me/player/play";
    api_request(url, HTTP_METHOD_PUT, nullptr, 0);
}

void spotify_api_next() {
    api_request("https://api.spotify.com/v1/me/player/next",
                HTTP_METHOD_POST, nullptr, 0);
}

void spotify_api_prev() {
    api_request("https://api.spotify.com/v1/me/player/previous",
                HTTP_METHOD_POST, nullptr, 0);
}

void spotify_api_seek(int position_ms) {
    if (position_ms < 0) position_ms = 0;
    char url[128];
    snprintf(url, sizeof(url),
             "https://api.spotify.com/v1/me/player/seek?position_ms=%d",
             position_ms);
    api_request(url, HTTP_METHOD_PUT, nullptr, 0);
}

void spotify_api_play_random_liked() {
    // Step 1: get total liked songs count
    auto *buf = static_cast<char *>(heap_caps_malloc(RESP_BUF_SIZE, MALLOC_CAP_SPIRAM));
    if (!buf) return;

    int status = api_request(
        "https://api.spotify.com/v1/me/tracks?limit=1&offset=0",
        HTTP_METHOD_GET, buf, RESP_BUF_SIZE);

    int total = 0;
    if (status == 200) {
        json_int(buf, "total", &total);
    }

    if (total <= 0) {
        ESP_LOGW(TAG, "No liked songs found");
        heap_caps_free(buf);
        return;
    }

    // Step 2: pick random offset and fetch that track
    int offset = esp_random() % total;
    char url[128];
    snprintf(url, sizeof(url),
             "https://api.spotify.com/v1/me/tracks?limit=1&offset=%d", offset);

    status = api_request(url, HTTP_METHOD_GET, buf, RESP_BUF_SIZE);
    if (status != 200) {
        heap_caps_free(buf);
        return;
    }

    // Extract track URI
    char uri[128] = {};
    // Look inside "items" > "track" > "uri"
    const char *items = strstr(buf, "\"items\"");
    if (items) {
        const char *track = strstr(items, "\"track\"");
        if (track) {
            json_str(track, "uri", uri, sizeof(uri));
        }
    }

    heap_caps_free(buf);

    if (uri[0] == '\0') {
        ESP_LOGW(TAG, "Could not extract track URI");
        return;
    }

    // Step 3: play it
    char body[256];
    snprintf(body, sizeof(body), "{\"uris\":[\"%s\"]}", uri);
    api_request("https://api.spotify.com/v1/me/player/play",
                HTTP_METHOD_PUT, nullptr, 0, body);

    ESP_LOGI(TAG, "DJ spin! Playing: %s", uri);
}

int spotify_api_fetch_art(const char *url, uint8_t *buf, int buf_size) {
    if (!url || url[0] == '\0') return 0;

    struct ArtResp {
        uint8_t *buf;
        int len;
        int cap;
    };

    ArtResp art = {buf, 0, buf_size};

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = SPOTIFY_HTTP_TIMEOUT;
    cfg.event_handler = [](esp_http_client_event_t *evt) -> esp_err_t {
        auto *a = static_cast<ArtResp *>(evt->user_data);
        if (evt->event_id == HTTP_EVENT_ON_DATA && a && evt->data_len > 0) {
            int needed = a->len + evt->data_len;
            if (needed <= a->cap) {
                memcpy(a->buf + a->len, evt->data, evt->data_len);
                a->len = needed;
            }
        }
        return ESP_OK;
    };
    cfg.user_data = &art;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    auto *client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) return 0;
    return art.len;
}
