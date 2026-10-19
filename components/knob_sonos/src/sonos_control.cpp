#include "sonos_control.h"
#include "soap.h"
#include "track_info.h"
#include "zone_topology.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

static constexpr const char *TAG = "sonos";

struct Endpoint {
  char ip[40];
  int port;
  char uuid[40];
};

static Endpoint s_room;  // what the user picked
static Endpoint s_coord; // who runs the room's group right now
static SonosGroup s_group;
static ZoneTopology s_topology;

static const char *const *s_station_urls;
static int s_station_count;

static void copy(char *dst, size_t dst_len, const char *src) {
  snprintf(dst, dst_len, "%s", src);
}

static void set_endpoint(Endpoint *e, const char *ip, int port,
                         const char *uuid) {
  copy(e->ip, sizeof(e->ip), ip);
  e->port = port;
  copy(e->uuid, sizeof(e->uuid), uuid);
}

static bool same_endpoint(const Endpoint &a, const Endpoint &b) {
  return a.port == b.port && strcmp(a.ip, b.ip) == 0;
}

static bool call_at(const Endpoint &e, const SoapService &service,
                    const char *action, const char *args,
                    SoapResponse *resp = nullptr) {
  if (!e.ip[0])
    return false;
  return soap_call(e.ip, e.port, service, action, args, resp);
}

static bool coord_call(const SoapService &service, const char *action,
                       const char *args, SoapResponse *resp = nullptr) {
  return call_at(s_coord, service, action, args, resp);
}

void control_set_stations(const char *const *urls, int count) {
  s_station_urls = urls;
  s_station_count = count;
}

void control_set_room(const char *ip, int port) {
  set_endpoint(&s_room, ip, port, "");
  // Until the topology says otherwise, assume the room runs itself.
  s_coord = s_room;
  s_group = {};
}

static void add_group_member(const ZoneMember &m) {
  if (s_group.count >= SONOS_GROUP_MAX)
    return;
  copy(s_group.member_uuids[s_group.count], sizeof(s_group.member_uuids[0]),
       m.uuid);
  copy(s_group.member_names[s_group.count], sizeof(s_group.member_names[0]),
       m.name);
  s_group.count++;
}

static void build_group(const ZoneMember &room) {
  s_group = {};
  copy(s_group.room_uuid, sizeof(s_group.room_uuid), room.uuid);
  copy(s_group.coordinator_uuid, sizeof(s_group.coordinator_uuid),
       room.coordinator_uuid);
  add_group_member(room);
  for (int i = 0; i < s_topology.count; i++) {
    const ZoneMember &m = s_topology.members[i];
    if (!m.invisible && &m != &room &&
        strcmp(m.coordinator_uuid, room.coordinator_uuid) == 0)
      add_group_member(m);
  }
}

bool control_refresh_topology() {
  if (!s_room.ip[0] ||
      !zone_topology_fetch(s_room.ip, s_room.port, &s_topology))
    return false;

  const ZoneMember *room = zone_topology_find_ip(&s_topology, s_room.ip);
  if (!room) {
    ESP_LOGW(TAG, "%s isn't in the household topology", s_room.ip);
    return false;
  }
  copy(s_room.uuid, sizeof(s_room.uuid), room->uuid);

  const ZoneMember *coord =
      zone_topology_find_uuid(&s_topology, room->coordinator_uuid);
  if (!coord)
    coord = room;

  if (strcmp(coord->ip, s_coord.ip) != 0)
    ESP_LOGI(TAG, "%s is coordinated by %s (%s)", room->name, coord->name,
             coord->ip);
  set_endpoint(&s_coord, coord->ip, coord->port, coord->uuid);
  build_group(*room);
  return true;
}

const char *control_coordinator_ip() { return s_coord.ip; }

void control_play_url(const char *url) {
  char uri[300], escaped[400], args[512];
  radio_uri(url, uri, sizeof(uri));
  xml_escape(uri, escaped, sizeof(escaped));
  snprintf(args, sizeof(args),
           SOAP_INSTANCE_ID "<CurrentURI>%s</CurrentURI>"
                            "<CurrentURIMetaData></CurrentURIMetaData>",
           escaped);
  if (coord_call(AV_TRANSPORT, "SetAVTransportURI", args))
    control_play();
}

bool control_transport(const char *action) {
  return coord_call(AV_TRANSPORT, action, SOAP_INSTANCE_ID);
}

bool control_play() {
  return coord_call(AV_TRANSPORT, "Play", SOAP_INSTANCE_ID "<Speed>1</Speed>");
}

bool control_seek_start() {
  return coord_call(AV_TRANSPORT, "Seek",
                    SOAP_INSTANCE_ID "<Unit>REL_TIME</Unit>"
                                     "<Target>00:00:00</Target>");
}

static void set_volume_at(const Endpoint &e, int level, bool new_gesture) {
  if (new_gesture)
    call_at(e, GROUP_RENDERING_CONTROL, "SnapshotGroupVolume",
            SOAP_INSTANCE_ID);
  char args[96];
  snprintf(args, sizeof(args),
           SOAP_INSTANCE_ID "<DesiredVolume>%d</DesiredVolume>",
           std::clamp(level, VOLUME_MIN, VOLUME_MAX));
  call_at(e, GROUP_RENDERING_CONTROL, "SetGroupVolume", args);
}

void control_set_volume(int level, bool new_gesture) {
  set_volume_at(s_coord, level, new_gesture);
}

static bool playing_at(const Endpoint &e) {
  char buf[512];
  SoapResponse resp = {buf, sizeof(buf), 0, false};
  return call_at(e, AV_TRANSPORT, "GetTransportInfo", SOAP_INSTANCE_ID,
                 &resp) &&
         parse_transport_state(buf) == PlayState::Playing;
}

static bool group_playing() { return playing_at(s_coord); }

static void standalone(const char *ip, int port) {
  soap_call(ip, port, AV_TRANSPORT, "BecomeCoordinatorOfStandaloneGroup",
            SOAP_INSTANCE_ID);
}

// A member walks out of its group; a coordinator sends the others away, so
// it keeps whatever it plays. A stereo pair's hidden half isn't a room and
// stays with its partner.
static void break_up_group() {
  if (s_group.count <= 1)
    return;
  if (strcmp(s_group.room_uuid, s_group.coordinator_uuid) != 0) {
    ESP_LOGI(TAG, "Taking %s out of its group", s_room.ip);
    standalone(s_room.ip, s_room.port);
  } else {
    for (int i = 0; i < s_topology.count; i++) {
      const ZoneMember &m = s_topology.members[i];
      if (!m.invisible && strcmp(m.uuid, s_room.uuid) != 0 &&
          strcmp(m.coordinator_uuid, s_room.uuid) == 0) {
        ESP_LOGI(TAG, "Sending %s out of %s's group", m.name, s_room.ip);
        standalone(m.ip, m.port);
      }
    }
  }
  control_refresh_topology();
}

void control_select(const char *ip, int port, const char *resume_url) {
  Endpoint old_coord = s_coord;
  control_set_room(ip, port);
  control_refresh_topology();

  bool moving = resume_url && resume_url[0];
  if (moving || !group_playing())
    break_up_group();
  if (!moving)
    return;
  if (old_coord.ip[0] && same_endpoint(old_coord, s_coord))
    return; // the music is already playing in this room's group

  if (old_coord.ip[0])
    soap_call(old_coord.ip, old_coord.port, AV_TRANSPORT, "Stop",
              SOAP_INSTANCE_ID);
  control_play_url(resume_url);
}

bool control_join(const char *ip, int port) {
  if (!s_coord.uuid[0])
    control_refresh_topology();
  if (!s_coord.uuid[0]) {
    ESP_LOGW(TAG, "Can't group %s: coordinator unknown", ip);
    return false;
  }
  if (strcmp(ip, s_coord.ip) == 0)
    return true;

  char args[256];
  snprintf(args, sizeof(args),
           SOAP_INSTANCE_ID "<CurrentURI>x-rincon:%s</CurrentURI>"
                            "<CurrentURIMetaData></CurrentURIMetaData>",
           s_coord.uuid);
  ESP_LOGI(TAG, "Grouping %s with %s", ip, s_coord.ip);
  bool ok = soap_call(ip, port, AV_TRANSPORT, "SetAVTransportURI", args);
  control_refresh_topology();
  return ok;
}

bool control_leave(const char *ip, int port) {
  ESP_LOGI(TAG, "Ungrouping %s", ip);
  bool ok = soap_call(ip, port, AV_TRANSPORT,
                      "BecomeCoordinatorOfStandaloneGroup", SOAP_INSTANCE_ID);
  control_refresh_topology();
  return ok;
}

static int volume_at(const Endpoint &e) {
  char buf[512];
  SoapResponse resp = {buf, sizeof(buf), 0, false};
  char val[8] = {};
  if (!call_at(e, GROUP_RENDERING_CONTROL, "GetGroupVolume", SOAP_INSTANCE_ID,
               &resp) ||
      !xml_extract(buf, "CurrentVolume", val, sizeof(val)))
    return -1;
  return std::clamp(atoi(val), VOLUME_MIN, VOLUME_MAX);
}

int control_get_volume() { return volume_at(s_coord); }

// What the group was doing before an alarm. The URI and metadata are kept
// XML-escaped, as Sonos sends them, ready to go back into a request.
struct Resume {
  bool playing;
  int volume;
  int track; // queue position, 0 for a stream
  char rel_time[16];
  char uri[512];
  char metadata[2048];
};
static Resume *s_resume;    // PSRAM, only while an alarm sounds
static Endpoint s_alarm_at; // the coordinator of the room that rings

static constexpr int MEDIA_INFO_SIZE = 6144;

static bool read_media(const Endpoint &e, Resume *r) {
  auto *buf =
      static_cast<char *>(heap_caps_malloc(MEDIA_INFO_SIZE, MALLOC_CAP_SPIRAM));
  if (!buf)
    return false;
  SoapResponse resp = {buf, MEDIA_INFO_SIZE, 0, false};
  bool ok = call_at(e, AV_TRANSPORT, "GetMediaInfo", SOAP_INSTANCE_ID, &resp) &&
            !resp.truncated;
  if (ok) {
    xml_extract(buf, "CurrentURI", r->uri, sizeof(r->uri));
    xml_extract(buf, "CurrentURIMetaData", r->metadata, sizeof(r->metadata));
  }
  SoapResponse pos = {buf, MEDIA_INFO_SIZE, 0, false};
  if (ok &&
      call_at(e, AV_TRANSPORT, "GetPositionInfo", SOAP_INSTANCE_ID, &pos)) {
    char track[8] = {};
    xml_extract(buf, "Track", track, sizeof(track));
    r->track = atoi(track);
    xml_extract(buf, "RelTime", r->rel_time, sizeof(r->rel_time));
  }
  heap_caps_free(buf);
  return ok;
}

static bool set_uri(const Endpoint &e, const char *escaped_uri,
                    const char *escaped_metadata) {
  constexpr size_t SIZE = sizeof(Resume::uri) + sizeof(Resume::metadata) + 160;
  auto *args = static_cast<char *>(heap_caps_malloc(SIZE, MALLOC_CAP_SPIRAM));
  if (!args)
    return false;
  snprintf(args, SIZE,
           SOAP_INSTANCE_ID "<CurrentURI>%s</CurrentURI>"
                            "<CurrentURIMetaData>%s</CurrentURIMetaData>",
           escaped_uri, escaped_metadata);
  bool ok = call_at(e, AV_TRANSPORT, "SetAVTransportURI", args);
  heap_caps_free(args);
  return ok;
}

static void seek(const Endpoint &e, const char *unit, const char *target) {
  char args[128];
  snprintf(args, sizeof(args),
           SOAP_INSTANCE_ID "<Unit>%s</Unit><Target>%s</Target>", unit, target);
  call_at(e, AV_TRANSPORT, "Seek", args);
}

// The coordinator of the group `room` is in, found by name in the household.
// The controlled group's coordinator when there's no such room.
static Endpoint alarm_target(const char *room) {
  control_refresh_topology();
  for (int i = 0; room && room[0] && i < s_topology.count; i++) {
    const ZoneMember &m = s_topology.members[i];
    if (m.invisible || strcasecmp(m.name, room) != 0)
      continue;
    const ZoneMember *coord =
        zone_topology_find_uuid(&s_topology, m.coordinator_uuid);
    if (!coord)
      coord = &m;
    Endpoint e;
    set_endpoint(&e, coord->ip, coord->port, coord->uuid);
    return e;
  }
  if (room && room[0])
    ESP_LOGW(TAG, "No room called %s; ringing here instead", room);
  return s_coord;
}

bool control_alarm_start(const char *url, const char *room, int volume) {
  if (!s_resume) {
    s_alarm_at = alarm_target(room);
    s_resume = static_cast<Resume *>(
        heap_caps_malloc(sizeof(Resume), MALLOC_CAP_SPIRAM));
    if (!s_resume)
      return false;
    *s_resume = {};
    s_resume->playing = playing_at(s_alarm_at);
    s_resume->volume = volume_at(s_alarm_at);
    if (!read_media(s_alarm_at, s_resume))
      s_resume->uri[0] = '\0';
  }
  char escaped[400];
  xml_escape(url, escaped, sizeof(escaped));
  ESP_LOGI(TAG, "Sounding the alarm on %s", s_alarm_at.ip);
  if (!set_uri(s_alarm_at, escaped, ""))
    return false;
  set_volume_at(s_alarm_at, volume, true);
  call_at(s_alarm_at, AV_TRANSPORT, "Play",
          SOAP_INSTANCE_ID "<Speed>1</Speed>");
  return true; // the source was replaced, so it must be put back
}

bool control_alarm_shares_group() {
  return s_resume && same_endpoint(s_alarm_at, s_coord);
}

void control_alarm_restore() {
  if (!s_resume)
    return;
  Resume *r = s_resume;
  const Endpoint &e = s_alarm_at;
  s_resume = nullptr;
  call_at(e, AV_TRANSPORT, "Stop", SOAP_INSTANCE_ID);
  // With nothing loaded before, point the group back at its own queue, so a
  // later Play can't sound the chime again.
  if (!r->uri[0] && e.uuid[0])
    snprintf(r->uri, sizeof(r->uri), "x-rincon-queue:%s#0", e.uuid);
  if (r->uri[0]) {
    set_uri(e, r->uri, r->metadata);
    if (strncmp(r->uri, "x-rincon-queue:", 15) == 0 && r->track > 0) {
      char track[12];
      snprintf(track, sizeof(track), "%d", r->track);
      seek(e, "TRACK_NR", track);
      if (r->rel_time[0] && strcmp(r->rel_time, "NOT_IMPLEMENTED") != 0)
        seek(e, "REL_TIME", r->rel_time);
    }
  }
  if (r->volume >= 0)
    set_volume_at(e, r->volume, true);
  if (r->playing)
    call_at(e, AV_TRANSPORT, "Play", SOAP_INSTANCE_ID "<Speed>1</Speed>");
  heap_caps_free(r);
}

static void poll_track(SonosState *out) {
  static char buf[8192];
  SoapResponse resp = {buf, sizeof(buf), 0, false};
  if (!coord_call(AV_TRANSPORT, "GetPositionInfo", SOAP_INSTANCE_ID, &resp))
    return;

  char track_uri[300] = {};
  xml_extract(buf, "TrackURI", track_uri, sizeof(track_uri));
  xml_unescape(track_uri);
  out->station_index =
      match_station(track_uri, s_station_urls, s_station_count);
  if (out->station_index < 0)
    parse_media_info(buf, s_coord.ip, s_coord.port, &out->media);
}

static uint8_t poll_actions() {
  char buf[512];
  SoapResponse resp = {buf, sizeof(buf), 0, false};
  if (!coord_call(AV_TRANSPORT, "GetCurrentTransportActions", SOAP_INSTANCE_ID,
                  &resp))
    return 0;
  return parse_transport_actions(buf);
}

bool control_poll(SonosState *out) {
  *out = {};
  out->station_index = -1;

  char buf[512];
  SoapResponse resp = {buf, sizeof(buf), 0, false};
  if (!coord_call(AV_TRANSPORT, "GetTransportInfo", SOAP_INSTANCE_ID, &resp))
    return false;
  out->play_state = parse_transport_state(buf);
  out->volume = control_get_volume();
  poll_track(out);
  out->actions = poll_actions();
  out->group = s_group;
  return true;
}
