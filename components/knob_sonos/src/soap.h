#pragma once

#include <cstddef>
#include <cstdint>

struct SoapService {
  const char *path;
  const char *ns;
};

inline constexpr SoapService AV_TRANSPORT = {
    "/MediaRenderer/AVTransport/Control",
    "urn:schemas-upnp-org:service:AVTransport:1"};
inline constexpr SoapService GROUP_RENDERING_CONTROL = {
    "/MediaRenderer/GroupRenderingControl/Control",
    "urn:schemas-upnp-org:service:GroupRenderingControl:1"};
inline constexpr SoapService ZONE_GROUP_TOPOLOGY = {
    "/ZoneGroupTopology/Control",
    "urn:schemas-upnp-org:service:ZoneGroupTopology:1"};

#define SOAP_INSTANCE_ID "<InstanceID>0</InstanceID>"

struct SoapResponse {
  char *data;
  int capacity;
  int len;
  bool truncated;
};

// POST a SOAP action to ip:port. `args` is the raw argument XML. Returns
// false on transport errors and on any non-200 reply (UPnP faults).
bool soap_call(const char *ip, int port, const SoapService &service,
               const char *action, const char *args,
               SoapResponse *resp = nullptr);

bool xml_extract(const char *xml, const char *tag, char *out, size_t out_len);

// Decodes &lt; &gt; &amp; &quot; &apos; in place, one layer deep.
void xml_unescape(char *s);

void xml_escape(const char *in, char *out, size_t out_len);

// "http://192.168.1.20:1400/xml/x.xml" → "192.168.1.20", 1400.
bool url_host_port(const char *url, char *host, size_t host_len,
                   uint16_t *port);
