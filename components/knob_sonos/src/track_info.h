#pragma once

#include "sonos_config.h"

#include <cstddef>

PlayState parse_transport_state(const char *get_transport_info_response);

// TransportAction bits from a GetCurrentTransportActions response.
uint8_t parse_transport_actions(const char *response);

// Sonos only plays plain http(s) radio streams behind this scheme.
void radio_uri(const char *url, char *out, size_t out_len);

// Index of the station whose stream `track_uri` plays, or -1.
int match_station(const char *track_uri, const char *const *station_urls,
                  int station_count);

// Human name for whatever feeds the track: "TV", "Line-In", "AirPlay",
// "Spotify", "Apple Music", ...
void detect_source(const char *track_uri, char *out, size_t out_len);

// Parse a GetPositionInfo response. Relative album art paths are resolved
// against the speaker that answered (`host`:`port`).
void parse_media_info(const char *position_info, const char *host, int port,
                      MediaInfo *info);
