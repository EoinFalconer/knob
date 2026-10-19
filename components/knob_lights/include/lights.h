#pragma once

#include "lights_events.h"

// The home's lights, through an IKEA TRADFRI gateway on the local network
// (CoAP over DTLS, no cloud). Everything is taken together: all on, all off,
// all at one brightness. Lights named like CONFIG_LIGHTS_IGNORE are left
// alone, and so is any room with one in it, light by light instead.
//
// The calls only queue work for the lights task and return at once. What the
// gateway reports posts APP_EVENT_LIGHTS_CHANGED (lights_events.h), once the
// lights have caught up with the latest request; the caller shows what it
// asked for meanwhile.

void lights_init();
// Wi-Fi is up: pair with the gateway if the knob never has, and load the
// lights.
void lights_start();
// Reads the lights again, for when they're about to be shown.
void lights_refresh();
void lights_set_on(bool on);
// 0..100; 0 turns them off. Calls faster than the gateway can keep up with
// collapse into the latest.
void lights_set_level(int level);
LightsState lights_state();
