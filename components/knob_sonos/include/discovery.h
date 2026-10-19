#pragma once

#include <cstddef>
#include <cstdint>

constexpr int DISCOVERY_MAX_SPEAKERS = 16;
constexpr int DISCOVERY_TIMEOUT_MS = 3000;

struct SonosSpeaker {
  char ip[40];
  char name[64];
  char uuid[40];             // RINCON_xxx; empty if only found via SSDP
  char coordinator_uuid[40]; // coordinator of its group (itself if alone)
  uint16_t port;
  bool grouped; // true if this speaker is a non-coordinator member of a group
};

struct DiscoveryResult {
  SonosSpeaker speakers[DISCOVERY_MAX_SPEAKERS];
  int count;
};

void discovery_init();
int discovery_scan(DiscoveryResult *out, int timeout_ms = DISCOVERY_TIMEOUT_MS);

// Fetch speaker room name from device description XML at ip:port
bool discovery_get_speaker_name(const char *ip, int port, char *name,
                                size_t name_len);

// Find a speaker by room name in discovery results. Returns nullptr if not
// found.
const SonosSpeaker *discovery_find_by_name(const DiscoveryResult *result,
                                           const char *name);
