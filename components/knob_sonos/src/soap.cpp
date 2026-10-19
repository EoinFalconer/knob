#include "soap.h"
#include "sonos_config.h"

#include "esp_http_client.h"
#include "esp_log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static constexpr const char *TAG = "soap";

static constexpr const char *ENVELOPE_FMT =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
    " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body>"
    "</s:Envelope>";

// Once a chunk doesn't fit, later (smaller) chunks must not be appended or
// the XML gets spliced into something that parses but lies.
static esp_err_t on_http_event(esp_http_client_event_t *evt) {
  auto *resp = static_cast<SoapResponse *>(evt->user_data);
  if (evt->event_id != HTTP_EVENT_ON_DATA || !resp || evt->data_len <= 0 ||
      resp->truncated)
    return ESP_OK;
  int room = resp->capacity - 1 - resp->len;
  int n = std::min(room, evt->data_len);
  if (n < evt->data_len)
    resp->truncated = true;
  if (n > 0) {
    memcpy(resp->data + resp->len, evt->data, static_cast<size_t>(n));
    resp->len += n;
  }
  resp->data[resp->len] = '\0';
  return ESP_OK;
}

bool soap_call(const char *ip, int port, const SoapService &service,
               const char *action, const char *args, SoapResponse *resp) {
  char url[96];
  snprintf(url, sizeof(url), "http://%s:%d%s", ip, port, service.path);

  char soap_action[160];
  snprintf(soap_action, sizeof(soap_action), "\"%s#%s\"", service.ns, action);

  char envelope[1024];
  int envelope_len = snprintf(envelope, sizeof(envelope), ENVELOPE_FMT, action,
                              service.ns, args, action);
  if (envelope_len < 0 || envelope_len >= static_cast<int>(sizeof(envelope))) {
    ESP_LOGE(TAG, "%s: envelope overflow", action);
    return false;
  }

  char fault_buf[512];
  SoapResponse fault = {fault_buf, sizeof(fault_buf), 0, false};
  SoapResponse *sink = resp ? resp : &fault;
  sink->len = 0;
  sink->truncated = false;
  sink->data[0] = '\0';

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = SONOS_HTTP_TIMEOUT_MS;
  cfg.event_handler = on_http_event;
  cfg.user_data = sink;

  auto *client = esp_http_client_init(&cfg);
  if (!client)
    return false;

  esp_http_client_set_header(client, "Content-Type",
                             "text/xml; charset=\"utf-8\"");
  esp_http_client_set_header(client, "SOAPAction", soap_action);
  esp_http_client_set_post_field(client, envelope, envelope_len);

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "%s @ %s failed: %s", action, ip, esp_err_to_name(err));
    return false;
  }
  if (status != 200) {
    char code[8] = "?";
    xml_extract(sink->data, "errorCode", code, sizeof(code));
    ESP_LOGW(TAG, "%s @ %s: HTTP %d, UPnP error %s", action, ip, status, code);
    return false;
  }
  return true;
}

bool xml_extract(const char *xml, const char *tag, char *out, size_t out_len) {
  char open[64], close[64];
  snprintf(open, sizeof(open), "<%s>", tag);
  snprintf(close, sizeof(close), "</%s>", tag);

  const char *start = strstr(xml, open);
  if (!start)
    return false;
  start += strlen(open);

  const char *end = strstr(start, close);
  if (!end)
    return false;

  size_t len = std::min(static_cast<size_t>(end - start), out_len - 1);
  memcpy(out, start, len);
  out[len] = '\0';
  return true;
}

void xml_unescape(char *s) {
  static constexpr struct {
    const char *entity;
    char ch;
  } ENTITIES[] = {
      {"&lt;", '<'},   {"&gt;", '>'},    {"&amp;", '&'},
      {"&quot;", '"'}, {"&apos;", '\''},
  };

  char *r = s, *w = s;
  while (*r) {
    bool matched = false;
    if (*r == '&') {
      for (const auto &e : ENTITIES) {
        size_t n = strlen(e.entity);
        if (strncmp(r, e.entity, n) == 0) {
          *w++ = e.ch;
          r += n;
          matched = true;
          break;
        }
      }
    }
    if (!matched)
      *w++ = *r++;
  }
  *w = '\0';
}

void xml_escape(const char *in, char *out, size_t out_len) {
  size_t w = 0;
  for (; *in; in++) {
    const char *rep = nullptr;
    switch (*in) {
    case '&':
      rep = "&amp;";
      break;
    case '<':
      rep = "&lt;";
      break;
    case '>':
      rep = "&gt;";
      break;
    case '"':
      rep = "&quot;";
      break;
    default:
      break;
    }
    size_t n = rep ? strlen(rep) : 1;
    if (w + n >= out_len)
      break;
    if (rep)
      memcpy(out + w, rep, n);
    else
      out[w] = *in;
    w += n;
  }
  out[w] = '\0';
}

bool url_host_port(const char *url, char *host, size_t host_len,
                   uint16_t *port) {
  const char *start = strstr(url, "://");
  if (!start)
    return false;
  start += 3;

  size_t span = strcspn(start, ":/");
  size_t len = std::min(span, host_len - 1);
  memcpy(host, start, len);
  host[len] = '\0';

  *port = start[span] == ':' ? static_cast<uint16_t>(atoi(start + span + 1))
                             : static_cast<uint16_t>(SONOS_PORT);
  return host[0] != '\0';
}
