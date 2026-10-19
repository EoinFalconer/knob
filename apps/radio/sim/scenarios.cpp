// Walks the radio UI through its states. Screenshots land in the output
// directory; redraw cost per interaction prints to stdout.
#include "sim.h"

#include "app_config.h"
#include "discovery.h"
#include "sonos_config.h"
#include "ui/ui.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>

static constexpr int CX = 180;
static constexpr int CY = 180;

static MediaInfo media(const char *title, const char *artist,
                       const char *source, const char *art) {
  MediaInfo m = {};
  snprintf(m.title, sizeof(m.title), "%s", title);
  snprintf(m.artist, sizeof(m.artist), "%s", artist);
  snprintf(m.source, sizeof(m.source), "%s", source);
  snprintf(m.art_url, sizeof(m.art_url), "%s", art);
  m.has_media = true;
  return m;
}

static SonosGroup group(std::initializer_list<const char *> names) {
  SonosGroup g = {};
  for (const char *n : names) {
    snprintf(g.member_uuids[g.count], sizeof(g.member_uuids[0]), "RINCON_%s",
             n);
    snprintf(g.member_names[g.count], sizeof(g.member_names[0]), "%s", n);
    g.count++;
  }
  snprintf(g.room_uuid, sizeof(g.room_uuid), "%s", g.member_uuids[0]);
  snprintf(g.coordinator_uuid, sizeof(g.coordinator_uuid), "%s",
           g.member_uuids[0]);
  return g;
}

// A group as the speakers would report it, from rows of a scan; the first
// row is the controlled room.
static SonosGroup scan_group(const DiscoveryResult &scan,
                             std::initializer_list<int> rows) {
  SonosGroup g = {};
  for (int i : rows) {
    snprintf(g.member_uuids[g.count], sizeof(g.member_uuids[0]), "%s",
             scan.speakers[i].uuid);
    snprintf(g.member_names[g.count], sizeof(g.member_names[0]), "%s",
             scan.speakers[i].name);
    g.count++;
  }
  snprintf(g.room_uuid, sizeof(g.room_uuid), "%s", g.member_uuids[0]);
  snprintf(g.coordinator_uuid, sizeof(g.coordinator_uuid), "%s",
           g.member_uuids[0]);
  return g;
}

static void speaker(DiscoveryResult *r, const char *name, const char *uuid,
                    const char *coordinator) {
  auto &s = r->speakers[r->count++];
  snprintf(s.name, sizeof(s.name), "%s", name);
  snprintf(s.uuid, sizeof(s.uuid), "%s", uuid);
  snprintf(s.coordinator_uuid, sizeof(s.coordinator_uuid), "%s", coordinator);
  snprintf(s.ip, sizeof(s.ip), "10.0.0.%d", r->count + 10);
  s.port = 1400;
}

static void dump(lv_obj_t *obj, int depth = 0) {
  for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
    lv_obj_t *c = lv_obj_get_child(obj, static_cast<int32_t>(i));
    lv_area_t a;
    lv_obj_get_coords(c, &a);
    std::printf("%*s%s '%s' opa=%d hidden=%d (%d,%d %dx%d)\n", depth * 2, "",
                lv_obj_check_type(c, &lv_label_class) ? "label" : "obj",
                lv_obj_check_type(c, &lv_label_class) ? lv_label_get_text(c)
                                                      : "",
                lv_obj_get_style_opa(c, LV_PART_MAIN),
                lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN), static_cast<int>(a.x1),
                static_cast<int>(a.y1), static_cast<int>(lv_area_get_width(&a)),
                static_cast<int>(lv_area_get_height(&a)));
    dump(c, depth + 1);
  }
}

void sim_dump() { dump(lv_screen_active()); }

static void weather(const char *symbol = "partlycloudy_day",
                    const char *condition = "Partly cloudy", float temp = 12.4f,
                    bool night = false) {
  WeatherData w = {};
  w.temperature = temp;
  w.high = temp + 3;
  w.low = temp - 5;
  w.wind_speed = 4.2f;
  w.humidity = 76;
  w.sunrise = 8 * 60;
  w.sunset = 18 * 60 + 2;
  snprintf(w.condition, sizeof(w.condition), "%s", condition);
  snprintf(w.symbol, sizeof(w.symbol), "%s", symbol);
  w.night = night;
  w.valid = true;
  ui_set_weather(&w);
}

// Moves local time to the given hour by picking a timezone, since the faces
// read the real clock.
static void set_local_hour(int hour) {
  time_t now = time(nullptr);
  tm utc = {};
  gmtime_r(&now, &utc);
  int offset = ((hour - utc.tm_hour) % 24 + 24 + 12) % 24 - 12;
  char tz[16];
  snprintf(tz, sizeof(tz), "SIM%+d", -offset);
  setenv("TZ", tz, 1);
  tzset();
}

// The clock face at each part of the day.
static void palette_gallery() {
  const struct {
    int hour;
    const char *shot;
  } parts[] = {{8, "clock-morning"},
               {13, "clock-day"},
               {17, "clock-evening"},
               {23, "clock-night"}};
  for (const auto &p : parts) {
    set_local_hour(p.hour);
    weather(); // a weather update rechecks the time of day
    sim_run(1400);
    sim_screenshot(p.shot);
  }
  unsetenv("TZ");
  tzset();
  weather();
}

// One frame of the weather face per kind of sky.
static void weather_gallery() {
  struct Look {
    const char *symbol, *condition, *shot;
    float temp;
    bool night;
  };
  const Look looks[] = {
      {"clearsky_day", "Clear", "wx-clear-day", 21, false},
      {"cloudy", "Cloudy", "wx-cloudy", 9, false},
      {"heavyrain", "Heavy rain", "wx-rain", 7, false},
      {"rainandthunder", "Thunder", "wx-thunder", 14, false},
      {"snow", "Snow", "wx-snow", -3, false},
      {"snow", "Snow", "wx-snow-cold", -12, false},
      {"fog", "Fog", "wx-fog", 4, false},
      {"clearsky_night", "Clear", "wx-clear-night", 6, true},
      {"partlycloudy_night", "Partly cloudy", "wx-cloudy-night", 8, true},
  };
  for (const Look &l : looks) {
    weather(l.symbol, l.condition, l.temp, l.night);
    sim_run(100);
    sim_screenshot(l.shot);
  }
  weather();
}

// Swiped sideways, the timer sheet shows the lights; swiped back, the timer
// as it was left.
static void lights_scenarios() {
  auto swipe = [](const char *film, int32_t from_x, int32_t to_x) {
    sim_touch_down(from_x, CY + 40);
    sim_touch_move(to_x, CY + 46, 140);
    sim_touch_up();
    char shot[48];
    for (int i = 0; i < 12; i++) {
      snprintf(shot, sizeof(shot), "film-%s-%02d", film, i);
      sim_screenshot(shot);
      sim_run(40);
    }
  };
  sim_mark("lights: swipe to the lights page");
  swipe("lights-in", 290, 70);
  sim_run(300);
  sim_screenshot("14a-lights-on");

  sim_mark("lights: knob dims them");
  for (int i = 0; i < 4; i++) {
    sim_turn(-1);
    sim_run(80);
  }
  sim_run(300);
  sim_screenshot("14b-lights-dimmed");
  // The fill's edge runs through the percent: each part takes the color of
  // what's under it.
  for (int i = 0; i < 24; i++) {
    sim_turn(1);
    sim_run(40);
  }
  sim_run(300);
  sim_screenshot("14b1-lights-edge-in-the-percent");
  for (int i = 0; i < 24; i++) {
    sim_turn(-1);
    sim_run(40);
  }
  sim_run(300);

  sim_mark("lights: tap switches them off");
  sim_tap(CX, CY);
  sim_run(400);
  sim_screenshot("14c-lights-off");
  sim_tap(CX, CY);
  sim_run(400);
  sim_screenshot("14d-lights-back-on");

  LightsState was = sim_lights;
  sim_lights.status = LightsStatus::Unset;
  ui_set_lights(&sim_lights);
  sim_run(300);
  sim_screenshot("14e-lights-not-set-up");
  sim_lights = was;
  ui_set_lights(&sim_lights);

  sim_mark("lights: swipe back to the timer");
  swipe("lights-out", 70, 290);
  sim_run(300);
  sim_screenshot("14f-back-to-the-timer");
}

// The timer, from a swipe down to answering it.
static void timer_scenarios() {
  sim_mark("timer: swipe down");
  sim_touch_down(CX, 30);
  sim_touch_move(CX, 200, 160);
  sim_screenshot("13a0-timer-sheet-following-finger");
  sim_touch_move(CX, 250, 40);
  sim_touch_up();
  sim_run(500);
  sim_screenshot("13a-timer-set");

  sim_mark("timer: knob sets 12 min");
  for (int i = 0; i < 7; i++) {
    sim_turn(1);
    sim_run(60);
  }
  sim_screenshot("13b0-timer-setting-mid-slide");
  sim_run(300);
  sim_screenshot("13b-timer-12-min");
  lights_scenarios();

  // Starting winds the ring up, then the sheet fades over the timer face,
  // which shows the same dial: only the frost may change, not the numbers.
  sim_mark("timer: tap to start, 3 s");
  sim_tap(CX, CY);
  sim_run(150);
  sim_screenshot("13c0-timer-winding-up");
  sim_run(340);
  sim_screenshot("13c1-timer-dissolving");
  sim_run(2510);
  sim_screenshot("13d-timer-on-the-clock");

  sim_mark("timer: running, IDLE_MS + 1 s");
  sim_run(IDLE_MS + 1000);
  std::printf("  backlight with a timer running, untouched: %d\n",
              sim_backlight());

  // Browsing with nothing playing still shows the countdown.
  sim_tap(CX, CY);
  sim_run(400);
  sim_screenshot("13d1-timer-browse");
  sim_tap(CX, CY, 700);
  sim_run(600);

  // Brought back while it runs, the whole sheet is a cancel button.
  sim_mark("timer: swipe down while running");
  sim_touch_down(CX, 30);
  sim_touch_move(CX, 170, 120);
  sim_screenshot("13c-0-timer-cancel-mid-drag");
  sim_touch_move(CX, 250, 80);
  sim_touch_up();
  sim_run(500);
  sim_screenshot("13c-timer-running-cancel");
  sim_touch_down(CX, 330);
  sim_touch_move(CX, 120, 150);
  sim_touch_up();
  sim_run(500);

  sim_mark("timer: pill while playing");
  ui_set_play_state(PlayState::Playing);
  sim_run(800);
  sim_screenshot("13e-timer-pill");

  sim_mark("timer: rings");
  ui_timer_fired("Pasta");
  sim_run(600);
  sim_screenshot("13f-timer-ringing");
  sim_tap(CX + 52, CY + 62); // Stop
  // On its way out the sheet still rings, rather than flashing the setting.
  sim_run(60);
  sim_screenshot("13g0-timer-stop-leaving");
  sim_run(540);
  std::printf("  alarm still sounding after Stop: %s\n",
              sim_alarm_sounding ? "yes" : "no");
  sim_screenshot("13g-timer-stopped");
  ui_set_play_state(PlayState::Stopped);
  sim_run(IDLE_MS + 1000);
  std::printf("  backlight once the timer is done: %d\n", sim_backlight());
}

void sim_scenarios(const char *art_path) {
  ui_init();
  sim_run(250);
  sim_screenshot("00a-splash-250ms");
  sim_run(650);
  sim_screenshot("00b-splash");
  sim_run(1100); // splash out
  ui_set_speaker_name("Living Room");
  ui_set_group(nullptr);
  weather();
  sim_run(600);
  sim_screenshot("01-idle");

  // The faces went up at ui_init, so the first change lands 20 s after it.
  sim_mark("idle, clock face");
  sim_run(20000 - 2600 - 20);

  sim_mark("face change: clock to weather");
  sim_run(20 + 80);
  sim_screenshot("01b-face-change-80ms");
  sim_run(120);
  sim_screenshot("01c-face-change-200ms");
  sim_run(150);
  sim_screenshot("01d-face-change-350ms");
  sim_run(1000);
  sim_screenshot("01e-weather-face");
  weather_gallery();

  sim_mark("weather face, 8 s");
  sim_run(8000);
  sim_run(2000); // back to the clock
  sim_mark("clock colors change with the day");
  palette_gallery();

  // Swiping sideways brings up the other face at once, either way.
  auto swipe_film = [](const char *name, int32_t from_x, int32_t to_x) {
    sim_touch_down(from_x, 190);
    sim_touch_move(to_x, 200, 120);
    sim_touch_up();
    char shot[48];
    for (int i = 0; i < 16; i++) {
      snprintf(shot, sizeof(shot), "film-%s-%02d", name, i);
      sim_screenshot(shot);
      sim_run(40);
    }
  };
  sim_mark("faces: swipe left to the weather");
  swipe_film("swipe-left", 280, 90);
  sim_screenshot("01j-swiped-to-weather");
  sim_mark("faces: swipe right back to the clock");
  swipe_film("swipe-right", 90, 280);
  sim_screenshot("01k-swiped-to-clock");
  sim_run(1000);

  sim_mark("volume on the clock face");
  sim_turn(1);
  sim_run(90);
  sim_screenshot("01f0-clock-volume-enter-90ms");
  sim_turn(1);
  sim_run(60);
  sim_screenshot("01f1-clock-numerals-wobble");
  for (int i = 0; i < 3; i++) {
    sim_turn(1);
    sim_run(100);
  }
  sim_screenshot("01f-clock-volume");
  sim_mark("clock volume: retract");
  sim_run(1400 + 200);
  sim_screenshot("01g-clock-volume-retract-200ms");
  sim_run(200);
  sim_screenshot("01h-clock-volume-retract-400ms");
  sim_run(1000);

  // A fast spin, two detents every 20 ms poll for a second: the numerals
  // should lean and hold, not shake.
  sim_mark("clock: fast spin, 1 s");
  for (int i = 0; i < 50; i++) {
    sim_turn(2);
    sim_run(20);
  }
  sim_screenshot("01i-clock-fast-spin");
  sim_mark("clock: spin let go");
  sim_run(1500);
  sim_run(1500);

  sim_mark("start playing radio");
  ui_set_group(nullptr);
  SonosGroup g = group({"Living Room", "Kitchen"});
  ui_set_group(&g);
  ui_set_play_state(PlayState::Playing);
  sim_run(800);
  sim_screenshot("02-radio-playing");

  sim_mark("radio playing, 10 s");
  sim_run(10000);

  // The timer comes down over now playing too.
  sim_touch_down(CX, 30);
  sim_touch_move(CX, 250, 200);
  sim_touch_up();
  sim_run(500);
  sim_screenshot("02a-timer-over-radio");
  sim_touch_down(CX, 330);
  sim_touch_move(CX, 120, 150);
  sim_touch_up();
  sim_run(500);

  sim_mark("volume: 8 detents in 0.8 s");
  for (int i = 0; i < 8; i++) {
    sim_turn(1);
    sim_run(100);
  }
  sim_screenshot("03-volume");
  sim_mark("volume: settle");
  sim_run(1400 + 150);
  sim_screenshot("03a-volume-settling-150ms");
  sim_run(950);

  sim_mark("browse: open + 3 stations");
  sim_tap(CX, CY + 100);
  sim_run(300);
  sim_screenshot("04-browse");
  for (int i = 0; i < 3; i++) {
    sim_turn(1);
    sim_run(i == 2 ? 50 : 400);
  }
  sim_screenshot("05a-browse-sliding-50ms");
  sim_run(350);
  sim_screenshot("05-browse-rotated");
  // The timer comes down over browse as well.
  sim_touch_down(CX, 30);
  sim_touch_move(CX, 250, 200);
  sim_touch_up();
  sim_run(500);
  sim_screenshot("05b-timer-over-browse");
  sim_touch_down(CX, 330);
  sim_touch_move(CX, 120, 150);
  sim_touch_up();
  sim_run(500);
  sim_mark("browse: fast spin through 12 stations");
  for (int i = 0; i < 12; i++) {
    sim_turn(1);
    sim_run(30);
  }
  sim_run(400);

  sim_mark("confirm station (crossfade)");
  sim_tap(CX, CY + 100);
  sim_run(1200);
  sim_screenshot("06-new-station");

  sim_mark("spotify track arrives");
  ui_set_transport_actions(ACTION_STOP | ACTION_PAUSE | ACTION_NEXT |
                           ACTION_PREVIOUS);
  MediaInfo song =
      media("Kashmir (Remaster)", "Led Zeppelin", "Spotify", art_path);
  ui_set_media_info(&song);
  sim_run(1000);
  sim_screenshot("07-spotify");

  sim_mark("spotify playing, 10 s");
  sim_run(10000);

  sim_mark("long title playing, 10 s");
  MediaInfo long_song =
      media("The Rain Song (Remaster) - Live at Madison Square Garden",
            "Led Zeppelin", "Spotify", art_path);
  ui_set_media_info(&long_song);
  sim_run(1000);
  sim_screenshot("07b-long-title");
  for (int s = 1; s < 40; s += 3) {
    char label[40];
    snprintf(label, sizeof(label), "  title t=%ds..%ds", s, s + 3);
    sim_mark(label);
    sim_run(3000);
  }
  sim_screenshot("07c-long-title-rest");

  sim_mark("tap artwork to pause");
  sim_tap(CX, CY - 30);
  sim_run(400);
  sim_screenshot("08-spotify-paused");
  sim_run(2000);

  sim_mark("tv source");
  ui_set_play_state(PlayState::Playing);
  sim_run(1600);
  ui_set_transport_actions(ACTION_STOP);
  MediaInfo tv = media("TV", "", "TV", "");
  ui_set_media_info(&tv);
  sim_run(800);
  sim_screenshot("09-tv");

  sim_mark("stop (long press) to idle");
  sim_tap(CX, CY + 100, 700);
  sim_run(800);
  sim_screenshot("10-stopped");

  // Every face hands a tap to the radio, the weather face included: tap where
  // its facts row sits, which once swallowed taps.
  sim_mark("tap the weather face");
  sim_run(20000);
  sim_turn(-2);
  sim_run(150);
  sim_screenshot("10a-weather-volume");
  sim_run(2500);
  sim_screenshot("10b-weather-before-tap");
  sim_tap(CX, CY + 95);
  sim_run(400);
  sim_screenshot("10c-weather-tapped");
  sim_tap(CX, CY + 95, 700); // hold: back out of browse
  sim_run(600);

  // The sheet follows the finger, then keeps its speed when let go. With no
  // scan yet it opens on the spinner, which the first scan replaces.
  sim_mark("drag the speakers sheet up");
  sim_touch_down(CX, 330);
  sim_touch_move(CX, 200, 160);
  sim_screenshot("11a0-sheet-following-finger");
  sim_touch_move(CX, 150, 40);
  sim_touch_up();
  // Caught on its way up and pulled on past open, it stretches, and still
  // covers the screen to the bottom.
  sim_run(60);
  sim_touch_down(CX, 300);
  sim_touch_move(CX, 40, 200);
  sim_screenshot("11a0b-sheet-stretched");
  sim_touch_up();
  sim_mark("sheet settles open");
  sim_run(120);
  sim_screenshot("11a1-sheet-settling");
  sim_run(400);
  ui_show_scanning();
  sim_screenshot("11a-scanning");

  sim_mark("first scan lands");
  DiscoveryResult scan = {};
  speaker(&scan, "Living Room", "RINCON_LR", "RINCON_LR");
  speaker(&scan, "Kitchen", "RINCON_K", "RINCON_LR");
  speaker(&scan, "Bedroom", "RINCON_B", "RINCON_B");
  speaker(&scan, "Office", "RINCON_O", "RINCON_O");
  ui_show_speaker_picker(&scan);
  sim_run(300);
  sim_screenshot("11-speakers");
  if (sim_verbose)
    sim_dump();

  // Speakers acknowledge a join before their topology shows it, so the next
  // reports still carry the old group. The new room must stay checked.
  sim_mark("hold Bedroom to group it");
  sim_tap(CX, CY - 54, 700); // rows sort by name: Bedroom is on top
  sim_run(100);
  sim_screenshot("11b-speakers-grouped");
  SonosGroup before = scan_group(scan, {0, 1});
  sim_run(1600);
  ui_set_group(&before);
  sim_run(100);
  sim_screenshot("11c-speakers-stale-report");
  SonosGroup after = scan_group(scan, {0, 1, 2});
  ui_set_group(&after);
  sim_run(200);
  sim_turn(2);
  sim_run(400);
  sim_screenshot("12-speakers-scrolled");

  sim_mark("drag the sheet down to close");
  sim_touch_down(CX, 50);
  sim_touch_move(CX, 190, 200);
  sim_screenshot("12a0-sheet-dragged-down");
  sim_touch_move(CX, 230, 40);
  sim_touch_up();
  sim_run(500);
  sim_screenshot("12a-speakers-closed");

  // Reopening shows the last list at once; the rescan runs once it settles.
  sim_mark("reopen: cached list mid-drag");
  sim_touch_down(CX, 330);
  sim_touch_move(CX, 180, 150);
  sim_screenshot("12b0-sheet-cached-list");
  // A slow, short drag back down springs it to where it came from.
  sim_touch_move(CX, 200, 300);
  sim_run(300);
  sim_touch_up();
  sim_run(500);
  sim_screenshot("12b1-sheet-sprang-closed");

  sim_mark("catch the sheet mid-slide");
  sim_touch_down(CX, 330);
  sim_touch_move(CX, 120, 80);
  sim_touch_up();
  sim_run(60);
  sim_touch_down(CX, 250); // grabs the moving sheet
  sim_run(100);
  sim_screenshot("12b2-sheet-caught");
  sim_touch_move(CX, 330, 120);
  sim_touch_up();
  sim_run(500);
  sim_screenshot("12b3-sheet-thrown-closed");

  // Scan results that change the rooms land while the sheet still slides.
  sim_mark("rooms change mid-slide");
  sim_touch_down(CX, 330);
  sim_touch_move(CX, 150, 80);
  sim_touch_up();
  sim_run(40);
  DiscoveryResult more = scan;
  speaker(&more, "Bathroom", "RINCON_BA", "RINCON_BA");
  ui_show_speaker_picker(&more);
  sim_run(500);
  sim_screenshot("12c0-sheet-rooms-changed");

  sim_mark("speaker sheet left alone, IDLE_MS + 1 s");
  sim_run(IDLE_MS - 1500);
  sim_screenshot("12c1-speakers-idle-almost");
  sim_run(2500);
  sim_screenshot("12c-speakers-idle-closed");
  // Whatever woke the screen, it dims again on its own.
  sim_run(IDLE_MS + 1000);
  std::printf("  backlight IDLE_MS + 1 s after the sheet closed: %d\n",
              sim_backlight());

  timer_scenarios();
}
