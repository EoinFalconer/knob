#pragma once

#include "pending_ring.h"

// Undelivered coffee events, persisted in NVS so a reboot while offline loses
// nothing. Single-writer: only the command task pushes and pops.

/// Load the queue from NVS. Safe to call before WiFi.
void kaffi_pending_init();

/// Number of events waiting to be delivered.
int kaffi_pending_count();

/// Assign a fresh ID, append and persist before delivery. Call after WiFi has
/// started (it supplies RNG entropy, even while disconnected).
/// When full, the oldest is dropped (logged).
void kaffi_pending_push(const PendingEvent &e);

/// Oldest waiting event, or nullptr when empty or persistence needs retrying.
/// Call after WiFi has started; migrated events receive IDs here.
const PendingEvent *kaffi_pending_front();

/// Drop the oldest event (after a successful delivery) and persist.
void kaffi_pending_pop();
