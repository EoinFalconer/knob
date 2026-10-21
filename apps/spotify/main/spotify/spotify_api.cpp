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

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <strings.h>

static constexpr const char *TAG = "spotify_api";
static constexpr int RESP_BUF_SIZE = 4096;

ESP_EVENT_DECLARE_BASE(APP_EVENT);

static TaskHandle_t s_poll_task = nullptr;
static bool s_running = false;
static bool s_was_idle = false;
static int64_t s_devices_fetched_at = 0;

// Reads `key` from the object that opens at or after `from`, skipping
// nested objects and arrays. Spotify's album object lists its artists
// (each with a "name") before its own "name", so a plain search for the
// first "name" returns the artist.
static bool json_str_own(const char *from, const char *key, char *out,
                         size_t out_len) {
    const char *p = strchr(from, '{');
    if (!p) return false;
    size_t key_len = strlen(key);
    int depth = 0;
    for (; *p; p++) {
        if (*p == '"') {
            const char *start = p + 1;
            for (p = start; *p && *p != '"'; p++)
                if (*p == '\\' && p[1]) p++;
            if (!*p) return false;
            if (depth == 1 && (size_t)(p - start) == key_len &&
                strncmp(start, key, key_len) == 0) {
                const char *q = p + 1;
                while (*q == ' ') q++;
                if (*q == ':') return json_str(start - 1, key, out, out_len);
            }
        } else if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (--depth == 0) return false;
        }
    }
    return false;
}
static SpotifyState s_idle_state = {};
static SpotifyState s_last_playing = {};
static char s_playing_track[128] = {};
static char s_playing_art_url[256] = {};

struct ApiResponse {
    Response body;
    int retry_after_ms;
};

static esp_err_t api_event(esp_http_client_event_t *event) {
    auto *response = static_cast<ApiResponse *>(event->user_data);
    if (!response) return ESP_OK;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key &&
        event->header_value && strcasecmp(event->header_key, "Retry-After") == 0) {
        response->retry_after_ms = atoi(event->header_value) * 1000;
        return ESP_OK;
    }
    void *user_data = event->user_data;
    event->user_data = &response->body;
    esp_err_t result = on_http_event(event);
    event->user_data = user_data;
    return result;
}

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
        json_str(device, "id", state->device_id, sizeof(state->device_id));
        json_str(device, "name", state->device_name,
                 sizeof(state->device_name));
        state->has_device = state->device_id[0] != '\0';
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
        json_str_own(album, "name", state->album, sizeof(state->album));
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

static void parse_recent(const char *json, SpotifyState *state) {
    const char *items = strstr(json, "\"items\"");
    const char *track = items ? strstr(items, "\"track\"") : nullptr;
    if (!track) return;
    json_str_last(track, "name", state->last_track, sizeof(state->last_track));
    json_str_last(track, "uri", state->last_uri, sizeof(state->last_uri));
    const char *artists = strstr(track, "\"artists\"");
    if (artists)
        json_str(artists, "name", state->last_artist, sizeof(state->last_artist));
    const char *images = strstr(track, "\"images\"");
    if (images) {
        const char *first = strstr(images, "\"url\"");
        const char *second = first ? strstr(first + 5, "\"url\"") : nullptr;
        if (first)
            json_str(second ? second : first, "url", state->last_art_url,
                     sizeof(state->last_art_url));
    }
}

static void parse_queue(char *json, SpotifyState *state) {
  const char *queue = strstr(json, "\"queue\"");
  char *track = queue ? strchr(const_cast<char *>(queue), '{') : nullptr;
  if (!track)
    return;
  int depth = 0;
  bool quoted = false;
  bool escaped = false;
  char *end = track;
  for (; *end; ++end) {
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (*end == '\\')
        escaped = true;
      else if (*end == '"')
        quoted = false;
      continue;
    }
    if (*end == '"') {
      quoted = true;
    } else if (*end == '{') {
      ++depth;
    } else if (*end == '}' && --depth == 0) {
      ++end;
      break;
    }
  }
  if (depth != 0)
    return;
  char saved = *end;
  *end = '\0';
  json_str_last(track, "name", state->next_track,
                sizeof(state->next_track));
  const char *artists = strstr(track, "\"artists\"");
  if (artists)
    json_str(artists, "name", state->next_artist,
             sizeof(state->next_artist));
  const char *images = strstr(track, "\"images\"");
  if (images) {
    const char *first = strstr(images, "\"url\"");
    const char *second = first ? strstr(first + 5, "\"url\"") : nullptr;
    if (first)
      json_str(second ? second : first, "url", state->next_art_url,
               sizeof(state->next_art_url));
  }
  *end = saved;
}

static void parse_devices(const char *json, SpotifyState *state) {
    state->has_device = false;
    state->device_id[0] = '\0';
    state->device_name[0] = '\0';
    const char *p = strstr(json, "\"devices\"");
    while (p && (p = strstr(p, "\"id\""))) {
        const char *end = strchr(p, '}');
        if (!end) break;
        bool restricted = false;
        json_bool(p, "is_restricted", &restricted);
        char id[128] = {};
        char name[64] = {};
        json_str(p, "id", id, sizeof(id));
        json_str(p, "name", name, sizeof(name));
        if (!restricted && id[0] && name[0]) {
            snprintf(state->device_id, sizeof(state->device_id), "%s", id);
            snprintf(state->device_name, sizeof(state->device_name), "%s", name);
            state->has_device = true;
            bool active = false;
            json_bool(p, "is_active", &active);
            if (active) return;
        }
        p = end + 1;
    }
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
                       const char *body = nullptr,
                       int *retry_after_ms = nullptr) {
    ApiResponse response = {{resp_buf, 0, resp_cap}, 0};
    Response &resp = response.body;
    if (resp_buf) resp_buf[0] = '\0';

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = method;
    cfg.timeout_ms = SPOTIFY_HTTP_TIMEOUT;
    cfg.event_handler = api_event;
    cfg.user_data = &response;
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

    if (retry_after_ms)
        *retry_after_ms = response.retry_after_ms;
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
        int retry_after_ms = 0;
        int status = api_request(
            "https://api.spotify.com/v1/me/player",
            HTTP_METHOD_GET, buf, RESP_BUF_SIZE, nullptr, &retry_after_ms);

        int64_t now = esp_timer_get_time();
        if (status == 200 && buf[0] == '{') {
            SpotifyState state;
            if (parse_player_state(buf, &state)) {
                bool track_changed = strcmp(s_playing_track, state.track) != 0 ||
                                     strcmp(s_playing_art_url,
                                            state.art_url) != 0;
                if (track_changed) {
                    if (s_playing_track[0]) {
                        snprintf(state.last_track, sizeof(state.last_track),
                                 "%s", s_last_playing.track);
                        snprintf(state.last_artist, sizeof(state.last_artist),
                                 "%s", s_last_playing.artist);
                        snprintf(state.last_art_url,
                                 sizeof(state.last_art_url), "%s",
                                 s_last_playing.art_url);
                    } else {
                        int recent = api_request(
                            "https://api.spotify.com/v1/me/player/recently-played?limit=1",
                            HTTP_METHOD_GET, buf, RESP_BUF_SIZE);
                        if (recent == 200)
                            parse_recent(buf, &state);
                    }
                    auto *queue_buf = static_cast<char *>(
                        heap_caps_malloc(12 * 1024, MALLOC_CAP_SPIRAM));
                    if (queue_buf) {
                        int queued = api_request(
                            "https://api.spotify.com/v1/me/player/queue",
                            HTTP_METHOD_GET, queue_buf, 12 * 1024);
                        if (queued == 200)
                            parse_queue(queue_buf, &state);
                        heap_caps_free(queue_buf);
                    }
                    snprintf(s_playing_track, sizeof(s_playing_track), "%s",
                             state.track);
                    snprintf(s_playing_art_url, sizeof(s_playing_art_url),
                             "%s", state.art_url);
                    s_last_playing = state;
                } else {
                    snprintf(state.last_track, sizeof(state.last_track), "%s",
                             s_last_playing.last_track);
                    snprintf(state.last_artist, sizeof(state.last_artist), "%s",
                             s_last_playing.last_artist);
                    snprintf(state.last_art_url,
                             sizeof(state.last_art_url), "%s",
                             s_last_playing.last_art_url);
                    snprintf(state.next_track, sizeof(state.next_track), "%s",
                             s_last_playing.next_track);
                    snprintf(state.next_artist,
                             sizeof(state.next_artist), "%s",
                             s_last_playing.next_artist);
                    snprintf(state.next_art_url,
                             sizeof(state.next_art_url), "%s",
                             s_last_playing.next_art_url);
                    s_last_playing = state;
                }
                s_was_idle = false;
                esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_STATE_UPDATE,
                               &state, sizeof(state), 0);
            }
        } else if (status == 204) {
            if (!s_was_idle) {
                s_idle_state = {};
                int recent = api_request(
                    "https://api.spotify.com/v1/me/player/recently-played?limit=1",
                    HTTP_METHOD_GET, buf, RESP_BUF_SIZE);
                if (recent == 200)
                    parse_recent(buf, &s_idle_state);
            }
            if (!s_was_idle || now - s_devices_fetched_at >= 30000000LL) {
                int devices = api_request(
                    "https://api.spotify.com/v1/me/player/devices",
                    HTTP_METHOD_GET, buf, RESP_BUF_SIZE);
                if (devices == 200)
                    parse_devices(buf, &s_idle_state);
                s_devices_fetched_at = now;
            }
            s_was_idle = true;
            esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_STATE_UPDATE,
                           &s_idle_state, sizeof(s_idle_state), 0);
        } else if (status > 0) {
            ESP_LOGW(TAG, "Player poll: HTTP %d", status);
        }
        int32_t poll_status = status;
        esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_POLL_STATUS,
                       &poll_status, sizeof(poll_status), 0);

        int delay_ms = status == 429
                           ? std::max(retry_after_ms, SPOTIFY_POLL_MS)
                           : SPOTIFY_POLL_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
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

int spotify_api_set_volume(int volume) {
    char url[128];
    snprintf(url, sizeof(url),
             "https://api.spotify.com/v1/me/player/volume?volume_percent=%d",
             volume);
    return api_request(url, HTTP_METHOD_PUT, nullptr, 0);
}

int spotify_api_play_pause(bool currently_playing) {
    const char *url = currently_playing
        ? "https://api.spotify.com/v1/me/player/pause"
        : "https://api.spotify.com/v1/me/player/play";
    return api_request(url, HTTP_METHOD_PUT, nullptr, 0);
}

int spotify_api_next() {
    return api_request("https://api.spotify.com/v1/me/player/next",
                       HTTP_METHOD_POST, nullptr, 0);
}

int spotify_api_prev() {
    return api_request("https://api.spotify.com/v1/me/player/previous",
                       HTTP_METHOD_POST, nullptr, 0);
}

int spotify_api_seek(int position_ms) {
    if (position_ms < 0) position_ms = 0;
    char url[128];
    snprintf(url, sizeof(url),
             "https://api.spotify.com/v1/me/player/seek?position_ms=%d",
             position_ms);
    return api_request(url, HTTP_METHOD_PUT, nullptr, 0);
}

int spotify_api_transfer(const char *device_id) {
    char body[256];
    snprintf(body, sizeof(body), "{\"device_ids\":[\"%s\"],\"play\":true}",
             device_id);
    return api_request("https://api.spotify.com/v1/me/player",
                       HTTP_METHOD_PUT, nullptr, 0, body);
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
