// Drives the real control, SOAP and parsing code against a fake household
// that behaves like Sonos: commands to a group member are refused, and a
// SetAVTransportURI on a member pulls it out of its group.
#include "../include/sonos_group.h"
#include "../src/soap.h"
#include "../src/sonos_control.h"
#include "../src/sonos_engine.h"
#include "../src/track_info.h"
#include "../src/zone_topology.h"
#include "stubs/esp_stub.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static std::string escape(const std::string &s) {
  std::string out;
  for (char c : s) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '\'':
      out += "&apos;";
      break;
    default:
      out += c;
    }
  }
  return out;
}

// One layer, in one pass, like an XML parser.
static std::string unescape(const std::string &s) {
  static const std::pair<const char *, char> ENTITIES[] = {{"&amp;", '&'},
                                                           {"&lt;", '<'},
                                                           {"&gt;", '>'},
                                                           {"&quot;", '"'},
                                                           {"&apos;", '\''}};
  std::string out;
  for (size_t i = 0; i < s.size();) {
    bool matched = false;
    for (auto [entity, c] : ENTITIES)
      if (s.compare(i, strlen(entity), entity) == 0) {
        out += c;
        i += strlen(entity);
        matched = true;
        break;
      }
    if (!matched)
      out += s[i++];
  }
  return out;
}

static std::string between(const std::string &s, const std::string &open,
                           const std::string &close) {
  size_t a = s.find(open);
  if (a == std::string::npos)
    return "";
  a += open.size();
  return s.substr(a, s.find(close, a) - a);
}

// ─── Fake household ─────────────────────────────────────────────────────────

struct Speaker {
  std::string uuid, ip, name, coordinator;
  bool invisible = false;
  std::string satellites; // raw <Satellite/> children
  std::string uri;        // only meaningful on coordinators
  std::string state = "STOPPED";
  std::string actions = "Set, Stop, Play";
  std::string metadata;
  std::string uri_metadata; // what GetMediaInfo reports with the URI
  int track = 0;            // queue position
  std::string rel_time = "0:00:00";
  int volume = 20;
};

static std::vector<Speaker> household;
static std::vector<std::string> calls; // "ip Action body"

static Speaker *by_ip(const std::string &ip) {
  for (auto &s : household)
    if (s.ip == ip)
      return &s;
  return nullptr;
}

static Speaker *by_uuid(const std::string &uuid) {
  for (auto &s : household)
    if (s.uuid == uuid)
      return &s;
  return nullptr;
}

static Speaker speaker(const char *uuid, const char *ip, const char *name,
                       const char *coordinator) {
  Speaker s;
  s.uuid = uuid;
  s.ip = ip;
  s.name = name;
  s.coordinator = coordinator;
  return s;
}

static bool bonded(const Speaker &a, const Speaker &b) {
  return &a != &b && b.invisible && a.name == b.name;
}

static bool solo(const Speaker &s) {
  for (auto &m : household)
    if (&m != &s && m.coordinator == s.uuid && !bonded(s, m))
      return false;
  return true;
}

// Like Sonos: a coordinator that walks out hands its group (and what it was
// playing) to a remaining member; a stereo pair's hidden half moves with it.
static void move_to(Speaker *s, const std::string &coordinator) {
  if (s->coordinator == s->uuid) {
    Speaker *heir = nullptr;
    for (auto &m : household) {
      if (&m == s || m.coordinator != s->uuid || bonded(*s, m))
        continue;
      if (!heir) {
        heir = &m;
        heir->uri = s->uri;
        heir->state = s->state;
        heir->actions = s->actions;
        heir->metadata = s->metadata;
      }
      m.coordinator = heir->uuid;
    }
  }
  s->coordinator = coordinator;
  for (auto &m : household)
    if (bonded(*s, m))
      m.coordinator = coordinator;
}

// Visible rooms you can hear, sorted, e.g. "Kitchen,Living Room".
static std::string audible() {
  std::vector<std::string> rooms;
  for (auto &m : household) {
    Speaker *coord = by_uuid(m.coordinator);
    if (!m.invisible && coord && coord->state == "PLAYING")
      rooms.push_back(m.name);
  }
  std::sort(rooms.begin(), rooms.end());
  std::string out;
  for (auto &r : rooms)
    out += (out.empty() ? "" : ",") + r;
  return out;
}

static void reset_household() {
  household = {
      speaker("RINCON_LR", "10.0.0.10", "Living Room", "RINCON_LR"),
      speaker("RINCON_K", "10.0.0.11", "Kitchen", "RINCON_LR"),
      speaker("RINCON_BR", "10.0.0.12", "Kid's Room", "RINCON_BR"),
      speaker("RINCON_BR_R", "10.0.0.13", "Kid's Room", "RINCON_BR"),
      speaker("RINCON_TV", "10.0.0.14", "TV Room", "RINCON_TV"),
  };
  household[3].invisible = true;
  household[4].satellites =
      "<Satellite UUID=\"RINCON_SUB\" Location=\"http://10.0.0.15:1400/"
      "xml/device_description.xml\" ZoneName=\"TV Room\" Invisible=\"1\"/>";
  calls.clear();
}

static std::string zone_group_state() {
  std::string xml = "<ZoneGroupState><ZoneGroups>";
  for (auto &coord : household) {
    if (coord.coordinator != coord.uuid)
      continue;
    xml += "<ZoneGroup Coordinator=\"" + coord.uuid + "\" ID=\"" + coord.uuid +
           ":7\">";
    for (auto &m : household) {
      if (m.coordinator != coord.uuid)
        continue;
      xml += "<ZoneGroupMember UUID=\"" + m.uuid + "\" Location=\"http://" +
             m.ip + ":1400/xml/device_description.xml\" ZoneName=\"" +
             escape(m.name) + "\"" + (m.invisible ? " Invisible=\"1\"" : "");
      xml += m.satellites.empty() ? "/>"
                                  : ">" + m.satellites + "</ZoneGroupMember>";
    }
    xml += "</ZoneGroup>";
  }
  xml += "</ZoneGroups><VanishedDevices><Device UUID=\"RINCON_OLD\" "
         "ZoneName=\"Garage\" Location=\"http://10.0.0.99:1400/x.xml\"/>"
         "</VanishedDevices></ZoneGroupState>";
  return "<ZoneGroupState>" + escape(xml) + "</ZoneGroupState>";
}

static int respond(const std::string &ip, const std::string &action,
                   const std::string &body, std::string *out) {
  Speaker *s = by_ip(ip);
  if (!s)
    return 0;
  bool coordinator = s->coordinator == s->uuid;
  auto fault = [&] {
    *out = "<errorCode>800</errorCode>";
    return 500;
  };

  if (action == "GetZoneGroupState") {
    *out = zone_group_state();
    return 200;
  }
  if (action == "SetAVTransportURI") {
    std::string uri = unescape(between(body, "<CurrentURI>", "</CurrentURI>"));
    s->uri_metadata = unescape(
        between(body, "<CurrentURIMetaData>", "</CurrentURIMetaData>"));
    if (uri.rfind("x-rincon:", 0) == 0) {
      Speaker *target = by_uuid(uri.substr(9));
      if (!target || target->coordinator != target->uuid)
        return fault(); // can only join a coordinator
      move_to(s, target->uuid);
      return 200;
    }
    if (!coordinator) {
      // A member given its own stream leaves the group: the echo bug.
      move_to(s, s->uuid);
      s->state = "STOPPED";
    }
    s->uri = uri;
    return 200;
  }
  if (action == "BecomeCoordinatorOfStandaloneGroup") {
    if (coordinator && solo(*s))
      return 200;
    move_to(s, s->uuid);
    s->state = "STOPPED";
    s->uri.clear();
    return 200;
  }
  if (!coordinator)
    return fault();
  if (action == "Play") {
    s->state = "PLAYING";
    // Real players report a playing stream with its scheme nested again.
    const std::string radio = "x-rincon-mp3radio://";
    if (s->uri.rfind(radio, 0) == 0 &&
        s->uri.find("://", radio.size()) == std::string::npos)
      s->uri.insert(radio.size(), "http://");
  } else if (action == "Pause") {
    s->state = "PAUSED_PLAYBACK";
  } else if (action == "Stop") {
    s->state = "STOPPED";
  } else if (action == "SetGroupVolume") {
    s->volume = std::stoi(between(body, "<DesiredVolume>", "<"));
  } else if (action == "GetTransportInfo") {
    *out = "<CurrentTransportState>" + s->state + "</CurrentTransportState>";
  } else if (action == "GetGroupVolume") {
    *out = "<CurrentVolume>" + std::to_string(s->volume) + "</CurrentVolume>";
  } else if (action == "GetCurrentTransportActions") {
    *out = "<Actions>" + s->actions + "</Actions>";
  } else if (action == "GetPositionInfo") {
    *out = "<Track>" + std::to_string(s->track) + "</Track><TrackURI>" +
           escape(s->uri) + "</TrackURI><TrackMetaData>" + escape(s->metadata) +
           "</TrackMetaData><RelTime>" + s->rel_time + "</RelTime>";
  } else if (action == "GetMediaInfo") {
    *out = "<CurrentURI>" + escape(s->uri) +
           "</CurrentURI><CurrentURIMetaData>" + escape(s->uri_metadata) +
           "</CurrentURIMetaData>";
  } else if (action == "Seek") {
    std::string unit = between(body, "<Unit>", "</Unit>");
    std::string target = between(body, "<Target>", "</Target>");
    if (unit == "TRACK_NR")
      s->track = std::stoi(target);
    else
      s->rel_time = target;
  }
  return 200;
}

// ─── HTTP double ────────────────────────────────────────────────────────────

struct HttpClient {
  esp_http_client_config_t cfg;
  std::string action, body;
  int status = 0;
};

static size_t chunk_size = 1000;

esp_http_client_handle_t
esp_http_client_init(const esp_http_client_config_t *cfg) {
  return new HttpClient{*cfg, "", "", 0};
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,
                                     const char *key, const char *value) {
  if (strcmp(key, "SOAPAction") == 0) {
    std::string v = value;
    c->action = v.substr(v.find('#') + 1);
    c->action.pop_back();
  }
  return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c,
                                         const char *data, int len) {
  c->body.assign(data, static_cast<size_t>(len));
  return ESP_OK;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
  std::string url = c->cfg.url;
  std::string ip = between(url, "http://", ":");
  calls.push_back(ip + " " + c->action + " " +
                  between(c->body, "<s:Body>", "</s:Body>"));
  std::string out;
  c->status = respond(ip, c->action, c->body, &out);
  if (!c->status)
    return ESP_FAIL;
  for (size_t at = 0; at < out.size(); at += chunk_size) {
    std::string part = out.substr(at, chunk_size);
    esp_http_client_event_t evt = {HTTP_EVENT_ON_DATA, c->cfg.user_data,
                                   static_cast<int>(part.size()), part.data()};
    c->cfg.event_handler(&evt);
  }
  return ESP_OK;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) {
  return c->status;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
  delete c;
  return ESP_OK;
}

static int count_calls(const std::string &needle) {
  return static_cast<int>(
      std::count_if(calls.begin(), calls.end(), [&](const std::string &c) {
        return c.find(needle) != std::string::npos;
      }));
}

static const char *const STATIONS[] = {
    "https://cdn0.example.net/p1_mp3_h",
    "http://cdn0.example.net/jazz_mp3_h",
};

static void point_at(const char *ip) {
  reset_household();
  control_set_room(ip, 1400);
  CHECK(control_refresh_topology());
  calls.clear();
}

// ─── Tests ──────────────────────────────────────────────────────────────────

static void test_topology_parse() {
  reset_household();
  std::string response = zone_group_state();
  ZoneTopology topo;
  CHECK(zone_topology_parse(response.data(), &topo) == 5);

  const ZoneMember *kids = zone_topology_find_ip(&topo, "10.0.0.12");
  CHECK(kids && strcmp(kids->name, "Kid's Room") == 0);
  CHECK(kids && !kids->invisible);
  const ZoneMember *right = zone_topology_find_ip(&topo, "10.0.0.13");
  CHECK(right && right->invisible);

  // The Sub's Invisible="1" must not hide the soundbar it hangs off.
  const ZoneMember *tv = zone_topology_find_ip(&topo, "10.0.0.14");
  CHECK(tv && !tv->invisible);
  CHECK(!zone_topology_find_ip(&topo, "10.0.0.15"));
  CHECK(!zone_topology_find_ip(&topo, "10.0.0.99"));

  const ZoneMember *kitchen = zone_topology_find_ip(&topo, "10.0.0.11");
  CHECK(kitchen && strcmp(kitchen->coordinator_uuid, "RINCON_LR") == 0);
  CHECK(kitchen && kitchen->port == 1400);
}

// The echo bug: controlling a grouped room sent the stream to that room,
// splitting it out of the group into an unsynced duplicate.
static void test_member_room_plays_through_coordinator() {
  point_at("10.0.0.11");
  CHECK(strcmp(control_coordinator_ip(), "10.0.0.10") == 0);

  control_play_url(STATIONS[0]);
  CHECK(count_calls("10.0.0.10 SetAVTransportURI") == 1);
  CHECK(count_calls("10.0.0.10 Play") == 1);
  CHECK(count_calls("10.0.0.11 ") == 0);
  CHECK(by_ip("10.0.0.11")->coordinator == "RINCON_LR");
  CHECK(by_ip("10.0.0.10")->uri ==
        "x-rincon-mp3radio://http://cdn0.example.net/p1_mp3_h");

  control_transport("Pause");
  CHECK(by_ip("10.0.0.10")->state == "PAUSED_PLAYBACK");
}

static void test_poll_reports_group_and_station() {
  point_at("10.0.0.11");
  control_set_stations(STATIONS, 2);
  control_play_url(STATIONS[1]);

  SonosState state;
  CHECK(control_poll(&state));
  CHECK(state.play_state == PlayState::Playing);
  CHECK(state.station_index == 1);
  CHECK(state.group.count == 2);
  CHECK(strcmp(state.group.coordinator_uuid, "RINCON_LR") == 0);

  point_at("10.0.0.12");
  CHECK(control_poll(&state));
  CHECK(state.group.count == 1); // the stereo pair's right half is hidden
}

static void test_join_uses_the_groups_coordinator() {
  point_at("10.0.0.11"); // Kitchen is a member of Living Room's group
  CHECK(control_join("10.0.0.12", 1400));
  CHECK(count_calls("10.0.0.12 SetAVTransportURI") == 1);
  CHECK(count_calls("x-rincon:RINCON_LR") == 1);
  CHECK(by_ip("10.0.0.12")->coordinator == "RINCON_LR");

  SonosState state;
  control_poll(&state);
  CHECK(state.group.count == 3);
}

static void test_leave_shrinks_the_group() {
  point_at("10.0.0.10");
  CHECK(control_leave("10.0.0.11", 1400));
  CHECK(count_calls("10.0.0.11 BecomeCoordinatorOfStandaloneGroup") == 1);
  SonosState state;
  control_poll(&state);
  CHECK(state.group.count == 1);
}

static void test_leaving_room_retargets_to_itself() {
  point_at("10.0.0.11");
  control_leave("10.0.0.11", 1400);
  CHECK(strcmp(control_coordinator_ip(), "10.0.0.11") == 0);
}

static void test_select_moves_playback_between_groups() {
  point_at("10.0.0.10");
  control_play_url(STATIONS[0]);
  calls.clear();

  control_select("10.0.0.12", 1400, STATIONS[0]);
  CHECK(by_ip("10.0.0.10")->state == "STOPPED");
  CHECK(by_ip("10.0.0.12")->state == "PLAYING");
  CHECK(count_calls("10.0.0.13") == 0);
}

static void test_select_member_of_playing_group_plays_it_alone() {
  point_at("10.0.0.10");
  control_play_url(STATIONS[0]);
  calls.clear();

  control_select("10.0.0.11", 1400, STATIONS[0]);
  CHECK(by_ip("10.0.0.11")->coordinator == "RINCON_K");
  CHECK(by_ip("10.0.0.11")->state == "PLAYING");
  CHECK(by_ip("10.0.0.10")->state == "STOPPED");
}

static void test_select_coordinator_keeps_playing_and_sends_others_away() {
  point_at("10.0.0.10");
  control_play_url(STATIONS[0]);
  calls.clear();

  control_select("10.0.0.10", 1400, STATIONS[0]);
  CHECK(count_calls("10.0.0.10 Stop") == 0); // no gap in the music
  CHECK(count_calls("10.0.0.11 BecomeCoordinatorOfStandaloneGroup") == 1);
  CHECK(audible() == "Living Room");
}

static void test_select_leaves_someone_elses_playing_group_alone() {
  point_at("10.0.0.12");
  Speaker *lr = by_ip("10.0.0.10");
  lr->state = "PLAYING";
  lr->uri = "x-sonos-vli:RINCON_LR:2,spotify:x";

  control_select("10.0.0.11", 1400, ""); // the knob isn't playing
  CHECK(count_calls("BecomeCoordinatorOfStandaloneGroup") == 0);
  CHECK(audible() == "Kitchen,Living Room");
}

static void test_select_breaks_up_a_silent_group() {
  point_at("10.0.0.12");
  control_select("10.0.0.11", 1400, "");
  CHECK(by_ip("10.0.0.11")->coordinator == "RINCON_K");
  control_play();
  CHECK(audible() == "Kitchen");
}

static void test_group_volume() {
  point_at("10.0.0.11");
  control_set_volume(40, true);
  control_set_volume(42, false);
  CHECK(count_calls("10.0.0.10 SnapshotGroupVolume") == 1);
  CHECK(count_calls("10.0.0.10 SetGroupVolume") == 2);
  CHECK(count_calls("SetVolume") == 0);
  CHECK(by_ip("10.0.0.10")->volume == 42);
  control_set_volume(150, false);
  CHECK(by_ip("10.0.0.10")->volume == 100);
}

static void test_spotify_track() {
  point_at("10.0.0.10");
  Speaker *lr = by_ip("10.0.0.10");
  lr->state = "PLAYING";
  lr->actions = "Set, Stop, Pause, Seek, Next, Previous";
  lr->uri = "x-sonos-vli:RINCON_LR:2,spotify:abc123";
  lr->metadata =
      "<DIDL-Lite><item><dc:title>Rock &amp; Roll</dc:title>"
      "<dc:creator>Led Zeppelin</dc:creator>"
      "<upnp:albumArtURI>/getaa?s=1&amp;u=x-sonos-spotify</upnp:albumArtURI>"
      "</item></DIDL-Lite>";

  SonosState state;
  CHECK(control_poll(&state));
  CHECK(state.station_index == -1);
  CHECK(state.media.has_media);
  CHECK(strcmp(state.media.source, "Spotify") == 0);
  CHECK(strcmp(state.media.title, "Rock & Roll") == 0);
  CHECK(strcmp(state.media.artist, "Led Zeppelin") == 0);
  CHECK(strcmp(state.media.art_url,
               "http://10.0.0.10:1400/getaa?s=1&u=x-sonos-spotify") == 0);
  CHECK(state.actions & ACTION_PAUSE);
  CHECK(state.actions & ACTION_NEXT);
  CHECK(!(state.actions & ACTION_PLAY));
}

static void test_tv_has_no_skip_or_pause() {
  point_at("10.0.0.14");
  Speaker *tv = by_ip("10.0.0.14");
  tv->state = "PLAYING";
  tv->actions = "Set, Stop";
  tv->uri = "x-sonos-htastream:RINCON_TV:spdif";

  SonosState state;
  CHECK(control_poll(&state));
  CHECK(strcmp(state.media.source, "TV") == 0);
  CHECK(strcmp(state.media.title, "TV") == 0);
  CHECK(state.media.has_media);
  CHECK(!(state.actions & (ACTION_PAUSE | ACTION_NEXT | ACTION_PREVIOUS)));
}

static void test_source_names() {
  struct {
    const char *uri, *source;
  } cases[] = {
      {"x-rincon-stream:RINCON_PORT", "Line-In"},
      {"x-sonos-htastream:RINCON_TV:spdif", "TV"},
      {"x-sonos-vli:RINCON_LR:1,airplay:abc", "AirPlay"},
      {"x-sonos-vli:RINCON_LR:2,spotify:abc", "Spotify"},
      {"x-sonos-http:song%3a123.mp4?sid=204&flags=8224&sn=3", "Apple Music"},
      {"x-sonos-http:ABC.mp3?sid=201&flags=0", "Amazon Music"},
      {"x-sonos-spotify:spotify%3atrack%3aabc?sid=12&flags=8224", "Spotify"},
      {"x-sonosapi-hls-static:ALkSOiE?sid=284&flags=8", "YouTube Music"},
      {"x-sonos-http:track.mp3?sid=9999", "Media"},
      {"x-file-cifs://nas/music/a.flac", "Music Library"},
      {"", ""},
  };
  for (auto &c : cases) {
    char out[32];
    detect_source(c.uri, out, sizeof(out));
    if (strcmp(out, c.source) != 0)
      std::printf("  %s -> %s (want %s)\n", c.uri, out, c.source);
    CHECK(strcmp(out, c.source) == 0);
  }
}

static void test_station_uris() {
  char uri[128];
  radio_uri("https://a.example/x", uri, sizeof(uri));
  CHECK(strcmp(uri, "x-rincon-mp3radio://a.example/x") == 0);
  radio_uri("x-sonosapi-stream:s1", uri, sizeof(uri));
  CHECK(strcmp(uri, "x-sonosapi-stream:s1") == 0);

  CHECK(match_station("x-rincon-mp3radio://cdn0.example.net/jazz_mp3_h",
                      STATIONS, 2) == 1);
  CHECK(match_station("x-rincon-mp3radio://http://cdn0.example.net/jazz_mp3_h",
                      STATIONS, 2) == 1);
  CHECK(match_station("x-rincon:RINCON_LR", STATIONS, 2) == -1);
  CHECK(match_station("x-rincon-mp3radio://cdn0.example.net/p1_mp3_hd",
                      STATIONS, 2) == -1);

  point_at("10.0.0.10");
  control_play_url("https://a.example/live?token=1&q=2");
  CHECK(count_calls("live?token=1&amp;q=2</CurrentURI>") == 1);
}

static void test_transport_actions() {
  CHECK(parse_transport_actions(
            "<Actions>Set, Play, Stop, X_DLNA_SeekTime, Next</Actions>") ==
        (ACTION_PLAY | ACTION_STOP | ACTION_NEXT));
  CHECK(parse_transport_actions("<Actions></Actions>") == 0);
}

static void test_member_refuses_transport() {
  // Guards the fake itself: if this passed, routing tests would prove nothing.
  reset_household();
  CHECK(!soap_call("10.0.0.11", 1400, AV_TRANSPORT, "Play", SOAP_INSTANCE_ID));
}

static void test_oversized_response_is_cut_not_spliced() {
  point_at("10.0.0.10");
  by_ip("10.0.0.10")->metadata =
      std::string(3000, 'x') + "<dc:title>T</dc:title>";
  chunk_size = 700;
  char buf[2048];
  SoapResponse resp = {buf, sizeof(buf), 0, false};
  CHECK(soap_call("10.0.0.10", 1400, AV_TRANSPORT, "GetPositionInfo",
                  SOAP_INSTANCE_ID, &resp));
  CHECK(resp.truncated);
  CHECK(resp.len == static_cast<int>(sizeof(buf)) - 1);
  CHECK(!strstr(buf, "dc:title"));
  chunk_size = 1000;
}

// ─── Engine: the net task's behaviour over time
// ──────────────────────────────────────

static std::vector<SonosState> published;
static std::string saved_room;
static uint32_t clock_ms;

void engine_publish(const SonosState &state) { published.push_back(state); }
void engine_save_room(const char *ip) { saved_room = ip; }

static constexpr uint32_t POLL_MS = 2000;

static void send(CmdType type, const char *ip = "", const char *uri = "",
                 int volume = 0) {
  Command cmd = {};
  cmd.type = type;
  cmd.port = 1400;
  cmd.volume = volume;
  snprintf(cmd.ip, sizeof(cmd.ip), "%s", ip);
  snprintf(cmd.uri, sizeof(cmd.uri), "%s", uri);
  // Same order as the net task loop: handle, then tick.
  engine_handle(cmd, clock_ms);
  engine_tick(clock_ms);
}

static void run_for(uint32_t ms) {
  for (uint32_t end = clock_ms + ms; clock_ms < end; clock_ms += 100)
    engine_tick(clock_ms);
}

static std::string group_label() {
  if (published.empty())
    return "(nothing published)";
  char label[160];
  sonos_group_label(published.back().group, label, sizeof(label));
  return label;
}

// Boot the way main.cpp does: saved room, then WiFi comes up.
static void boot(const char *room_ip) {
  reset_household();
  control_set_stations(STATIONS, 2);
  engine_init(POLL_MS);
  published.clear();
  saved_room.clear();
  clock_ms = 1000;
  send(CmdType::SetRoom, room_ip);
  engine_set_active(true);
  run_for(200);
  calls.clear();
}

static void test_engine_idle_until_started() {
  reset_household();
  engine_init(POLL_MS);
  published.clear();
  clock_ms = 1000;

  send(CmdType::SetRoom, "10.0.0.12");
  send(CmdType::PlayUri, "", STATIONS[0]);
  run_for(10000);
  CHECK(saved_room == "10.0.0.12"); // the room choice survives WiFi down
  CHECK(calls.empty());
  CHECK(published.empty());
  CHECK(audible().empty());

  engine_set_active(true);
  run_for(200);
  CHECK(count_calls("GetZoneGroupState") == 1);
  CHECK(published.size() == 1);
  CHECK(group_label() == "Kid's Room");
}

static void test_engine_stop_halts_polling_and_drops_commands() {
  boot("10.0.0.10");
  send(CmdType::PlayUri, "", STATIONS[0]);
  CHECK(audible() == "Kitchen,Living Room");

  engine_set_active(false);
  calls.clear();
  size_t before = published.size();
  send(CmdType::Stop);
  send(CmdType::Next);
  run_for(20000);
  CHECK(calls.empty());
  CHECK(published.size() == before);
  CHECK(audible() == "Kitchen,Living Room");
}

static void test_engine_restart_sees_regrouping_done_while_offline() {
  boot("10.0.0.12");
  engine_set_active(false);
  run_for(500);

  // Meanwhile, in the Sonos app: Kid's Room is added to Living Room's group.
  move_to(by_ip("10.0.0.12"), "RINCON_LR");

  engine_set_active(true);
  run_for(200);
  calls.clear();
  send(CmdType::PlayUri, "", STATIONS[1]);
  CHECK(count_calls("10.0.0.10 SetAVTransportURI") == 1);
  CHECK(count_calls("10.0.0.12 ") == 0);
  CHECK(audible() == "Kid's Room,Kitchen,Living Room");
}

static void test_engine_notices_regrouping_while_running() {
  boot("10.0.0.12");
  move_to(by_ip("10.0.0.12"), "RINCON_LR");
  run_for(TOPOLOGY_REFRESH_MS + 200);
  CHECK(strcmp(control_coordinator_ip(), "10.0.0.10") == 0);
  CHECK(group_label() == "Kid's Room + 2");
}

static void test_engine_polls_on_schedule() {
  boot("10.0.0.10");
  size_t before = published.size();
  run_for(POLL_MS * 5);
  CHECK(published.size() == before + 5);
  CHECK(count_calls("GetZoneGroupState") == 0); // topology isn't due yet
}

static void test_engine_unreachable_room_publishes_nothing() {
  boot("10.0.0.10");
  household.clear(); // every speaker gone
  size_t before = published.size();
  run_for(POLL_MS * 3);
  CHECK(published.size() == before); // no bogus "Unknown" state reaches the UI

  calls.clear();
  run_for(TOPOLOGY_RETRY_MS * 2);
  CHECK(count_calls("GetZoneGroupState") <= 3); // backs off, doesn't spin
}

static void test_switch_room_stops_the_old_group() {
  boot("10.0.0.10");
  send(CmdType::PlayUri, "", STATIONS[0]);
  CHECK(audible() == "Kitchen,Living Room");

  send(CmdType::Select, "10.0.0.12", STATIONS[0]);
  CHECK(audible() == "Kid's Room");
  CHECK(by_ip("10.0.0.12")->uri ==
        "x-rincon-mp3radio://http://cdn0.example.net/p1_mp3_h");
  CHECK(saved_room == "10.0.0.12");
  CHECK(group_label() == "Kid's Room");
  CHECK(published.back().station_index == 0);

  // The old group isn't dissolved, only silenced.
  CHECK(by_ip("10.0.0.11")->coordinator == "RINCON_LR");
}

static void test_switch_to_a_member_of_another_playing_group() {
  boot("10.0.0.12");
  send(CmdType::PlayUri, "", STATIONS[0]);
  by_ip("10.0.0.10")->state = "PLAYING"; // Living Room plays its own thing
  by_ip("10.0.0.10")->uri = "x-sonos-vli:RINCON_LR:2,spotify:x";

  send(CmdType::Select, "10.0.0.11", STATIONS[0]);
  CHECK(audible() == "Kitchen,Living Room");
  CHECK(by_ip("10.0.0.11")->coordinator == "RINCON_K"); // split off
  CHECK(by_ip("10.0.0.11")->uri ==
        "x-rincon-mp3radio://http://cdn0.example.net/p1_mp3_h");
  CHECK(by_ip("10.0.0.10")->uri == "x-sonos-vli:RINCON_LR:2,spotify:x");
  CHECK(group_label() == "Kitchen");
}

static void test_switch_room_within_group_plays_it_alone() {
  boot("10.0.0.10");
  send(CmdType::PlayUri, "", STATIONS[0]);
  send(CmdType::Select, "10.0.0.11", STATIONS[0]);
  CHECK(audible() == "Kitchen");
  CHECK(group_label() == "Kitchen");
}

// Play in one room, group a second, switch away, then come back to the
// second: only it plays, until it's grouped again.
static void test_coming_back_to_a_grouped_room_plays_it_alone() {
  boot("10.0.0.14");
  send(CmdType::PlayUri, "", STATIONS[0]);
  send(CmdType::Join, "10.0.0.12");
  CHECK(audible() == "Kid's Room,TV Room");

  send(CmdType::Select, "10.0.0.10", STATIONS[0]);
  CHECK(audible() == "Living Room");

  send(CmdType::Select, "10.0.0.12", STATIONS[0]);
  CHECK(audible() == "Kid's Room");
  CHECK(group_label() == "Kid's Room");
  CHECK(by_ip("10.0.0.13")->coordinator == "RINCON_BR"); // pair stays whole

  send(CmdType::Select, "10.0.0.14", STATIONS[0]);
  CHECK(audible() == "TV Room");
}

static void test_stop_fades_out_then_restores_volume() {
  boot("10.0.0.10");
  send(CmdType::PlayUri, "", STATIONS[0]);
  by_ip("10.0.0.10")->volume = 30;
  calls.clear();

  send(CmdType::Stop);
  run_for(FADE_OUT_MS / 2);
  int mid = by_ip("10.0.0.10")->volume;
  CHECK(mid > 5 && mid < 25);
  CHECK(audible() == "Kitchen,Living Room");
  for (const SonosState &s : published)
    CHECK(s.volume == -1 || s.volume == 20 || s.volume == 30);

  run_for(FADE_OUT_MS);
  CHECK(audible().empty());
  CHECK(by_ip("10.0.0.10")->volume == 30);
  CHECK(count_calls("SnapshotGroupVolume") == 1);
}

static void test_play_during_fade_calls_the_stop_off() {
  boot("10.0.0.10");
  send(CmdType::PlayUri, "", STATIONS[0]);
  by_ip("10.0.0.10")->volume = 30;
  send(CmdType::Stop);
  run_for(FADE_OUT_MS / 3);
  send(CmdType::PlayUri, "", STATIONS[1]);
  CHECK(by_ip("10.0.0.10")->volume == 30);
  run_for(FADE_OUT_MS * 2);
  CHECK(audible() == "Kitchen,Living Room");
}

static void test_turning_during_fade_finishes_the_stop() {
  boot("10.0.0.10");
  send(CmdType::PlayUri, "", STATIONS[0]);
  by_ip("10.0.0.10")->volume = 30;
  send(CmdType::Stop);
  run_for(FADE_OUT_MS / 3);
  send(CmdType::Volume, "", "", 40);
  CHECK(audible().empty());
  CHECK(by_ip("10.0.0.10")->volume == 40);
}

static void test_switch_room_without_resume_leaves_both_alone() {
  boot("10.0.0.10");
  Speaker *lr = by_ip("10.0.0.10");
  lr->state = "PLAYING";
  lr->uri = "x-sonos-vli:RINCON_LR:2,spotify:x"; // someone's Spotify
  send(CmdType::Select, "10.0.0.12", "");
  CHECK(audible() == "Kitchen,Living Room");
  CHECK(count_calls("Stop") == 0);
}

static void test_switch_room_while_stopped_plays_nothing() {
  boot("10.0.0.10");
  send(CmdType::Select, "10.0.0.12", "");
  CHECK(audible().empty());
  send(CmdType::PlayUri, "", STATIONS[1]);
  CHECK(audible() == "Kid's Room");
}

static void test_long_press_joins_a_room_to_the_playing_group() {
  boot("10.0.0.12");
  send(CmdType::PlayUri, "", STATIONS[0]);
  CHECK(audible() == "Kid's Room");

  SonosGroup group = published.back().group;
  CHECK(sonos_group_toggle(group, "RINCON_TV") == GroupToggle::Join);
  send(CmdType::Join, "10.0.0.14");
  CHECK(count_calls("10.0.0.14 SetAVTransportURI") == 1);
  CHECK(count_calls("10.0.0.14 Play") == 0); // members follow, never stream
  CHECK(audible() == "Kid's Room,TV Room");
  CHECK(group_label() == "Kid's Room + TV Room");

  // Joining a room out of another group splits only that room off.
  send(CmdType::Join, "10.0.0.11");
  CHECK(by_ip("10.0.0.10")->coordinator == "RINCON_LR");
  CHECK(audible() == "Kid's Room,Kitchen,TV Room");
  CHECK(group_label() == "Kid's Room + 2");
}

// Sonos acks a join before its topology shows it; read it again shortly so the
// picker's check mark doesn't wait the full refresh interval.
static void test_regroup_rereads_topology_soon() {
  boot("10.0.0.12");
  send(CmdType::Join, "10.0.0.14");
  calls.clear();
  run_for(REGROUP_SETTLE_MS + 100);
  CHECK(count_calls("GetZoneGroupState") == 1);
}

static void test_long_press_coordinator_of_other_group() {
  boot("10.0.0.12");
  send(CmdType::PlayUri, "", STATIONS[0]);
  by_ip("10.0.0.10")->state = "PLAYING";

  send(CmdType::Join, "10.0.0.10"); // Living Room leaves Kitchen behind
  CHECK(by_ip("10.0.0.10")->coordinator == "RINCON_BR");
  CHECK(by_ip("10.0.0.11")->coordinator == "RINCON_K");
  CHECK(audible() == "Kid's Room,Kitchen,Living Room");
  CHECK(group_label() == "Kid's Room + Living Room");
}

static void test_long_press_again_removes_the_room() {
  boot("10.0.0.12");
  send(CmdType::PlayUri, "", STATIONS[0]);
  send(CmdType::Join, "10.0.0.14");
  CHECK(sonos_group_toggle(published.back().group, "RINCON_TV") ==
        GroupToggle::Leave);
  send(CmdType::Leave, "10.0.0.14");
  CHECK(audible() == "Kid's Room");
  CHECK(group_label() == "Kid's Room");
}

static void test_long_press_own_room_leaves_and_keeps_others_playing() {
  boot("10.0.0.10"); // Living Room coordinates Kitchen
  send(CmdType::PlayUri, "", STATIONS[0]);
  CHECK(sonos_group_toggle(published.back().group, "RINCON_LR") ==
        GroupToggle::Leave);
  send(CmdType::Leave, "10.0.0.10");
  CHECK(audible() == "Kitchen"); // coordination handed to Kitchen
  CHECK(group_label() == "Living Room");
  CHECK(strcmp(control_coordinator_ip(), "10.0.0.10") == 0);

  send(CmdType::Stop);
  run_for(FADE_OUT_MS + 100);
  CHECK(audible() == "Kitchen"); // the knob now only controls Living Room
}

static void test_long_press_own_room_alone_does_nothing() {
  boot("10.0.0.12");
  CHECK(sonos_group_toggle(published.back().group, "RINCON_BR") ==
        GroupToggle::None);
}

static void test_join_with_unknown_coordinator_sends_nothing() {
  reset_household();
  engine_init(POLL_MS);
  clock_ms = 1000;
  household.erase(household.begin() + 2); // the room itself is unreachable
  send(CmdType::SetRoom, "10.0.0.12");
  engine_set_active(true);
  run_for(200);
  calls.clear();
  send(CmdType::Join, "10.0.0.14");
  CHECK(count_calls("x-rincon:") == 0);
}

static void test_controls_reach_the_whole_group() {
  boot("10.0.0.11"); // Kitchen, a member
  send(CmdType::PlayUri, "", STATIONS[0]);
  CHECK(audible() == "Kitchen,Living Room");
  send(CmdType::Pause);
  CHECK(audible().empty());
  send(CmdType::Play);
  CHECK(audible() == "Kitchen,Living Room");
  send(CmdType::Stop);
  run_for(FADE_OUT_MS + 100);
  CHECK(audible().empty());
  CHECK(count_calls("10.0.0.11 ") == 0);
}

static void test_engine_volume_gestures() {
  boot("10.0.0.11");
  send(CmdType::Volume, "", "", 30);
  clock_ms += 300;
  send(CmdType::Volume, "", "", 32);
  clock_ms += 300;
  send(CmdType::Volume, "", "", 34);
  CHECK(count_calls("SnapshotGroupVolume") == 1);
  CHECK(count_calls("10.0.0.10 SetGroupVolume") == 3);

  clock_ms += VOLUME_GESTURE_GAP_MS;
  send(CmdType::Volume, "", "", 20);
  CHECK(count_calls("SnapshotGroupVolume") == 2);
  run_for(POLL_MS);
  CHECK(published.back().volume == 20);
}

static void test_track_skip_publishes_soon() {
  boot("10.0.0.10");
  run_for(POLL_MS / 2);
  size_t before = published.size();
  send(CmdType::Next);
  run_for(TRACK_CHANGE_SETTLE_MS + 100);
  CHECK(published.size() == before + 1);
}

static constexpr const char *ALARM_URL = "http://10.0.0.50:8080/alarm.mp3";

static void ring(const char *room) {
  Command cmd = {};
  cmd.type = CmdType::AlarmStart;
  snprintf(cmd.uri, sizeof(cmd.uri), "%s", ALARM_URL);
  snprintf(cmd.room, sizeof(cmd.room), "%s", room);
  engine_handle(cmd, clock_ms);
  engine_tick(clock_ms);
}

// The knob controls the Kid's Room; the timer rings in the Kitchen, which
// plays the radio with Living Room. Both hear the chime, then the radio.
static void test_alarm_takes_over_the_kitchen_then_resumes_its_music() {
  boot("10.0.0.12");
  Speaker *lr = by_ip("10.0.0.10");
  lr->uri = "x-rincon-mp3radio://http://cdn0.example.net/p1_mp3_h";
  lr->state = "PLAYING";
  lr->volume = 12;
  run_for(POLL_MS);

  ring("kitchen");
  CHECK(lr->uri == ALARM_URL);
  CHECK(audible() == "Kitchen,Living Room");
  CHECK(lr->volume == ALARM_VOLUME);
  CHECK(count_calls("10.0.0.11 ") == 0); // through Kitchen's coordinator
  size_t before = published.size();
  run_for(POLL_MS * 2);
  CHECK(published.size() > before); // the knob's own room still updates

  send(CmdType::Volume, "", "", 40); // the knob's room, not the chime
  CHECK(by_ip("10.0.0.12")->volume == 40);
  CHECK(lr->uri == ALARM_URL);

  send(CmdType::AlarmStop);
  CHECK(lr->uri == "x-rincon-mp3radio://http://cdn0.example.net/p1_mp3_h");
  CHECK(audible() == "Kitchen,Living Room");
  CHECK(lr->volume == 12);
}

static void test_alarm_in_the_controlled_room_hides_the_chime() {
  boot("10.0.0.11"); // the knob controls the Kitchen itself
  send(CmdType::PlayUri, "", STATIONS[0]);
  run_for(POLL_MS);
  ring("Kitchen");
  size_t before = published.size();
  run_for(POLL_MS * 2);
  CHECK(published.size() == before); // the chime isn't the music
  send(CmdType::AlarmStop);
  run_for(POLL_MS);
  CHECK(published.size() > before);
  CHECK(published.back().station_index == 0);
}

static void test_alarm_without_that_room_rings_in_the_controlled_one() {
  boot("10.0.0.14");
  ring("Garage");
  CHECK(by_ip("10.0.0.14")->uri == ALARM_URL);
  CHECK(audible() == "TV Room");
  send(CmdType::AlarmStop);
}

static void test_alarm_puts_a_paused_queue_back_where_it_was() {
  boot("10.0.0.12");
  Speaker *lr = by_ip("10.0.0.10");
  lr->uri = "x-rincon-queue:RINCON_LR#0";
  lr->uri_metadata = "<DIDL-Lite id=\"q\">R&amp;B</DIDL-Lite>";
  lr->track = 3;
  lr->rel_time = "0:01:23";
  lr->state = "PAUSED_PLAYBACK";

  ring("Kitchen");
  lr->track = 1;
  lr->rel_time = "0:00:04";
  send(CmdType::AlarmStop);
  CHECK(lr->uri == "x-rincon-queue:RINCON_LR#0");
  CHECK(lr->uri_metadata == "<DIDL-Lite id=\"q\">R&amp;B</DIDL-Lite>");
  CHECK(lr->track == 3);
  CHECK(lr->rel_time == "0:01:23");
  CHECK(audible().empty()); // it was paused, so it stays quiet
}

static void test_alarm_gives_up_after_a_minute() {
  boot("10.0.0.12");
  ring("Kitchen");
  CHECK(audible() == "Kitchen,Living Room");
  run_for(ALARM_MAX_MS + 200);
  CHECK(audible().empty());
  CHECK(by_ip("10.0.0.10")->uri == "x-rincon-queue:RINCON_LR#0");
}

static void test_playing_during_the_alarm_ends_it_first() {
  boot("10.0.0.12");
  ring("Kid's Room");
  send(CmdType::PlayUri, "", STATIONS[1]);
  CHECK(by_ip("10.0.0.12")->uri.find("jazz") != std::string::npos);
  CHECK(audible() == "Kid's Room");
  send(CmdType::AlarmStop); // nothing left to put back
  CHECK(audible() == "Kid's Room");
}

static void test_group_label_and_toggle_helpers() {
  SonosGroup g = {};
  char label[160];
  sonos_group_label(g, label, sizeof(label));
  CHECK(strcmp(label, "") == 0);
  CHECK(sonos_group_toggle(g, "RINCON_X") == GroupToggle::None);

  snprintf(g.room_uuid, sizeof(g.room_uuid), "RINCON_A");
  snprintf(g.coordinator_uuid, sizeof(g.coordinator_uuid), "RINCON_B");
  sonos_group_apply(&g, GroupToggle::Join, "RINCON_A", "Office");
  sonos_group_apply(&g, GroupToggle::Join, "RINCON_B", "Hall");
  sonos_group_apply(&g, GroupToggle::Join, "RINCON_B", "Hall");
  CHECK(g.count == 2);
  sonos_group_label(g, label, sizeof(label));
  CHECK(strcmp(label, "Office + Hall") == 0);

  SonosGroup reordered = {};
  sonos_group_apply(&reordered, GroupToggle::Join, "RINCON_B", "Hall");
  CHECK(!sonos_group_same_members(g, reordered));
  sonos_group_apply(&reordered, GroupToggle::Join, "RINCON_A", "Office");
  CHECK(sonos_group_same_members(g, reordered));

  sonos_group_apply(&g, GroupToggle::Leave, "RINCON_B", "Hall");
  sonos_group_label(g, label, sizeof(label));
  CHECK(strcmp(label, "Office") == 0);

  sonos_group_apply(&g, GroupToggle::Join, "RINCON_B", "Hall");
  sonos_group_apply(&g, GroupToggle::Leave, "RINCON_A", "Office");
  CHECK(g.count == 1);
  CHECK(strcmp(g.coordinator_uuid, "RINCON_A") == 0);
}

int main() {
  test_topology_parse();
  test_member_refuses_transport();
  test_member_room_plays_through_coordinator();
  test_poll_reports_group_and_station();
  test_join_uses_the_groups_coordinator();
  test_leave_shrinks_the_group();
  test_leaving_room_retargets_to_itself();
  test_select_moves_playback_between_groups();
  test_select_member_of_playing_group_plays_it_alone();
  test_select_coordinator_keeps_playing_and_sends_others_away();
  test_select_leaves_someone_elses_playing_group_alone();
  test_select_breaks_up_a_silent_group();
  test_group_volume();
  test_spotify_track();
  test_tv_has_no_skip_or_pause();
  test_source_names();
  test_station_uris();
  test_transport_actions();
  test_oversized_response_is_cut_not_spliced();

  test_engine_idle_until_started();
  test_engine_stop_halts_polling_and_drops_commands();
  test_engine_restart_sees_regrouping_done_while_offline();
  test_engine_notices_regrouping_while_running();
  test_engine_polls_on_schedule();
  test_engine_unreachable_room_publishes_nothing();
  test_switch_room_stops_the_old_group();
  test_switch_to_a_member_of_another_playing_group();
  test_switch_room_within_group_plays_it_alone();
  test_coming_back_to_a_grouped_room_plays_it_alone();
  test_stop_fades_out_then_restores_volume();
  test_play_during_fade_calls_the_stop_off();
  test_turning_during_fade_finishes_the_stop();
  test_switch_room_without_resume_leaves_both_alone();
  test_switch_room_while_stopped_plays_nothing();
  test_long_press_joins_a_room_to_the_playing_group();
  test_regroup_rereads_topology_soon();
  test_long_press_coordinator_of_other_group();
  test_long_press_again_removes_the_room();
  test_long_press_own_room_leaves_and_keeps_others_playing();
  test_long_press_own_room_alone_does_nothing();
  test_join_with_unknown_coordinator_sends_nothing();
  test_controls_reach_the_whole_group();
  test_engine_volume_gestures();
  test_track_skip_publishes_soon();
  test_alarm_takes_over_the_kitchen_then_resumes_its_music();
  test_alarm_in_the_controlled_room_hides_the_chime();
  test_alarm_without_that_room_rings_in_the_controlled_one();
  test_alarm_puts_a_paused_queue_back_where_it_was();
  test_alarm_gives_up_after_a_minute();
  test_playing_during_the_alarm_ends_it_first();
  test_group_label_and_toggle_helpers();

  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("knob_sonos: all tests passed\n");
  return 0;
}
