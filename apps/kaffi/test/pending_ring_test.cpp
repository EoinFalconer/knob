// Host test for the pending-event ring buffer. Run: apps/kaffi/test/run.sh
#include "../main/kaffi/pending_ring.h"

#include <cstdio>
#include <cstring>

static int failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static PendingEvent ev(int n) {
  PendingEvent e = {};
  std::snprintf(e.person_id, sizeof(e.person_id), "person-%d", n);
  std::snprintf(e.person_name, sizeof(e.person_name), "Person %d", n);
  e.kind = n % 2;
  e.quantity = 1 + n % 2;
  std::snprintf(e.occurred_at, sizeof(e.occurred_at), "2026-09-24T18:%02d:00Z",
                n % 60);
  return e;
}

static void empty_ring_has_no_front() {
  PendingRing<4> r;
  CHECK(r.empty());
  CHECK(r.front() == nullptr);
  r.pop(); // no-op, must not underflow
  CHECK(r.size() == 0);
}

static void delivers_in_tap_order() {
  PendingRing<4> r;
  CHECK(r.push(ev(1)));
  CHECK(r.push(ev(2)));
  CHECK(r.push(ev(3)));
  CHECK(r.size() == 3);
  CHECK(std::strcmp(r.front()->person_id, "person-1") == 0);
  r.pop();
  CHECK(std::strcmp(r.front()->person_id, "person-2") == 0);
  r.pop();
  CHECK(std::strcmp(r.front()->person_id, "person-3") == 0);
  r.pop();
  CHECK(r.empty());
}

static void full_ring_drops_oldest() {
  PendingRing<3> r;
  r.push(ev(1));
  r.push(ev(2));
  r.push(ev(3));
  CHECK(!r.push(ev(4))); // reports the drop
  CHECK(r.size() == 3);
  CHECK(std::strcmp(r.front()->person_id, "person-2") == 0);
  r.pop();
  r.pop();
  CHECK(std::strcmp(r.front()->person_id, "person-4") == 0);
}

static void wraps_around_after_pops() {
  PendingRing<2> r;
  for (int i = 1; i <= 7; i++) {
    r.push(ev(i));
    r.pop();
  }
  CHECK(r.empty());
  r.push(ev(8));
  CHECK(std::strcmp(r.front()->occurred_at, "2026-09-24T18:08:00Z") == 0);
}

int main() {
  empty_ring_has_no_front();
  delivers_in_tap_order();
  full_ring_drops_oldest();
  wraps_around_after_pops();
  if (failures == 0)
    std::printf("pending_ring: all tests passed\n");
  return failures ? 1 : 0;
}
