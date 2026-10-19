#include "track_info.h"
#include "soap.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

PlayState parse_transport_state(const char *response) {
  char val[32] = {};
  if (!xml_extract(response, "CurrentTransportState", val, sizeof(val)))
    return PlayState::Unknown;
  if (strcmp(val, "PLAYING") == 0)
    return PlayState::Playing;
  if (strcmp(val, "PAUSED_PLAYBACK") == 0)
    return PlayState::Paused;
  if (strcmp(val, "STOPPED") == 0)
    return PlayState::Stopped;
  if (strcmp(val, "TRANSITIONING") == 0)
    return PlayState::Transitioning;
  return PlayState::Unknown;
}

uint8_t parse_transport_actions(const char *response) {
  static constexpr struct {
    const char *name;
    uint8_t bit;
  } ACTIONS[] = {
      {"Play", ACTION_PLAY},         {"Pause", ACTION_PAUSE},
      {"Stop", ACTION_STOP},         {"Next", ACTION_NEXT},
      {"Previous", ACTION_PREVIOUS},
  };

  char list[256] = {};
  if (!xml_extract(response, "Actions", list, sizeof(list)))
    return 0;

  uint8_t bits = 0;
  for (const char *tok = list; *tok;) {
    tok += strspn(tok, ", ");
    size_t len = strcspn(tok, ", ");
    for (const auto &a : ACTIONS) {
      if (len == strlen(a.name) && strncmp(tok, a.name, len) == 0)
        bits |= a.bit;
    }
    tok += len;
  }
  return bits;
}

// Once a stream has played, Sonos reports it with the original scheme nested
// inside its own ("x-rincon-mp3radio://http://host/path"), so peel them all.
static const char *strip_scheme(const char *url) {
  while (const char *sep = strstr(url, "://")) {
    if (memchr(url, '/', static_cast<size_t>(sep - url)))
      break;
    url = sep + 3;
  }
  return url;
}

void radio_uri(const char *url, char *out, size_t out_len) {
  if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0)
    snprintf(out, out_len, "x-rincon-mp3radio://%s", strip_scheme(url));
  else
    snprintf(out, out_len, "%s", url);
}

int match_station(const char *track_uri, const char *const *station_urls,
                  int station_count) {
  if (!strstr(track_uri, "://"))
    return -1;
  const char *playing = strip_scheme(track_uri);
  for (int i = 0; i < station_count; i++) {
    if (strcmp(playing, strip_scheme(station_urls[i])) == 0)
      return i;
  }
  return -1;
}

static bool starts_with(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

// Music service IDs Sonos tags queue tracks with (`...?sid=204&flags=...`).
static const char *service_name(int sid) {
  switch (sid) {
  case 2:
    return "Deezer";
  case 9:
  case 12:
    return "Spotify";
  case 31:
    return "Qobuz";
  case 160:
    return "SoundCloud";
  case 174:
    return "TIDAL";
  case 201:
    return "Amazon Music";
  case 204:
    return "Apple Music";
  case 212:
    return "Plex";
  case 239:
    return "Audible";
  case 254:
  case 333:
    return "TuneIn";
  case 284:
    return "YouTube Music";
  case 303:
    return "Sonos Radio";
  default:
    return nullptr;
  }
}

static const char *service_from_sid(const char *uri) {
  for (const char *p = strstr(uri, "sid="); p; p = strstr(p + 1, "sid=")) {
    if (p > uri && (p[-1] == '?' || p[-1] == '&'))
      return service_name(atoi(p + 4));
  }
  return nullptr;
}

// Spotify Connect, AirPlay and friends stream into a "virtual line-in":
// x-sonos-vli:RINCON_xxx:2,spotify:<session>
static void vli_source(const char *uri, char *out, size_t out_len) {
  const char *comma = strchr(uri, ',');
  if (!comma) {
    snprintf(out, out_len, "Media");
    return;
  }
  const char *kind = comma + 1;
  size_t len = std::min(strcspn(kind, ":"), out_len - 1);
  if (len == 7 && strncmp(kind, "airplay", 7) == 0) {
    snprintf(out, out_len, "AirPlay");
    return;
  }
  memcpy(out, kind, len);
  out[len] = '\0';
  out[0] = static_cast<char>(toupper(static_cast<unsigned char>(out[0])));
}

void detect_source(const char *uri, char *out, size_t out_len) {
  if (!uri[0]) {
    out[0] = '\0';
    return;
  }
  if (starts_with(uri, "x-sonos-vli:")) {
    vli_source(uri, out, out_len);
    return;
  }

  const char *source = "Media";
  if (starts_with(uri, "x-sonos-htastream:"))
    source = "TV";
  else if (starts_with(uri, "x-rincon-stream:"))
    source = "Line-In";
  else if (const char *service = service_from_sid(uri))
    source = service;
  else if (starts_with(uri, "x-sonos-spotify:"))
    source = "Spotify";
  else if (starts_with(uri, "x-rincon-mp3radio:") ||
           starts_with(uri, "x-sonosapi-stream:") ||
           starts_with(uri, "x-sonosapi-radio:") ||
           starts_with(uri, "x-sonosapi-hls:") || starts_with(uri, "aac:") ||
           starts_with(uri, "hls-radio:"))
    source = "Radio";
  else if (starts_with(uri, "x-file-cifs:"))
    source = "Music Library";
  snprintf(out, out_len, "%s", source);
}

static void copy_field(const char *start, size_t len, char *out,
                       size_t out_len) {
  len = std::min(len, out_len - 1);
  memcpy(out, start, len);
  out[len] = '\0';
}

// Radio streams put "TYPE=SNG|TITLE x|ARTIST y" or "Artist - Title" here.
static void parse_stream_content(const char *stream, MediaInfo *info) {
  if (const char *artist = strstr(stream, "ARTIST ")) {
    artist += 7;
    copy_field(artist, strcspn(artist, "|"), info->artist,
               sizeof(info->artist));
    if (const char *title = strstr(stream, "TITLE ");
        title && !info->title[0]) {
      title += 6;
      copy_field(title, strcspn(title, "|"), info->title, sizeof(info->title));
    }
    return;
  }
  if (const char *sep = strstr(stream, " - ")) {
    copy_field(stream, static_cast<size_t>(sep - stream), info->artist,
               sizeof(info->artist));
    if (!info->title[0])
      snprintf(info->title, sizeof(info->title), "%s", sep + 3);
  }
}

static void extract_text(const char *didl, const char *tag, char *out,
                         size_t out_len) {
  if (xml_extract(didl, tag, out, out_len))
    xml_unescape(out);
}

static void parse_didl(const char *position_info, const char *host, int port,
                       MediaInfo *info) {
  // DIDL-Lite arrives escaped inside the SOAP body; its own text nodes are
  // escaped once more, so every value needs a second unescape.
  static char didl[4096];
  if (!xml_extract(position_info, "TrackMetaData", didl, sizeof(didl)) ||
      strcmp(didl, "NOT_IMPLEMENTED") == 0)
    return;
  xml_unescape(didl);

  extract_text(didl, "dc:title", info->title, sizeof(info->title));
  if (strstr(info->title, "://"))
    info->title[0] = '\0';
  extract_text(didl, "dc:creator", info->artist, sizeof(info->artist));

  if (!info->artist[0]) {
    char stream[256] = {};
    extract_text(didl, "r:streamContent", stream, sizeof(stream));
    parse_stream_content(stream, info);
  }

  char art[256] = {};
  extract_text(didl, "upnp:albumArtURI", art, sizeof(art));
  if (art[0] == '/')
    snprintf(info->art_url, sizeof(info->art_url), "http://%s:%d%s", host, port,
             art);
  else
    snprintf(info->art_url, sizeof(info->art_url), "%s", art);
}

void parse_media_info(const char *position_info, const char *host, int port,
                      MediaInfo *info) {
  *info = {};

  char track_uri[300] = {};
  xml_extract(position_info, "TrackURI", track_uri, sizeof(track_uri));
  xml_unescape(track_uri);
  detect_source(track_uri, info->source, sizeof(info->source));

  parse_didl(position_info, host, port, info);

  // TV and line-in rarely carry a title; the source is the best name we have.
  if (!info->title[0] && !info->artist[0])
    snprintf(info->title, sizeof(info->title), "%s", info->source);
  info->has_media = info->title[0] != '\0';
}
