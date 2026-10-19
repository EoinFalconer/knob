// Kaffi's screens, end to end: first boot, people, logging, the leaderboard
// and being offline.
#include "kaffi_sim.h"

#include "app_config.h"
#include "ui/ui.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

std::string s_people_dir;
int s_logged;

void on_event(int32_t id, const void *data, size_t) {
  if (id != APP_EVENT_KAFFI_LOG)
    return;
  auto *cmd = static_cast<const KaffiLogCmd *>(data);
  std::printf("  logged %s %s x%d\n", cmd->person_name,
              cmd->kind == KAFFI_BREW ? "brew" : "cup",
              static_cast<int>(cmd->quantity));
  s_logged++;
}

struct Seed {
  const char *name;
  const char *photo; // in the people folder, or null
  int pots, cups, streak;
  bool brewed_today;
};

constexpr Seed PEOPLE[] = {
    {"Åse Berg", "women-44.jpg", 14, 31, 4, true},
    {"Jonas Lie", "men-32.jpg", 3, 26, 0, false},
    {"Ingrid Haugen", "women-68.jpg", 9, 18, 3, false},
    {"Øystein Dahl", "men-75.jpg", 6, 12, 0, false},
    {"Mia Strand", "women-12.jpg", 0, 0, 0, false},
    {"Kristoffer Nygård-Solheim", "men-46.jpg", 2, 9, 0, false},
    {"Emil Bakke", nullptr, 4, 14, 2, true},
};

KaffiState make_state() {
  static KaffiState state;
  state = {};
  for (const Seed &seed : PEOPLE) {
    Person &p = state.people[state.count];
    snprintf(p.id, sizeof(p.id), "person-%d", state.count);
    snprintf(p.name, sizeof(p.name), "%s", seed.name);
    if (seed.photo)
      snprintf(p.image_url, sizeof(p.image_url), "%s/%s", s_people_dir.c_str(),
               seed.photo);
    p.pots = seed.pots;
    p.cups = seed.cups;
    p.credit = seed.pots * KAFFI_FIRST_POT_CUPS;
    p.streak = seed.streak;
    p.brewed_today = seed.brewed_today;
    state.count++;
  }
  return state;
}

constexpr int CX = 180;
// The log sheet's large rows, from the top (sheet_list.cpp: 92 px down,
// 56 px rows 8 px apart).
constexpr int ROW_Y[] = {120, 184, 248};

void first_boot() {
  sim_mark("boot");
  ui_show_splash();
  sim_run(300);
  sim_screenshot("00-starting");

  ui_show_office_picker([](int office) {
    std::printf("  office picked: %s\n", KAFFI_OFFICES[office].name);
  });
  sim_run(600);
  sim_screenshot("01-office-picker");
  sim_turn(1);
  sim_run(700);
  sim_screenshot("01b-office-next");
  sim_turn(-1);
  sim_run(700);
  sim_tap(CX, 180);
  sim_run(200);

  ui_set_status("Connecting to Wi-Fi\u2026");
  sim_run(300);
  sim_screenshot("02-connecting");
}

void people() {
  sim_mark("people: first list");
  KaffiState state = make_state();
  ui_update_people(&state);
  sim_run(400);
  sim_screenshot("03-people-no-photos");
  std::printf("  photos delivered: %d\n", sim_deliver_avatars());
  sim_run(600);
  sim_screenshot("04-people");

  sim_mark("people: one detent");
  sim_turn(1);
  sim_run(60);
  sim_deliver_avatars();
  sim_screenshot("05a-people-gliding");
  sim_run(600);
  sim_screenshot("05-person-next");

  sim_mark("people: fast spin");
  sim_turn(5);
  sim_run(700);
  sim_deliver_avatars();
  sim_run(400);
  sim_screenshot("06-person-no-photo");
  sim_turn(-1);
  sim_run(700);
  sim_screenshot("06b-long-name");
  sim_turn(-4);
  sim_run(700);
}

void logging() {
  sim_mark("log: tap opens the sheet");
  sim_tap(CX, 180);
  sim_run(500);
  sim_screenshot("07-log-sheet");
  sim_turn(1);
  sim_run(300);
  sim_screenshot("07b-log-sheet-focus");

  sim_mark("log: brew a pot");
  sim_tap(CX, ROW_Y[1]);
  sim_run(500);
  sim_screenshot("08-logged-toast");
  std::printf("  log events posted: %d\n", s_logged);
  sim_run(2200);
  sim_screenshot("08b-toast-gone");

  sim_mark("log: swipe up opens it too");
  sim_swipe(CX, 300, CX, 120);
  sim_run(500);
  sim_screenshot("09-log-swiped-up");
  sim_swipe(CX, 120, CX, 340);
  sim_run(500);
}

void leaderboard() {
  sim_mark("board: swipe down");
  sim_touch_down(CX, 40);
  sim_touch_move(CX, 200, 120);
  sim_screenshot("10a-board-following-finger");
  sim_touch_up();
  sim_run(500);
  sim_screenshot("10-board");
  sim_turn(4);
  sim_run(500);
  sim_screenshot("10b-board-scrolled");

  sim_mark("board: closes when left alone");
  sim_run(KAFFI_SHEET_IDLE_MS + 1500);
  sim_screenshot("10c-board-idle-closed");
}

void offline() {
  sim_mark("offline");
  ui_set_offline(true, 2);
  sim_run(300);
  sim_screenshot("11-offline-waiting");
  ui_set_offline(false, 1);
  sim_run(300);
  sim_screenshot("11b-sending");
  ui_set_offline(false, 0);
  sim_run(300);
}

void wifi_setup() {
  ui_show_wifi_setup("kaffi");
  sim_run(300);
  sim_screenshot("12-wifi-setup");
}

} // namespace

void sim_scenarios(const char *people_dir) {
  s_people_dir = people_dir;
  sim_on_event(on_event);
  ui_init();
  first_boot();
  people();
  logging();
  leaderboard();
  offline();
  wifi_setup();
}
