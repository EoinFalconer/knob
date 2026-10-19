// Home-screen and speaker-picker decisions, without LVGL.
#include "../main/ui/face_palette.h"
#include "../main/ui/now_playing.h"
#include "../main/ui/weather_kind.h"
#include "art_tint.h"
#include "sonos_group.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static constexpr uint8_t SPOTIFY_PLAYING =
    ACTION_STOP | ACTION_PAUSE | ACTION_NEXT | ACTION_PREVIOUS;
static constexpr uint8_t SPOTIFY_PAUSED =
    ACTION_STOP | ACTION_PLAY | ACTION_NEXT | ACTION_PREVIOUS;
static constexpr uint8_t TV = ACTION_STOP;

static MediaInfo media(const char *title, const char *artist,
                       const char *source) {
  MediaInfo m = {};
  snprintf(m.title, sizeof(m.title), "%s", title);
  snprintf(m.artist, sizeof(m.artist), "%s", artist);
  snprintf(m.source, sizeof(m.source), "%s", source);
  m.has_media = true;
  return m;
}

static SonosGroup group_of(std::initializer_list<const char *> names) {
  SonosGroup g = {};
  for (const char *name : names) {
    snprintf(g.member_uuids[g.count], sizeof(g.member_uuids[0]), "RINCON_%s",
             name);
    snprintf(g.member_names[g.count], sizeof(g.member_names[0]), "%s", name);
    g.count++;
  }
  snprintf(g.room_uuid, sizeof(g.room_uuid), "%s", g.member_uuids[0]);
  return g;
}

static void test_spotify_controls() {
  MediaControls playing =
      media_controls(true, PlayState::Playing, SPOTIFY_PLAYING);
  CHECK(playing.prev && playing.next && playing.toggle);
  CHECK(!playing.play_badge);

  MediaControls paused =
      media_controls(true, PlayState::Paused, SPOTIFY_PAUSED);
  CHECK(paused.toggle && paused.play_badge);
}

static void test_tv_and_line_in_get_no_controls() {
  MediaControls tv = media_controls(true, PlayState::Playing, TV);
  CHECK(!tv.prev && !tv.next && !tv.toggle && !tv.play_badge);
}

static void test_radio_has_no_media_controls() {
  MediaControls radio =
      media_controls(false, PlayState::Playing, SPOTIFY_PLAYING);
  CHECK(!radio.prev && !radio.next && !radio.toggle);
}

static void test_controls_unknown_until_polled() {
  MediaControls fresh = media_controls(true, PlayState::Playing, 0);
  CHECK(!fresh.prev && !fresh.next && !fresh.toggle);
}

static void test_subtitles() {
  MediaInfo song = media("Kashmir", "Led Zeppelin", "Spotify");
  CHECK(strcmp(now_playing_subtitle(true, song, PlayState::Playing),
               "Led Zeppelin") == 0);
  CHECK(strcmp(now_playing_subtitle(true, song, PlayState::Paused), "Paused") ==
        0);

  MediaInfo podcast = media("Episode 12", "", "Apple Music");
  CHECK(strcmp(now_playing_subtitle(true, podcast, PlayState::Playing),
               "Apple Music") == 0);

  MediaInfo tv = media("TV", "", "TV");
  CHECK(strcmp(now_playing_subtitle(true, tv, PlayState::Playing), "Playing") ==
        0);

  MediaInfo none = {};
  CHECK(strcmp(now_playing_subtitle(false, none, PlayState::Playing),
               "Playing") == 0);
  CHECK(strcmp(now_playing_subtitle(false, none, PlayState::Stopped),
               "Tap to browse stations") == 0);
  CHECK(strcmp(now_playing_subtitle(false, none, PlayState::Transitioning),
               "Loading…") == 0);
}

static void test_art_placeholders() {
  CHECK(art_placeholder(media("TV", "", "TV")) == ArtPlaceholder::Tv);
  CHECK(art_placeholder(media("Line-In", "", "Line-In")) ==
        ArtPlaceholder::LineIn);
  CHECK(art_placeholder(media("x", "", "AirPlay")) == ArtPlaceholder::AirPlay);
  CHECK(art_placeholder(media("Kashmir", "Led Zeppelin", "Spotify")) ==
        ArtPlaceholder::Music);
}

static void test_speaker_line_names_the_group() {
  char line[160];
  speaker_line(SonosGroup{}, "Kitchen", line, sizeof(line));
  CHECK(strcmp(line, "Kitchen") == 0); // before the first poll

  speaker_line(group_of({"Kitchen"}), "Kitchen", line, sizeof(line));
  CHECK(strcmp(line, "Kitchen") == 0);

  speaker_line(group_of({"Kitchen", "Living Room"}), "Kitchen", line,
               sizeof(line));
  CHECK(strcmp(line, "Kitchen + Living Room") == 0);

  speaker_line(group_of({"Kitchen", "Living Room", "Office", "Bath"}),
               "Kitchen", line, sizeof(line));
  CHECK(strcmp(line, "Kitchen + 3") == 0);

  speaker_line(SonosGroup{}, nullptr, line, sizeof(line));
  CHECK(strcmp(line, "") == 0);
}

static void test_switching_rooms_moves_only_our_radio() {
  const char *url = "https://radio.example/p3";
  CHECK(switch_resume_url(false, PlayState::Playing, url) == url);
  CHECK(switch_resume_url(false, PlayState::Transitioning, url) == url);
  CHECK(switch_resume_url(false, PlayState::Stopped, url) == nullptr);
  CHECK(switch_resume_url(false, PlayState::Paused, url) == nullptr);
  // Someone else's Spotify or the TV stays where it is.
  CHECK(switch_resume_url(true, PlayState::Playing, url) == nullptr);
}

static SonosSpeaker spk(const char *name, const char *uuid,
                        const char *coordinator) {
  SonosSpeaker s = {};
  snprintf(s.name, sizeof(s.name), "%s", name);
  snprintf(s.uuid, sizeof(s.uuid), "%s", uuid);
  snprintf(s.coordinator_uuid, sizeof(s.coordinator_uuid), "%s", coordinator);
  return s;
}

static void test_group_from_discovery() {
  DiscoveryResult scan = {};
  scan.speakers[scan.count++] = spk("Living Room", "RINCON_LR", "RINCON_LR");
  scan.speakers[scan.count++] = spk("Kitchen", "RINCON_K", "RINCON_LR");
  scan.speakers[scan.count++] = spk("Office", "RINCON_O", "RINCON_O");

  SonosGroup g = group_from_discovery(scan, "Kitchen");
  CHECK(g.count == 2);
  CHECK(strcmp(g.room_uuid, "RINCON_K") == 0);
  CHECK(strcmp(g.member_names[0], "Kitchen") == 0); // controlled room first
  CHECK(sonos_group_has(g, "RINCON_LR"));
  CHECK(!sonos_group_has(g, "RINCON_O"));

  // Long-press semantics in the picker built from that scan.
  CHECK(sonos_group_toggle(g, "RINCON_O") == GroupToggle::Join);
  CHECK(sonos_group_toggle(g, "RINCON_LR") == GroupToggle::Leave);
  CHECK(sonos_group_toggle(g, "RINCON_K") == GroupToggle::Leave);

  CHECK(group_from_discovery(scan, "Garage").count == 0);
  CHECK(group_from_discovery(scan, "").count == 0);

  // The SSDP-only fallback has no UUIDs: no group, so no grouping offered.
  DiscoveryResult fallback = {};
  fallback.speakers[fallback.count++] = spk("Den", "", "");
  SonosGroup none = group_from_discovery(fallback, "Den");
  CHECK(none.count == 0);
  CHECK(sonos_group_toggle(none, "") == GroupToggle::None);
}

// Byte-swapped RGB565, like art_decode_jpeg produces.
static void fill(std::vector<uint8_t> &img, int from, int to, int r, int g,
                 int b) {
  uint16_t v = static_cast<uint16_t>((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
  for (int i = from; i < to; i++) {
    img[static_cast<size_t>(i) * 2] = static_cast<uint8_t>(v >> 8);
    img[static_cast<size_t>(i) * 2 + 1] = static_cast<uint8_t>(v & 0xFF);
  }
}

static int channel(uint32_t rgb, int shift) { return (rgb >> shift) & 0xFF; }

static void test_artwork_tint() {
  constexpr int N = 120 * 120;
  std::vector<uint8_t> img(N * 2);

  fill(img, 0, N, 200, 30, 30); // red sleeve
  uint32_t red = artwork_tint(img.data(), N);
  CHECK(channel(red, 16) == 0x28); // darkened to the station palette depth
  CHECK(channel(red, 16) > 3 * channel(red, 0));

  // A small vivid patch beats a large white area, like Apple's backdrop.
  fill(img, 0, N, 245, 245, 245);
  fill(img, 0, N / 10, 20, 60, 220);
  uint32_t blue = artwork_tint(img.data(), N);
  CHECK(channel(blue, 0) == 0x28);
  CHECK(channel(blue, 0) > 2 * channel(blue, 16));

  fill(img, 0, N, 0, 0, 0); // black sleeve: neutral, never pitch black
  CHECK(artwork_tint(img.data(), N) == 0x141414);

  fill(img, 0, N, 128, 128, 128); // gray stays gray (RGB565 green rounds)
  uint32_t gray = artwork_tint(img.data(), N);
  CHECK(std::abs(channel(gray, 16) - channel(gray, 8)) <= 1);
  CHECK(channel(gray, 16) == channel(gray, 0));
}

// met.no codes nest words in each other ("partlycloudy" holds "cloudy",
// "rainshowersandthunder" holds "rain"), so the most specific must win.
static void test_weather_kinds() {
  struct Case {
    const char *symbol;
    WeatherKind kind;
  };
  const Case cases[] = {
      {"clearsky_day", WeatherKind::Clear},
      {"fair_night", WeatherKind::Fair},
      {"partlycloudy_day", WeatherKind::PartlyCloudy},
      {"cloudy", WeatherKind::Cloudy},
      {"fog", WeatherKind::Fog},
      {"lightrain", WeatherKind::Drizzle},
      {"heavyrain", WeatherKind::Rain},
      {"lightrainshowers_day", WeatherKind::Showers},
      {"rainshowersandthunder_night", WeatherKind::Thunder},
      {"lightsnowshowers_day", WeatherKind::SnowShowers},
      {"heavysnow", WeatherKind::Snow},
      {"lightsleetshowers_polartwilight", WeatherKind::Sleet},
  };
  for (const Case &c : cases) {
    if (weather_kind(c.symbol) != c.kind)
      std::printf("  %s\n", c.symbol);
    CHECK(weather_kind(c.symbol) == c.kind);
  }

  CHECK(weather_is_night("clearsky_night", false));
  CHECK(!weather_is_night("clearsky_polartwilight", true));
  CHECK(weather_is_night("rain", true)); // no suffix: the caller's guess
  CHECK(!weather_is_night("rain", false));
}

// The clock's colors follow the real sun when it's known; bedtime always goes
// dark, even under a midsummer sun.
static void test_clock_follows_the_sun() {
  auto at = [](int h, int m = 0) { return h * 60 + m; };
  SunClock oslo_october{at(8), at(18, 2)};
  CHECK(day_part(at(7, 30), oslo_october) == DayPart::Night);
  CHECK(day_part(at(9), oslo_october) == DayPart::Morning);
  CHECK(day_part(at(13), oslo_october) == DayPart::Day);
  CHECK(day_part(at(17), oslo_october) == DayPart::Evening);
  CHECK(day_part(at(18, 30), oslo_october) == DayPart::Night);

  SunClock midsummer{at(3, 50), at(22, 45)};
  CHECK(day_part(at(22, 30), midsummer) == DayPart::Evening);
  CHECK(day_part(at(23, 10), midsummer) == DayPart::Night);

  bool down = true;
  SunClock symbols_only;
  symbols_only.sun_down = &down;
  CHECK(day_part(at(16), symbols_only) == DayPart::Night);
  CHECK(day_part(at(7), SunClock{}) == DayPart::Morning);
}

int main() {
  test_weather_kinds();
  test_clock_follows_the_sun();
  test_artwork_tint();
  test_spotify_controls();
  test_tv_and_line_in_get_no_controls();
  test_radio_has_no_media_controls();
  test_controls_unknown_until_polled();
  test_subtitles();
  test_art_placeholders();
  test_speaker_line_names_the_group();
  test_switching_rooms_moves_only_our_radio();
  test_group_from_discovery();

  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("radio now_playing: all tests passed\n");
  return 0;
}
