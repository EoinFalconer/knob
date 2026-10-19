#pragma once

#include "sim.h"

// Hands the UI every photo it has asked for since the last call; returns how
// many.
int sim_deliver_avatars();
