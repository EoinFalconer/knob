#pragma once

#include <cstddef>
#include <cstdint>

// All calls are non-blocking: they queue work for the Sonos net task.
// Commands always reach the coordinator of the controlled room's group.

void sonos_init();
// Resume/pause polling and command handling (e.g. on WiFi up/down).
void sonos_start();
void sonos_stop();

// Register station URLs for URI-to-index matching during state polling
void sonos_set_stations(const char *const *urls, int count);

// Control a room. Persisted as the saved speaker.
void sonos_set_speaker(const char *ip, int port = 1400);

// Switch to another room, which then plays on its own (any group it's in is
// broken up, unless that group is playing something the knob isn't moving).
// If `resume_url` is set, the old group stops and the new room plays it.
void sonos_select_speaker(const char *ip, int port, const char *resume_url);

// Add a room to the controlled group / make a room standalone again.
void sonos_join_group(const char *ip, int port);
void sonos_leave_group(const char *ip, int port);

void sonos_play_uri(const char *uri);
void sonos_play();
void sonos_pause();
// Fades the group out over FADE_OUT_MS, stops it, then restores the volume.
// A play in the meantime calls the stop off.
void sonos_stop_playback();
void sonos_previous();
void sonos_next();
// Seek to the beginning of the current track (restart).
void sonos_seek_start();

// Sounds `url` (a plain http:// file) in `room`, found by name, at group
// volume 30, for up to a minute. It takes over whatever that room's
// group plays (rooms grouped with it hear it too). Stopping it, or any other
// command, puts that back where it was, at its old volume, playing again if
// it was. Without a room by that name it rings in the controlled room.
void sonos_start_alarm(const char *url, const char *room);
void sonos_stop_alarm();

// Group volume. Rapid calls are coalesced to the latest level.
void sonos_set_volume(int level);

// Download album art JPEG from the given URL into a caller-provided buffer.
// Returns the number of bytes written, or 0 on failure.
int sonos_fetch_art(const char *url, uint8_t *buf, int buf_size);
