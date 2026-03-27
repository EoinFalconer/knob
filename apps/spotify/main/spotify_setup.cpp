#include "spotify_setup.h"
#include "spotify/json_parse.h"
#include "app_config.h"

#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

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

// ─── Shared state between HTTP handlers

static char s_device_ip[32] = {};
static char s_client_id[128] = {};
static SemaphoreHandle_t s_done_sem = nullptr;

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
    char tok[16] = {};
    size_t len = sizeof(tok);
    // Just check if key exists and is non-empty
    esp_err_t err = nvs_get_str(h, "refresh_tok", tok, &len);
    nvs_close(h);
    return err == ESP_OK && tok[0] != '\0';
}

// ─── HTML: Spotify setup page (GET /spotify)
// Template has 4 substitutions: %s = redirect_uri, %s = client_id, %s = auth_url, %s = device_ip

static const char SETUP_PAGE_TEMPLATE[] = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Knob Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
background:#111;color:#fff;display:flex;justify-content:center;
align-items:center;min-height:100vh;padding:20px}
.wrap{max-width:420px;width:100%%;text-align:center}
h1{font-size:1.2em;margin-bottom:4px}
.sub{color:#b3b3b3;font-size:.85em;margin-bottom:20px}
.card{background:#1a1a1a;border-radius:16px;padding:24px;text-align:left;
box-shadow:0 8px 32px rgba(0,0,0,.4)}
.step{margin-bottom:16px;padding:12px;background:#222;border-radius:10px;
border-left:3px solid #1DB954}
.step-num{color:#1DB954;font-weight:700;font-size:.85em;margin-bottom:4px}
.step p{color:#ccc;font-size:.85em;line-height:1.4}
.step code{background:#333;padding:2px 6px;border-radius:4px;font-size:.78em;
color:#1DB954;word-break:break-all;user-select:all}
label{display:block;color:#b3b3b3;font-size:.85em;margin-bottom:6px;margin-top:16px}
input[type=text]{width:100%%;padding:12px;
border:1px solid #333;border-radius:8px;background:#2a2a2a;color:#fff;
font-size:1em;outline:none}
input:focus{border-color:#1DB954}
a.btn{display:block;text-align:center;padding:14px;border:none;border-radius:24px;
background:#1DB954;color:#fff;font-size:1em;font-weight:600;text-decoration:none;
margin-top:20px;transition:background .2s}
a.btn:hover{background:#1ed760}
.status{text-align:center;margin-top:12px;color:#b3b3b3;font-size:.9em}
.status.ok{color:#1DB954}
.spinner{display:inline-block;width:16px;height:16px;border:2px solid #555;
border-top-color:#1DB954;border-radius:50%%;animation:spin .6s linear infinite;
vertical-align:middle;margin-right:6px}
@keyframes spin{to{transform:rotate(360deg)}}
</style>
</head>
<body>
<div class="wrap">
<h1>Connect Spotify</h1>
<p class="sub">Link your Spotify account to your knob.</p>
<div class="card">
<div class="step">
 <div class="step-num">One-time setup</div>
 <p>Go to <a href="https://developer.spotify.com/dashboard" target="_blank" style="color:#1DB954">developer.spotify.com/dashboard</a>, open your app, and add this <strong>Redirect URI</strong>:</p>
 <p style="margin-top:6px"><code>%s</code></p>
</div>
<div id="cid-section">
<label for="client_id">Client ID</label>
<input type="text" id="client_id" placeholder="Paste your Spotify Client ID" value="%s">
<a class="btn" id="auth-link" href="#" onclick="return go()">Connect with Spotify</a>
</div>
<div class="status" id="status"></div>
</div>
</div>
<script>
var AUTH_URL='%s';
var DEVICE_IP='%s';

function go(){
 var cid=document.getElementById('client_id').value.trim();
 if(!cid||cid.length<10){
  document.getElementById('status').textContent='Please enter a valid Client ID';
  return false;
 }
 // Save client_id to device
 fetch('/save_client_id',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
  body:'client_id='+encodeURIComponent(cid)}).catch(function(){});
 // Replace client_id in auth URL if user changed it
 var url=AUTH_URL.replace(/client_id=[^&]+/,'client_id='+encodeURIComponent(cid));
 window.location.href=url;
 return false;
}
// Auto-redirect if client_id is pre-filled
if(document.getElementById('client_id').value.length>10){
 document.getElementById('status').textContent='Client ID found. Tap Connect to authorize.';
}
</script>
</body>
</html>
)rawhtml";

// ─── HTML: Callback page (GET /callback)

static const char CALLBACK_PAGE_TEMPLATE[] = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Knob - Connecting...</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;
background:#111;color:#fff;display:flex;justify-content:center;
align-items:center;min-height:100vh;padding:20px;text-align:center}
.wrap{max-width:380px;width:100%%}
h1{font-size:1.3em;margin-bottom:12px}
.status{color:#b3b3b3;font-size:.95em;margin-top:16px}
.status.error{color:#e74c3c}
.status.ok{color:#1DB954}
.spinner{display:inline-block;width:20px;height:20px;border:2px solid #555;
border-top-color:#1DB954;border-radius:50%%;animation:spin .6s linear infinite;
vertical-align:middle;margin-right:8px}
@keyframes spin{to{transform:rotate(360deg)}}
</style>
</head>
<body>
<div class="wrap">
<h1>Connecting to Spotify...</h1>
<div class="status" id="status"><span class="spinner"></span>Exchanging authorization code...</div>
</div>
<script>
var DEVICE_IP='%s';
(async function(){
 var st=document.getElementById('status');
 var params=new URLSearchParams(window.location.search);
 var code=params.get('code');
 var error=params.get('error');
 if(error){
  st.className='status error';
  st.textContent='Spotify denied access: '+error;
  return;
 }
 if(!code){
  st.className='status error';
  st.textContent='No authorization code received.';
  return;
 }
 var verifier=localStorage.getItem('pkce_verifier');
 var cid=localStorage.getItem('client_id');
 if(!verifier||!cid){
  st.className='status error';
  st.textContent='Session data lost. Please go back and try again.';
  return;
 }
 try{
  var resp=await fetch('http://'+DEVICE_IP+':8888/exchange',{
   method:'POST',
   headers:{'Content-Type':'application/x-www-form-urlencoded'},
   body:'code='+encodeURIComponent(code)+'&verifier='+encodeURIComponent(verifier)+'&client_id='+encodeURIComponent(cid)
  });
  var data=await resp.json();
  if(data.ok){
   st.className='status ok';
   st.innerHTML='&#10003; Connected! Your knob is now linked to Spotify.<br><br>You can close this page.';
   sessionStorage.removeItem('pkce_verifier');
   sessionStorage.removeItem('client_id');
  }else{
   st.className='status error';
   st.textContent='Error: '+(data.error||'Token exchange failed');
  }
 }catch(e){
  st.className='status error';
  st.textContent='Network error: '+e.message;
 }
})();
</script>
</body>
</html>
)rawhtml";

// ─── HTTP handler: GET /spotify — setup page

static esp_err_t handle_setup(httpd_req_t *req) {
    const char *cid = spotify_client_id();

    // Generate fresh PKCE each time setup page is loaded
    generate_pkce();

    // Build Spotify auth URL with PKCE challenge and device IP as state
    // URL-encode the redirect URI for the query string
    char encoded_redir[256] = {};
    {
        const char *src = REDIRECT_URI;
        int di = 0;
        for (int si = 0; src[si] && di < (int)sizeof(encoded_redir) - 4; si++) {
            char c = src[si];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
                encoded_redir[di++] = c;
            } else {
                di += snprintf(encoded_redir + di, 4, "%%%02X", (uint8_t)c);
            }
        }
        encoded_redir[di] = '\0';
    }

    char auth_url[768];
    snprintf(auth_url, sizeof(auth_url),
        "https://accounts.spotify.com/authorize"
        "?client_id=%s"
        "&response_type=code"
        "&redirect_uri=%s"
        "&scope=user-read-playback-state%%20user-modify-playback-state"
        "%%20user-read-currently-playing%%20user-library-read"
        "&code_challenge_method=S256"
        "&code_challenge=%s"
        "&state=%s",
        cid, encoded_redir, s_pkce_challenge, s_device_ip);

    ESP_LOGI(TAG, "Auth URL (first 200): %.200s", auth_url);

    int page_size = sizeof(SETUP_PAGE_TEMPLATE) + 1024;
    char *page = static_cast<char *>(malloc(page_size));
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    snprintf(page, page_size, SETUP_PAGE_TEMPLATE, REDIRECT_URI, cid, auth_url, s_device_ip);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return ESP_OK;
}

// ─── HTTP handler: POST /save_client_id
static esp_err_t handle_save_client_id(httpd_req_t *req) {
    char body[256] = {};
    httpd_req_recv(req, body, sizeof(body) - 1);
    char *p = strstr(body, "client_id=");
    if (p) {
        p += 10;
        char *end = strchr(p, '&');
        if (end) *end = '\0';
        // Save to NVS
        nvs_handle_t h;
        if (nvs_open("spotify", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "client_id", p);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// ─── Background token exchange task

static char s_auth_code[512] = {};

// Forward declaration
void ui_set_status(const char *msg);

static void exchange_task(void *) {
    ESP_LOGI(TAG, "Exchanging auth code for tokens...");
    ui_set_status("Connecting to Spotify...\nThis can take a moment");
    const char *client_id = spotify_client_id();

    char post_body[1536];
    snprintf(post_body, sizeof(post_body),
             "grant_type=authorization_code"
             "&code=%s"
             "&redirect_uri=%s"
             "&client_id=%s"
             "&code_verifier=%s",
             s_auth_code, REDIRECT_URI, client_id, s_pkce_verifier);

    char resp_buf[2048] = {};
    int resp_len = 0;

    esp_http_client_config_t cfg = {};
    cfg.url = "https://accounts.spotify.com/api/token";
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    auto *client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type",
                               "application/x-www-form-urlencoded");

    esp_err_t err = esp_http_client_open(client, strlen(post_body));
    int status = 0;
    if (err == ESP_OK) {
        esp_http_client_write(client, post_body, strlen(post_body));
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        resp_len = esp_http_client_read(client, resp_buf, sizeof(resp_buf) - 1);
        if (resp_len > 0) resp_buf[resp_len] = '\0';
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Token exchange: status=%d", status);

    if (status == 200 && resp_len > 0) {
        char refresh_token[256] = {};
        json_str(resp_buf, "refresh_token", refresh_token, sizeof(refresh_token));
        if (refresh_token[0] != '\0') {
            nvs_handle_t h;
            if (nvs_open("spotify", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_str(h, "refresh_tok", refresh_token);
                nvs_set_str(h, "client_id", client_id);
                nvs_commit(h);
                nvs_close(h);
            }
            ESP_LOGI(TAG, "Spotify auth complete — tokens saved");
            ui_set_status("Connected to Spotify!\nStarting...");
        }
    } else {
        ESP_LOGE(TAG, "Token exchange failed: %s", resp_buf);
        ui_set_status("Token exchange failed\nRestart and try again");
    }

    if (s_done_sem) xSemaphoreGive(s_done_sem);
    vTaskDelete(nullptr);
}

// ─── HTTP handler: GET /callback?code=XXX

static esp_err_t handle_callback(httpd_req_t *req) {
    ESP_LOGI(TAG, "Callback handler hit");

    char query[1024] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No query");
        return ESP_OK;
    }

    char code[512] = {};
    if (httpd_query_key_value(query, "code", code, sizeof(code)) != ESP_OK) {
        // Check for error
        char error[64] = {};
        httpd_query_key_value(query, "error", error, sizeof(error));
        char msg[256];
        snprintf(msg, sizeof(msg),
            "<html><body style='background:#111;color:#fff;font-family:sans-serif;"
            "display:flex;justify-content:center;align-items:center;height:100vh'>"
            "<div style='text-align:center'><h2>Auth failed</h2><p>%s</p></div>"
            "</body></html>", error[0] ? error : "No auth code received");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Got auth code, starting background exchange");

    // Save code and launch exchange on a separate task (TLS needs big stack)
    strncpy(s_auth_code, code, sizeof(s_auth_code) - 1);
    xTaskCreatePinnedToCore(exchange_task, "token_xchg", 16384, nullptr, 5,
                            nullptr, 1);

    // Respond immediately — don't block the browser
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body style='background:#111;color:#fff;font-family:sans-serif;"
        "display:flex;justify-content:center;align-items:center;height:100vh'>"
        "<div style='text-align:center'>"
        "<h1 style='color:#1DB954'>Your knob is connected to Spotify!</h1>"
        "<p style='color:#888;margin-top:8px'>Give it a spin.</p>"
        "</div></body></html>");
    return ESP_OK;
}

// ─── URL-decode helper

static void url_decode(char *dst, const char *src, size_t dst_size) {
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_size - 1; si++) {
        if (src[si] == '%' && src[si + 1] && src[si + 2]) {
            char hex[3] = {src[si + 1], src[si + 2], 0};
            dst[di++] = (char)strtol(hex, nullptr, 16);
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

// ─── Parse a form field from body

static bool parse_field(const char *body, const char *name,
                        char *out, size_t out_size) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s=", name);
    const char *p = body;
    // Find field at start of body or after '&'
    while ((p = strstr(p, pattern)) != nullptr) {
        if (p == body || *(p - 1) == '&') break;
        p += strlen(pattern);
    }
    if (!p) return false;
    p += strlen(pattern);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_size) len = out_size - 1;
    char raw[512] = {};
    if (len >= sizeof(raw)) len = sizeof(raw) - 1;
    memcpy(raw, p, len);
    raw[len] = '\0';
    url_decode(out, raw, out_size);
    return out[0] != '\0';
}

// ─── HTTP handler: POST /exchange — exchange code for tokens

static esp_err_t handle_exchange(httpd_req_t *req) {
    ESP_LOGI(TAG, "Exchange handler called");

    // CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    // Read POST body
    char body[1024] = {};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        ESP_LOGE(TAG, "Exchange: no body received");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No body\"}");
        return ESP_OK;
    }
    body[recv_len] = '\0';
    ESP_LOGI(TAG, "Exchange: body len=%d", recv_len);

    // Parse fields: code, verifier, client_id
    char code[512] = {};
    char verifier[256] = {};
    char client_id[128] = {};

    if (!parse_field(body, "code", code, sizeof(code)) ||
        !parse_field(body, "verifier", verifier, sizeof(verifier)) ||
        !parse_field(body, "client_id", client_id, sizeof(client_id))) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Missing fields\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Exchanging auth code for tokens (client_id=%.8s...)", client_id);

    // Extract redirect_uri from POST (must match what auth request used)
    char redirect_uri[128] = {};
    char *p = strstr(body, "redirect_uri=");
    if (p) {
        p += 13;
        char *end = strchr(p, '&');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len >= sizeof(redirect_uri)) len = sizeof(redirect_uri) - 1;
        // URL decode
        for (size_t si = 0, di = 0; si < len && di < sizeof(redirect_uri) - 1; si++, di++) {
            if (p[si] == '%' && si + 2 < len) {
                char hex[3] = {p[si+1], p[si+2], 0};
                redirect_uri[di] = (char)strtol(hex, nullptr, 16);
                si += 2;
            } else if (p[si] == '+') {
                redirect_uri[di] = ' ';
            } else {
                redirect_uri[di] = p[si];
            }
        }
    }
    if (redirect_uri[0] == '\0') {
        strncpy(redirect_uri, "http://127.0.0.1:8888/callback", sizeof(redirect_uri) - 1);
    }

    char post_body[1536];
    snprintf(post_body, sizeof(post_body),
             "grant_type=authorization_code"
             "&code=%s"
             "&redirect_uri=%s"
             "&client_id=%s"
             "&code_verifier=%s",
             code, redirect_uri, client_id, verifier);

    // Make HTTP POST to Spotify token endpoint
    // Use open/write/fetch_headers/read pattern to avoid event loop issues
    char resp_buf[2048] = {};
    int resp_len = 0;

    esp_http_client_config_t cfg = {};
    cfg.url = "https://accounts.spotify.com/api/token";
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    auto *client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type",
                               "application/x-www-form-urlencoded");

    esp_err_t err = esp_http_client_open(client, strlen(post_body));
    int status = 0;
    if (err == ESP_OK) {
        esp_http_client_write(client, post_body, strlen(post_body));
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        resp_len = esp_http_client_read(client, resp_buf, sizeof(resp_buf) - 1);
        if (resp_len > 0) resp_buf[resp_len] = '\0';
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Token exchange: err=%d status=%d resp_len=%d", err, status, resp_len);
    if (resp_len > 0) ESP_LOGI(TAG, "Response: %.200s", resp_buf);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Token exchange failed: err=%d status=%d", err, status);

        char error_msg[256];
        // Try to extract error description from Spotify response
        char desc[128] = {};
        if (json_str(resp_buf, "error_description", desc, sizeof(desc)) &&
            desc[0] != '\0') {
            snprintf(error_msg, sizeof(error_msg),
                     "{\"ok\":false,\"error\":\"%s\"}", desc);
        } else {
            snprintf(error_msg, sizeof(error_msg),
                     "{\"ok\":false,\"error\":\"HTTP %d\"}", status);
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, error_msg);
        return ESP_OK;
    }

    // Parse tokens from response
    char access_token[512] = {};
    char refresh_token[256] = {};
    json_str(resp_buf, "access_token", access_token, sizeof(access_token));
    json_str(resp_buf, "refresh_token", refresh_token, sizeof(refresh_token));

    if (access_token[0] == '\0' || refresh_token[0] == '\0') {
        ESP_LOGE(TAG, "Missing tokens in response: %s", resp_buf);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No tokens in response\"}");
        return ESP_OK;
    }

    // Save to NVS
    save_refresh_token_nvs(refresh_token);
    save_client_id_nvs(client_id);

    // Also save to module state for immediate use
    strncpy(s_pkce_verifier, "", sizeof(s_pkce_verifier));
    strncpy(s_client_id, client_id, sizeof(s_client_id) - 1);

    ESP_LOGI(TAG, "Spotify auth complete — tokens saved to NVS");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    // Signal that setup is done
    if (s_done_sem) {
        xSemaphoreGive(s_done_sem);
    }

    return ESP_OK;
}

// ─── Public API

void spotify_setup_start(const char *device_ip) {
    strncpy(s_device_ip, device_ip, sizeof(s_device_ip) - 1);
    s_done_sem = xSemaphoreCreateBinary();

    ESP_LOGI(TAG, "Starting Spotify setup server on %s:8888", s_device_ip);

    // Start HTTP server on port 8888
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.server_port = 8888;
    http_cfg.max_uri_handlers = 8;
    http_cfg.max_open_sockets = 3;
    http_cfg.max_resp_headers = 16;
    http_cfg.stack_size = 16384; // needs headroom for TLS token exchange
    http_cfg.recv_wait_timeout = 10;
    http_cfg.send_wait_timeout = 10;

    httpd_handle_t server = nullptr;
    esp_err_t err = httpd_start(&server, &http_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_done_sem);
        s_done_sem = nullptr;
        return;
    }

    // GET /spotify — setup page
    const httpd_uri_t uri_setup = {
        .uri = "/spotify",
        .method = HTTP_GET,
        .handler = handle_setup,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &uri_setup);

    // GET /callback — Spotify redirect landing
    const httpd_uri_t uri_callback = {
        .uri = "/callback",
        .method = HTTP_GET,
        .handler = handle_callback,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &uri_callback);

    // POST /exchange — token exchange
    const httpd_uri_t uri_exchange = {
        .uri = "/exchange",
        .method = HTTP_POST,
        .handler = handle_exchange,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &uri_exchange);

    // OPTIONS /exchange — CORS preflight
    const httpd_uri_t uri_exchange_options = {
        .uri = "/exchange",
        .method = HTTP_OPTIONS,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
            httpd_resp_send(req, nullptr, 0);
            return ESP_OK;
        },
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &uri_exchange_options);

    // POST /save_client_id
    const httpd_uri_t uri_save_cid = {
        .uri = "/save_client_id",
        .method = HTTP_POST,
        .handler = handle_save_client_id,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &uri_save_cid);

    ESP_LOGI(TAG, "Spotify setup server ready at http://%s:8888/spotify",
             s_device_ip);

    // Block until auth completes
    xSemaphoreTake(s_done_sem, portMAX_DELAY);

    // Brief delay so the JSON response is fully sent
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Stop server
    httpd_stop(server);
    vSemaphoreDelete(s_done_sem);
    s_done_sem = nullptr;

    ESP_LOGI(TAG, "Spotify setup complete — server stopped");
}
