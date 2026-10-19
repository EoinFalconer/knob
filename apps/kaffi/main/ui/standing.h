#pragma once

#include "app_config.h"

#include <cstddef>

// What the screens say about people, with no LVGL in it.

// Mon-Fri with a synced clock: the only days a streak can die.
bool is_weekday_today();

struct Streak {
  int days;     // 0 when there's no streak to show (one day isn't one)
  bool at_risk; // not fed yet today, on a day it can die
};
Streak streak_of(const Person &p, bool weekday);
// "5 days", or "Save day 5" while it's at risk.
void streak_text(const Streak &streak, char *out, size_t len);

// "Coffee hero, +7", "Freeloader, −3", "Even".
void standing_text(int balance, char *out, size_t len);
// "12 pots, 30 cups", like Apple Music's "12 songs, 45 minutes".
void stats_text(const Person &p, char *out, size_t len);
// "+4", "−1" with a real minus, "0".
void signed_text(int value, char *out, size_t len);
// The first letter of a name, capitalized, or "?".
void initial_of(const char *name, char *out, size_t len);

// The leaderboard: people with anything logged this year, best balance
// first. Returns how many went into `order`, as indices into `state`.
int leaderboard_order(const KaffiState &state, int *order);

// What the pill cheers after a log, before it's counted: a streak the brew
// extends or saves beats the usual quips, which take turns.
const char *log_toast(const Person &p, int kind, int qty, bool weekday);
