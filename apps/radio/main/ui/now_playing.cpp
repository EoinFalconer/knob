#include "now_playing.h"
#include "sonos_group.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

bool is_playing(PlayState state) {
  return state == PlayState::Playing || state == PlayState::Transitioning;
}

MediaControls media_controls(bool external, PlayState state, uint8_t actions) {
  if (!external)
    return {};
  bool playing = is_playing(state);
  bool toggle = actions & (playing ? ACTION_PAUSE : ACTION_PLAY);
  return {
      .prev = (actions & ACTION_PREVIOUS) != 0,
      .next = (actions & ACTION_NEXT) != 0,
      .play_badge = toggle && !playing,
      .toggle = toggle,
  };
}

ArtPlaceholder art_placeholder(const MediaInfo &media) {
  static constexpr struct {
    const char *source;
    ArtPlaceholder glyph;
  } SOURCES[] = {
      {"TV", ArtPlaceholder::Tv},
      {"Line-In", ArtPlaceholder::LineIn},
      {"AirPlay", ArtPlaceholder::AirPlay},
      {"Radio", ArtPlaceholder::Radio},
  };
  for (const auto &s : SOURCES) {
    if (strcmp(media.source, s.source) == 0)
      return s.glyph;
  }
  return ArtPlaceholder::Music;
}

const char *now_playing_subtitle(bool external, const MediaInfo &media,
                                 PlayState state) {
  switch (state) {
  case PlayState::Paused:
    return "Paused";
  case PlayState::Stopped:
    return "Tap to browse stations";
  case PlayState::Transitioning:
    return "Loading…";
  case PlayState::Playing:
    break;
  default:
    return "";
  }
  if (!external)
    return "Playing";
  if (media.artist[0])
    return media.artist;
  // A TV or line-in is already titled by its source; don't say it twice.
  if (media.source[0] && strcmp(media.source, media.title) != 0)
    return media.source;
  return "Playing";
}

void speaker_line(const SonosGroup &group, const char *saved_name, char *out,
                  size_t out_len) {
  if (group.count > 0)
    sonos_group_label(group, out, out_len);
  else
    snprintf(out, out_len, "%s", saved_name ? saved_name : "");
}

static void add_member(SonosGroup *group, const SonosSpeaker &spk) {
  if (group->count >= SONOS_GROUP_MAX)
    return;
  snprintf(group->member_uuids[group->count], sizeof(group->member_uuids[0]),
           "%s", spk.uuid);
  snprintf(group->member_names[group->count], sizeof(group->member_names[0]),
           "%s", spk.name);
  group->count++;
}

SonosGroup group_from_discovery(const DiscoveryResult &scan,
                                const char *room_name) {
  SonosGroup group = {};
  const SonosSpeaker *room = nullptr;
  for (int i = 0; i < scan.count && room_name && room_name[0]; i++) {
    if (strcmp(scan.speakers[i].name, room_name) == 0)
      room = &scan.speakers[i];
  }
  if (!room || !room->uuid[0])
    return group;

  snprintf(group.room_uuid, sizeof(group.room_uuid), "%s", room->uuid);
  snprintf(group.coordinator_uuid, sizeof(group.coordinator_uuid), "%s",
           room->coordinator_uuid);
  add_member(&group, *room);
  for (int i = 0; i < scan.count; i++) {
    const SonosSpeaker &spk = scan.speakers[i];
    if (&spk != room && room->coordinator_uuid[0] &&
        strcmp(spk.coordinator_uuid, room->coordinator_uuid) == 0)
      add_member(&group, spk);
  }
  return group;
}

const char *switch_resume_url(bool external, PlayState state,
                              const char *station_url) {
  return !external && is_playing(state) ? station_url : nullptr;
}
