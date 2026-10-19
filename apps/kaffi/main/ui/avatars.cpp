#include "ui/avatars.h"
#include "art_decoder.h"
#include "art_tint.h"
#include "squircle.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"

// lv_image_cache_drop is only declared in the cache's own header.
#include "src/misc/cache/instance/lv_image_cache.h"

#include <cstring>

namespace {

enum class State : uint8_t { Empty, Requested, Loaded };

struct Slot {
  State state;
  char url[KAFFI_URL_LEN];
  uint8_t *photo_pixels; // AVATAR_SIZE square, RGB565 swapped
  uint8_t *thumb_pixels; // THUMB_SIZE square, round, RGB565A8
  lv_image_dsc_t photo;
  lv_image_dsc_t thumb;
  uint32_t tint;
};

EXT_RAM_BSS_ATTR Slot s_slots[KAFFI_MAX_PEOPLE];

void release(Slot &s) {
  lv_image_cache_drop(&s.photo);
  lv_image_cache_drop(&s.thumb);
  heap_caps_free(s.photo_pixels);
  squircle_free(s.thumb_pixels);
  s = {};
}

void describe(lv_image_dsc_t &dsc, uint8_t *pixels, int size) {
  dsc = {};
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
  dsc.header.w = static_cast<uint32_t>(size);
  dsc.header.h = static_cast<uint32_t>(size);
  dsc.header.stride = static_cast<uint32_t>(size * 2);
  dsc.data_size = static_cast<uint32_t>(size * size * 2);
  dsc.data = pixels;
}

// Photos come as asked, AVATAR_SIZE square, unless the original was smaller.
uint8_t *to_tile_size(uint8_t *pixels, int w, int h) {
  if (w == AVATAR_SIZE && h == AVATAR_SIZE)
    return pixels;
  uint8_t *scaled = art_scale_rgb565(pixels, w, h, AVATAR_SIZE, AVATAR_SIZE);
  heap_caps_free(pixels);
  return scaled;
}

// Scaled once rather than drawn scaled: a transformed image goes through
// LVGL's slow per-pixel path on every frame.
void bake_thumb(Slot &s) {
  uint8_t *small = art_scale_rgb565(s.photo_pixels, AVATAR_SIZE, AVATAR_SIZE,
                                    THUMB_SIZE, THUMB_SIZE);
  if (!small)
    return;
  lv_image_dsc_t plain;
  describe(plain, small, THUMB_SIZE);
  s.thumb_pixels = squircle_alloc(THUMB_SIZE);
  if (s.thumb_pixels)
    circle_bake(&plain, lv_color_hex(s.tint), THUMB_SIZE, s.thumb_pixels,
                &s.thumb);
  lv_image_cache_drop(&plain);
  heap_caps_free(small);
}

} // namespace

void avatars_follow(const KaffiState &state) {
  EXT_RAM_BSS_ATTR static Slot moved[KAFFI_MAX_PEOPLE];
  bool taken[KAFFI_MAX_PEOPLE] = {};
  memset(moved, 0, sizeof(moved));
  for (int i = 0; i < state.count; i++) {
    const char *url = state.people[i].image_url;
    if (!url[0])
      continue;
    for (int j = 0; j < KAFFI_MAX_PEOPLE; j++)
      if (!taken[j] && s_slots[j].state != State::Empty &&
          strcmp(s_slots[j].url, url) == 0) {
        moved[i] = s_slots[j];
        taken[j] = true;
        break;
      }
  }
  // LVGL caches images by descriptor address, and the addresses now change
  // hands.
  for (int j = 0; j < KAFFI_MAX_PEOPLE; j++) {
    lv_image_cache_drop(&s_slots[j].photo);
    lv_image_cache_drop(&s_slots[j].thumb);
    if (!taken[j])
      release(s_slots[j]);
  }
  memcpy(s_slots, moved, sizeof(s_slots));
}

void avatars_request(const KaffiState &state, int index) {
  if (index < 0 || index >= state.count)
    return;
  Slot &s = s_slots[index];
  const char *url = state.people[index].image_url;
  if (s.state != State::Empty || !url[0])
    return;
  strncpy(s.url, url, sizeof(s.url) - 1);
  s.state = State::Requested;
  kaffi_request_avatar(url);
}

void avatars_preload(const KaffiState &state, int first) {
  avatars_request(state, first);
  for (int i = 0; i < state.count; i++)
    avatars_request(state, i);
}

int avatars_store(const KaffiState &state, const char *url, uint8_t *pixels,
                  int w, int h) {
  int index = -1;
  for (int i = 0; url && i < state.count; i++)
    if (strcmp(state.people[i].image_url, url) == 0) {
      index = i;
      break;
    }
  if (index < 0) {
    heap_caps_free(pixels);
    return -1;
  }
  Slot &s = s_slots[index];
  if (!pixels) {
    s.state = State::Empty;
    return -1;
  }
  pixels = to_tile_size(pixels, w, h);
  if (!pixels) {
    s.state = State::Empty;
    return -1;
  }
  release(s);
  strncpy(s.url, url, sizeof(s.url) - 1);
  s.state = State::Loaded;
  s.photo_pixels = pixels;
  describe(s.photo, pixels, AVATAR_SIZE);
  s.tint = artwork_tint(pixels, AVATAR_SIZE * AVATAR_SIZE);
  bake_thumb(s);
  return index;
}

const lv_image_dsc_t *avatar_photo(int index) {
  if (index < 0 || index >= KAFFI_MAX_PEOPLE)
    return nullptr;
  return s_slots[index].state == State::Loaded ? &s_slots[index].photo
                                               : nullptr;
}

const lv_image_dsc_t *avatar_thumb(int index) {
  if (index < 0 || index >= KAFFI_MAX_PEOPLE)
    return nullptr;
  return s_slots[index].thumb_pixels ? &s_slots[index].thumb : nullptr;
}

lv_color_t avatar_tint(int index) {
  bool loaded = index >= 0 && index < KAFFI_MAX_PEOPLE &&
                s_slots[index].state == State::Loaded;
  return lv_color_hex(loaded ? s_slots[index].tint : ART_TINT_NONE);
}
