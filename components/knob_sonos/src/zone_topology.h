#pragma once

#include <cstddef>
#include <cstdint>

constexpr int ZONE_MAX_MEMBERS = 32;

struct ZoneMember {
  char uuid[40];
  char coordinator_uuid[40];
  char name[64];
  char ip[40];
  uint16_t port;
  // Right half of a stereo pair: never addressed directly.
  bool invisible;
};

struct ZoneTopology {
  ZoneMember members[ZONE_MAX_MEMBERS];
  int count;
};

// Parse a GetZoneGroupState SOAP response. Decodes `soap_response` in place.
// Satellites (surrounds, Sub) aren't zone members and are skipped.
int zone_topology_parse(char *soap_response, ZoneTopology *out);

const ZoneMember *zone_topology_find_ip(const ZoneTopology *topo,
                                        const char *ip);
const ZoneMember *zone_topology_find_uuid(const ZoneTopology *topo,
                                          const char *uuid);

// Query `ip` for the household topology. Allocates its own response buffer.
bool zone_topology_fetch(const char *ip, int port, ZoneTopology *out);
