#pragma once

#include "sim.h"

extern bool sim_alarm_sounding;
// What the stand-in gateway reports; scenarios may change it.
struct LightsState;
extern LightsState sim_lights;
