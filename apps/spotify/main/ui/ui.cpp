#include "ui/ui.h"
#include "activity.h"
#include "art_decoder.h"
#include "art_tint.h"
#include "backdrop.h"
#include "caption.h"
#include "carousel.h"
#include "display.h"
#include "encoder.h"
#include "fonts.h"
#include "haptic.h"
#include "motion.h"
#include "primitives.h"
#include "sheet.h"
#include "squircle.h"
#include "spotify/spotify_api.h"
#include "theme.h"
#include "toast.h"
#include "ui/images/images.h"
#include "volume_ring.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

// lv_image_cache_drop is only declared in the cache's own header.
#include "src/misc/cache/instance/lv_image_cache.h"

namespace {
constexpr const char *TAG = "spotify_ui";
constexpr int ART = 120, JPEG_CAP = 128 * 1024, RING_MAX = 1000;
// Decode near full size, then box-average down to ART. Decoding at ART
// makes TJpgDec pick 1/4 for Spotify's 300 px art (75 px), which then has
// to be stretched up to 120 px, doubling every other row and column.
constexpr int ART_DECODE = 300;
constexpr int SEEK_STEP = 3000;
constexpr int SWIPE_TAKE = 10;
constexpr int SWIPE_COMMIT = 50;
constexpr int SWIPE_FLING = 700;
// Rows a track swipe may start in: the covers, give or take a finger.
constexpr int SWIPE_BAND_TOP = 70, SWIPE_BAND_BOTTOM = 290;
lv_obj_t *screen, *art_slot, *art_img, *placeholder, *glyph, *seek_label;
lv_obj_t *status, *status_label, *qr_view;
lv_obj_t *drawer, *drawer_content, *clock_value, *device_line;
Caption caption;
IconPill pill;
int volume = 50, progress_ms, duration_ms, seek_ms;
bool has_track, playing, idle, offline, starting, volume_shown, touch_down;
bool seeking, dimmed, clock_idle = true;
bool strip_dragging;
bool strip_open;
bool current_placeholder;
bool external_change_pending;
int poll_failures;
int pending_skip;
int64_t local_skip_until;
int encoder_during_touch;
int64_t progress_at, volume_hide_at, volume_local_until, last_activity;
int64_t touch_sample_at;
lv_point_t touch_start, touch_sample;
int32_t touch_velocity;
char art_url[256], track[128], device_name[64], device_id[128];
uint8_t *art_pixels;
lv_image_dsc_t art_dsc;
uint8_t *placeholder_pixels;
lv_image_dsc_t placeholder_dsc;
lv_timer_t *placeholder_timer;
char pending_art_url[256];
char side_art_url[2][256];
bool side_available[2];
bool side_loading[2];
bool side_placeholder[2];
lv_timer_t *side_placeholder_timer[2];

enum class TouchAxis : uint8_t { None, Horizontal, Vertical };
TouchAxis touch_axis;

constexpr int SKIP_RESULTS = 8;
constexpr int CONTROL_NEXT = 3;
constexpr int CONTROL_PREV = 4;
int skip_results[SKIP_RESULTS];
int skip_result_head;
int skip_result_count;

enum class ArtSlot : uint8_t { Previous, Current, Next };

struct ArtRequest {
  ArtSlot slot;
  uint32_t generation;
  char url[256];
};

QueueHandle_t art_queue;
volatile uint32_t art_generation[3];
uint8_t *cover_pixels[3];
lv_image_dsc_t cover_dsc[3];

enum class Surface { Clock, Idle, Playing, Drawer, Setup };

//            tap                 swipe down     turn              hold
// Clock      reveal idle card    drawer         wake              -
// Idle       transfer + resume   drawer         wake              -
// Playing    play/pause          drawer         volume            seek
// Drawer     rows                close upward   volume if playing -
// Setup      -                   -              wake              -

Surface surface() {
  if (qr_view)
    return Surface::Setup;
  if (drawer && sheet_is_open(drawer))
    return Surface::Drawer;
  if (idle)
    return Surface::Idle;
  return Surface::Playing;
}

void hidden(lv_obj_t *o, bool yes) { lv_obj_set_flag(o, LV_OBJ_FLAG_HIDDEN, yes); }

CarouselTile playing_tile(int item) {
  if (item < 0 || item > 2)
    return {nullptr, lv_color_black(), nullptr, false, false};
  if (item != 1 && !side_available[item == 0 ? 0 : 1])
    return {nullptr, lv_color_black(), nullptr, false, false};
  if (item != 1) {
    int side = item == 0 ? 0 : 1;
    if (side_loading[side] && side_placeholder[side])
      return {nullptr, lv_color_hex(COL_PLACEHOLDER), ICON_MUSIC, false};
    if (side_loading[side] && !cover_pixels[item])
      return {nullptr, lv_color_black(), nullptr, false, false};
  }
  if (item == 1 && current_placeholder)
    return {nullptr, lv_color_hex(COL_PLACEHOLDER), ICON_MUSIC, false};
  return {cover_pixels[item] ? &cover_dsc[item] : nullptr,
          lv_color_hex(COL_PLACEHOLDER), ICON_MUSIC, false};
}

void playing_moved(int, int32_t) {}

void enqueue_art(ArtSlot slot, const char *url) {
  int index = static_cast<int>(slot);
  ++art_generation[index];
  if (!art_queue || !url || !url[0])
    return;
  ArtRequest request = {};
  request.slot = slot;
  request.generation = art_generation[index];
  strlcpy(request.url, url, sizeof(request.url));
  if (xQueueSend(art_queue, &request, 0) != pdTRUE) {
    ArtRequest dropped;
    xQueueReceive(art_queue, &dropped, 0);
    xQueueSend(art_queue, &request, 0);
  }
}

void art_task(void *) {
  auto *jpeg_buf = static_cast<uint8_t *>(
      heap_caps_malloc(JPEG_CAP, MALLOC_CAP_SPIRAM));
  if (!jpeg_buf) {
    ESP_LOGE(TAG, "Album-art JPEG buffer allocation failed");
    vTaskDelete(nullptr);
    return;
  }
  ArtRequest request;
  while (true) {
    if (xQueueReceive(art_queue, &request, portMAX_DELAY) != pdTRUE)
      continue;
    ArtRequest newer;
    while (xQueueReceive(art_queue, &newer, 0) == pdTRUE) {
      if (newer.slot == request.slot)
        request = newer;
      else
        xQueueSendToFront(art_queue, &newer, 0);
      if (newer.slot != request.slot)
        break;
    }
    int index = static_cast<int>(request.slot);
    if (request.generation != art_generation[index])
      continue;
    int width = 0;
    int height = 0;
    int length = spotify_api_fetch_art(request.url, jpeg_buf, JPEG_CAP);
    uint8_t *decoded = nullptr;
    if (length <= 0 ||
        !art_decode_jpeg(jpeg_buf, length, &decoded, &width, &height,
                         ART_DECODE) ||
        !decoded)
      continue;
    uint8_t *scaled = decoded;
    if (width != ART || height != ART) {
      scaled = art_scale_rgb565(decoded, width, height, ART, ART);
      heap_caps_free(decoded);
      if (!scaled)
        continue;
    }
    uint32_t tint = artwork_tint(scaled, ART * ART);
    auto *tile = squircle_alloc(ART);
    if (!tile) {
      heap_caps_free(scaled);
      continue;
    }
    lv_image_dsc_t raw = {};
    raw.header.magic = LV_IMAGE_HEADER_MAGIC;
    raw.header.w = ART;
    raw.header.h = ART;
    raw.header.stride = ART * 2;
    raw.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
    raw.data_size = ART * ART * 2;
    raw.data = scaled;
    lv_image_dsc_t ready = {};
    squircle_bake(&raw, lv_color_hex(tint), ART, tile, &ready);
    heap_caps_free(scaled);
    if (!display_lock(200)) {
      squircle_free(tile);
      continue;
    }
    if (request.generation != art_generation[index]) {
      display_unlock();
      squircle_free(tile);
      continue;
    }
    uint8_t *old = cover_pixels[index];
    lv_image_dsc_t old_dsc = cover_dsc[index];
    if (request.slot == ArtSlot::Current)
      lv_image_cache_drop(&art_dsc);
    cover_pixels[index] = tile;
    cover_dsc[index] = ready;
    cover_dsc[index].data = tile;
    if (request.slot == ArtSlot::Current) {
      art_pixels = tile;
      art_dsc = cover_dsc[index];
      lv_image_set_src(art_img, &art_dsc);
      pending_art_url[0] = '\0';
      current_placeholder = false;
      if (placeholder_timer) {
        lv_timer_delete(placeholder_timer);
        placeholder_timer = nullptr;
      }
      hidden(placeholder, true);
      hidden(glyph, true);
      hidden(art_img, false);
      backdrop_set(lv_color_hex(tint), !dimmed);
      if (external_change_pending) {
        external_change_pending = false;
        if (strip_open)
          carousel_crossfade_reload(3, 1, 240);
        else
          carousel_refresh(index);
      } else {
        carousel_refresh(index);
      }
    } else {
      int side = request.slot == ArtSlot::Previous ? 0 : 1;
      side_loading[side] = false;
      side_placeholder[side] = false;
      if (side_placeholder_timer[side]) {
        lv_timer_delete(side_placeholder_timer[side]);
        side_placeholder_timer[side] = nullptr;
      }
      carousel_refresh(index);
    }
    if (old) {
      lv_image_cache_drop(&old_dsc);
      squircle_free(old);
    }
    display_unlock();
  }
}

void fade_cb(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, v, 0); }
void fade(lv_obj_t *o, lv_opa_t to, uint32_t ms) {
  int from = lv_obj_get_style_opa(o, LV_PART_MAIN);
  if (from == to) return;
  lv_anim_delete(o, fade_cb);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, ms);
  lv_anim_set_exec_cb(&a, fade_cb);
  motion_ease_out(&a);
  lv_anim_start(&a);
}
void poke() {
  last_activity = esp_timer_get_time();
  if (dimmed) {
    display_set_backlight(BACKLIGHT_NORMAL);
    dimmed = false;
  }
}
int current_progress(int64_t now) {
  int p = progress_ms + (playing ? (int)((now - progress_at) / 1000) : 0);
  return std::clamp(p, 0, duration_ms);
}
int ring_value(int ms) {
  return duration_ms > 0 ? (int)((int64_t)ms * RING_MAX / duration_ms) : 0;
}
void progress_ring(uint32_t ms = 300) {
  RingLook look = ring_rest_look(lv_color_white());
  look.indicator_opa = LV_OPA_60;
  volume_ring_set_look(look, ms);
  volume_ring_set_level(ring_value(current_progress(esp_timer_get_time())));
}
void green_ring(int value) {
  volume_ring_set_level(value);
  volume_ring_set_look({lv_color_hex(COL_GREEN), LV_OPA_COVER,
                        RING_TRACK_OPA, 8, true}, 140);
}
void update_pill() {
  if (toast_active()) return;
  lv_obj_set_style_bg_color(pill.pill, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(pill.pill, PILL_OPA, LV_PART_MAIN);
  ui_pill_tint(pill, lv_color_white(), TEXT_OPA_SOFT);
  if (offline) {
    ui_icon_pill_set_icon(pill, ICON_WIFI_OFF, 0);
    ui_icon_pill_set_text(pill, "Offline");
    hidden(pill.pill, false);
    return;
  }
  if (poll_failures >= 3) {
    ui_icon_pill_set_icon(pill, ICON_WIFI_OFF, 0);
    ui_icon_pill_set_text(pill, "Can't reach Spotify");
    hidden(pill.pill, false);
    return;
  }
  if (starting) {
    ui_icon_pill_set_icon(pill, ICON_PLAY, 0);
    ui_icon_pill_set_text(pill, "Starting…");
    hidden(pill.pill, false);
    return;
  }
  if (idle) {
    if (device_id[0]) {
      char text[96];
      snprintf(text, sizeof(text), "Resume on %s", device_name);
      ui_icon_pill_set_icon(pill, ICON_PLAY, 0);
      ui_icon_pill_set_text(pill, text);
      hidden(pill.pill, false);
    } else if (has_track) {
      ui_icon_pill_set_icon(pill, "", 0);
      ui_icon_pill_set_text(pill, "Open Spotify to play");
      hidden(pill.pill, false);
    } else {
      hidden(pill.pill, true);
    }
    return;
  }
  ui_icon_pill_set_icon(pill, playing ? ICON_PLAY : ICON_PAUSE, 0);
  ui_icon_pill_set_text(pill, playing ? "Playing" : "Paused");
  hidden(pill.pill, !has_track);
}
void clear_qr() {
  if (qr_view) {
    lv_obj_delete(qr_view);
    qr_view = nullptr;
  }
}
void show_playback(bool show) {
  for (lv_obj_t *o : {art_slot, caption.title, caption.subtitle, caption.detail})
    hidden(o, !show);
  hidden(pill.pill, !show);
  hidden(status, show);
}

void show_playing_strip(bool show) {
  hidden(art_slot, show);
  if (show) {
    if (!strip_open)
      carousel_open(3, 1, nullptr);
    strip_open = true;
  } else if (strip_open) {
    carousel_hide_after(0, nullptr);
    strip_open = false;
  }
}

void rotate_covers(int steps) {
  int incoming = steps > 0 ? 2 : 0;
  int outgoing = steps > 0 ? 0 : 2;
  int becomes_side = steps > 0 ? 0 : 2;
  if (!cover_pixels[incoming])
    return;
  uint8_t *discarded = cover_pixels[outgoing];
  lv_image_dsc_t discarded_dsc = cover_dsc[outgoing];
  cover_pixels[becomes_side] = cover_pixels[1];
  cover_dsc[becomes_side] = cover_dsc[1];
  cover_pixels[1] = cover_pixels[incoming];
  cover_dsc[1] = cover_dsc[incoming];
  cover_pixels[incoming] = nullptr;
  cover_dsc[incoming] = {};
  art_pixels = cover_pixels[1];
  lv_image_cache_drop(&art_dsc);
  art_dsc = cover_dsc[1];
  lv_image_set_src(art_img, &art_dsc);
  if (discarded) {
    lv_image_cache_drop(&discarded_dsc);
    squircle_free(discarded);
  }
}
void show_placeholder() {
  hidden(art_img, true);
  hidden(placeholder, false);
  hidden(glyph, false);
  lv_label_set_text(glyph, ICON_MUSIC);
}

void placeholder_due(lv_timer_t *timer) {
  lv_timer_delete(timer);
  placeholder_timer = nullptr;
  if (pending_art_url[0]) {
    show_placeholder();
    current_placeholder = true;
    carousel_refresh(1);
  }
}

void side_placeholder_due(lv_timer_t *timer) {
  int side = static_cast<int>(reinterpret_cast<intptr_t>(
      lv_timer_get_user_data(timer)));
  lv_timer_delete(timer);
  side_placeholder_timer[side] = nullptr;
  if (!side_loading[side] || !side_available[side])
    return;
  side_placeholder[side] = true;
  carousel_refresh(side == 0 ? 0 : 2);
}

void remember_skip(int steps) {
  if (skip_result_count == SKIP_RESULTS) {
    skip_result_head = (skip_result_head + 1) % SKIP_RESULTS;
    --skip_result_count;
  }
  int tail = (skip_result_head + skip_result_count) % SKIP_RESULTS;
  skip_results[tail] = steps;
  ++skip_result_count;
}

int take_skip_result() {
  if (!skip_result_count)
    return 0;
  int steps = skip_results[skip_result_head];
  skip_result_head = (skip_result_head + 1) % SKIP_RESULTS;
  --skip_result_count;
  return steps;
}

bool commit_skip(int steps, uint32_t settle_ms) {
  if (!steps || !side_available[steps < 0 ? 0 : 1])
    return false;
  if (offline) {
    carousel_settle(0, 280);
    toast_show(ToastKind::Failure, "Offline");
    return false;
  }
  carousel_settle(steps, settle_ms);
  pending_skip = steps;
  local_skip_until = esp_timer_get_time() + 6000000;
  remember_skip(steps);
  haptic_buzz_double();
  esp_event_post(APP_EVENT,
                 steps > 0 ? APP_EVENT_SPOTIFY_NEXT
                           : APP_EVENT_SPOTIFY_PREV,
                 nullptr, 0, 0);
  return true;
}

int peek_at(lv_point_t point) {
  constexpr int PEEK_GAP = 12;
  constexpr int PEEK_TOP = 88;
  constexpr int PEEK_BOTTOM = 248;
  constexpr int CENTER_LEFT = (360 - ART) / 2;
  constexpr int CENTER_RIGHT = CENTER_LEFT + ART;
  if (point.y < PEEK_TOP || point.y > PEEK_BOTTOM)
    return 0;
  if (point.x < CENTER_LEFT - PEEK_GAP)
    return -1;
  if (point.x > CENTER_RIGHT + PEEK_GAP)
    return 1;
  return 0;
}

void pressed(lv_event_t *) {
  poke();
  if (surface() != Surface::Playing || !has_track) return;
  touch_down = true;
  seeking = false;
  volume_shown = false;
  encoder_during_touch = 0;
  strip_dragging = false;
  touch_axis = TouchAxis::None;
  touch_velocity = 0;
  touch_sample_at = esp_timer_get_time();
  if (lv_indev_t *indev = lv_indev_active()) {
    lv_indev_get_point(indev, &touch_start);
    touch_sample = touch_start;
  }
  fade(art_slot, LV_OPA_60, 100);
  green_ring(ring_value(current_progress(esp_timer_get_time())));
}
void pressing(lv_event_t *) {
  if (!touch_down || surface() != Surface::Playing || seeking)
    return;
  lv_indev_t *indev = lv_indev_active();
  if (!indev)
    return;
  lv_point_t point;
  lv_indev_get_point(indev, &point);
  int32_t dx = point.x - touch_start.x;
  int32_t dy = point.y - touch_start.y;
  if (touch_axis == TouchAxis::None) {
    if (std::abs(dx) < SWIPE_TAKE && std::abs(dy) < SWIPE_TAKE)
      return;
    // A track swipe has to start in the cover band and be clearly
    // sideways. A pull from the top edge drifts sideways as it starts
    // (the screen is round), and must stay with the drawer.
    bool in_band = touch_start.y >= SWIPE_BAND_TOP &&
                   touch_start.y <= SWIPE_BAND_BOTTOM;
    bool sideways = std::abs(dx) * 2 > std::abs(dy) * 3;
    touch_axis = in_band && sideways ? TouchAxis::Horizontal
                                     : TouchAxis::Vertical;
    encoder_during_touch = 1;
    if (touch_axis == TouchAxis::Vertical)
      return;
    strip_dragging = true;
    carousel_begin_drag();
    fade(art_slot, LV_OPA_COVER, 80);
    progress_ring(80);
  }
  if (touch_axis != TouchAxis::Horizontal)
    return;
  int64_t now = esp_timer_get_time();
  int64_t elapsed = now - touch_sample_at;
  if (elapsed >= 10000) {
    touch_velocity = static_cast<int32_t>(
        static_cast<int64_t>(point.x - touch_sample.x) * 1000000 / elapsed);
    touch_sample = point;
    touch_sample_at = now;
  }
  bool empty_side = (dx > 0 && !side_available[0]) ||
                    (dx < 0 && !side_available[1]);
  carousel_drag(empty_side ? dx / 3 : dx);
}
void finish_drag() {
  lv_indev_t *indev = lv_indev_active();
  lv_point_t point = touch_sample;
  if (indev)
    lv_indev_get_point(indev, &point);
  int32_t dx = point.x - touch_start.x;
  int direction = std::abs(dx) >= SWIPE_TAKE ? (dx < 0 ? 1 : -1)
                                             : (touch_velocity < 0 ? 1 : -1);
  bool commit = std::abs(dx) >= SWIPE_COMMIT ||
                std::abs(touch_velocity) >= SWIPE_FLING;
  int steps = commit ? direction : 0;
  bool available = steps == 0 || side_available[steps < 0 ? 0 : 1];
  if (!available)
    steps = 0;
  uint32_t settle_ms = std::abs(touch_velocity) >= SWIPE_FLING ? 180 : 280;
  strip_dragging = false;
  touch_down = false;
  progress_ring();
  if (!commit_skip(steps, settle_ms))
    carousel_settle(0, settle_ms);
}
void finish_touch() {
  if (!touch_down) return;
  bool commit = seeking;
  touch_down = seeking = false;
  fade(art_slot, LV_OPA_COVER, 180);
  hidden(seek_label, true);
  if (commit) {
    int32_t p = seek_ms;
    esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_SEEK, &p, sizeof(p), 0);
    progress_ms = p;
    progress_at = esp_timer_get_time();
  }
  progress_ring();
}
void released(lv_event_t *) {
  if (strip_dragging) {
    finish_drag();
    return;
  }
  if (touch_axis != TouchAxis::None) {
    touch_down = false;
    hidden(seek_label, true);
    progress_ring();
    return;
  }
  finish_touch();
}
void touch_reset(lv_event_t *) {
  if (strip_dragging)
    carousel_settle(0, 220);
  strip_dragging = false;
  touch_axis = TouchAxis::Vertical;
  encoder_during_touch = 1;
  touch_down = false;
  hidden(seek_label, true);
  progress_ring();
}
void tapped(lv_event_t *) {
  poke();
  if (encoder_during_touch) return;
  if (surface() == Surface::Setup || surface() == Surface::Drawer)
    return;
  if (surface() == Surface::Playing && has_track) {
    int steps = peek_at(touch_start);
    if (steps) {
      commit_skip(steps, 280);
      return;
    }
  }
  if (offline) {
    toast_show(ToastKind::Failure, "Offline");
    return;
  }
  if (surface() == Surface::Idle) {
    if (!device_id[0]) {
      toast_show(ToastKind::Failure, "Open Spotify to play");
      return;
    }
    starting = true;
    update_pill();
    volume_ring_set_look({lv_color_white(), LV_OPA_60, RING_TRACK_OPA,
                          RING_W_REST, true}, 180);
    SpotifyTransfer transfer = {};
    snprintf(transfer.device_id, sizeof(transfer.device_id), "%s", device_id);
    snprintf(transfer.device_name, sizeof(transfer.device_name), "%s",
             device_name);
    esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_TRANSFER, &transfer,
                   sizeof(transfer), 0);
    return;
  }
  if (!has_track) return;
  haptic_buzz();
  playing = !playing;
  update_pill();
  esp_event_post(APP_EVENT, playing ? APP_EVENT_SPOTIFY_PLAY : APP_EVENT_SPOTIFY_PAUSE,
                 nullptr, 0, 0);
}
void turn(int32_t steps, int64_t now) {
  poke();
  Surface current = surface();
  if (current == Surface::Setup) {
    haptic_buzz();
    return;
  }
  if (offline) {
    toast_show(ToastKind::Failure, "Offline");
    return;
  }
  if (current == Surface::Idle || current == Surface::Clock) {
    haptic_buzz();
    return;
  }
  if (current == Surface::Drawer && !playing)
    return;
  if (!has_track) return;
  if (touch_down) {
    encoder_during_touch += std::abs(steps);
    haptic_buzz();
    int origin = current_progress(now);
    if (!seeking) {
      seek_ms = origin;
      seeking = true;
    }
    seek_ms = std::clamp(seek_ms + (int)steps * SEEK_STEP, 0, duration_ms);
    green_ring(ring_value(seek_ms));
    char text[16];
    int offset = (seek_ms - origin) / 1000;
    snprintf(text, sizeof(text), offset >= 0 ? "+%ds" : "%ds", offset);
    lv_label_set_text(seek_label, text);
    hidden(seek_label, false);
    return;
  }
  haptic_buzz();
  volume = std::clamp(volume + (int)steps * 2, 0, 100);
  volume_local_until = now + 5000000;
  int32_t v = volume;
  esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_VOLUME_SET, &v, sizeof(v), 0);
  green_ring(volume * 10);
  volume_shown = true;
  volume_hide_at = now + VOL_DISPLAY_MS * 1000LL;
}
void tick(lv_timer_t *) {
  int64_t now = esp_timer_get_time();
  if (!dimmed && now - last_activity > BACKLIGHT_INACTIVITY_MS * 1000LL) {
    display_set_backlight(BACKLIGHT_DIM);
    dimmed = true;
  }
  if (!touch_down && volume_shown && now >= volume_hide_at) {
    volume_shown = false;
    progress_ring(420);
  } else if (!touch_down && !volume_shown && has_track && playing) {
    volume_ring_set_level(ring_value(current_progress(now)));
  }
  int32_t steps = encoder_take_steps();
  if (steps) turn(steps, now);
  if (drawer)
    sheet_close_if_idle(drawer, 10000);
}
lv_obj_t *overlay() {
  lv_obj_t *o = ui_box(screen);
  lv_obj_set_size(o, 360, 360);
  lv_obj_set_style_bg_color(o, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  ui_absorb_taps(o);
  return o;
}
void build_home() {
  backdrop_create(screen, lv_color_hex(ART_TINT_NONE));
  volume_ring_create(screen, 0, RING_MAX, 0);
  progress_ring(0);
  art_slot = ui_box(screen);
  lv_obj_set_size(art_slot, ART, ART);
  lv_obj_align(art_slot, LV_ALIGN_CENTER, 0, stage_tile_y(ART));
  placeholder_pixels = squircle_alloc(ART);
  if (placeholder_pixels)
    squircle_bake(nullptr, lv_color_hex(COL_PLACEHOLDER), ART,
                   placeholder_pixels, &placeholder_dsc);
  placeholder = lv_image_create(art_slot);
  if (placeholder_pixels)
    lv_image_set_src(placeholder, &placeholder_dsc);
  lv_obj_center(placeholder);
  glyph = lv_label_create(art_slot);
  lv_obj_set_style_text_font(glyph, &lucide_48, 0);
  lv_obj_set_style_text_color(glyph, lv_color_white(), 0);
  lv_label_set_text(glyph, ICON_MUSIC);
  lv_obj_center(glyph);
  art_img = lv_image_create(art_slot);
  lv_obj_center(art_img);
  hidden(art_img, true);
  static constexpr CarouselHooks PLAYING_HOOKS = {
      playing_tile, playing_moved, &lucide_48, ART, false, false};
  carousel_create(screen, stage_tile_y(ART), PLAYING_HOOKS);
  seek_label = lv_label_create(screen);
  lv_obj_set_style_text_font(seek_label, &sf_semibold_28, 0);
  lv_obj_set_style_text_color(seek_label, lv_color_hex(COL_GREEN), 0);
  lv_obj_align(seek_label, LV_ALIGN_CENTER, 0, 25);
  hidden(seek_label, true);
  caption = ui_caption(screen);
  pill = ui_pill(screen, ICON_PLAY);
  lv_obj_align(pill.pill, LV_ALIGN_TOP_MID, 0, PILL_TOP);
  lv_obj_move_foreground(volume_ring_obj());
}
void build_status() {
  status = ui_box(screen);
  hidden(status, true);
  status_label = caption.subtitle;
}

void read_clock_idle() {
  nvs_handle_t handle;
  uint8_t enabled = 1;
  if (nvs_open("spotify", NVS_READONLY, &handle) == ESP_OK) {
    nvs_get_u8(handle, "clock_idle", &enabled);
    nvs_close(handle);
  }
  clock_idle = enabled != 0;
}

void save_clock_idle() {
  nvs_handle_t handle;
  if (nvs_open("spotify", NVS_READWRITE, &handle) != ESP_OK)
    return;
  nvs_set_u8(handle, "clock_idle", clock_idle ? 1 : 0);
  nvs_commit(handle);
  nvs_close(handle);
}

void toggle_clock(lv_event_t *) {
  clock_idle = !clock_idle;
  lv_label_set_text(clock_value, clock_idle ? "On" : "Off");
  save_clock_idle();
  haptic_buzz();
}

lv_obj_t *drawer_row(lv_obj_t *parent, bool control) {
  lv_obj_t *row = control ? lv_obj_create(parent) : ui_box(parent);
  lv_obj_set_size(row, 292, 58);
  lv_obj_set_style_radius(row, 18, LV_PART_MAIN);
  lv_obj_set_style_bg_color(row, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(row, LV_OPA_10, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  if (control) {
    lv_obj_set_style_bg_opa(row, LV_OPA_20,
                            LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_30,
                            LV_PART_MAIN | LV_STATE_PRESSED);
  }
  return row;
}

void build_drawer_content() {
  lv_obj_clean(drawer_content);
  read_clock_idle();

  lv_obj_t *toggle = drawer_row(drawer_content, true);
  lv_obj_align(toggle, LV_ALIGN_TOP_MID, 0, 82);
  lv_obj_add_event_cb(toggle, toggle_clock, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *label = lv_label_create(toggle);
  lv_obj_set_style_text_font(label, &sf_regular_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
  lv_label_set_text(label, "Clock when idle");
  lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);
  clock_value = lv_label_create(toggle);
  lv_obj_set_style_text_font(clock_value, &sf_regular_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(clock_value, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_opa(clock_value, TEXT_OPA_SECONDARY, LV_PART_MAIN);
  lv_label_set_text(clock_value, clock_idle ? "On" : "Off");
  lv_obj_align(clock_value, LV_ALIGN_RIGHT_MID, -20, 0);

  lv_obj_t *info = drawer_row(drawer_content, false);
  lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 150);
  lv_obj_t *icon = lv_label_create(info);
  lv_obj_set_style_text_font(icon, &lucide_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(icon, lv_color_white(), LV_PART_MAIN);
  lv_label_set_text(icon, ICON_SPEAKER);
  lv_obj_align(icon, LV_ALIGN_LEFT_MID, 20, 0);
  device_line = lv_label_create(info);
  lv_obj_set_style_text_font(device_line, &sf_regular_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(device_line, lv_color_white(), LV_PART_MAIN);
  char text[96];
  if (device_name[0])
    snprintf(text, sizeof(text), "%s on %s", playing ? "Playing" : "Last",
             device_name);
  else
    snprintf(text, sizeof(text), "No device");
  lv_label_set_text(device_line, text);
  lv_obj_align(device_line, LV_ALIGN_LEFT_MID, 52, 0);
}

bool drawer_may_open() {
  Surface current = surface();
  return current == Surface::Clock || current == Surface::Idle ||
         current == Surface::Playing;
}

bool drawer_may_close(lv_point_t) { return true; }

void drawer_appear() {
  show_playing_strip(false);
  build_drawer_content();
}

void drawer_closed() {
  if (!idle && has_track)
    show_playing_strip(true);
}

void build_drawer() {
  static constexpr SheetPolicy POLICY = {
      .may_open = drawer_may_open,
      .may_close_from = drawer_may_close,
      .on_appear = drawer_appear,
      .on_opened = nullptr,
      .on_closed = drawer_closed,
  };
  drawer = sheet_create(screen, SheetEdge::Top, POLICY);
  drawer_content = sheet_add_content(drawer);
}
void show_qr(const char *title, const char *data, const char *hint) {
  clear_qr();
  show_playback(false);
  show_playing_strip(false);
  hidden(status, true);
  qr_view = overlay();
  lv_obj_t *heading = lv_label_create(qr_view);
  lv_obj_set_style_text_font(heading, &sf_regular_22, 0);
  lv_obj_set_style_text_color(heading, lv_color_white(), 0);
  lv_label_set_text(heading, title);
  lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 32);
  lv_obj_t *card = ui_box(qr_view);
  lv_obj_set_size(card, 164, 164);
  lv_obj_set_style_radius(card, 24, 0);
  lv_obj_set_style_bg_color(card, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_align(card, LV_ALIGN_CENTER, 0, -4);
  lv_obj_t *qr = lv_qrcode_create(card);
  lv_qrcode_set_size(qr, 148);
  lv_qrcode_set_dark_color(qr, lv_color_black());
  lv_qrcode_set_light_color(qr, lv_color_white());
  lv_qrcode_update(qr, data, strlen(data));
  lv_obj_center(qr);
  lv_obj_t *line = lv_label_create(qr_view);
  lv_obj_set_width(line, 280);
  lv_obj_set_style_text_font(line, &sf_regular_16, 0);
  lv_obj_set_style_text_color(line, lv_color_white(), 0);
  lv_obj_set_style_text_opa(line, TEXT_OPA_SECONDARY, 0);
  lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(line, hint);
  lv_obj_align(line, LV_ALIGN_CENTER, 0, 112);
}
} // namespace

void ui_init() {
  lv_display_t *d = nullptr;
  lv_indev_t *touch = nullptr;
  display_init(&d, &touch);
  if (!display_lock(1000)) return;
  screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(screen, pressed, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(screen, pressing, LV_EVENT_PRESSING, nullptr);
  lv_obj_add_event_cb(screen, released, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(screen, touch_reset, LV_EVENT_PRESS_LOST, nullptr);
  lv_obj_add_event_cb(screen, touch_reset, LV_EVENT_INDEV_RESET, nullptr);
  lv_obj_add_event_cb(screen, tapped, LV_EVENT_SHORT_CLICKED, nullptr);
  build_home();
  build_status();
  build_drawer();
  sheets_track_touch(touch);
  toast_attach(pill, update_pill);
  show_playback(false);
  last_activity = esp_timer_get_time();
  lv_timer_create(tick, ENCODER_POLL_MS, nullptr);
  display_set_backlight(BACKLIGHT_NORMAL);
  display_unlock();
  art_queue = xQueueCreate(6, sizeof(ArtRequest));
  xTaskCreatePinnedToCore(art_task, "spotify_art", 8192, nullptr, 4, nullptr,
                          1);
}
void ui_show_splash() { ui_set_status("Starting Spotify…"); }
void ui_splash_set_status(const char *m) { ui_set_status(m); }
void ui_dismiss_splash() {}
void ui_set_status(const char *m) {
  if (!display_lock(200)) return;
  clear_qr();
  idle = false;
  has_track = false;
  show_playing_strip(false);
  show_playback(true);
  ui_marquee_text(caption.title, "Spotify");
  // The caption lines are one line each, centred on fixed rows, so a
  // two-line message would grow up into the title. Split it instead.
  const char *nl = strchr(m, '\n');
  if (nl) {
    char first[64];
    strlcpy(first, m, std::min(sizeof(first), (size_t)(nl - m) + 1));
    lv_label_set_text(status_label, first);
    lv_label_set_text(caption.detail, nl + 1);
  } else {
    lv_label_set_text(status_label, m);
    lv_label_set_text(caption.detail, "");
  }
  lv_image_set_src(art_img, &spotify_logo_64);
  hidden(placeholder, false);
  hidden(glyph, true);
  hidden(art_img, false);
  update_pill();
  volume_ring_set_look({lv_color_white(), 0, 0, RING_W_REST, false}, 0);
  display_unlock();
}
void ui_update_state(const SpotifyState *s) {
  if (!s || !display_lock(200)) return;
  clear_qr();
  SpotifyState shown = *s;
  idle = !s->track[0];
  if (idle) {
    snprintf(device_name, sizeof(device_name), "%s", s->device_name);
    snprintf(device_id, sizeof(device_id), "%s", s->device_id);
    snprintf(shown.track, sizeof(shown.track), "%s", s->last_track);
    snprintf(shown.artist, sizeof(shown.artist), "%s", s->last_artist);
    snprintf(shown.art_url, sizeof(shown.art_url), "%s", s->last_art_url);
    shown.is_playing = false;
  } else {
    if (s->device_name[0]) {
      snprintf(device_name, sizeof(device_name), "%s", s->device_name);
      snprintf(device_id, sizeof(device_id), "%s", s->device_id);
    }
    starting = false;
  }
  s = &shown;
  if (!s->track[0]) {
    has_track = playing = false;
    show_playback(true);
    show_placeholder();
    ui_marquee_text(caption.title, "Nothing playing");
    lv_label_set_text(caption.subtitle, "Play from any Spotify app");
    lv_label_set_text(caption.detail, "");
    update_pill();
    show_playing_strip(false);
    volume_ring_set_look({lv_color_white(), 0, 0, RING_W_REST, false}, 300);
    display_unlock();
    return;
  }
  has_track = true;
  playing = s->is_playing;
  if (esp_timer_get_time() >= volume_local_until) volume = s->volume;
  progress_ms = s->progress_ms;
  duration_ms = s->duration_ms;
  progress_at = esp_timer_get_time();
  show_playback(true);
  bool changed = strcmp(track, s->track) || strcmp(art_url, s->art_url);
  bool knob_change = changed && pending_skip != 0 &&
                     esp_timer_get_time() < local_skip_until;
  bool external_ready = false;
  if (knob_change) {
    int side = pending_skip > 0 ? 1 : 0;
    if (side_art_url[side][0] &&
        strcmp(side_art_url[side], s->art_url) == 0)
      rotate_covers(pending_skip);
  } else if (changed) {
    pending_skip = 0;
    local_skip_until = 0;
    for (int side = 0; side < 2; ++side)
      if (side_art_url[side][0] && cover_pixels[side == 0 ? 0 : 2] &&
          strcmp(side_art_url[side], s->art_url) == 0) {
        rotate_covers(side == 0 ? -1 : 1);
        external_ready = true;
        break;
      }
    external_change_pending = !external_ready;
  }
  show_playing_strip(!idle);
  if (!idle && knob_change && strip_open)
    carousel_reload(3, 1);
  ui_marquee_text(caption.title, s->track);
  lv_label_set_text(caption.subtitle, s->artist);
  // Leave the album line empty rather than repeat the artist or track.
  bool album_repeats = strcasecmp(s->album, s->artist) == 0 ||
                       strcasecmp(s->album, s->track) == 0;
  lv_label_set_text(caption.detail, album_repeats ? "" : s->album);
  update_pill();
  if (idle)
    volume_ring_set_look({lv_color_white(), 0, 0, RING_W_REST, false}, 300);
  if (!idle && !touch_down && !volume_shown) progress_ring(0);
  strlcpy(track, s->track, sizeof(track));
  if (!changed || !s->art_url[0]) {
    if (!s->art_url[0]) {
      art_url[0] = 0;
      show_placeholder();
      backdrop_set(lv_color_hex(ART_TINT_NONE), true);
    }
    display_unlock();
    return;
  }
  strlcpy(art_url, s->art_url, sizeof(art_url));
  strlcpy(pending_art_url, s->art_url, sizeof(pending_art_url));
  if (placeholder_timer)
    lv_timer_delete(placeholder_timer);
  placeholder_timer = lv_timer_create(placeholder_due, 600, nullptr);
  lv_timer_set_repeat_count(placeholder_timer, 1);
  enqueue_art(ArtSlot::Current, s->art_url);
  const char *side_urls[2] = {s->last_art_url, s->next_art_url};
  for (int i = 0; i < 2; ++i) {
    bool side_changed = strcmp(side_art_url[i], side_urls[i]) != 0;
    side_available[i] = side_urls[i][0] != '\0';
    if (!side_changed)
      continue;
    strlcpy(side_art_url[i], side_urls[i], sizeof(side_art_url[i]));
    side_loading[i] = side_available[i];
    side_placeholder[i] = false;
    if (side_placeholder_timer[i])
      lv_timer_delete(side_placeholder_timer[i]);
    side_placeholder_timer[i] = nullptr;
    if (side_loading[i]) {
      side_placeholder_timer[i] = lv_timer_create(
          side_placeholder_due, 600,
          reinterpret_cast<void *>(static_cast<intptr_t>(i)));
      lv_timer_set_repeat_count(side_placeholder_timer[i], 1);
    }
    ArtSlot slot = i == 0 ? ArtSlot::Previous : ArtSlot::Next;
    enqueue_art(slot, side_urls[i]);
    carousel_refresh(i == 0 ? 0 : 2);
  }
  if (external_ready && strip_open)
    carousel_crossfade_reload(3, 1, 240);
  display_unlock();
}
void ui_show_volume(int v) {
  if (!display_lock(50)) return;
  green_ring(v * 10);
  volume_shown = true;
  volume_hide_at = esp_timer_get_time() + VOL_DISPLAY_MS * 1000LL;
  display_unlock();
}
void ui_control_result(const SpotifyControlResult *result) {
  if (!result || !display_lock(200)) return;
  bool ok = result->status >= 200 && result->status < 300;
  bool skip = result->event_id == CONTROL_NEXT ||
              result->event_id == CONTROL_PREV;
  if (skip) {
    int steps = take_skip_result();
    if (!ok) {
      pending_skip = 0;
      local_skip_until = 0;
      if (steps)
        carousel_settle(-steps, 280);
      toast_show(ToastKind::Failure,
                 result->status == 403 ? "Needs Spotify Premium"
                                       : "Couldn't skip track");
    }
    display_unlock();
    return;
  }
  if (result->status == 429) {
    starting = false;
    update_pill();
    display_unlock();
    return;
  }
  if (result->device_name[0]) {
    starting = false;
    char text[96];
    if (ok) {
      haptic_buzz();
      snprintf(text, sizeof(text), "Playing on %s", result->device_name);
      toast_show(ToastKind::Success, text);
    } else {
      snprintf(text, sizeof(text), "Couldn't start on %s", result->device_name);
      toast_show(ToastKind::Failure, text);
    }
    update_pill();
    display_unlock();
    return;
  }
  if (!ok) {
    if (result->event_id == 1 || result->event_id == 2)
      playing = !playing;
    if (result->event_id == 0)
      volume_local_until = 0;
    update_pill();
    toast_show(ToastKind::Failure,
               result->status == 403 ? "Needs Spotify Premium"
                                     : "Couldn't control Spotify");
  }
  display_unlock();
}

void ui_poll_status(int status_code) {
  if (!display_lock(100)) return;
  if (status_code == 200 || status_code == 204)
    poll_failures = 0;
  else if (status_code != 429)
    poll_failures++;
  update_pill();
  display_unlock();
}

void ui_set_offline(bool value) {
  if (!display_lock(200)) return;
  offline = value;
  starting = false;
  if (offline)
    volume_ring_set_look({lv_color_white(), 0, 0, RING_W_REST, false}, 180);
  update_pill();
  display_unlock();
}
void ui_show_wifi_setup(const char *name) {
  if (!display_lock(1000)) return;
  char data[96], hint[128];
  snprintf(data, sizeof(data), "WIFI:T:nopass;S:%s;;", name);
  snprintf(hint, sizeof(hint), "Scan to join \xE2\x80\x9C%s\xE2\x80\x9D,\nthen choose your Wi-Fi", name);
  show_qr("Set up Wi-Fi", data, hint);
  display_unlock();
}
void ui_show_spotify_setup(const char *url) {
  if (!display_lock(1000)) return;
  show_qr("Set up Spotify", url, "Scan with your phone to sign in");
  display_unlock();
}
