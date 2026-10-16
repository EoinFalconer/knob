#pragma once

#include <cstdint>

// A coffee event the knob could not deliver yet. Sizes mirror KAFFI_ID_LEN /
// KAFFI_NAME_LEN in app_config.h (checked there with static_asserts) but are
// spelled out so this header stays free of ESP-IDF and testable on a laptop.
struct PendingEvent {
  char person_id[40];
  char person_name[48];
  int32_t kind;     // KaffiKind
  int32_t quantity; // pots (1-2) or cups (1)
  char occurred_at[24]; // ISO-8601 UTC at tap time, or "" if the clock was unset
};

// Fixed-capacity FIFO of undelivered events, oldest first. Plain data so it
// can be persisted as one NVS blob. When full, the oldest entry is dropped:
// a knob offline for days should keep the most recent taps, not the first.
template <int N> struct PendingRing {
  PendingEvent items[N];
  int32_t head = 0;
  int32_t count = 0;

  static constexpr int capacity() { return N; }
  int size() const { return count; }
  bool empty() const { return count == 0; }

  // Returns false when an old entry had to be dropped to make room.
  bool push(const PendingEvent &e) {
    bool dropped = false;
    if (count == N) {
      head = (head + 1) % N;
      count--;
      dropped = true;
    }
    items[(head + count) % N] = e;
    count++;
    return !dropped;
  }

  const PendingEvent *front() const { return count ? &items[head] : nullptr; }

  void pop() {
    if (count == 0)
      return;
    head = (head + 1) % N;
    count--;
  }
};
