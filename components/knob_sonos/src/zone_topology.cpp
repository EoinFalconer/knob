#include "zone_topology.h"
#include "soap.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

static constexpr const char *TAG = "topology";

// Big households with stereo pairs and home theater sets easily pass 16 KB.
static constexpr int RESPONSE_SIZE = 48 * 1024;

static bool attr(const char *tag, const char *tag_end, const char *name,
                 char *out, size_t out_len) {
  char needle[32];
  snprintf(needle, sizeof(needle), " %s=\"", name);
  const char *start = strstr(tag, needle);
  if (!start || start >= tag_end)
    return false;
  start += strlen(needle);
  const char *end = strchr(start, '"');
  if (!end || end > tag_end)
    return false;
  size_t len = std::min(static_cast<size_t>(end - start), out_len - 1);
  memcpy(out, start, len);
  out[len] = '\0';
  return true;
}

static void copy(char *dst, size_t dst_len, const char *src) {
  strncpy(dst, src, dst_len - 1);
  dst[dst_len - 1] = '\0';
}

static void parse_member(const char *tag, const char *tag_end,
                         const char *coordinator, ZoneMember *m) {
  *m = {};
  copy(m->coordinator_uuid, sizeof(m->coordinator_uuid), coordinator);
  attr(tag, tag_end, "UUID", m->uuid, sizeof(m->uuid));
  attr(tag, tag_end, "ZoneName", m->name, sizeof(m->name));
  // Attribute values were escaped once more inside the escaped document.
  xml_unescape(m->name);

  char invisible[4] = {};
  attr(tag, tag_end, "Invisible", invisible, sizeof(invisible));
  m->invisible = strcmp(invisible, "1") == 0;

  char location[160] = {};
  attr(tag, tag_end, "Location", location, sizeof(location));
  url_host_port(location, m->ip, sizeof(m->ip), &m->port);
}

int zone_topology_parse(char *soap_response, ZoneTopology *out) {
  *out = {};
  xml_unescape(soap_response);

  // Members listed under VanishedDevices aren't reachable — stop before them.
  if (char *vanished = strstr(soap_response, "<VanishedDevices"))
    *vanished = '\0';

  const char *group = soap_response;
  while ((group = strstr(group, "<ZoneGroup ")) != nullptr) {
    const char *group_tag_end = strchr(group, '>');
    const char *group_end = strstr(group, "</ZoneGroup>");
    if (!group_tag_end || !group_end)
      break;

    char coordinator[40] = {};
    attr(group, group_tag_end, "Coordinator", coordinator, sizeof(coordinator));

    const char *member = group;
    while ((member = strstr(member + 1, "<ZoneGroupMember ")) != nullptr &&
           member < group_end && out->count < ZONE_MAX_MEMBERS) {
      // Bound attribute lookups to this tag: a home theater member nests
      // <Satellite Invisible="1" .../> children that must not leak in.
      const char *tag_end = strchr(member, '>');
      if (!tag_end)
        break;
      ZoneMember &m = out->members[out->count];
      parse_member(member, tag_end, coordinator, &m);
      if (m.uuid[0] && m.ip[0])
        out->count++;
    }
    group = group_end;
  }
  return out->count;
}

const ZoneMember *zone_topology_find_ip(const ZoneTopology *topo,
                                        const char *ip) {
  for (int i = 0; i < topo->count; i++) {
    if (strcmp(topo->members[i].ip, ip) == 0)
      return &topo->members[i];
  }
  return nullptr;
}

const ZoneMember *zone_topology_find_uuid(const ZoneTopology *topo,
                                          const char *uuid) {
  for (int i = 0; i < topo->count; i++) {
    if (strcmp(topo->members[i].uuid, uuid) == 0)
      return &topo->members[i];
  }
  return nullptr;
}

bool zone_topology_fetch(const char *ip, int port, ZoneTopology *out) {
  auto *buf =
      static_cast<char *>(heap_caps_malloc(RESPONSE_SIZE, MALLOC_CAP_SPIRAM));
  if (!buf)
    return false;

  SoapResponse resp = {buf, RESPONSE_SIZE, 0, false};
  bool ok = soap_call(ip, port, ZONE_GROUP_TOPOLOGY, "GetZoneGroupState",
                      SOAP_INSTANCE_ID, &resp);
  if (ok && resp.truncated)
    ESP_LOGW(TAG, "ZoneGroupState truncated at %d bytes", resp.len);
  ok = ok && zone_topology_parse(buf, out) > 0;
  heap_caps_free(buf);
  return ok;
}
