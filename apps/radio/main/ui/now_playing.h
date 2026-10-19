#pragma once

#include "discovery.h"
#include "sonos_config.h"

#include <cstddef>

// Home-screen decisions with no LVGL in them, so they can be host-tested.

struct MediaControls {
  bool prev;
  bool next;
  bool play_badge; // paused source that can resume: tap the artwork to play
  bool toggle;     // tapping the artwork pauses/plays
};

bool is_playing(PlayState state);

// Only offer what the source supports: Spotify or AirPlay can pause and
// skip, a TV or line-in can't.
MediaControls media_controls(bool external, PlayState state, uint8_t actions);

// What the artwork tile shows while there's no cover: a glyph for the source,
// like Apple Music's grey note.
enum class ArtPlaceholder { Music, Tv, LineIn, AirPlay, Radio };
ArtPlaceholder art_placeholder(const MediaInfo &media);

// Line under the title: artist, else the source, else the play state.
const char *now_playing_subtitle(bool external, const MediaInfo &media,
                                 PlayState state);

// Line at the bottom: the rooms playing, e.g. "Kitchen + Living Room".
void speaker_line(const SonosGroup &group, const char *saved_name, char *out,
                  size_t out_len);

// The controlled room's group as a fresh discovery scan sees it, room first.
// Empty if the room isn't in the scan.
SonosGroup group_from_discovery(const DiscoveryResult &scan,
                                const char *room_name);


// Station to carry over when switching rooms: only the knob's own radio
// moves along. Someone else's Spotify or TV stays where it is.
const char *switch_resume_url(bool external, PlayState state,
                              const char *station_url);
