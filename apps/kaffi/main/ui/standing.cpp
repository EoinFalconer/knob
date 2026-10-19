#include "ui/standing.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace {

constexpr int HERO = 6;
constexpr const char *MINUS = "\u2212";

const char *standing_name(int balance) {
  if (balance >= HERO)
    return "Coffee hero";
  if (balance > 0)
    return "In credit";
  if (balance <= -HERO)
    return "Mega freeloader";
  return "Freeloader";
}

} // namespace

bool is_weekday_today() {
  time_t now = time(nullptr);
  struct tm local;
  localtime_r(&now, &local);
  if (local.tm_year + 1900 < 2023)
    return false;
  return local.tm_wday >= 1 && local.tm_wday <= 5;
}

Streak streak_of(const Person &p, bool weekday) {
  if (p.streak < 2)
    return {0, false};
  return {p.streak, !p.brewed_today && weekday};
}

void streak_text(const Streak &streak, char *out, size_t len) {
  if (streak.days == 0)
    snprintf(out, len, "%s", "");
  else if (streak.at_risk)
    snprintf(out, len, "Save day %d", streak.days);
  else
    snprintf(out, len, "%d days", streak.days);
}

void signed_text(int value, char *out, size_t len) {
  if (value > 0)
    snprintf(out, len, "+%d", value);
  else if (value < 0)
    snprintf(out, len, "%s%d", MINUS, -value);
  else
    snprintf(out, len, "0");
}

void standing_text(int balance, char *out, size_t len) {
  if (balance == 0) {
    snprintf(out, len, "Even");
    return;
  }
  char number[16];
  signed_text(balance, number, sizeof(number));
  snprintf(out, len, "%s, %s", standing_name(balance), number);
}

void stats_text(const Person &p, char *out, size_t len) {
  snprintf(out, len, "%d %s, %d %s", p.pots, p.pots == 1 ? "pot" : "pots",
           p.cups, p.cups == 1 ? "cup" : "cups");
}

// Latin-1 letters come as two UTF-8 bytes; their capitals are 0x20 below.
void initial_of(const char *name, char *out, size_t len) {
  auto b = reinterpret_cast<const unsigned char *>(name);
  if (b[0] >= 'a' && b[0] <= 'z')
    snprintf(out, len, "%c", toupper(b[0]));
  else if (b[0] >= 'A' && b[0] <= 'Z')
    snprintf(out, len, "%c", b[0]);
  else if (b[0] == 0xC3 && b[1] >= 0x80 && b[1] <= 0xBE && b[1] != 0x97 &&
           b[1] != 0x9F && b[1] != 0xB7)
    snprintf(out, len, "%c%c", 0xC3, b[1] >= 0xA0 ? b[1] - 0x20 : b[1]);
  else
    snprintf(out, len, "?");
}

int leaderboard_order(const KaffiState &state, int *order) {
  int n = 0;
  for (int i = 0; i < state.count; i++)
    if (state.people[i].pots > 0 || state.people[i].cups > 0)
      order[n++] = i;
  auto balance = [&](int i) {
    return state.people[i].credit - state.people[i].cups;
  };
  // Insertion sort keeps equal balances in list order.
  for (int i = 1; i < n; i++) {
    int key = order[i], j = i - 1;
    for (; j >= 0 && balance(order[j]) < balance(key); j--)
      order[j + 1] = order[j];
    order[j + 1] = key;
  }
  return n;
}

const char *log_toast(const Person &p, int kind, int qty, bool weekday) {
  static const char *const CUPS[] = {"Enjoy!", "Cup logged", "Skål!",
                                     "Drink up!"};
  static const char *const POTS[] = {"What a hero!", "Fresh pot!",
                                     "Legend move", "Brewtiful!"};
  static unsigned turn;
  static char streak[24];
  turn++;
  if (kind != KAFFI_BREW)
    return CUPS[turn % 4];
  if (!p.brewed_today && p.streak + 1 >= 2) {
    bool saved = p.streak >= 2 && weekday;
    snprintf(streak, sizeof(streak), saved ? "Saved! Day %d" : "Day %d streak!",
             p.streak + 1);
    return streak;
  }
  return qty == 2 ? "Double hero!" : POTS[turn % 4];
}
