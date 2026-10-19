#pragma once

#include "app_config.h"

#include "lvgl.h"

#include <cstdint>

// Everyone's photo, decoded once and kept in PSRAM so turning through people
// never waits on the network. Slots follow the people list by index; a new
// list is matched to the old one by photo URL, so a reorder keeps them all.

constexpr int AVATAR_SIZE = KAFFI_PHOTO_PX; // the carousel's middle tile
constexpr int THUMB_SIZE = 28;   // a list row's circle

// Fetches are asked of main.cpp's avatar task, once per photo URL.
void kaffi_request_avatar(const char *url);

// Takes on a new people list.
void avatars_follow(const KaffiState &state);
// Asks for everyone's photo, `first` first.
void avatars_preload(const KaffiState &state, int first);
void avatars_request(const KaffiState &state, int index);
// Takes ownership of `pixels` (RGB565 swapped). Returns the index it belongs
// to, or -1 if nobody has that photo any more. Null pixels mark a failed
// fetch, which is tried again on the next refresh.
int avatars_store(const KaffiState &state, const char *url, uint8_t *pixels,
                  int w, int h);

// Null until the photo is in.
const lv_image_dsc_t *avatar_photo(int index);
// Round, baked onto the background so it draws without a layer.
const lv_image_dsc_t *avatar_thumb(int index);
// The photo's color, for the screen behind it; a dark gray without one.
lv_color_t avatar_tint(int index);
