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

struct Rgb {
  int r, g, b;
};

static Rgb unpack_swapped_rgb565(const uint8_t *px) {
  auto v = static_cast<uint16_t>(px[0] << 8 | px[1]);
  int r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
  return {r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2};
}

// Same depth as the station palette (brightest channel 0x28).
static constexpr int TINT_PEAK = 0x28;
static constexpr uint32_t TINT_NEUTRAL = 0x141414;

struct Bin {
  uint32_t weight, r, g, b;
};

// 3 bits per channel: coarse enough that a color region lands in one bin,
// small enough (8 KB) to allocate briefly once per track.
static constexpr int BINS = 512;

uint32_t artwork_tint(const uint8_t *rgb565_swapped, int pixel_count) {
  auto *bins = static_cast<Bin *>(calloc(BINS, sizeof(Bin)));
  if (!bins)
    return TINT_NEUTRAL;

  for (int i = 0; i < pixel_count; i += 3) {
    Rgb c = unpack_swapped_rgb565(rgb565_swapped + i * 2);
    int hi = std::max({c.r, c.g, c.b});
    int lo = std::min({c.r, c.g, c.b});
    if (hi < 24)
      continue; // near-black says nothing about the artwork's color
    // Favour vivid pixels so a red logo on a white sleeve still reads red.
    uint32_t w = 1 + static_cast<uint32_t>(hi - lo);
    Bin &bin = bins[(c.r >> 5) << 6 | (c.g >> 5) << 3 | (c.b >> 5)];
    bin.weight += w;
    bin.r += w * static_cast<uint32_t>(c.r);
    bin.g += w * static_cast<uint32_t>(c.g);
    bin.b += w * static_cast<uint32_t>(c.b);
  }

  const Bin *best =
      std::max_element(bins, bins + BINS, [](const Bin &a, const Bin &b) {
        return a.weight < b.weight;
      });
  if (!best->weight) {
    free(bins);
    return TINT_NEUTRAL;
  }
  int r = static_cast<int>(best->r / best->weight);
  int g = static_cast<int>(best->g / best->weight);
  int b = static_cast<int>(best->b / best->weight);
  free(bins);

  int hi = std::max({r, g, b, 1});
  return static_cast<uint32_t>(r * TINT_PEAK / hi) << 16 |
         static_cast<uint32_t>(g * TINT_PEAK / hi) << 8 |
         static_cast<uint32_t>(b * TINT_PEAK / hi);
}

const char *switch_resume_url(bool external, PlayState state,
                              const char *station_url) {
  return !external && is_playing(state) ? station_url : nullptr;
}
