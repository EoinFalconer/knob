#include "ui.h"
#include "activity.h"
#include "app_config.h"
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
#include "settings.h"
#include "sheet.h"
#include "sheet_list.h"
#include "sonos.h"
#include "sonos_group.h"
#include "squircle.h"
#include "theme.h"
#include "ui/faces.h"
#include "ui/images/images.h"
#include "ui/now_playing.h"
#include "ui/splash.h"

#include "ui/timer_sheet.h"
#include "ui_pages.h"
#include "volume_ring.h"

#include "esp_heap_caps.h"

#include "esp_log.h"
#include "lvgl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <utility>

static constexpr const char *TAG = "ui";

// ─── Interaction Model
// ────────────────────────────────────────────────────────────────────
//
//  VOLUME mode (default):
//    encoder turn  → volume arc brightens + number appears
//    screen tap    → enter BROWSE mode
//
//  BROWSE mode:
//    encoder turn  → glide the station strip (ui/carousel.h; wraps)
//    screen tap    → select station, start streaming, → VOLUME
//    IDLE_MS idle  → cancel, revert, → VOLUME
//

enum class Mode { Volume, Browse };

static constexpr int VOL_DISPLAY_MS = 1500;
static constexpr int VOL_LOCAL_GRACE_MS = 2000;
// Ignore polled state this long after a local change, so an in-flight poll
// from before the command doesn't flip the UI back.
static constexpr int STATE_LOCAL_GRACE_MS = 1500;
// A regroup takes a topology read or two to show up. Until the speakers report
// the group the user made, keep showing it, unless they never do.
static constexpr int GROUP_CONFIRM_TIMEOUT_MS = 6000;
static constexpr int ENCODER_POLL_MS = 20;
static constexpr int ANIM_FADE_MS = 200;
static constexpr int ANIM_QUICK_MS = 100;

// Volume ring. Turning the knob swells it; letting go settles it back slower,
// so the change is felt at once but never snaps away.
static constexpr uint8_t RING_W_ACTIVE = 8;
static constexpr uint8_t RING_W_FACES = 6;
static constexpr lv_opa_t RING_LEVEL_OPA_BROWSE = 56;
static constexpr lv_opa_t RING_TRACK_OPA_ACTIVE = 46;
static constexpr lv_opa_t RING_TRACK_OPA_FACES = 56;
static constexpr uint32_t RING_SWELL_MS = 140;
static constexpr uint32_t RING_SETTLE_MS = 420;
static constexpr uint32_t RING_ENTER_MS = 240;
static constexpr uint32_t RING_RETRACT_MS = 560;
// How long the faces take to fade in over the home screen (faces.cpp).
static constexpr uint32_t FACES_FADE_MS = 300;

static constexpr uint8_t BACKLIGHT_NORMAL = 80;
static constexpr uint8_t BACKLIGHT_DIM = 10;
static constexpr int BACKLIGHT_FADE_STEP_MS = 30;

static constexpr int ART_BUF_SIZE = 256 * 1024;
static constexpr int ART_SIZE = 120; // artwork widget size (px)

// Now playing is laid out like every home screen (caption.h): the route
// pill, the artwork, then the text. The pill weighs down the top so the block
// sits a touch above center, where a round face reads as balanced.
static constexpr int EXTERNAL_IDLE_TIMEOUT_MS = 300000; // 5 minutes

// ─── Palette
// ────────────────────────────────────────────────────────────────────────────

#define COL_BG lv_color_hex(0x000000)
#define COL_TEXT lv_color_hex(0xFFFFFF)

// ─── State
// ──────────────────────────────────────────────────────────────────────────────

static Mode s_mode = Mode::Volume;
static int s_volume;
static int s_station_index;
static int s_browse_index;
static PlayState s_play_state = PlayState::Stopped;
static uint32_t s_local_play_ms;
static uint8_t s_actions; // TransportAction bits, 0 = unknown
static bool s_was_playing;
static bool s_idle_active = false;
static bool s_external_playing = false;
static bool s_browse_has_external = false;
static MediaInfo s_media = {};
static volatile bool s_art_fetching = false;
static uint32_t s_track_changed_ms;

// ─── Widgets
// ────────────────────────────────────────────────────────────────────────────

static lv_obj_t *s_screen;
static lv_obj_t *s_home;

// Status bar (top)
static lv_obj_t *s_wifi_dot;

// Background (two solid color layers for tear-free crossfade)

// Artwork area (center)
static lv_obj_t *s_img_logo;

// Info area (below artwork)
static lv_obj_t *s_lbl_station;
static lv_obj_t *s_lbl_subtitle;

// Browse mode extras
static lv_obj_t *s_lbl_position;

// Speaker (bottom)
static lv_obj_t *s_route; // pill naming the rooms that play
static lv_obj_t *s_route_label;
static IconPill s_route_pill;

// Artwork container (rounded card with shadow)
// Holds the artwork, so browse can hide it under the strip without fighting
// the code that shows and fades the artwork itself.
static lv_obj_t *s_art_slot;
static lv_obj_t *s_logo_container;

// Media controls (external sources only)
static lv_obj_t *s_btn_prev;
static lv_obj_t *s_btn_next;
static lv_obj_t *s_btn_play;

static lv_timer_t *s_vol_hide_timer;
static bool s_vol_active; // the knob is being turned
static RingLook s_ring_look;
static uint32_t s_local_vol_ms; // Timestamp of last local volume change

// Browse timeout
static lv_timer_t *s_browse_timer;

// External media idle timeout (revert to radio after inactivity)
static lv_timer_t *s_external_idle_timer;

// Touch press detection (manual long-press via timer)
static lv_timer_t *s_press_timer;
static bool s_press_was_long;
static bool s_swiped; // this touch already changed the face
static lv_point_t s_press_point;

// Backlight dimming
static lv_timer_t *s_bl_timer;
static lv_timer_t *s_bl_inactivity_timer;
static uint8_t s_bl_current = BACKLIGHT_NORMAL;
static uint8_t s_bl_target = BACKLIGHT_NORMAL;

// Album art (external media)
static uint8_t *s_art_jpeg;      // Raw JPEG download buffer (PSRAM)
static uint8_t *s_art_pixels;    // Decoded RGB565 pixels (PSRAM)
static lv_image_dsc_t s_art_dsc; // LVGL image descriptor for decoded art
static lv_obj_t *s_art_placeholder;
static lv_obj_t *s_art_glyph;
// What the artwork tile shows, baked into an app icon's shape: the logo or
// cover on its color, and the placeholder's empty tile.
static uint8_t *s_tile_pixels;
static lv_image_dsc_t s_tile_dsc;
static uint8_t *s_placeholder_pixels;
static lv_image_dsc_t s_placeholder_dsc;
static char s_art_last_url[256]; // Dedup — skip re-download if URL unchanged
static uint32_t s_art_tint;      // background color picked from the art

// Speaker picker: a sheet over the home screen
static lv_obj_t *s_speaker_sheet;
static lv_obj_t *s_speaker_content; // rebuilt; the sheet's grabber isn't
static bool s_speaker_list_stale = true;
static lv_obj_t *s_picker_list;
static DiscoveryResult s_discovered;
static int s_speaker_highlight;
static char s_current_speaker[64];
static SonosGroup s_group;        // the controlled room's group
static uint32_t s_local_group_ms; // 0 once the speakers confirm it

// ─── Station Images (compiled into firmware as C arrays)
// ────────────────────────────────────────────────────────

static const lv_image_dsc_t *const s_logos[STATION_COUNT] = {
    &nrk_p1, &nrk_p2, &nrk_p3, &nrk_mp3,    &nrk_jazz,    &nrk_nyheter,
    &p4,     &p5,     &p9,     &radio_rock, &radio_norge, &nrj,
};

static void show_on_tile(const lv_image_dsc_t *picture, lv_color_t color) {
  if (!s_tile_pixels)
    s_tile_pixels = squircle_alloc(ART_SIZE);
  if (!s_tile_pixels)
    return;
  squircle_bake(picture, color, ART_SIZE, s_tile_pixels, &s_tile_dsc);
  lv_image_set_src(s_img_logo, &s_tile_dsc);
}

static void set_logo(int index) {
  if (index < 0 || index >= STATION_COUNT)
    return;
  show_on_tile(s_logos[index], lv_color_hex(STATIONS[index].color));
  // A station always has its logo, never the cover placeholder.
  lv_obj_remove_flag(s_img_logo, LV_OBJ_FLAG_HIDDEN);
  if (s_art_placeholder)
    lv_obj_add_flag(s_art_placeholder, LV_OBJ_FLAG_HIDDEN);
}

// The station's color for radio, the artwork's tint for anything else.
static void update_bg(bool animate) {
  uint32_t color = STATIONS[s_station_index].color;
  if (s_external_playing)
    color = s_art_pixels ? s_art_tint : ART_TINT_NONE;
  backdrop_set(lv_color_hex(color), animate);
}

// ─── Forward Declarations
// ─────────────────────────────────────────────────────────────

static void enter_browse();
static void exit_browse();
static void confirm_browse();
static void update_subtitle();
static bool home_should_idle();
static void show_idle_ui(bool idle);
static void do_tap();
static void do_long_press();
static void on_page_changed(int index, const char *id);
static void on_encoder_poll(lv_timer_t *);
static void on_prev_tap(lv_event_t *);
static void on_next_tap(lv_event_t *);

static void update_media_controls();
static void select_speaker(int index);

// ─── Animation Helpers
// ────────────────────────────────────────────────────────────────

static void anim_opa_cb(void *obj, int32_t v) {
  lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), v, LV_PART_MAIN);
}

static void anim_hide_done(lv_anim_t *a) {
  lv_obj_add_flag(static_cast<lv_obj_t *>(a->var), LV_OBJ_FLAG_HIDDEN);
}

static void set_title(const char *text) {
  ui_marquee_text(s_lbl_station, text);
}

static void anim_fade(lv_obj_t *obj, lv_anim_exec_xcb_t exec_cb, int32_t start,
                      int32_t end, int duration,
                      void (*done_cb)(lv_anim_t *) = nullptr) {
  // Skip animation — apply final value immediately
  if (duration <= 0) {
    exec_cb(obj, end);
    if (done_cb) {
      lv_anim_t dummy = {};
      lv_anim_set_var(&dummy, obj);
      done_cb(&dummy);
    }
    return;
  }

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, exec_cb);
  lv_anim_set_values(&a, start, end);
  lv_anim_set_duration(&a, duration);
  motion_ease_out(&a);
  if (done_cb)
    lv_anim_set_completed_cb(&a, done_cb);
  lv_anim_start(&a);
}

// ─── Volume Arc
// ─────────────────────────────────────────────────────────────────────

// ─── Backlight Fade
// ─────────────────────────────────────────────────────────────────────

static void on_backlight_step(lv_timer_t *) {
  if (s_bl_current == s_bl_target) {
    lv_timer_pause(s_bl_timer);
    return;
  }
  if (s_bl_current < s_bl_target)
    s_bl_current = std::min<uint8_t>(s_bl_current + 3, s_bl_target);
  else
    s_bl_current = (s_bl_current > s_bl_target + 3)
                       ? static_cast<uint8_t>(s_bl_current - 3)
                       : s_bl_target;
  display_set_backlight(s_bl_current);
}

static void backlight_fade_to(uint8_t target) {
  s_bl_target = target;
  if (s_bl_current == target)
    return;
  lv_timer_reset(s_bl_timer);
  lv_timer_resume(s_bl_timer);
}

// Where an untouched screen settles: dim, except while a timer counts down or
// rings, which should read from across the kitchen.
static void backlight_rest() {
  backlight_fade_to(timer_running() || timer_ringing() ? BACKLIGHT_NORMAL
                                                       : BACKLIGHT_DIM);
}

// Whatever woke the screen (a drawer, a tap on the faces, music), it dims
// again once nothing has touched it for a while.
static void on_bl_inactivity(lv_timer_t *) {
  lv_timer_pause(s_bl_inactivity_timer);
  backlight_rest();
}

static void backlight_poke() {
  backlight_fade_to(BACKLIGHT_NORMAL);
  if (s_bl_inactivity_timer) {
    lv_timer_reset(s_bl_inactivity_timer);
    lv_timer_resume(s_bl_inactivity_timer);
  }
}

static void sync_volume_ring(bool animate);

static void set_volume_active(bool active) {
  if (active == s_vol_active)
    return;
  s_vol_active = active;
  sync_volume_ring(true);
}

static void on_vol_hide(lv_timer_t *) {
  set_volume_active(false);
  lv_timer_pause(s_vol_hide_timer);
  if (s_idle_active)
    backlight_rest();
}

// ─── Idle
// ────────────────────────────────────────────────────────────────────────────

static bool home_should_idle() {
  return s_play_state == PlayState::Stopped && s_mode == Mode::Volume;
}

static void show_idle_ui(bool idle) {
  if (!pages_is_home())
    return;
  if (idle == s_idle_active)
    return;
  s_idle_active = idle;

  if (idle) {
    backlight_rest();
    lv_timer_pause(s_bl_inactivity_timer);
  } else {
    backlight_poke();
  }

  lv_anim_delete(s_logo_container, anim_opa_cb);
  lv_anim_delete(s_lbl_station, anim_opa_cb);
  lv_anim_delete(s_route, anim_opa_cb);
  lv_anim_delete(s_lbl_subtitle, anim_opa_cb);
  backdrop_finish();

  faces_show(idle);
  sync_volume_ring(true);

  if (idle) {
    // The faces cover all of this. It still fades so nothing pops back when
    // idle ends.
    int32_t logo_opa = lv_obj_get_style_opa(s_logo_container, LV_PART_MAIN);
    anim_fade(s_logo_container, anim_opa_cb, logo_opa, LV_OPA_TRANSP,
              ANIM_FADE_MS, anim_hide_done);
    anim_fade(s_lbl_station, anim_opa_cb, LV_OPA_COVER, LV_OPA_TRANSP,
              ANIM_FADE_MS, anim_hide_done);
    anim_fade(s_route, anim_opa_cb, LV_OPA_COVER, LV_OPA_TRANSP, ANIM_FADE_MS,
              anim_hide_done);
    lv_obj_set_style_opa(s_lbl_subtitle, LV_OPA_TRANSP, LV_PART_MAIN);
    return;
  }

  if (!s_external_playing) {
    lv_obj_remove_flag(s_logo_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_logo_container, LV_OPA_TRANSP, LV_PART_MAIN);
    anim_fade(s_logo_container, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
              ANIM_FADE_MS);
  }
  lv_obj_remove_flag(s_lbl_station, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_flag(s_route, LV_OBJ_FLAG_HIDDEN,
                  !lv_label_get_text(s_route_label)[0]);
  lv_obj_remove_flag(s_lbl_subtitle, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_opa(s_lbl_station, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_opa(s_route, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_opa(s_lbl_subtitle, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_align(s_lbl_subtitle, LV_ALIGN_CENTER, 0, CAPTION_SUBTITLE_Y);
  anim_fade(s_lbl_station, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
            ANIM_FADE_MS);
  anim_fade(s_route, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, ANIM_FADE_MS);
  anim_fade(s_lbl_subtitle, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
            ANIM_FADE_MS);
}

static void show_volume(int level) {
  backlight_poke();
  set_volume_active(true);
  volume_ring_set_level(level);

  lv_timer_reset(s_vol_hide_timer);
  lv_timer_resume(s_vol_hide_timer);
}

// ─── Mode Switching
// ─────────────────────────────────────────────────────────────────

static int browse_total() {
  return STATION_COUNT + (s_browse_has_external ? 1 : 0);
}

// Browse items count from 0; the entry for what's playing elsewhere (Spotify,
// TV) comes first, as index -1, then the stations.
static int browse_base() { return s_browse_has_external ? -1 : 0; }

static void browse_update_position() {
  char pos[24];
  snprintf(pos, sizeof(pos), "%d / %d", s_browse_index - browse_base() + 1,
           browse_total());
  lv_label_set_text(s_lbl_position, pos);
}

static void show_browse_item() {
  if (s_browse_index < 0) {
    set_title(s_media.title[0] ? s_media.title : "Unknown");
    lv_label_set_text(s_lbl_subtitle,
                      s_media.source[0] ? s_media.source : "Now playing");
  } else {
    set_title(STATIONS[s_browse_index].name);
    lv_label_set_text(s_lbl_subtitle, "Tap to play");
  }
  browse_update_position();
}

static CarouselTile browse_tile(int item) {
  int index = item + browse_base();
  if (index >= 0)
    return {s_logos[index], lv_color_hex(STATIONS[index].color), nullptr,
            false};
  if (s_art_pixels)
    return {&s_art_dsc, lv_color_hex(s_art_tint), nullptr, false};
  return {nullptr, lv_color_hex(COL_PLACEHOLDER), ICON_MUSIC, false};
}

// The title follows the strip: it fades out as a tile leaves the middle and
// changes at the halfway point, when it's gone, so it always names the tile
// nearest the middle.
static void browse_moved(int item, int32_t offset) {
  if (s_mode != Mode::Browse)
    return;
  int index = item + browse_base();
  if (index != s_browse_index) {
    s_browse_index = index;
    show_browse_item();
  }
  if (offset == 0 && lv_anim_get(s_lbl_station, anim_opa_cb))
    return; // still fading in with the rest of the screen
  lv_anim_delete(s_lbl_station, anim_opa_cb);
  lv_obj_set_style_opa(
      s_lbl_station,
      static_cast<lv_opa_t>(LV_OPA_COVER * (500 - std::abs(offset)) / 500),
      LV_PART_MAIN);
}

// The ring dims for browse once the strip has opened, and comes back once it
// has closed: both repaint a lot, and in the same frames they stuttered.
static void browse_opened() {
  if (s_mode == Mode::Browse)
    sync_volume_ring(true);
}

// The tile now playing is the one the strip settled on, so swapping back to
// it shows no change.
static void browse_closed() {
  if (s_mode == Mode::Browse)
    return;
  lv_obj_remove_flag(s_art_slot, LV_OBJ_FLAG_HIDDEN);
  sync_volume_ring(true);
}

static void enter_browse() {
  // Browse lives on the now-playing surface, whatever played last.
  show_idle_ui(false);
  s_mode = Mode::Browse;
  update_media_controls();
  s_was_playing = (s_play_state == PlayState::Playing);
  s_browse_has_external = s_external_playing;
  s_browse_index = s_browse_has_external ? -1 : s_station_index;

  lv_obj_set_style_text_color(s_lbl_station, COL_TEXT, LV_PART_MAIN);
  lv_obj_add_flag(s_art_slot, LV_OBJ_FLAG_HIDDEN);
  carousel_open(browse_total(), s_browse_index - browse_base(), browse_opened);

  show_browse_item();
  lv_obj_set_style_text_opa(s_lbl_subtitle, TEXT_OPA_SOFT, LV_PART_MAIN);
  lv_anim_delete(s_lbl_subtitle, anim_opa_cb);
  lv_obj_set_style_opa(s_lbl_subtitle, LV_OPA_TRANSP, LV_PART_MAIN);
  anim_fade(s_lbl_subtitle, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
            ANIM_QUICK_MS);

  lv_obj_set_style_opa(s_lbl_position, LV_OPA_TRANSP, LV_PART_MAIN);
  if (!timer_running()) {
    lv_obj_remove_flag(s_lbl_position, LV_OBJ_FLAG_HIDDEN);
    anim_fade(s_lbl_position, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
              ANIM_QUICK_MS);
  }

  s_vol_active = false;
  lv_timer_pause(s_vol_hide_timer);

  lv_timer_reset(s_browse_timer);
  lv_timer_resume(s_browse_timer);
}

// The strip glides back to what plays, the station just picked or the one
// browsing started from, and folds into the artwork.
static void exit_browse() {
  s_mode = Mode::Volume;
  lv_anim_delete(s_lbl_station, anim_opa_cb);
  lv_obj_set_style_opa(s_lbl_station, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_lbl_station, COL_TEXT, LV_PART_MAIN);

  if (s_external_playing) {
    set_title(s_media.title[0] ? s_media.title : "Unknown");
  } else {
    set_title(STATIONS[s_station_index].name);
  }
  update_subtitle();

  update_bg(true);

  if (!s_external_playing)
    set_logo(s_station_index);
  int playing = s_external_playing ? -1 : s_station_index;
  // With nothing playing the clock fades in over the strip, which then needn't
  // glide anywhere underneath it.
  if (s_was_playing)
    carousel_close(playing - browse_base(), browse_closed);
  else
    carousel_hide_after(FACES_FADE_MS, browse_closed);

  if (s_was_playing) {
    lv_anim_delete(s_lbl_subtitle, anim_opa_cb);
    lv_obj_set_style_opa(s_lbl_subtitle, LV_OPA_TRANSP, LV_PART_MAIN);
    anim_fade(s_lbl_subtitle, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
              ANIM_QUICK_MS);
  } else {
    show_idle_ui(true);
  }

  lv_obj_set_style_text_opa(s_lbl_subtitle, TEXT_OPA_SECONDARY, LV_PART_MAIN);
  update_media_controls();

  lv_anim_delete(s_lbl_position, anim_opa_cb);
  anim_fade(s_lbl_position, anim_opa_cb,
            lv_obj_get_style_opa(s_lbl_position, LV_PART_MAIN), LV_OPA_TRANSP,
            ANIM_QUICK_MS, anim_hide_done);

  volume_ring_set_level(s_volume);
  lv_timer_pause(s_browse_timer);
}

static void confirm_browse() {
  // A tap mid-glide picks where the strip is headed.
  s_browse_index = carousel_item() + browse_base();
  if (s_browse_index < 0) {
    // External media entry — just exit browse, keep current stream
    s_was_playing = true;
    exit_browse();
    return;
  }
  // Same station already playing — just go back to volume mode
  if (s_browse_index == s_station_index && !s_external_playing &&
      s_play_state == PlayState::Playing) {
    s_was_playing = true;
    exit_browse();
    return;
  }
  s_external_playing = false;
  s_media = {};
  s_station_index = s_browse_index;
  set_logo(s_station_index);
  update_bg(true);
  int32_t idx = s_station_index;
  esp_event_post(APP_EVENT, APP_EVENT_STATION_CHANGED, &idx, sizeof(idx), 0);
  esp_event_post(APP_EVENT, APP_EVENT_PLAY_REQUESTED, nullptr, 0, 0);
  s_play_state = PlayState::Playing;
  s_local_play_ms = lv_tick_get();
  s_was_playing = true;
  exit_browse();
}

static void on_browse_timeout(lv_timer_t *) { exit_browse(); }

static void on_external_idle_timeout(lv_timer_t *) {
  lv_timer_pause(s_external_idle_timer);
  if (!s_external_playing)
    return;
  if (s_play_state == PlayState::Playing ||
      s_play_state == PlayState::Transitioning)
    return;

  ESP_LOGI(TAG, "External media idle timeout — reverting to radio");
  s_external_playing = false;
  s_media = {};
  s_art_last_url[0] = '\0';
  set_title(STATIONS[s_station_index].name);
  set_logo(s_station_index);
  update_bg(true);
  lv_obj_remove_flag(s_logo_container, LV_OBJ_FLAG_HIDDEN);
  lv_anim_delete(s_logo_container, anim_opa_cb);
  anim_fade(s_logo_container, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
            ANIM_FADE_MS);
  update_subtitle();
  update_media_controls();
  show_idle_ui(home_should_idle());
}

// ─── Subtitle (play state)
// ────────────────────────────────────────────────────────────

static void update_subtitle() {
  if (s_external_playing && s_media.artist[0] &&
      s_play_state == PlayState::Playing) {
    lv_obj_set_width(s_lbl_subtitle, LCD_H_RES - 100);
    lv_obj_set_style_text_align(s_lbl_subtitle, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    ui_marquee_text(s_lbl_subtitle, s_media.artist);
    return;
  }

  lv_label_set_long_mode(s_lbl_subtitle, LV_LABEL_LONG_WRAP);
  lv_obj_set_size(s_lbl_subtitle, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_label_set_text(
      s_lbl_subtitle,
      now_playing_subtitle(s_external_playing, s_media, s_play_state));
}

// ─── Media controls (external sources)
// ────────────────────────────────────────────────
//
// Tapping the artwork toggles play/pause; a play badge shows while paused.

static MediaControls current_controls() {
  return media_controls(s_external_playing, s_play_state, s_actions);
}

static void update_speaker_line() {
  char line[160];
  speaker_line(s_group, s_current_speaker, line, sizeof(line));
  ui_icon_pill_set_text(s_route_pill, line);
  lv_obj_set_flag(s_route, LV_OBJ_FLAG_HIDDEN, !line[0] || s_idle_active);
}

static void show_if(lv_obj_t *obj, bool visible) {
  if (!obj)
    return;
  if (visible)
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void update_media_controls() {
  bool on = s_mode == Mode::Volume && !s_idle_active;
  MediaControls c = current_controls();
  show_if(s_btn_prev, on && c.prev);
  show_if(s_btn_next, on && c.next);
  show_if(s_btn_play, on && c.play_badge);
}

static void toggle_playback() {
  backlight_poke();
  haptic_buzz();
  // Assume the way back is open until the next poll says otherwise, so the
  // play badge shows the moment the music stops.
  if (is_playing(s_play_state)) {
    sonos_pause();
    s_play_state = PlayState::Paused;
    s_actions |= ACTION_PLAY;
  } else {
    sonos_play();
    s_play_state = PlayState::Playing;
    s_actions |= ACTION_PAUSE;
  }
  s_local_play_ms = lv_tick_get();
  update_subtitle();
  update_media_controls();
}

// ─── Surfaces
// ─────────────────────────────────────────────────────────────────────────
//
// What's on screen decides what a gesture means. The flags behind it (picker
// open, pager page, browse mode, idle) are derived into one value here, and
// every gesture handler switches on it rather than testing flags itself.
//
//            tap             hold      swipe up    swipe down   turn
//  Faces     open browse     -         speakers    timer        volume
//            (a swipe sideways brings up the other face)
//  Playing   browse / art    stop      speakers    timer        volume
//            (the route pill at the top opens speakers, the timer pill at
//            the bottom the timer; browse shows the timer pill too)
//  Browse    play station    cancel    speakers    timer        pick station
//  Page      -               -         speakers    -            change page
//  Picker    (its own sheet handles taps)                       highlight
//  Timer     (its own sheet handles taps)                       minutes, or
//                                                               volume once
//                                                               it runs
//            (swiped sideways, the sheet shows the lights: a
//            tap switches them, the knob dims them)
//
// Vertical swipes aren't routed here: sheets recognize them on raw touch
// (ui/sheet.h) and take the touch away, so the screen sees a reset instead of
// a release.
// An open sheet is pushed back toward its own edge.

enum class Surface { Timer, Picker, Page, Faces, Playing, Browse };

static bool picker_open() {
  return s_speaker_sheet && sheet_is_open(s_speaker_sheet);
}

static Surface surface() {
  if (timer_sheet_is_open())
    return Surface::Timer;
  if (picker_open())
    return Surface::Picker;
  if (!pages_is_home())
    return Surface::Page;
  if (s_mode == Mode::Browse)
    return Surface::Browse;
  return s_idle_active ? Surface::Faces : Surface::Playing;
}

// The ring's look follows the home surface, even under a page or the picker.
// On the faces it borrows the face's own color and only shows while the knob
// turns, so it reads as part of the dial rather than a ring laid on top.
static RingLook ring_look() {
  if (surface() == Surface::Browse)
    return {COL_TEXT, RING_LEVEL_OPA_BROWSE, RING_TRACK_OPA, RING_W_REST, true};
  if (s_idle_active)
    return {faces_accent(), LV_OPA_COVER, RING_TRACK_OPA_FACES, RING_W_FACES,
            s_vol_active};
  if (s_vol_active)
    return {COL_TEXT, LV_OPA_COVER, RING_TRACK_OPA_ACTIVE, RING_W_ACTIVE, true};
  return ring_rest_look(COL_TEXT);
}

static uint32_t ring_duration(const RingLook &from, const RingLook &to) {
  if (from.shown != to.shown)
    return to.shown ? RING_ENTER_MS : RING_RETRACT_MS;
  return to.width > from.width ? RING_SWELL_MS : RING_SETTLE_MS;
}

// The only place the ring's look is decided; call it after any state change.
static void sync_volume_ring(bool animate) {
  RingLook look = ring_look();
  volume_ring_set_look(look, animate ? ring_duration(s_ring_look, look) : 0);
  s_ring_look = look;
}

static bool artwork_hit(const lv_point_t &p) {
  if (surface() != Surface::Playing || !current_controls().toggle)
    return false;
  lv_area_t a;
  lv_obj_get_coords(s_logo_container, &a);
  return p.x >= a.x1 && p.x <= a.x2 && p.y >= a.y1 && p.y <= a.y2;
}

// ─── Touch
// ────────────────────────────────────────────────────────────────────────────

static void do_tap() {
  switch (surface()) {
  case Surface::Picker:
    if (s_discovered.count > 0 && s_speaker_highlight >= 0 &&
        s_speaker_highlight < s_discovered.count)
      select_speaker(s_speaker_highlight);
    break;
  case Surface::Faces:
  case Surface::Playing:
    enter_browse();
    break;
  case Surface::Browse:
    confirm_browse();
    break;
  case Surface::Page:
  case Surface::Timer:
    break;
  }
}

static void stop_playback() {
  s_play_state = PlayState::Stopped;
  s_local_play_ms = lv_tick_get();
  esp_event_post(APP_EVENT, APP_EVENT_STOP_REQUESTED, nullptr, 0, 0);
  update_subtitle();
  show_idle_ui(true);
}

static void do_long_press() {
  switch (surface()) {
  case Surface::Browse:
    exit_browse();
    if (is_playing(s_play_state))
      stop_playback();
    break;
  case Surface::Playing:
    if (is_playing(s_play_state))
      stop_playback();
    break;
  case Surface::Picker:
  case Surface::Page:
  case Surface::Faces:
  case Surface::Timer:
    break;
  }
}

static void on_press_timer(lv_timer_t *) {
  s_press_was_long = true;
  lv_timer_pause(s_press_timer);
  do_long_press();
}

// A sheet took the touch over as a drag: it is neither a tap nor a hold.
static void on_screen_reset(lv_event_t *) {
  lv_timer_pause(s_press_timer);
  s_press_was_long = true;
}

static void on_screen_pressed(lv_event_t *) {
  s_press_was_long = false;
  s_swiped = false;
  lv_indev_t *indev = lv_indev_active();
  if (indev)
    lv_indev_get_point(indev, &s_press_point);
  lv_timer_reset(s_press_timer);
  lv_timer_resume(s_press_timer);
}

// Sideways, clearly more across than down. Vertical drags never get here:
// the sheets take them.
static int sideways_swipe(int32_t dx, int32_t dy) {
  constexpr int32_t SWIPE_MIN = 28;
  if (std::abs(dx) < SWIPE_MIN || std::abs(dx) < 2 * std::abs(dy))
    return 0;
  return dx < 0 ? -1 : 1;
}

static lv_point_t travel() {
  lv_point_t p = s_press_point;
  if (lv_indev_t *indev = lv_indev_active())
    lv_indev_get_point(indev, &p);
  return {p.x - s_press_point.x, p.y - s_press_point.y};
}

// The faces change as soon as the finger has clearly gone sideways, not when
// it lifts. Holding does nothing on the faces, so a slow swipe still counts.
static void on_screen_pressing(lv_event_t *) {
  if (s_swiped || surface() != Surface::Faces)
    return;
  lv_point_t d = travel();
  if (int swipe = sideways_swipe(d.x, d.y)) {
    s_swiped = true;
    lv_timer_pause(s_press_timer);
    faces_swipe(swipe);
  }
}

static void on_screen_released(lv_event_t *) {
  lv_timer_pause(s_press_timer);
  if (s_press_was_long || s_swiped)
    return;

  // A finger that travelled was a swipe nobody took, not a tap.
  constexpr int32_t TAP_SLOP = 16;
  lv_point_t d = travel();
  if (std::abs(d.x) > TAP_SLOP || std::abs(d.y) > TAP_SLOP)
    return;

  if (artwork_hit(s_press_point)) {
    toggle_playback();
    return;
  }

  do_tap();
}

// ─── Main Screen
// ────────────────────────────────────────────────────────────────────

// ─── Home Page (page 0 in pager)
// ────────────────────────────────────────────────────────

static void open_picker();

static void on_route_tap(lv_event_t *) {
  backlight_poke();
  haptic_buzz();
  open_picker();
}

// Where the sound goes, as a glass pill like AirPlay's. It doubles as the
// visible way into the speaker picker, which swipe up also opens.
static void build_route_pill(lv_obj_t *parent) {
  s_route_pill = ui_pill(parent, ICON_SPEAKER);
  s_route = s_route_pill.pill;
  s_route_label = s_route_pill.label;
  lv_obj_align(s_route, LV_ALIGN_TOP_MID, 0, PILL_TOP);
  lv_obj_add_flag(s_route, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_route, on_route_tap, LV_EVENT_SHORT_CLICKED, nullptr);
  lv_obj_add_flag(s_route, LV_OBJ_FLAG_HIDDEN);
}

static void home_page_build(lv_obj_t *parent) {
  s_home = parent;

  backdrop_create(parent, lv_color_hex(STATIONS[0].color));

  volume_ring_create(parent, VOLUME_MIN, VOLUME_MAX, s_volume);

  // ── Station logo — an app icon's shape, baked into its pictures ──
  // Only as big as the artwork: hiding it repaints just that.
  s_art_slot = ui_box(parent);
  lv_obj_set_size(s_art_slot, ART_SIZE, ART_SIZE);
  lv_obj_align(s_art_slot, LV_ALIGN_CENTER, 0, STAGE_TILE_Y);
  s_logo_container = lv_obj_create(s_art_slot);
  lv_obj_set_size(s_logo_container, 120, 120);
  lv_obj_set_style_bg_opa(s_logo_container, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_logo_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_logo_container, 0, LV_PART_MAIN);
  lv_obj_remove_flag(s_logo_container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_logo_container, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(s_logo_container);
  static constexpr CarouselHooks BROWSE_HOOKS = {browse_tile, browse_moved,
                                                 &lucide_48};
  carousel_create(parent, STAGE_TILE_Y, BROWSE_HOOKS);

  // ── Prev/Next buttons — flanking album art, only shown in external mode ──
  auto make_media_btn = [&](bool is_next) -> lv_obj_t * {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, 44, 44);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, COL_TEXT, LV_PART_MAIN);
    // Light up on touch-down, not on release: the press must feel heard.
    lv_obj_set_style_bg_opa(
        btn, LV_OPA_20,
        static_cast<lv_style_selector_t>(LV_PART_MAIN | LV_STATE_PRESSED));
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(btn, LV_ALIGN_CENTER, is_next ? 110 : -110, STAGE_TILE_Y);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_color(lbl, COL_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_opa(lbl, TEXT_OPA_SOFT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lucide_22, LV_PART_MAIN);
    lv_label_set_text(lbl, is_next ? ICON_SKIP_FORWARD : ICON_SKIP_BACK);
    lv_obj_center(lbl);
    return btn;
  };

  s_btn_prev = make_media_btn(false);
  s_btn_next = make_media_btn(true);
  lv_obj_add_event_cb(s_btn_prev, on_prev_tap, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(s_btn_next, on_next_tap, LV_EVENT_CLICKED, nullptr);

  // ── Artwork placeholder: a still tile with the source's glyph, under the
  // image so a cover can fade in over it. No pulse: it would repaint the tile
  // on every frame for as long as the download takes.
  s_art_placeholder = lv_image_create(s_logo_container);
  lv_obj_set_size(s_art_placeholder, ART_SIZE, ART_SIZE);
  if (!s_placeholder_pixels)
    s_placeholder_pixels = squircle_alloc(ART_SIZE);
  if (s_placeholder_pixels) {
    squircle_bake(nullptr, lv_color_hex(COL_PLACEHOLDER), ART_SIZE,
                  s_placeholder_pixels, &s_placeholder_dsc);
    lv_image_set_src(s_art_placeholder, &s_placeholder_dsc);
  }
  lv_obj_remove_flag(s_art_placeholder, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_art_placeholder, LV_OBJ_FLAG_HIDDEN);
  s_art_glyph = lv_label_create(s_art_placeholder);
  lv_obj_set_style_text_font(s_art_glyph, &lucide_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_art_glyph, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_opa(s_art_glyph, TEXT_OPA_TERTIARY, LV_PART_MAIN);
  lv_label_set_text(s_art_glyph, ICON_MUSIC);
  lv_obj_center(s_art_glyph);

  s_img_logo = lv_image_create(s_logo_container);
  lv_obj_set_size(s_img_logo, 120, 120);
  lv_image_set_inner_align(s_img_logo, LV_IMAGE_ALIGN_CENTER);
  lv_obj_set_pos(s_img_logo, 0, 0);
  set_logo(0);

  // ── Play badge — shown on the artwork while an external source is paused ──
  s_btn_play = lv_obj_create(s_logo_container);
  lv_obj_set_size(s_btn_play, 52, 52);
  lv_obj_set_style_radius(s_btn_play, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_btn_play, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_btn_play, LV_OPA_50, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_btn_play, 0, LV_PART_MAIN);
  lv_obj_remove_flag(s_btn_play, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_btn_play, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(s_btn_play);
  lv_obj_add_flag(s_btn_play, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *play_icon = lv_label_create(s_btn_play);
  lv_obj_set_style_text_color(play_icon, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(play_icon, &lucide_22, LV_PART_MAIN);
  lv_label_set_text(play_icon, ICON_PLAY);
  // A triangle's mass sits a third of the way from its flat side, so centering
  // its box makes it look shifted left; this puts the centroid near center.
  lv_obj_align(play_icon, LV_ALIGN_CENTER, 1, 0);

  // ── Station name, play state or "Tap to play", and the position while
  // browsing ──
  Caption caption = ui_caption(parent);
  s_lbl_station = caption.title;
  s_lbl_subtitle = caption.subtitle;
  s_lbl_position = caption.detail;
  set_title(STATIONS[0].name);
  lv_obj_add_flag(s_lbl_position, LV_OBJ_FLAG_HIDDEN);

  build_route_pill(parent);
  timer_pill_create(parent);

  // Faces cover everything on the page except the volume arc.
  faces_create(parent);
  lv_obj_move_foreground(volume_ring_obj());
  if (s_idle_active)
    faces_show(true);
  sync_volume_ring(false);
}

static void home_page_destroy() { s_home = nullptr; }

static const PageDef s_home_page = {
    .id = "home",
    .build = home_page_build,
    .destroy = home_page_destroy,
    .tick = nullptr,
};

static void build_main_screen() {
  s_screen = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_screen, COL_BG, LV_PART_MAIN);
  lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_screen, LCD_H_RES, LCD_V_RES);
  lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_screen, on_screen_pressed, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(s_screen, on_screen_reset, LV_EVENT_INDEV_RESET, nullptr);
  lv_obj_add_event_cb(s_screen, on_screen_released, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(s_screen, on_screen_pressing, LV_EVENT_PRESSING, nullptr);

  // Pager: creates the horizontal strip and builds home page into it
  pages_init(s_screen, &s_home_page, on_page_changed);

  // ── Overlays (above pager strip) ──

  // ── WiFi dot — only visible when disconnected (red) ──
  s_wifi_dot = ui_box(s_screen);
  lv_obj_set_size(s_wifi_dot, 8, 8);
  lv_obj_set_style_radius(s_wifi_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_wifi_dot, lv_color_hex(COL_RED), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_wifi_dot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(s_wifi_dot, LV_ALIGN_TOP_MID, 0, 32);
  lv_obj_add_flag(s_wifi_dot, LV_OBJ_FLAG_HIDDEN);
}

// ─── Speaker Picker
// ───────────────────────────────────────────────────────────────

static void close_picker() { sheet_close(s_speaker_sheet); }

static void open_picker() { sheet_open(s_speaker_sheet); }

// The menu leaves once nothing has touched or turned the knob for a while.
// Before any room is chosen it stays until one is.
static void on_picker_idle_check(lv_timer_t *) {
  if (s_current_speaker[0])
    sheet_close_if_idle(s_speaker_sheet, IDLE_MS);
}

// Tap: control this room, on its own. The knob's own radio follows it to the
// new room; the previous group stops so it doesn't keep playing an unsynced
// copy. Groups are only made by holding rows.
static void select_speaker(int index) {
  if (index < 0 || index >= s_discovered.count)
    return;
  auto &speaker = s_discovered.speakers[index];
  const char *resume = switch_resume_url(s_external_playing, s_play_state,
                                         STATIONS[s_station_index].url);
  sonos_select_speaker(speaker.ip, speaker.port, resume);
  settings_set_speaker_name(speaker.name);
  ESP_LOGI(TAG, "Speaker selected: %s (%s)%s", speaker.name, speaker.ip,
           resume ? ", moving the radio along" : "");

  snprintf(s_current_speaker, sizeof(s_current_speaker), "%s", speaker.name);
  s_group = {}; // the next poll reports the new room's group
  s_local_group_ms = 0;
  s_speaker_list_stale = true; // the dot moves
  update_speaker_line();
  close_picker();
}

// ─── Multi-room grouping — long-press a room to add it to the group the knob
// controls (Sonos keeps members sample-synced), or to take it out again.

// AirPlay-style check circle at the end of each row: filled when the room
// plays in the controlled group, an empty ring when it doesn't.
static void style_group_ring(lv_obj_t *card, bool grouped) {
  lv_obj_t *circle = lv_obj_get_child(card, -1);
  lv_obj_set_style_bg_opa(circle, grouped ? LV_OPA_COVER : LV_OPA_TRANSP,
                          LV_PART_MAIN);
  lv_obj_set_style_border_width(circle, grouped ? 0 : 2, LV_PART_MAIN);
  show_if(lv_obj_get_child(circle, 0), grouped);
}

static lv_obj_t *make_group_circle(lv_obj_t *card) {
  lv_obj_t *circle = lv_obj_create(card);
  lv_obj_set_size(circle, LIST_ROW_CIRCLE, LIST_ROW_CIRCLE);
  lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(circle, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_border_color(circle, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_border_opa(circle, LIST_RING_OPA, LV_PART_MAIN);
  lv_obj_set_style_pad_all(circle, 0, LV_PART_MAIN);
  lv_obj_remove_flag(circle, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(circle, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(circle, LV_ALIGN_RIGHT_MID, -LIST_ROW_INSET, 0);

  lv_obj_t *check = lv_label_create(circle);
  lv_obj_set_style_text_font(check, &lucide_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(check, COL_BG, LV_PART_MAIN);
  lv_label_set_text(check, ICON_CHECK);
  lv_obj_center(check);
  return circle;
}

static void restyle_group_rings() {
  if (!s_picker_list)
    return;
  int cards = static_cast<int>(lv_obj_get_child_count(s_picker_list));
  for (int i = 0; i < std::min(cards, s_discovered.count); i++)
    style_group_ring(lv_obj_get_child(s_picker_list, i),
                     sonos_group_has(s_group, s_discovered.speakers[i].uuid));
}

static bool accept_reported_group(const SonosGroup &reported) {
  if (s_local_group_ms == 0)
    return true;
  if (sonos_group_same_members(reported, s_group) ||
      lv_tick_elaps(s_local_group_ms) >= GROUP_CONFIRM_TIMEOUT_MS)
    s_local_group_ms = 0;
  return s_local_group_ms == 0;
}

static void toggle_speaker_group(int index) {
  if (index < 0 || index >= s_discovered.count)
    return;
  auto &spk = s_discovered.speakers[index];
  GroupToggle toggle = sonos_group_toggle(s_group, spk.uuid);
  if (toggle == GroupToggle::None)
    return;

  if (toggle == GroupToggle::Join)
    sonos_join_group(spk.ip, spk.port);
  else
    sonos_leave_group(spk.ip, spk.port);
  ESP_LOGI(TAG, "%s %s",
           toggle == GroupToggle::Join ? "Grouping" : "Ungrouping", spk.name);

  // Instant feedback; the speakers' own report confirms it shortly after.
  sonos_group_apply(&s_group, toggle, spk.uuid, spk.name);
  s_local_group_ms = lv_tick_get();
  restyle_group_rings();
  update_speaker_line();
  haptic_buzz();
}

static void on_speaker_long_press(lv_event_t *e) {
  auto index =
      static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  toggle_speaker_group(index);
}

static void on_speaker_tap(lv_event_t *e) {
  auto index =
      static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  select_speaker(index);
}

static void highlight_picker_item(int highlight) {
  ui_list_focus(s_picker_list, highlight);
}

static void on_picker_dismiss(lv_event_t *) { close_picker(); }

static void on_prev_tap(lv_event_t *) {
  backlight_poke();
  haptic_buzz();
  sonos_previous();
}

static void on_next_tap(lv_event_t *) {
  backlight_poke();
  haptic_buzz();
  sonos_next();
}

// The sheet lives as long as the device; scanning and the list are only its
// contents. (Deleting a screen while it slid in once froze the device.)
static void reset_picker_content() {
  lv_obj_clean(s_speaker_content);
  s_picker_list = nullptr;
}

static void build_picker_scanning() {
  reset_picker_content();

  lv_obj_t *title = lv_label_create(s_speaker_content);
  lv_obj_set_style_text_color(title, lv_color_hex(COL_GRAY), LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &sf_regular_22, LV_PART_MAIN);
  lv_label_set_text(title, "Speakers");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 54);

  ui_activity_message(s_speaker_content, "Scanning for speakers\u2026");
}

// The scan is the freshest view of who's grouped (e.g. via the Sonos app).
static void adopt_scanned_group() {
  SonosGroup scanned = group_from_discovery(s_discovered, s_current_speaker);
  if (scanned.count > 0 && accept_reported_group(scanned))
    s_group = scanned;
}

static void rebuild_speaker_list() {
  reset_picker_content();

  ui_list_title(s_speaker_content, "Speakers", "Tap to switch, hold to group");

  adopt_scanned_group();

  if (s_discovered.count > 0) {
    s_picker_list = ui_list_create(s_speaker_content, s_speaker_sheet);

    for (int i = 0; i < s_discovered.count; i++) {
      auto &spk = s_discovered.speakers[i];
      lv_obj_t *card = ui_list_row(s_picker_list, spk.name);

      // The controlled room's dot sits centered in the leading slot.
      if (s_current_speaker[0] && strcmp(spk.name, s_current_speaker) == 0) {
        lv_obj_t *dot = lv_obj_create(card);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, lv_color_hex(COL_GREEN), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID,
                     LIST_ROW_INSET + LIST_ROW_CIRCLE / 2 - 3, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
      }

      make_group_circle(card);
      style_group_ring(card, sonos_group_has(s_group, spk.uuid));

      lv_obj_add_event_cb(card, on_speaker_tap, LV_EVENT_SHORT_CLICKED,
                          reinterpret_cast<void *>(static_cast<intptr_t>(i)));
      lv_obj_add_event_cb(card, on_speaker_long_press, LV_EVENT_LONG_PRESSED,
                          reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }

    highlight_picker_item(0);
  } else {
    s_picker_list = nullptr;
    lv_obj_t *empty = lv_label_create(s_speaker_content);
    lv_obj_set_style_text_color(empty, lv_color_hex(COL_GRAY), LV_PART_MAIN);
    lv_obj_set_style_text_font(empty, &sf_regular_16, LV_PART_MAIN);
    lv_label_set_text(empty, "No speakers found.\nCheck your network.");
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
  }

  ui_list_back(s_speaker_content,
               s_current_speaker[0] ? ICON_ARROW_LEFT : ICON_X,
               s_current_speaker[0] ? "Back" : "Dismiss", on_picker_dismiss);
  s_speaker_list_stale = false;
}

// ─── Speakers sheet
//
// It opens on the last scan at once, so the slide never waits on the network,
// and rescans once it has settled; the list only changes if the rooms did.

static bool speakers_may_open() {
  return lv_screen_active() == s_screen && surface() != Surface::Picker;
}

// A drag down over the list scrolls it, unless it's already at the top.
static bool speakers_may_close_from(lv_point_t start) {
  return ui_list_may_close_from(s_picker_list, start);
}

static void speakers_appear() {
  if (s_discovered.count == 0)
    build_picker_scanning();
  else if (s_speaker_list_stale)
    rebuild_speaker_list();
  else
    restyle_group_rings();
  s_speaker_highlight = 0;
  highlight_picker_item(0);
  if (s_picker_list)
    lv_obj_scroll_to_y(s_picker_list, 0, LV_ANIM_OFF);
  backlight_poke();
}

static void speakers_opened() {
  lv_display_trigger_activity(nullptr);
  esp_event_post(APP_EVENT, APP_EVENT_SPEAKER_RESCAN, nullptr, 0, 0);
}

static void build_speaker_sheet() {
  static constexpr SheetPolicy POLICY = {
      .may_open = speakers_may_open,
      .may_close_from = speakers_may_close_from,
      .on_appear = speakers_appear,
      .on_opened = speakers_opened,
      .on_closed = nullptr,
  };
  s_speaker_sheet = sheet_create(s_screen, SheetEdge::Bottom, POLICY);
  s_speaker_content = sheet_add_content(s_speaker_sheet);
}

// ─── Timer sheet

static bool timer_may_open() {
  Surface s = surface();
  return lv_screen_active() == s_screen &&
         (s == Surface::Faces || s == Surface::Playing || s == Surface::Browse);
}

// Browse shows it too, in place of the station count: it sits where the pill
// goes, and a running timer matters more.
static bool timer_pill_shown() {
  Surface s = surface();
  return s == Surface::Playing || s == Surface::Browse;
}

static void timer_wake() {
  backlight_poke();
  lv_display_trigger_activity(nullptr);
}

// Under the sheet the face changes at once: animated, it would play out
// half-covered as the sheet comes or goes.
static void timer_changed() {
  faces_set_timer(timer_running(), !timer_sheet_is_visible());
  // Lit while it runs; once it's over the screen dims after the usual wait.
  backlight_poke();
  if (s_mode != Mode::Browse)
    return;
  lv_anim_delete(s_lbl_position, anim_opa_cb);
  lv_obj_set_style_opa(s_lbl_position, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_flag(s_lbl_position, LV_OBJ_FLAG_HIDDEN, timer_running());
}

static void build_timer_sheet() {
  static constexpr TimerSheetHooks HOOKS = {
      .may_open = timer_may_open,
      .pill_shown = timer_pill_shown,
      .wake = timer_wake,
      .changed = timer_changed,
  };
  timer_sheet_create(s_screen, HOOKS);
}

static bool same_rooms(const DiscoveryResult &a, const DiscoveryResult &b) {
  if (a.count != b.count)
    return false;
  for (int i = 0; i < a.count; i++)
    if (strcmp(a.speakers[i].uuid, b.speakers[i].uuid) != 0 ||
        strcmp(a.speakers[i].name, b.speakers[i].name) != 0)
      return false;
  return true;
}

// ─── Public API
// ─────────────────────────────────────────────────────────────────────

static void on_splash_done(lv_timer_t *t) {
  lv_timer_delete(t);
  if (display_lock(50)) {
    lv_screen_load_anim(s_screen, LV_SCR_LOAD_ANIM_FADE_IN, 400, 0, true);
    display_unlock();
  }
}

void ui_init() {
  lv_display_t *disp = nullptr;
  lv_indev_t *touch = nullptr;
  display_init(&disp, &touch);

  s_volume = settings_get_volume();

  if (display_lock(200)) {
    lv_screen_load(splash_create());
    lv_timer_create(on_splash_done, SPLASH_MS, nullptr);

    build_main_screen();
    build_speaker_sheet();
    build_timer_sheet();
    sheets_track_touch(touch);
    s_vol_hide_timer = lv_timer_create(on_vol_hide, VOL_DISPLAY_MS, nullptr);
    lv_timer_pause(s_vol_hide_timer);

    s_browse_timer = lv_timer_create(on_browse_timeout, IDLE_MS, nullptr);
    lv_timer_pause(s_browse_timer);

    lv_timer_create(on_picker_idle_check, 1000, nullptr);

    s_bl_timer =
        lv_timer_create(on_backlight_step, BACKLIGHT_FADE_STEP_MS, nullptr);
    lv_timer_pause(s_bl_timer);

    s_bl_inactivity_timer = lv_timer_create(on_bl_inactivity, IDLE_MS, nullptr);
    lv_timer_pause(s_bl_inactivity_timer);

    s_press_timer = lv_timer_create(on_press_timer, 500, nullptr);
    lv_timer_pause(s_press_timer);

    s_external_idle_timer = lv_timer_create(on_external_idle_timeout,
                                            EXTERNAL_IDLE_TIMEOUT_MS, nullptr);
    lv_timer_pause(s_external_idle_timer);

    lv_timer_create(on_encoder_poll, ENCODER_POLL_MS, nullptr);

    show_idle_ui(true);

    set_logo(s_station_index);
    update_bg(/*animate=*/false);

    settings_get_speaker_name(s_current_speaker, sizeof(s_current_speaker));
    update_speaker_line();

    update_subtitle();
    // Main screen loaded by on_splash_done timer — not here
    display_unlock();
  }

  ESP_LOGI(TAG, "UI ready");
}

void ui_set_lights(const LightsState *state) {
  if (!display_lock(200))
    return;
  timer_sheet_set_lights(*state);
  display_unlock();
}

void ui_timer_started(int total_s, uint32_t left_ms, const char *label) {
  if (!display_lock(200))
    return;
  timer_sheet_started(total_s, left_ms, label);
  display_unlock();
}

// The sheet drops down over whatever is up.
void ui_timer_fired(const char *label) {
  if (!display_lock(200))
    return;
  if (!pages_is_home())
    pages_go_home();
  close_picker();
  timer_sheet_fired(label);
  display_unlock();
}

void ui_timer_cancelled() {
  if (!display_lock(200))
    return;
  timer_sheet_cancelled();
  display_unlock();
}

void ui_set_volume(int level) {
  if (display_lock(200)) {
    if (lv_tick_elaps(s_local_vol_ms) < VOL_LOCAL_GRACE_MS) {
      display_unlock();
      return;
    }
    if (level != s_volume) {
      if (pages_is_home() && s_mode == Mode::Volume) {
        show_volume(level);
      } else {
        volume_ring_set_level(level);
      }
      s_volume = level;
    }
    display_unlock();
  }
}

void ui_set_play_state(PlayState state) {
  if (state == s_play_state)
    return;
  if (display_lock(50)) {
    if (lv_tick_elaps(s_local_play_ms) < STATE_LOCAL_GRACE_MS) {
      display_unlock();
      return;
    }
    s_play_state = state;
    if (state == PlayState::Stopped && s_external_playing) {
      lv_timer_pause(s_external_idle_timer);
      s_external_playing = false;
      s_media = {};
      set_title(STATIONS[s_station_index].name);
      set_logo(s_station_index);
      lv_obj_remove_flag(s_logo_container, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_opa(s_logo_container, LV_OPA_COVER, LV_PART_MAIN);
      update_bg(true);
    }
    // External idle timeout — revert to radio after 5 min of paused/stopped
    if (s_external_playing) {
      if (state == PlayState::Playing || state == PlayState::Transitioning) {
        lv_timer_pause(s_external_idle_timer);
      } else {
        lv_timer_reset(s_external_idle_timer);
        lv_timer_resume(s_external_idle_timer);
      }
    }
    if (s_mode == Mode::Volume) {
      update_subtitle();
      show_idle_ui(state == PlayState::Stopped);
    }
    update_media_controls();
    display_unlock();
  }
}

void ui_set_transport_actions(uint8_t actions) {
  if (actions == s_actions || !display_lock(50))
    return;
  s_actions = actions;
  update_media_controls();
  display_unlock();
}

void ui_set_station(int index) {
  if (index < 0 || index >= STATION_COUNT)
    return;
  if (index == s_station_index && !s_external_playing)
    return;
  if (display_lock(50)) {
    bool was_external = s_external_playing;
    s_station_index = index;
    s_external_playing = false;
    s_media = {};
    if (s_mode == Mode::Volume) {
      set_title(STATIONS[index].name);
      if (was_external) {
        lv_obj_remove_flag(s_logo_container, LV_OBJ_FLAG_HIDDEN);
        lv_anim_delete(s_logo_container, anim_opa_cb);
        lv_obj_set_style_opa(s_logo_container, LV_OPA_TRANSP, LV_PART_MAIN);
        anim_fade(s_logo_container, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
                  ANIM_FADE_MS);
      }
      set_logo(index);
      update_bg(true);
      update_subtitle();
    }
    update_media_controls();
    display_unlock();
  }
}

static const char *placeholder_glyph(const MediaInfo &media) {
  switch (art_placeholder(media)) {
  case ArtPlaceholder::Tv:
    return ICON_TV;
  case ArtPlaceholder::LineIn:
    return ICON_AUDIO_LINES;
  case ArtPlaceholder::AirPlay:
    return ICON_AIRPLAY;
  case ArtPlaceholder::Radio:
    return ICON_RADIO;
  case ArtPlaceholder::Music:
    break;
  }
  return ICON_MUSIC;
}

static void show_art_placeholder(const MediaInfo &media) {
  lv_label_set_text(s_art_glyph, placeholder_glyph(media));
  lv_obj_remove_flag(s_art_placeholder, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_img_logo, LV_OBJ_FLAG_HIDDEN);
}

static void hide_art_placeholder(lv_anim_t *) {
  lv_obj_add_flag(s_art_placeholder, LV_OBJ_FLAG_HIDDEN);
}

static void art_free_pixels() {
  if (s_art_pixels) {
    heap_caps_free(s_art_pixels);
    s_art_pixels = nullptr;
  }
  s_art_dsc = {};

  // Reset image widget back to station logo state after album art
  if (s_art_placeholder)
    lv_obj_add_flag(s_art_placeholder, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(s_img_logo, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_opa(s_img_logo, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_size(s_img_logo, 120, 120);
  lv_obj_set_pos(s_img_logo, 0, 0);
  lv_image_set_inner_align(s_img_logo, LV_IMAGE_ALIGN_CENTER);
}

void ui_set_media_info(const MediaInfo *info) {
  // Download + decode album art OUTSIDE the display lock (HTTP + decode block)
  // Guard against re-entry: if a fetch is already in progress, skip this one
  bool art_ready = false;
  if (info && info->has_media && info->art_url[0] &&
      strcmp(info->art_url, s_art_last_url) != 0 && !s_art_fetching) {
    s_art_fetching = true;
    ESP_LOGI(TAG, "Art URL changed: %.120s", info->art_url);
    strncpy(s_art_last_url, info->art_url, sizeof(s_art_last_url) - 1);
    s_art_last_url[sizeof(s_art_last_url) - 1] = '\0';

    if (display_lock(50)) {
      show_art_placeholder(*info);
      lv_obj_remove_flag(s_logo_container, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_opa(s_logo_container, LV_OPA_COVER, LV_PART_MAIN);
      display_unlock();
    }

    if (!s_art_jpeg)
      s_art_jpeg = static_cast<uint8_t *>(
          heap_caps_malloc(ART_BUF_SIZE, MALLOC_CAP_SPIRAM));
    if (s_art_jpeg) {
      int len = sonos_fetch_art(info->art_url, s_art_jpeg, ART_BUF_SIZE);
      if (len > 0) {
        uint8_t *px = nullptr;
        int aw = 0, ah = 0;
        bool decoded = art_decode_jpeg(s_art_jpeg, len, &px, &aw, &ah, 300);
        // Pre-scale to the exact widget size: a stretched lv_image goes
        // through LVGL's per-pixel CPU transform on every frame, while this
        // one-time box-average costs a few ms per track. (TJPGD only scales
        // by powers of two, hence decode large then resample.)
        if (decoded && px && (aw != ART_SIZE || ah != ART_SIZE)) {
          uint8_t *scaled = art_scale_rgb565(px, aw, ah, ART_SIZE, ART_SIZE);
          if (scaled) {
            heap_caps_free(px);
            px = scaled;
            aw = ART_SIZE;
            ah = ART_SIZE;
          }
        }
        if (decoded && px) {
          uint32_t tint = artwork_tint(px, aw * ah);
          if (display_lock(200)) {
            art_free_pixels();
            s_art_pixels = px;
            s_art_tint = tint;
            s_art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            s_art_dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
            s_art_dsc.header.w = static_cast<uint32_t>(aw);
            s_art_dsc.header.h = static_cast<uint32_t>(ah);
            s_art_dsc.header.stride = static_cast<uint32_t>(aw * 2);
            s_art_dsc.data_size = static_cast<uint32_t>(aw * ah * 2);
            s_art_dsc.data = s_art_pixels;
            art_ready = true;

            show_on_tile(&s_art_dsc, lv_color_hex(s_art_tint));

            // The cover fades in over the placeholder, which then goes.
            lv_obj_remove_flag(s_img_logo, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_opa(s_img_logo, LV_OPA_TRANSP, LV_PART_MAIN);
            anim_fade(s_img_logo, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER, 300,
                      hide_art_placeholder);
            display_unlock();
          } else {
            heap_caps_free(px);
          }
        }
      }
    }
    s_art_fetching = false;

    // Skip the normal display_lock section below — art was handled inline
    if (art_ready) {
      if (!display_lock(50))
        return;
      s_media = *info;
      s_external_playing = true;
      s_track_changed_ms = lv_tick_get();
      update_bg(true);
      if (s_mode == Mode::Volume && !s_idle_active) {
        set_title(s_media.title[0] ? s_media.title : "Unknown");
        update_subtitle();
        update_media_controls();
      }
      display_unlock();
      return;
    }
  }

  if (!display_lock(50))
    return;

  bool was_external = s_external_playing;

  if (info && info->has_media && s_play_state != PlayState::Stopped) {
    s_media = *info;
    s_external_playing = true;
    s_track_changed_ms = lv_tick_get();

    // A TV or line-in after a song must not keep showing that song's cover.
    if (!info->art_url[0] && s_art_pixels) {
      art_free_pixels();
      set_logo(s_station_index);
      s_art_last_url[0] = '\0';
    }
    update_bg(true);

    if (s_mode == Mode::Volume && !s_idle_active) {
      set_title(s_media.title[0] ? s_media.title : "Unknown");

      // Show album art if we decoded it, otherwise hide the logo
      if (s_art_pixels && (art_ready || !was_external)) {
        show_on_tile(&s_art_dsc, lv_color_hex(s_art_tint));
        lv_obj_remove_flag(s_logo_container, LV_OBJ_FLAG_HIDDEN);
        lv_anim_delete(s_logo_container, anim_opa_cb);
        if (!was_external) {
          lv_obj_set_style_opa(s_logo_container, LV_OPA_TRANSP, LV_PART_MAIN);
          anim_fade(s_logo_container, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
                    ANIM_FADE_MS);
        } else {
          lv_obj_set_style_opa(s_logo_container, LV_OPA_COVER, LV_PART_MAIN);
        }
      } else if (!s_art_pixels) {
        // No cover (TV, line-in, or the download failed): the tile stays,
        // with the source's glyph, so the layout doesn't jump.
        show_art_placeholder(s_media);
        if (lv_obj_has_flag(s_logo_container, LV_OBJ_FLAG_HIDDEN)) {
          lv_obj_remove_flag(s_logo_container, LV_OBJ_FLAG_HIDDEN);
          lv_obj_set_style_opa(s_logo_container, LV_OPA_TRANSP, LV_PART_MAIN);
          anim_fade(s_logo_container, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
                    ANIM_FADE_MS);
        }
      }

      update_subtitle();
      update_media_controls();
    }
  } else if (was_external) {
    s_external_playing = false;
    s_media = {};
    s_art_last_url[0] = '\0';
    update_media_controls();
    update_bg(true);
    if (s_mode == Mode::Volume && !s_idle_active) {
      art_free_pixels();
      set_title(STATIONS[s_station_index].name);
      set_logo(s_station_index);
      lv_obj_remove_flag(s_logo_container, LV_OBJ_FLAG_HIDDEN);
      lv_anim_delete(s_logo_container, anim_opa_cb);
      anim_fade(s_logo_container, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
                ANIM_FADE_MS);
      update_subtitle();
    }
  }

  display_unlock();
}

void ui_set_wifi_status(bool connected) {
  if (display_lock(50)) {
    if (connected) {
      lv_obj_add_flag(s_wifi_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_remove_flag(s_wifi_dot, LV_OBJ_FLAG_HIDDEN);
    }
    display_unlock();
  }
}

void ui_set_speaker_name(const char *name) {
  if (display_lock(50)) {
    snprintf(s_current_speaker, sizeof(s_current_speaker), "%s", name);
    s_speaker_list_stale = true;
    update_speaker_line();
    display_unlock();
  }
}

void ui_set_group(const SonosGroup *group) {
  if (!group || memcmp(group, &s_group, sizeof(s_group)) == 0)
    return;
  if (!display_lock(50))
    return;
  if (accept_reported_group(*group)) {
    s_group = *group;
    update_speaker_line();
    if (sheet_is_visible(s_speaker_sheet))
      restyle_group_rings();
  }
  display_unlock();
}

void ui_set_weather(const WeatherData *data) {
  faces_prepare_weather(data);
  if (!display_lock(50))
    return;
  faces_set_weather(data);
  display_unlock();
}

static void on_page_changed(int index, const char *id) {
  (void)id;
  if (index == 0) {
    volume_ring_set_level(s_volume);

    show_idle_ui(home_should_idle());
    update_subtitle();
  }
}

// Core encoder logic — called from LVGL task context (no lock needed)
static void turn_volume(int32_t steps) {
  int raw = s_volume + static_cast<int>(steps) * VOLUME_STEP;
  s_volume = std::clamp(raw, VOLUME_MIN, VOLUME_MAX);
  if (raw < VOLUME_MIN || raw > VOLUME_MAX)
    haptic_buzz();
  show_volume(s_volume);
  s_local_vol_ms = lv_tick_get();
  int32_t vol = s_volume;
  esp_event_post(APP_EVENT, APP_EVENT_VOLUME_CHANGED, &vol, sizeof(vol), 0);
  sonos_set_volume(s_volume);
}

static void handle_encoder(int32_t steps) {
  backlight_poke();
  lv_display_trigger_activity(nullptr);

  switch (surface()) {
  case Surface::Timer:
    if (!timer_sheet_turn(static_cast<int>(steps))) {
      turn_volume(steps);
      timer_sheet_show_volume(s_volume);
    }
    return;
  case Surface::Picker:
    if (s_discovered.count > 0) {
      s_speaker_highlight =
          std::clamp(s_speaker_highlight + static_cast<int>(steps), 0,
                     s_discovered.count - 1);
      highlight_picker_item(s_speaker_highlight);
    }
    return;
  case Surface::Page:
    pages_navigate(steps > 0 ? 1 : -1);
    pages_poke();
    return;
  case Surface::Faces:
    faces_nudge(static_cast<int>(steps));
    break;
  case Surface::Playing:
  case Surface::Browse:
    break;
  }

  pages_poke();

  switch (s_mode) {
  case Mode::Volume:
    turn_volume(steps);
    break;
  case Mode::Browse:
    carousel_turn(static_cast<int>(steps));
    lv_timer_reset(s_browse_timer);
    break;
  }
}

// LVGL timer — drains accumulated encoder steps in one shot
static void on_encoder_poll(lv_timer_t *) {
  int32_t steps = encoder_take_steps();
  if (steps != 0)
    handle_encoder(steps);
}

void ui_on_encoder_rotate(int32_t steps) {
  if (!display_lock(50))
    return;
  handle_encoder(steps);
  display_unlock();
}

void ui_on_touch_tap() {
  if (!display_lock(50))
    return;
  backlight_poke();
  pages_poke();
  if (!pages_is_home())
    pages_go_home();
  else
    do_tap();
  display_unlock();
}

void ui_on_touch_long_press() {
  if (!display_lock(50))
    return;
  do_long_press();
  display_unlock();
}

// A rescan with a list on hand is quiet: the sheet keeps showing it. Only the
// first scan shows the spinner, and opens the sheet if no room is set up yet.
void ui_show_scanning() {
  if (!display_lock(50))
    return;
  if (s_discovered.count == 0) {
    if (sheet_is_visible(s_speaker_sheet))
      build_picker_scanning();
    else if (!s_current_speaker[0])
      open_picker();
  }
  display_unlock();
}

void ui_show_speaker_picker(const DiscoveryResult *speakers) {
  if (display_lock(200)) {
    if (!speakers) {
      close_picker();
      display_unlock();
      return;
    }

    // A scan that found nothing is more likely a network hiccup than every
    // room gone at once: keep showing the rooms we know.
    if (speakers->count == 0 && s_discovered.count > 0) {
      display_unlock();
      return;
    }
    static DiscoveryResult sorted;
    memcpy(&sorted, speakers, sizeof(sorted));
    std::sort(sorted.speakers, sorted.speakers + sorted.count,
              [](const SonosSpeaker &a, const SonosSpeaker &b) {
                return strcmp(a.name, b.name) < 0;
              });
    bool changed = !same_rooms(sorted, s_discovered);
    memcpy(&s_discovered, &sorted, sizeof(s_discovered));
    s_speaker_list_stale = s_speaker_list_stale || changed;

    if (sheet_is_visible(s_speaker_sheet)) {
      if (s_speaker_list_stale || !s_picker_list) {
        rebuild_speaker_list();
        s_speaker_highlight = 0;
      } else {
        adopt_scanned_group();
        restyle_group_rings();
      }
    } else if (!s_current_speaker[0]) {
      open_picker();
    }
    display_unlock();
  }
}
