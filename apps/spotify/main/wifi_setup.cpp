#include "wifi_setup.h"
#include "settings.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"

#include "lwip/sockets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <cstring>
#include <cstdio>

static constexpr const char *TAG = "wifi_setup";

// ─── Event group for WiFi connection verification
static EventGroupHandle_t s_wifi_event_group = nullptr;
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;
static esp_event_handler_instance_t s_wifi_handler_instance = nullptr;
static esp_event_handler_instance_t s_ip_handler_instance = nullptr;

static void wifi_verify_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ─── HTML page served at GET /

static const char SETUP_HTML[] = R"rawhtml(
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
.wrap{max-width:380px;width:100%;text-align:center}
.knob{position:relative;width:160px;height:160px;margin:0 auto 24px}
.knob .ring{position:absolute;inset:0;border-radius:50%;
background:conic-gradient(from 0deg,#2266cc,#4499ff,#2266cc,#4499ff,
#2266cc,#4499ff,#2266cc,#4499ff,#2266cc,#4499ff,#2266cc,#4499ff,#2266cc);
box-shadow:0 0 30px rgba(68,136,255,0.2)}
.knob .bezel{position:absolute;inset:12px;border-radius:50%;
background:#000;box-shadow:inset 0 2px 6px rgba(0,0,0,0.8)}
.knob .screen{position:absolute;inset:18px;border-radius:50%;
background:linear-gradient(180deg,#0a0a1a,#1a1a3a);
display:flex;align-items:center;justify-content:center;flex-direction:column;gap:4px}
.knob .screen svg{width:36px;height:36px}
.knob .screen span{color:#888;font-size:8px;letter-spacing:2px}
h1{font-size:1.3em;margin-bottom:4px}
.sub{color:#b3b3b3;font-size:.85em;margin-bottom:24px}
.card{background:#1a1a1a;border-radius:16px;padding:24px;text-align:left;
box-shadow:0 8px 32px rgba(0,0,0,.4)}
label{display:block;color:#b3b3b3;font-size:.85em;margin-bottom:6px;margin-top:16px}
select,input[type=password],input[type=text]{width:100%;padding:12px;
border:1px solid #333;border-radius:8px;background:#2a2a2a;color:#fff;
font-size:1em;outline:none}
select:focus,input:focus{border-color:#1DB954}
.pass-wrap{position:relative}
.pass-wrap input{padding-right:48px}
.reveal{position:absolute;right:8px;top:50%;transform:translateY(-50%);
background:none;border:none;color:#b3b3b3;font-size:.8em;cursor:pointer;
padding:4px 8px;width:auto;margin:0}
.reveal:hover{color:#fff}
button.connect{width:100%;padding:14px;border:none;border-radius:24px;
background:#1DB954;color:#fff;font-size:1em;font-weight:600;
cursor:pointer;margin-top:24px;transition:background .2s}
button.connect:hover{background:#1ed760}
button.connect:disabled{background:#555;cursor:not-allowed}
.status{text-align:center;margin-top:16px;color:#b3b3b3;font-size:.9em;min-height:1.2em}
.status.error{color:#e74c3c}
.status.ok{color:#1DB954}
.spinner{display:inline-block;width:16px;height:16px;border:2px solid #555;
border-top-color:#1DB954;border-radius:50%;animation:spin .6s linear infinite;
vertical-align:middle;margin-right:6px}
@keyframes spin{to{transform:rotate(360deg)}}
</style>
</head>
<body>
<div class="wrap">
<div class="knob">
 <div class="ring"></div>
 <div class="bezel"></div>
 <div class="screen">
  <svg viewBox="0 0 24 24" fill="#1DB954">
   <path d="M12 0C5.4 0 0 5.4 0 12s5.4 12 12 12 12-5.4 12-12S18.66 0 12
   0zm5.521 17.34c-.24.359-.66.48-1.021.24-2.82-1.74-6.36-2.101-10.561-1.141
   -.418.122-.779-.179-.899-.539-.12-.421.18-.78.54-.9 4.56-1.021 8.52-.6
   11.64 1.32.42.18.479.659.301 1.02zm1.44-3.3c-.301.42-.841.6-1.262.3
   -3.239-1.98-8.159-2.58-11.939-1.38-.479.12-1.02-.12-1.14-.6-.12-.48.12
   -1.021.6-1.141C9.6 9.9 15 10.561 18.72 12.84c.361.181.54.78.241
   1.2zm.12-3.36C15.24 8.4 8.82 8.16 5.16 9.301c-.6.179-1.2-.181-1.38-.721
   -.18-.601.18-1.2.72-1.381 4.26-1.26 11.28-1.02 15.721 1.621.539.3.719
   1.02.419 1.56-.299.421-1.02.599-1.559.3z"/>
  </svg>
  <span>SETUP</span>
 </div>
</div>
<h1>Your knob needs WiFi</h1>
<p class="sub">Give it a connection.</p>
<div class="card">
<label for="ssid">WiFi Network</label>
<select id="ssid"><option value="">Scanning...</option></select>
<label for="pass">Password</label>
<div class="pass-wrap">
<input type="password" id="pass" placeholder="Enter password">
<button class="reveal" onclick="togglePass()">show</button>
</div>
<button class="connect" id="btn" onclick="doConnect()">Connect</button>
<div class="status" id="status"></div>
</div>
</div>
<script>
function togglePass(){
 var p=document.getElementById('pass');
 var b=document.querySelector('.reveal');
 if(p.type==='password'){p.type='text';b.textContent='hide'}
 else{p.type='password';b.textContent='show'}
}
function scan(){
 fetch('/scan').then(r=>r.json()).then(nets=>{
  var s=document.getElementById('ssid');
  s.innerHTML='';
  if(!nets.length){s.innerHTML='<option value="">No networks found</option>';return}
  nets.forEach(n=>{
   var o=document.createElement('option');
   o.value=n.ssid;
   o.textContent=n.ssid+' ('+n.rssi+' dBm'+(n.auth>0?' \u{1F512}':'')+')';
   s.appendChild(o);
  });
 }).catch(()=>{
  document.getElementById('status').textContent='Scan failed \u2014 retrying...';
  document.getElementById('status').className='status error';
  setTimeout(scan,3000);
 });
}
function doConnect(){
 var ssid=document.getElementById('ssid').value;
 var pass=document.getElementById('pass').value;
 if(!ssid){
  document.getElementById('status').textContent='Please select a network';
  document.getElementById('status').className='status error';
  return;
 }
 var btn=document.getElementById('btn');
 btn.disabled=true;
 btn.textContent='Connecting...';
 document.getElementById('status').innerHTML='<span class="spinner"></span>Saving...';
 document.getElementById('status').className='status';
 fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
  body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)
 }).then(r=>r.json()).then(d=>{
  if(d.ok){
   document.getElementById('status').textContent='Your knob is connected! Restarting...';
   document.getElementById('status').className='status ok';
  }else{
   document.getElementById('status').textContent='Error: '+(d.error||'unknown');
   document.getElementById('status').className='status error';
   btn.disabled=false;btn.textContent='Connect';
  }
 }).catch(()=>{
  document.getElementById('status').textContent='Connection error';
  document.getElementById('status').className='status error';
  btn.disabled=false;btn.textContent='Connect';
 });
}
scan();
</script>
</body>
</html>
)rawhtml";

// ─── DNS server task — responds to ALL queries with 192.168.4.1

static void dns_server_task(void *) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(nullptr);
        return;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "DNS server started on :53");

    uint8_t buf[512];
    while (true) {
        struct sockaddr_in client = {};
        socklen_t client_len = sizeof(client);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &client_len);
        if (len < 12) continue;

        // Build DNS response: copy the query, set response flags, append answer
        uint8_t resp[512];
        if (len > (int)(sizeof(resp) - 16)) continue; // safety

        memcpy(resp, buf, len);

        // Set QR=1 (response), AA=1, RA=1 — flags at bytes 2-3
        resp[2] = 0x84; // QR=1, AA=1
        resp[3] = 0x00; // no error

        // Set answer count = 1 (bytes 6-7)
        resp[6] = 0x00;
        resp[7] = 0x01;

        // Append answer after the query
        int pos = len;
        // Name pointer to the query name (offset 12)
        resp[pos++] = 0xC0;
        resp[pos++] = 0x0C;
        // Type A
        resp[pos++] = 0x00;
        resp[pos++] = 0x01;
        // Class IN
        resp[pos++] = 0x00;
        resp[pos++] = 0x01;
        // TTL = 60s
        resp[pos++] = 0x00;
        resp[pos++] = 0x00;
        resp[pos++] = 0x00;
        resp[pos++] = 0x3C;
        // RDLENGTH = 4
        resp[pos++] = 0x00;
        resp[pos++] = 0x04;
        // RDATA = 192.168.4.1
        resp[pos++] = 192;
        resp[pos++] = 168;
        resp[pos++] = 4;
        resp[pos++] = 1;

        sendto(sock, resp, pos, 0,
               (struct sockaddr *)&client, client_len);
    }
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

// ─── HTTP handlers

static esp_err_t handle_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SETUP_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handle_scan(httpd_req_t *req) {
    // Trigger a WiFi scan in AP+STA mode
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    scan_cfg.channel = 0; // all channels
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 120;
    scan_cfg.scan_time.active.max = 500;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20;

    auto *ap_list = static_cast<wifi_ap_record_t *>(
        calloc(ap_count, sizeof(wifi_ap_record_t)));
    if (!ap_list) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    // Build JSON array
    char json[2048];
    int pos = 0;
    json[pos++] = '[';

    // Track seen SSIDs to avoid duplicates
    for (int i = 0; i < ap_count && pos < (int)sizeof(json) - 128; i++) {
        if (ap_list[i].ssid[0] == '\0') continue;

        // Skip duplicates
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((char *)ap_list[i].ssid, (char *)ap_list[j].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        if (pos > 1) json[pos++] = ',';
        pos += snprintf(json + pos, sizeof(json) - pos,
                        "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                        (char *)ap_list[i].ssid,
                        ap_list[i].rssi,
                        (int)ap_list[i].authmode);
    }
    json[pos++] = ']';
    json[pos] = '\0';

    free(ap_list);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

static esp_err_t handle_connect(httpd_req_t *req) {
    char body[256] = {};
    int recv_len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    body[recv_len] = '\0';

    // Parse "ssid=...&pass=..."
    char ssid_raw[65] = {};
    char pass_raw[65] = {};
    char ssid[65] = {};
    char pass[65] = {};

    char *ssid_ptr = strstr(body, "ssid=");
    char *pass_ptr = strstr(body, "pass=");

    if (ssid_ptr) {
        ssid_ptr += 5;
        char *end = strchr(ssid_ptr, '&');
        size_t len = end ? (size_t)(end - ssid_ptr) : strlen(ssid_ptr);
        if (len >= sizeof(ssid_raw)) len = sizeof(ssid_raw) - 1;
        memcpy(ssid_raw, ssid_ptr, len);
        ssid_raw[len] = '\0';
    }

    if (pass_ptr) {
        pass_ptr += 5;
        char *end = strchr(pass_ptr, '&');
        size_t len = end ? (size_t)(end - pass_ptr) : strlen(pass_ptr);
        if (len >= sizeof(pass_raw)) len = sizeof(pass_raw) - 1;
        memcpy(pass_raw, pass_ptr, len);
        pass_raw[len] = '\0';
    }

    url_decode(ssid, ssid_raw, sizeof(ssid));
    url_decode(pass, pass_raw, sizeof(pass));

    if (ssid[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"No SSID\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Saving WiFi: SSID=%s", ssid);
    settings_wifi_save(ssid, pass);

    // ─── Verify credentials before responding ───
    // Stop current AP/STA, switch to STA, and try to connect.
    ESP_LOGI(TAG, "Verifying WiFi credentials...");

    s_wifi_event_group = xEventGroupCreate();

    // Register temporary event handlers for verification
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                        &wifi_verify_event_handler, nullptr,
                                        &s_wifi_handler_instance);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_verify_event_handler, nullptr,
                                        &s_ip_handler_instance);

    esp_wifi_stop();

    // Create STA netif if needed (AP netif already exists)
    static bool sta_netif_created = false;
    if (!sta_netif_created) {
        esp_netif_create_default_wifi_sta();
        sta_netif_created = true;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t sta_cfg = {};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    // Wait up to 10 seconds for connection or failure
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(10000));

    // Unregister verification handlers
    esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                          s_wifi_handler_instance);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                          s_ip_handler_instance);
    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = nullptr;

    if (bits & WIFI_CONNECTED_BIT) {
        // Connection succeeded — respond OK and restart
        ESP_LOGI(TAG, "WiFi credentials verified successfully");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":true}");

        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        return ESP_OK; // unreachable
    }

    // Connection failed — switch back to AP mode so user can retry
    ESP_LOGW(TAG, "WiFi connection failed — reverting to AP mode");
    esp_wifi_stop();

    wifi_config_t ap_cfg = {};
    strcpy((char *)ap_cfg.ap.ssid, "knob");
    ap_cfg.ap.ssid_len = strlen("knob");
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req,
        "{\"ok\":false,\"error\":\"Could not connect to WiFi. Check your password.\"}");
    return ESP_OK;
}

// ─── Captive portal redirect for common probe URLs

static esp_err_t handle_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

// ─── Public API

bool wifi_setup_has_credentials() {
    // Check NVS directly — settings_get_wifi_ssid falls back to Kconfig
    // defaults, which always has a compiled-in SSID. We only want to
    // return true if the user has explicitly saved credentials.
    nvs_handle_t h;
    if (nvs_open("radio", NVS_READONLY, &h) != ESP_OK) return false;
    char ssid[33] = {};
    size_t len = sizeof(ssid);
    esp_err_t err = nvs_get_str(h, "wifi_ssid", ssid, &len);
    nvs_close(h);
    return err == ESP_OK && ssid[0] != '\0';
}

void wifi_setup_start() {
    ESP_LOGI(TAG, "Starting captive portal AP...");

    // Stop existing WiFi if running (e.g. called after disconnect retries)
    // These may fail if WiFi was never started — that's OK.
    esp_wifi_stop();
    esp_wifi_deinit();

    // Ensure netif is initialized — only create AP netif if it doesn't exist yet
    // (wifi_picker may have already created it in APSTA mode)
    esp_netif_init();
    if (!esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) {
        esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    wifi_config_t ap_cfg = {};
    strcpy((char *)ap_cfg.ap.ssid, "knob");
    ap_cfg.ap.ssid_len = strlen("knob");
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started: SSID=knob, IP=192.168.4.1");

    // Start DNS server task
    xTaskCreate(dns_server_task, "dns_srv", 4096, nullptr, 5, nullptr);

    // Start HTTP server
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 8;
    http_cfg.max_open_sockets = 3;
    http_cfg.max_resp_headers = 16;
    http_cfg.stack_size = 8192;
    http_cfg.recv_wait_timeout = 10;
    http_cfg.send_wait_timeout = 10;

    httpd_handle_t server = nullptr;
    ESP_ERROR_CHECK(httpd_start(&server, &http_cfg));

    // Main page
    const httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handle_root,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &uri_root);

    // WiFi scan
    const httpd_uri_t uri_scan = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = handle_scan,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &uri_scan);

    // Connect / save credentials
    const httpd_uri_t uri_connect = {
        .uri = "/connect",
        .method = HTTP_POST,
        .handler = handle_connect,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &uri_connect);

    // Common captive portal probe URLs — redirect to root
    const char *redirect_uris[] = {
        "/generate_204",         // Android
        "/gen_204",              // Android
        "/hotspot-detect.html",  // Apple
        "/canonical.html",       // Firefox
        "/connecttest.txt",      // Windows
        "/redirect",             // Windows
    };

    for (const auto &ruri : redirect_uris) {
        httpd_uri_t rd = {
            .uri = ruri,
            .method = HTTP_GET,
            .handler = handle_redirect,
            .user_ctx = nullptr,
        };
        httpd_register_uri_handler(server, &rd);
    }

    ESP_LOGI(TAG, "Captive portal ready — connect to knob WiFi");

    // Block forever — device will restart after credentials are submitted
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
