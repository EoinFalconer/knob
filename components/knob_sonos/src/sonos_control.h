#pragma once

#include "sonos_config.h"

// Sonos control logic, free of FreeRTOS so it can run in host tests. All of
// it runs on the Sonos net task.
//
// The knob controls one room. Sonos only honours transport and group volume
// commands on a group's coordinator, and a SetAVTransportURI sent to a member
// pulls it out of its group into an unsynced copy of the stream. So every
// command is routed to the coordinator of whatever group the room is in.

void control_set_stations(const char *const *urls, int count);

// Point at a room. The coordinator is resolved by the next topology refresh.
void control_set_room(const char *ip, int port);
bool control_refresh_topology();

// Retarget to another room, which then plays on its own: picking a room in
// the knob's flat list means that room, and rooms are only added by holding.
// Its group is broken up first, unless the group is playing music the knob
// isn't moving. When `resume_url` is set, playback moves there: the old group
// stops and the room plays it.
void control_select(const char *ip, int port, const char *resume_url);

// Add a room to the controlled group, or make a room standalone.
bool control_join(const char *ip, int port);
bool control_leave(const char *ip, int port);

void control_play_url(const char *url);
// Any argument-free AVTransport action: Pause, Stop, Next, Previous.
bool control_transport(const char *action);
bool control_play();
bool control_seek_start();

// Group volume keeps the members' relative balance. Start each gesture with a
// fresh snapshot so ratios changed elsewhere (Sonos app) aren't reverted.
void control_set_volume(int level, bool new_gesture);
// The group volume, or -1 when the coordinator didn't answer.
int control_get_volume();

// Interrupts the group `room` is in (by name; the controlled group if there's
// no such room) with `url`, a plain http:// file, at group volume `volume`,
// after noting what that group was doing. Restore puts it back: the source,
// the queue position, the volume and whether it was playing.
bool control_alarm_start(const char *url, const char *room, int volume);
void control_alarm_restore();
// The alarm sounds on the group the knob controls, so polls would show it.
bool control_alarm_shares_group();

// Returns false when the coordinator didn't answer (state is then unknown).
bool control_poll(SonosState *out);

const char *control_coordinator_ip();
