#include "spotify_setup.h"
#include "spotify/json_parse.h"
#include "app_config.h"
#include "ui/ui.h"

#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mbedtls/sha256.h"
#include "esp_random.h"

#include <cstring>
#include <cstdio>

static constexpr const char *TAG = "spotify_setup";
static const char *REDIRECT_URI = CONFIG_SPOTIFY_REDIRECT_URI;

extern "C" const char *spotify_client_id(void);

// ─── Server-side PKCE generation

static char s_pkce_verifier[128];
static char s_pkce_challenge[64];

static void base64url_encode(const uint8_t *in, size_t in_len,
                             char *out, size_t out_size) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t oi = 0;
    for (size_t i = 0; i < in_len && oi < out_size - 4; i += 3) {
        uint32_t n = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) n |= (uint32_t)in[i+1] << 8;
        if (i + 2 < in_len) n |= in[i+2];
        out[oi++] = tbl[(n >> 18) & 0x3F];
        out[oi++] = tbl[(n >> 12) & 0x3F];
        if (i + 1 < in_len) out[oi++] = tbl[(n >> 6) & 0x3F];
        if (i + 2 < in_len) out[oi++] = tbl[n & 0x3F];
    }
    out[oi] = '\0';
    // base64url: replace +/ with -_, strip =
    for (size_t i = 0; i < oi; i++) {
        if (out[i] == '+') out[i] = '-';
        else if (out[i] == '/') out[i] = '_';
    }
}

static void generate_pkce() {
    // Generate 32 random bytes -> base64url verifier (~43 chars)
    uint8_t rand_bytes[32];
    esp_fill_random(rand_bytes, sizeof(rand_bytes));
    base64url_encode(rand_bytes, sizeof(rand_bytes),
                     s_pkce_verifier, sizeof(s_pkce_verifier));

    // challenge = base64url(SHA256(verifier))
    uint8_t hash[32];
    mbedtls_sha256((const uint8_t *)s_pkce_verifier,
                   strlen(s_pkce_verifier), hash, 0);
    base64url_encode(hash, sizeof(hash),
                     s_pkce_challenge, sizeof(s_pkce_challenge));

    ESP_LOGI(TAG, "PKCE generated (verifier len=%d, challenge len=%d)",
             (int)strlen(s_pkce_verifier), (int)strlen(s_pkce_challenge));
}

// ─── Shared state

static char s_client_id[128] = {};

// ─── NVS helpers (namespace "spotify", matching spotify_auth.cpp)

static void save_refresh_token_nvs(const char *token) {
    nvs_handle_t h;
    if (nvs_open("spotify", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "refresh_tok", token);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void save_client_id_nvs(const char *id) {
    nvs_handle_t h;
    if (nvs_open("spotify", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "client_id", id);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool spotify_setup_has_token() {
    nvs_handle_t h;
    if (nvs_open("spotify", NVS_READONLY, &h) != ESP_OK) return false;
    // Ask for the length only: a refresh token is far longer than any small
    // buffer, and nvs_get_str fails with ESP_ERR_NVS_INVALID_LENGTH when the
    // buffer is too short, which read as "no token" on every boot.
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, "refresh_tok", nullptr, &len);
    nvs_close(h);
    return err == ESP_OK && len > 1;
}


// ─── Relay sign-in
//
// A phone often can't reach the knob directly (guest and IoT networks
// isolate clients), so sign-in only uses outbound connections:
//  1. The knob shows a QR code for the callback page, carrying a one-time
//     ntfy topic, its PKCE challenge and the client ID.
//  2. The page sends the phone to Spotify, which returns it with a code.
//  3. The page posts that code to https://ntfy.sh/<topic>.
//  4. The knob polls the topic and trades the code for tokens. That needs
//     the PKCE verifier, which never leaves the knob, so a code seen on the
//     public relay is useless to anyone else.

static constexpr const char *RELAY_URL = "https://ntfy.sh/";
static constexpr int POLL_MS = 5000;
static constexpr int64_t SESSION_US = 10LL * 60 * 1000 * 1000;
static constexpr int BUF_SIZE = 2048;

static char s_topic[24] = {};
static char s_setup_url[384] = {};

static void new_session(const char *client_id) {
    generate_pkce();
    uint8_t r[8];
    esp_fill_random(r, sizeof(r));
    int n = snprintf(s_topic, sizeof(s_topic), "knob");
    for (uint8_t b : r) {
        n += snprintf(s_topic + n, sizeof(s_topic) - n, "%02x", b);
    }
    snprintf(s_setup_url, sizeof(s_setup_url), "%s?t=%s&c=%s&id=%s",
             REDIRECT_URI, s_topic, s_pkce_challenge, client_id);
}

// Sends a request and reads the whole body into buf. Returns the HTTP
// status, or -1 if the connection failed.
static int http_request(const char *url, const char *body, char *buf,
                        int cap) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    auto *client = esp_http_client_init(&cfg);
    if (!client) return -1;
    if (body) {
        esp_http_client_set_header(client, "Content-Type",
                                   "application/x-www-form-urlencoded");
    }

    int status = -1;
    int len = 0;
    int body_len = body ? strlen(body) : 0;
    if (esp_http_client_open(client, body_len) == ESP_OK) {
        if (body) esp_http_client_write(client, body, body_len);
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        int r;
        while (len < cap - 1 &&
               (r = esp_http_client_read(client, buf + len, cap - 1 - len)) > 0) {
            len += r;
        }
    }
    buf[len] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

static bool exchange_code(const char *code, const char *client_id, char *buf) {
    char *post_body = static_cast<char *>(malloc(BUF_SIZE));
    if (!post_body) return false;
    snprintf(post_body, BUF_SIZE,
             "grant_type=authorization_code"
             "&code=%s"
             "&redirect_uri=%s"
             "&client_id=%s"
             "&code_verifier=%s",
             code, REDIRECT_URI, client_id, s_pkce_verifier);
    int status = http_request("https://accounts.spotify.com/api/token",
                              post_body, buf, BUF_SIZE);
    free(post_body);
    ESP_LOGI(TAG, "Token exchange: status=%d", status);

    char refresh_token[256] = {};
    if (status != 200 ||
        !json_str(buf, "refresh_token", refresh_token, sizeof(refresh_token))) {
        char desc[128] = {};
        json_str(buf, "error_description", desc, sizeof(desc));
        ESP_LOGE(TAG, "Token exchange failed: %s", desc[0] ? desc : "no token");
        return false;
    }
    save_refresh_token_nvs(refresh_token);
    save_client_id_nvs(client_id);
    strncpy(s_client_id, client_id, sizeof(s_client_id) - 1);
    ESP_LOGI(TAG, "Spotify auth complete — tokens saved to NVS");
    return true;
}

// ─── Public API

const char *spotify_setup_url() { return s_setup_url; }

void spotify_setup_start(const char *) {
    const char *client_id = spotify_client_id();
    if (client_id[0] == '\0') {
        ESP_LOGE(TAG, "No Spotify client ID — set CONFIG_SPOTIFY_CLIENT_ID");
        ui_set_status("Spotify needs a client ID\nSee the Spotify README");
        for (;;) vTaskDelay(portMAX_DELAY);
    }

    char *buf = static_cast<char *>(malloc(BUF_SIZE));
    if (!buf) {
        ESP_LOGE(TAG, "Out of memory for sign-in");
        return;
    }

    for (;;) {
        new_session(client_id);
        ESP_LOGI(TAG, "Sign-in session started (topic %.8s...)", s_topic);
        ui_show_spotify_setup(s_setup_url);

        char poll_url[96];
        snprintf(poll_url, sizeof(poll_url), "%s%s/json?poll=1&since=all", RELAY_URL,
                 s_topic);
        int64_t until = esp_timer_get_time() + SESSION_US;
        while (esp_timer_get_time() < until) {
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            int status = http_request(poll_url, nullptr, buf, BUF_SIZE);
            if (status != 200) {
                ESP_LOGW(TAG, "Relay poll failed: status=%d", status);
                continue;
            }
            char code[512] = {};
            if (!json_str(buf, "message", code, sizeof(code))) continue;

            ESP_LOGI(TAG, "Got sign-in code (len=%d)", (int)strlen(code));
            ui_set_status("Connecting to Spotify...\nThis can take a moment");
            if (exchange_code(code, client_id, buf)) {
                free(buf);
                s_setup_url[0] = '\0';
                return;
            }
            ui_set_status("Spotify sign-in failed\nScan the new code to try again");
            vTaskDelay(pdMS_TO_TICKS(3000));
            break;
        }
    }
}
