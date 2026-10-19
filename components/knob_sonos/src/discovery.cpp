#include "discovery.h"
#include "soap.h"
#include "sonos_config.h"
#include "zone_topology.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lwip/sockets.h"

#include <cstdio>
#include <cstring>

static constexpr const char *TAG = "discovery";

static constexpr const char *SSDP_ADDR = "239.255.255.250";
static constexpr uint16_t SSDP_PORT = 1900;

static constexpr const char *MSEARCH =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 2\r\n"
    "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n"
    "\r\n";

static bool parse_location(const char *response, char *out, size_t out_len) {
  const char *loc = strcasestr(response, "LOCATION:");
  if (!loc)
    return false;
  loc += 9;
  while (*loc == ' ')
    loc++;
  size_t len = strcspn(loc, "\r\n");
  snprintf(out, out_len, "%.*s", static_cast<int>(len), loc);
  return true;
}

// ─── Device description (fallback when the topology query fails) ───────────

struct HttpBuf {
  char *data;
  int len;
  int capacity;
};

static esp_err_t on_http_data(esp_http_client_event_t *evt) {
  auto *buf = static_cast<HttpBuf *>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && buf && evt->data_len > 0 &&
      buf->len + evt->data_len < buf->capacity) {
    memcpy(buf->data + buf->len, evt->data, evt->data_len);
    buf->len += evt->data_len;
    buf->data[buf->len] = '\0';
  }
  return ESP_OK;
}

static bool fetch_speaker_name(const char *ip, int port, char *name,
                               size_t name_len) {
  char url[128];
  snprintf(url, sizeof(url), "http://%s:%d/xml/device_description.xml", ip,
           port);

  static char resp_buf[2048];
  resp_buf[0] = '\0';
  HttpBuf buf = {resp_buf, 0, static_cast<int>(sizeof(resp_buf))};

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.timeout_ms = 2000;
  cfg.event_handler = on_http_data;
  cfg.user_data = &buf;

  auto *client = esp_http_client_init(&cfg);
  if (!client)
    return false;
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK || status != 200)
    return false;
  return xml_extract(resp_buf, "roomName", name, name_len) ||
         xml_extract(resp_buf, "friendlyName", name, name_len);
}

// ─── SSDP ───────────────────────────────────────────────────────────────────

// One responder is enough: any speaker can describe the whole household.
static bool ssdp_find_any(int timeout_ms, char *ip, size_t ip_len,
                          uint16_t *port) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "socket failed: errno %d", errno);
    return false;
  }

  struct timeval tv = {};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  struct sockaddr_in dest = {};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(SSDP_PORT);
  inet_aton(SSDP_ADDR, &dest.sin_addr);
  if (sendto(sock, MSEARCH, strlen(MSEARCH), 0,
             reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest)) < 0) {
    ESP_LOGE(TAG, "sendto failed: errno %d", errno);
    close(sock);
    return false;
  }

  bool found = false;
  char recv_buf[1024];
  int64_t deadline =
      esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
  while (!found && esp_timer_get_time() < deadline) {
    int len =
        recvfrom(sock, recv_buf, sizeof(recv_buf) - 1, 0, nullptr, nullptr);
    if (len <= 0)
      break;
    recv_buf[len] = '\0';
    char location[256];
    found = parse_location(recv_buf, location, sizeof(location)) &&
            url_host_port(location, ip, ip_len, port);
  }
  close(sock);
  return found;
}

static void add_visible_members(const ZoneTopology *topo,
                                DiscoveryResult *out) {
  for (int i = 0; i < topo->count && out->count < DISCOVERY_MAX_SPEAKERS; i++) {
    const ZoneMember &m = topo->members[i];
    if (m.invisible)
      continue;
    auto &s = out->speakers[out->count++];
    snprintf(s.ip, sizeof(s.ip), "%s", m.ip);
    snprintf(s.uuid, sizeof(s.uuid), "%s", m.uuid);
    snprintf(s.coordinator_uuid, sizeof(s.coordinator_uuid), "%s",
             m.coordinator_uuid);
    if (m.name[0])
      snprintf(s.name, sizeof(s.name), "%s", m.name);
    else
      snprintf(s.name, sizeof(s.name), "Sonos (%s)", m.ip);
    s.port = m.port;
    s.grouped = strcmp(m.uuid, m.coordinator_uuid) != 0;
    ESP_LOGI(TAG, "%s at %s:%d%s", s.name, s.ip, s.port,
             s.grouped ? " (grouped)" : "");
  }
}

// ─── Public API ─────────────────────────────────────────────────────────────

void discovery_init() { ESP_LOGI(TAG, "SSDP discovery ready"); }

int discovery_scan(DiscoveryResult *out, int timeout_ms) {
  *out = {};

  char ip[40] = {};
  uint16_t port = SONOS_PORT;
  ESP_LOGI(TAG, "Scanning for Sonos speakers...");
  if (!ssdp_find_any(timeout_ms, ip, sizeof(ip), &port)) {
    ESP_LOGW(TAG, "No speakers found via SSDP");
    return 0;
  }

  auto *topo = static_cast<ZoneTopology *>(
      heap_caps_malloc(sizeof(ZoneTopology), MALLOC_CAP_SPIRAM));
  if (topo && zone_topology_fetch(ip, port, topo))
    add_visible_members(topo, out);
  heap_caps_free(topo);
  if (out->count > 0)
    return out->count;

  ESP_LOGW(TAG, "Zone group query failed, using the SSDP responder only");
  auto &s = out->speakers[0];
  snprintf(s.ip, sizeof(s.ip), "%s", ip);
  s.port = port;
  if (!fetch_speaker_name(ip, port, s.name, sizeof(s.name)))
    snprintf(s.name, sizeof(s.name), "Sonos (%s)", ip);
  out->count = 1;
  return out->count;
}

bool discovery_get_speaker_name(const char *ip, int port, char *name,
                                size_t name_len) {
  return fetch_speaker_name(ip, port, name, name_len);
}

const SonosSpeaker *discovery_find_by_name(const DiscoveryResult *result,
                                           const char *name) {
  if (!result || !name || !name[0])
    return nullptr;
  for (int i = 0; i < result->count; i++) {
    if (strcmp(result->speakers[i].name, name) == 0)
      return &result->speakers[i];
  }
  return nullptr;
}
