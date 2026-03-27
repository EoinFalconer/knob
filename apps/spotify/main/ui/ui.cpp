#include "ui/ui.h"
#include "app_config.h"
#include "display.h"
#include "encoder.h"
#include "haptic.h"
#include "art_decoder.h"
#include "fonts.h"
#include "spotify/spotify_api.h"
#include "ui/images/images.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lvgl.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

static constexpr const char *TAG = "ui";

ESP_EVENT_DECLARE_BASE(APP_EVENT);

// ─── LVGL objects
static lv_obj_t  *s_screen       = nullptr;
static lv_obj_t  *s_bg           = nullptr;
static lv_obj_t  *s_art_img      = nullptr;
static lv_obj_t  *s_lbl_track    = nullptr;
static lv_obj_t  *s_lbl_artist   = nullptr;
static lv_obj_t  *s_lbl_status   = nullptr;
static lv_obj_t  *s_vol_arc      = nullptr;
static lv_obj_t  *s_progress_arc = nullptr;
static lv_obj_t  *s_play_icon    = nullptr;
static lv_obj_t  *s_logo         = nullptr;
static lv_obj_t  *s_art_loading  = nullptr; // pulsating placeholder
static lv_obj_t  *s_qr_code     = nullptr;  // QR code (setup screens)
static lv_obj_t  *s_seek_lbl    = nullptr;  // seek offset label ("+6s")

// ─── State
static volatile int s_volume     = 50;
static volatile bool s_has_track = false;
static bool         s_is_playing = false;
static bool         s_vol_visible = false;
static int64_t      s_vol_hide_at = 0;
static int64_t      s_last_activity = 0;
static int64_t      s_vol_local_until = 0;
static bool         s_dimmed     = false;
static char         s_current_art_url[256] = {};
static char         s_current_track[128] = {};
static int          s_progress_ms = 0;
static int          s_duration_ms = 0;
static int64_t      s_progress_update_at = 0; // when we last got a server update

// ─── Touch+seek state
static bool         s_touch_down = false;     // finger on screen
static bool         s_seeking    = false;     // encoder moved while touched
static int          s_seek_ms    = 0;         // current seek position
static int64_t      s_touch_down_at = 0;      // when finger went down
static int          s_encoder_while_touched = 0; // accumulated steps while touched

static constexpr int VOL_LOCAL_GRACE_MS = 5000;
static constexpr int SEEK_STEP_MS = 3000;    // ms per encoder step when seeking
static constexpr int SKIP_FLICK_THRESHOLD = 3; // steps in one poll = flick

// ─── Art image buffer (PSRAM)
static uint8_t     *s_art_jpeg_buf = nullptr;
static lv_image_dsc_t s_art_dsc   = {};
static uint8_t     *s_art_pixels  = nullptr;
static constexpr int ART_JPEG_BUF = 128 * 1024;
static constexpr int ART_MAX_DIM  = 300; // use 300px image at 1:1 (no scaling artifacts)

// ─── Colors
static constexpr lv_color_t COL_BG      = {.blue = 0x10, .green = 0x10, .red = 0x10};
static constexpr lv_color_t COL_GREEN   = {.blue = 0x54, .green = 0xB9, .red = 0x1D};
static constexpr lv_color_t COL_WHITE   = {.blue = 0xFF, .green = 0xFF, .red = 0xFF};
static constexpr lv_color_t COL_GREY    = {.blue = 0x88, .green = 0x88, .red = 0x88};
static constexpr lv_color_t COL_DIM     = {.blue = 0x30, .green = 0x30, .red = 0x30};

static void poke() {
    s_last_activity = esp_timer_get_time();
    if (s_dimmed) {
        display_set_backlight(BACKLIGHT_NORMAL);
        s_dimmed = false;
    }
}

// ─── Fade animation helper

static void fade_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), v, 0);
}

static void fade_to(lv_obj_t *obj, int32_t target_opa, int duration_ms) {
    int32_t current = lv_obj_get_style_opa(obj, LV_PART_MAIN);
    if (current == target_opa) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, current, target_opa);
    lv_anim_set_duration(&a, duration_ms);
    lv_anim_set_exec_cb(&a, fade_opa_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// ─── Build the UI layout

static void build_home(lv_obj_t *parent) {
    // Dark background
    s_bg = lv_obj_create(parent);
    lv_obj_remove_style_all(s_bg);
    lv_obj_set_size(s_bg, 360, 360);
    lv_obj_set_style_bg_color(s_bg, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bg, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_bg, LV_OBJ_FLAG_SCROLLABLE);

    // Progress arc (song progress — white, shown when volume is hidden)
    s_progress_arc = lv_arc_create(s_bg);
    lv_obj_set_size(s_progress_arc, 350, 350);
    lv_obj_center(s_progress_arc);
    lv_arc_set_rotation(s_progress_arc, 270);
    lv_arc_set_bg_angles(s_progress_arc, 0, 360);
    lv_arc_set_range(s_progress_arc, 0, 1000);
    lv_arc_set_value(s_progress_arc, 0);
    lv_obj_remove_style(s_progress_arc, nullptr, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_progress_arc, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_progress_arc, COL_WHITE, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_progress_arc, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_progress_arc, COL_DIM, LV_PART_MAIN);
    lv_obj_set_style_opa(s_progress_arc, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_progress_arc, LV_OBJ_FLAG_CLICKABLE);

    // Volume arc (green, shown on encoder turn)
    s_vol_arc = lv_arc_create(s_bg);
    lv_obj_set_size(s_vol_arc, 350, 350);
    lv_obj_center(s_vol_arc);
    lv_arc_set_rotation(s_vol_arc, 135);
    lv_arc_set_bg_angles(s_vol_arc, 0, 270);
    lv_arc_set_range(s_vol_arc, 0, 100);
    lv_arc_set_value(s_vol_arc, 50);
    lv_obj_remove_style(s_vol_arc, nullptr, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_vol_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_vol_arc, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_vol_arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_vol_arc, COL_DIM, LV_PART_MAIN);
    lv_obj_set_style_opa(s_vol_arc, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_vol_arc, LV_OBJ_FLAG_CLICKABLE);

    // Album art — hidden until we have art, auto-sized from image source
    s_art_img = lv_image_create(s_bg);
    lv_obj_align(s_art_img, LV_ALIGN_CENTER, 0, -15);
    lv_obj_set_style_radius(s_art_img, 16, 0);
    lv_obj_set_style_clip_corner(s_art_img, true, 0);
    lv_obj_add_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);

    // Play/pause icon overlay (centered on art area) with dark scrim
    s_play_icon = lv_obj_create(s_bg);
    lv_obj_remove_style_all(s_play_icon);
    lv_obj_set_size(s_play_icon, 160, 160);
    lv_obj_align(s_play_icon, LV_ALIGN_CENTER, 0, -15);
    lv_obj_set_style_bg_color(s_play_icon, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_play_icon, LV_OPA_50, 0);
    lv_obj_set_style_radius(s_play_icon, 16, 0);
    lv_obj_clear_flag(s_play_icon, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_flag(s_play_icon, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *pause_text = lv_label_create(s_play_icon);
    lv_label_set_text(pause_text, "| |");
    lv_obj_set_style_text_font(pause_text, &geist_medium_28, 0);
    lv_obj_set_style_text_color(pause_text, COL_WHITE, 0);
    lv_obj_set_style_text_letter_space(pause_text, 4, 0);
    lv_obj_center(pause_text);

    // Seek offset label (hidden, shown during seek)
    s_seek_lbl = lv_label_create(s_bg);
    lv_label_set_text(s_seek_lbl, "");
    lv_obj_set_style_text_font(s_seek_lbl, &geist_medium_28, 0);
    lv_obj_set_style_text_color(s_seek_lbl, COL_GREEN, 0);
    lv_obj_set_style_text_align(s_seek_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_seek_lbl, LV_ALIGN_CENTER, 0, -15);
    lv_obj_add_flag(s_seek_lbl, LV_OBJ_FLAG_HIDDEN);

    // Track name — Geist 22 for Norwegian char support
    s_lbl_track = lv_label_create(s_bg);
    lv_label_set_text(s_lbl_track, "");
    lv_obj_set_width(s_lbl_track, 280);
    lv_label_set_long_mode(s_lbl_track, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(s_lbl_track, COL_WHITE, 0);
    lv_obj_set_style_text_font(s_lbl_track, &geist_regular_22, 0);
    lv_obj_set_style_text_align(s_lbl_track, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_track, LV_ALIGN_CENTER, 0, 85);

    // Artist name — Geist 16
    s_lbl_artist = lv_label_create(s_bg);
    lv_label_set_text(s_lbl_artist, "");
    lv_obj_set_width(s_lbl_artist, 260);
    lv_label_set_long_mode(s_lbl_artist, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(s_lbl_artist, COL_GREY, 0);
    lv_obj_set_style_text_font(s_lbl_artist, &geist_regular_16, 0);
    lv_obj_set_style_text_align(s_lbl_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_artist, LV_ALIGN_CENTER, 0, 112);

    // Art loading placeholder (pulsating circle)
    s_art_loading = lv_obj_create(s_bg);
    lv_obj_remove_style_all(s_art_loading);
    lv_obj_set_size(s_art_loading, 160, 160);
    lv_obj_align(s_art_loading, LV_ALIGN_CENTER, 0, -15);
    lv_obj_set_style_bg_color(s_art_loading, COL_DIM, 0);
    lv_obj_set_style_bg_opa(s_art_loading, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_art_loading, 16, 0);
    lv_obj_add_flag(s_art_loading, LV_OBJ_FLAG_HIDDEN);

    // Spotify logo (hidden initially — splash screen shows first)
    s_logo = lv_image_create(s_bg);
    lv_image_set_src(s_logo, &spotify_logo_64);
    lv_obj_align(s_logo, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_flag(s_logo, LV_OBJ_FLAG_HIDDEN);

    // Status label (hidden initially)
    s_lbl_status = lv_label_create(s_bg);
    lv_label_set_text(s_lbl_status, "");
    lv_obj_set_width(s_lbl_status, 280);
    lv_obj_set_style_text_color(s_lbl_status, COL_GREY, 0);
    lv_obj_set_style_text_font(s_lbl_status, &geist_regular_16, 0);
    lv_obj_set_style_text_align(s_lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lbl_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, 20);
    lv_obj_add_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);

    // Touch: PRESSED — enter seek-ready mode
    lv_obj_add_event_cb(s_bg, [](lv_event_t *) {
        poke();
        if (!s_has_track) return;
        s_touch_down = true;
        s_touch_down_at = esp_timer_get_time();
        s_encoder_while_touched = 0;
        s_seeking = false;

        // Visual: dim the art to show "touch held" state
        lv_obj_set_style_opa(s_art_img, LV_OPA_60, 0);
        // Show progress arc prominently
        if (s_duration_ms > 0) {
            lv_obj_set_style_arc_width(s_progress_arc, 6, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(s_progress_arc, COL_GREEN, LV_PART_INDICATOR);
            fade_to(s_progress_arc, LV_OPA_COVER, 100);
        }
    }, LV_EVENT_PRESSED, nullptr);

    // Touch: RELEASED — exit seek mode, send final seek if needed
    lv_obj_add_event_cb(s_bg, [](lv_event_t *) {
        if (!s_has_track) return;
        bool was_seeking = s_seeking;
        s_touch_down = false;
        s_seeking = false;

        // Restore visuals
        lv_obj_set_style_opa(s_art_img, LV_OPA_COVER, 0);
        lv_obj_set_style_arc_width(s_progress_arc, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_progress_arc, COL_WHITE, LV_PART_INDICATOR);
        lv_obj_add_flag(s_seek_lbl, LV_OBJ_FLAG_HIDDEN);

        // If we were seeking, commit the final position
        if (was_seeking) {
            int32_t pos = s_seek_ms;
            esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_SEEK, &pos, sizeof(pos), 0);
            s_progress_ms = s_seek_ms;
            s_progress_update_at = esp_timer_get_time();
        }
    }, LV_EVENT_RELEASED, nullptr);

    lv_obj_add_event_cb(s_bg, [](lv_event_t *) {
        // Also handle press lost (finger dragged off)
        bool was_seeking = s_seeking;
        s_touch_down = false;
        s_seeking = false;
        lv_obj_set_style_opa(s_art_img, LV_OPA_COVER, 0);
        lv_obj_set_style_arc_width(s_progress_arc, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(s_progress_arc, COL_WHITE, LV_PART_INDICATOR);
        lv_obj_add_flag(s_seek_lbl, LV_OBJ_FLAG_HIDDEN);
        if (was_seeking) {
            int32_t pos = s_seek_ms;
            esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_SEEK, &pos, sizeof(pos), 0);
            s_progress_ms = s_seek_ms;
            s_progress_update_at = esp_timer_get_time();
        }
    }, LV_EVENT_PRESS_LOST, nullptr);

    // Touch: SHORT_CLICKED — play/pause only if we didn't seek
    lv_obj_add_event_cb(s_bg, [](lv_event_t *) {
        poke();
        if (!s_has_track) return;
        if (s_encoder_while_touched > 0) return; // was seeking/skipping, not a tap

        // Optimistic toggle
        s_is_playing = !s_is_playing;
        if (s_is_playing) {
            lv_obj_add_flag(s_play_icon, LV_OBJ_FLAG_HIDDEN);
            esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_PLAY, nullptr, 0, 0);
        } else {
            lv_obj_clear_flag(s_play_icon, LV_OBJ_FLAG_HIDDEN);
            esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_PAUSE, nullptr, 0, 0);
        }
    }, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_flag(s_bg, LV_OBJ_FLAG_CLICKABLE);
}

// ─── Encoder + timer tick

static void tick_cb(lv_timer_t *) {
    int64_t now = esp_timer_get_time();

    // Backlight dimming
    if (!s_dimmed && (now - s_last_activity) > (int64_t)BACKLIGHT_INACTIVITY_MS * 1000) {
        display_set_backlight(BACKLIGHT_DIM);
        s_dimmed = true;
    }

    // Skip volume/progress animation while touch is down (seek mode owns the arcs)
    if (!s_touch_down) {
        // Fade volume out, fade progress back in
        if (s_vol_visible && now >= s_vol_hide_at) {
            fade_to(s_vol_arc, LV_OPA_TRANSP, 300);
            s_vol_visible = false;
            if (s_has_track) {
                fade_to(s_progress_arc, LV_OPA_COVER, 400);
            }
        }

        // Smooth progress interpolation (ticks every 20ms while playing)
        if (s_has_track && s_is_playing && s_duration_ms > 0 && !s_vol_visible) {
            int64_t elapsed_us = now - s_progress_update_at;
            int interpolated_ms = s_progress_ms + (int)(elapsed_us / 1000);
            if (interpolated_ms > s_duration_ms) interpolated_ms = s_duration_ms;
            int progress = (int)((int64_t)interpolated_ms * 1000 / s_duration_ms);
            lv_arc_set_value(s_progress_arc, progress);
        }
    }

    // Poll encoder
    int32_t steps = encoder_take_steps();
    if (steps == 0) return;

    poke();
    haptic_buzz();

    if (!s_has_track) return;

    // ─── Touch down + encoder = seek / skip
    if (s_touch_down) {
        s_encoder_while_touched += abs(steps);
        static int64_t last_skip = 0;
        static constexpr int64_t SKIP_COOLDOWN_US = 2000000; // 2s

        // Fast flick while touching = skip track
        if (abs(steps) >= SKIP_FLICK_THRESHOLD &&
            (now - last_skip) > SKIP_COOLDOWN_US) {
            last_skip = now;
            if (steps > 0) {
                esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_NEXT, nullptr, 0, 0);
            } else {
                esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_PREV, nullptr, 0, 0);
            }
            s_seeking = false; // skip overrides seek
            return;
        }

        // Slow turn while touching = seek
        int origin_ms;
        if (!s_seeking) {
            // Initialize seek position from current interpolated progress
            int64_t elapsed_us = now - s_progress_update_at;
            origin_ms = s_progress_ms;
            if (s_is_playing) {
                origin_ms += (int)(elapsed_us / 1000);
            }
            s_seek_ms = origin_ms;
            s_seeking = true;

            // Hide volume arc, ensure progress arc visible
            lv_obj_set_style_opa(s_vol_arc, LV_OPA_TRANSP, 0);
            s_vol_visible = false;
        } else {
            // Compute origin for offset display
            int64_t elapsed_us = now - s_progress_update_at;
            origin_ms = s_progress_ms;
            if (s_is_playing) {
                origin_ms += (int)(elapsed_us / 1000);
            }
        }

        s_seek_ms += steps * SEEK_STEP_MS;
        if (s_seek_ms < 0) s_seek_ms = 0;
        if (s_seek_ms > s_duration_ms) s_seek_ms = s_duration_ms;

        // Update progress arc to show seek position
        if (s_duration_ms > 0) {
            int progress = (int)((int64_t)s_seek_ms * 1000 / s_duration_ms);
            lv_arc_set_value(s_progress_arc, progress);
        }

        // Show seek offset label (e.g. "+6s" or "-3s")
        int offset_s = (s_seek_ms - origin_ms) / 1000;
        char buf[16];
        if (offset_s >= 0) {
            snprintf(buf, sizeof(buf), "+%ds", offset_s);
        } else {
            snprintf(buf, sizeof(buf), "%ds", offset_s);
        }
        lv_label_set_text(s_seek_lbl, buf);
        lv_obj_clear_flag(s_seek_lbl, LV_OBJ_FLAG_HIDDEN);

        return;
    }

    // ─── No touch = volume adjust (unchanged)
    int cur_vol = s_volume;
    int new_vol = cur_vol + steps * 2;
    if (new_vol < 0) new_vol = 0;
    if (new_vol > 100) new_vol = 100;
    s_volume = new_vol;

    s_vol_local_until = now + (int64_t)VOL_LOCAL_GRACE_MS * 1000;

    int32_t vol = new_vol;
    esp_event_post(APP_EVENT, APP_EVENT_SPOTIFY_VOLUME_SET, &vol, sizeof(vol), 0);
    ui_show_volume(new_vol);
}

// ─── Public API

static void cleanup_qr() {
    if (s_qr_code) {
        lv_obj_delete(s_qr_code);
        s_qr_code = nullptr;
    }
}

// Create a QR code with the Spotify logo overlaid in the center.
// QR error correction handles ~30% coverage, logo is ~20%.
static lv_obj_t *create_branded_qr(lv_obj_t *parent, const char *data,
                                    int qr_size, int y_offset) {
    cleanup_qr();
    s_qr_code = lv_qrcode_create(parent);
    lv_qrcode_set_size(s_qr_code, qr_size);
    lv_qrcode_set_dark_color(s_qr_code, COL_WHITE);
    lv_qrcode_set_light_color(s_qr_code, COL_BG);
    lv_qrcode_update(s_qr_code, data, strlen(data));
    lv_obj_align(s_qr_code, LV_ALIGN_CENTER, 0, y_offset);

    // Dark circle background behind logo so QR pixels don't show through
    lv_obj_t *logo_bg = lv_obj_create(s_qr_code);
    lv_obj_remove_style_all(logo_bg);
    lv_obj_set_size(logo_bg, 40, 40);
    lv_obj_set_style_bg_color(logo_bg, COL_BG, 0);
    lv_obj_set_style_bg_opa(logo_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(logo_bg, LV_RADIUS_CIRCLE, 0);
    lv_obj_center(logo_bg);

    // Overlay Spotify logo
    lv_obj_t *logo_overlay = lv_image_create(s_qr_code);
    lv_image_set_src(logo_overlay, &spotify_logo_32);
    lv_obj_center(logo_overlay);

    return s_qr_code;
}

static void show_logo_centered() {
    lv_image_set_src(s_logo, &spotify_logo_64);
    lv_image_set_scale(s_logo, 256); // 1:1 native 64px
    lv_obj_align(s_logo, LV_ALIGN_CENTER, 0, -40);
    lv_obj_clear_flag(s_logo, LV_OBJ_FLAG_HIDDEN);
}

static void show_logo_top() {
    lv_image_set_src(s_logo, &spotify_logo_32);
    lv_image_set_scale(s_logo, 256); // 1:1 native 32px — crisp
    lv_obj_align(s_logo, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_clear_flag(s_logo, LV_OBJ_FLAG_HIDDEN);
}

// ─── Splash screen objects (temporary, deleted when splash ends)
static lv_obj_t *s_splash = nullptr;
static lv_obj_t *s_splash_status = nullptr;

static void scale_cb(void *obj, int32_t v) {
    lv_image_set_scale(static_cast<lv_obj_t *>(obj), static_cast<uint32_t>(v));
}

static void arc_angle_cb(void *obj, int32_t v) {
    lv_arc_set_angles(static_cast<lv_obj_t *>(obj), v % 360, (v + 90) % 360);
}

void ui_show_splash() {
    if (!display_lock(1000)) return;

    // Full-screen splash container
    s_splash = lv_obj_create(s_bg);
    lv_obj_remove_style_all(s_splash);
    lv_obj_set_size(s_splash, 360, 360);
    lv_obj_center(s_splash);
    lv_obj_clear_flag(s_splash, LV_OBJ_FLAG_SCROLLABLE);

    // ─── Spinning ring around the logo
    lv_obj_t *ring = lv_arc_create(s_splash);
    lv_obj_set_size(ring, 120, 120);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, -30);
    lv_arc_set_rotation(ring, 0);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_angles(ring, 0, 90);
    lv_obj_remove_style(ring, nullptr, LV_PART_KNOB);
    lv_obj_set_style_arc_width(ring, 2, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    // Spin the ring
    lv_anim_t spin;
    lv_anim_init(&spin);
    lv_anim_set_var(&spin, ring);
    lv_anim_set_values(&spin, 0, 360);
    lv_anim_set_duration(&spin, 1500);
    lv_anim_set_exec_cb(&spin, arc_angle_cb);
    lv_anim_set_repeat_count(&spin, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&spin, lv_anim_path_linear);
    lv_anim_start(&spin);

    // Fade the ring in after a beat
    lv_obj_set_style_opa(ring, LV_OPA_TRANSP, 0);
    lv_anim_t ring_fade;
    lv_anim_init(&ring_fade);
    lv_anim_set_var(&ring_fade, ring);
    lv_anim_set_values(&ring_fade, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&ring_fade, 600);
    lv_anim_set_delay(&ring_fade, 400);
    lv_anim_set_exec_cb(&ring_fade, fade_opa_cb);
    lv_anim_set_path_cb(&ring_fade, lv_anim_path_ease_out);
    lv_anim_start(&ring_fade);

    // ─── Logo — scale up from small + fade in
    lv_obj_t *logo = lv_image_create(s_splash);
    lv_image_set_src(logo, &spotify_logo_64);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_opa(logo, LV_OPA_TRANSP, 0);
    lv_image_set_scale(logo, 128); // start at 50%

    // Scale 50% -> 100%
    lv_anim_t zoom;
    lv_anim_init(&zoom);
    lv_anim_set_var(&zoom, logo);
    lv_anim_set_values(&zoom, 128, 256);
    lv_anim_set_duration(&zoom, 600);
    lv_anim_set_exec_cb(&zoom, scale_cb);
    lv_anim_set_path_cb(&zoom, lv_anim_path_overshoot);
    lv_anim_start(&zoom);

    // Fade in
    lv_anim_t logo_fade;
    lv_anim_init(&logo_fade);
    lv_anim_set_var(&logo_fade, logo);
    lv_anim_set_values(&logo_fade, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&logo_fade, 500);
    lv_anim_set_exec_cb(&logo_fade, fade_opa_cb);
    lv_anim_set_path_cb(&logo_fade, lv_anim_path_ease_out);
    lv_anim_start(&logo_fade);

    // ─── Tagline — fade in with delay
    lv_obj_t *tag = lv_label_create(s_splash);
    lv_label_set_text(tag, "every knob\ndeserves music");
    lv_obj_set_width(tag, 260);
    lv_obj_set_style_text_color(tag, COL_GREY, 0);
    lv_obj_set_style_text_font(tag, &geist_regular_16, 0);
    lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(tag, LV_ALIGN_CENTER, 0, 55);
    lv_obj_set_style_opa(tag, LV_OPA_TRANSP, 0);

    lv_anim_t tag_fade;
    lv_anim_init(&tag_fade);
    lv_anim_set_var(&tag_fade, tag);
    lv_anim_set_values(&tag_fade, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&tag_fade, 600);
    lv_anim_set_delay(&tag_fade, 300);
    lv_anim_set_exec_cb(&tag_fade, fade_opa_cb);
    lv_anim_set_path_cb(&tag_fade, lv_anim_path_ease_in);
    lv_anim_start(&tag_fade);

    // ─── Status line below tagline (hidden until scanning starts)
    s_splash_status = lv_label_create(s_splash);
    lv_label_set_text(s_splash_status, "");
    lv_obj_set_width(s_splash_status, 260);
    lv_obj_set_style_text_color(s_splash_status, COL_DIM, 0);
    lv_obj_set_style_text_font(s_splash_status, &geist_regular_16, 0);
    lv_obj_set_style_text_align(s_splash_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_splash_status, LV_ALIGN_CENTER, 0, 95);
    lv_obj_add_flag(s_splash_status, LV_OBJ_FLAG_HIDDEN);

    display_unlock();
}

void ui_splash_set_status(const char *msg) {
    if (!display_lock(200)) return;
    if (s_splash_status) {
        lv_label_set_text(s_splash_status, msg);
        lv_obj_clear_flag(s_splash_status, LV_OBJ_FLAG_HIDDEN);
    }
    display_unlock();
}

void ui_dismiss_splash() {
    if (!display_lock(200)) return;
    if (s_splash) {
        lv_obj_delete(s_splash);
        s_splash = nullptr;
        s_splash_status = nullptr;
    }
    display_unlock();
}

void ui_set_status(const char *msg) {
    if (!display_lock(100)) return;
    cleanup_qr();
    lv_obj_add_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_play_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_progress_arc, LV_OPA_TRANSP, 0);
    lv_label_set_text(s_lbl_track, "");
    lv_label_set_text(s_lbl_artist, "");
    show_logo_centered();
    lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_lbl_status, msg);
    display_unlock();
}

void ui_init() {
    lv_display_t *disp = nullptr;
    lv_indev_t *touch = nullptr;
    display_init(&disp, &touch);

    if (display_lock(1000)) {
        s_screen = lv_screen_active();
        lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

        // Font fallback for accented Latin chars (é, ü, ñ, etc.)
        const_cast<lv_font_t *>(&geist_regular_22)->fallback = &lv_font_montserrat_14;
        const_cast<lv_font_t *>(&geist_regular_16)->fallback = &lv_font_montserrat_14;
        const_cast<lv_font_t *>(&geist_medium_28)->fallback = &lv_font_montserrat_14;

        build_home(s_screen);

        s_art_jpeg_buf = static_cast<uint8_t *>(
            heap_caps_malloc(ART_JPEG_BUF, MALLOC_CAP_SPIRAM));
        if (!s_art_jpeg_buf) {
            ESP_LOGE(TAG, "Failed to alloc art JPEG buffer (%d bytes)", ART_JPEG_BUF);
        }

        lv_timer_create(tick_cb, ENCODER_POLL_MS, nullptr);

        s_last_activity = esp_timer_get_time();
        display_unlock();
    }
}

void ui_update_state(const SpotifyState *state) {
    if (!display_lock(100)) return;
    cleanup_qr();

    if (state->track[0] == '\0') {
        s_has_track = false;
        s_is_playing = false;
        lv_obj_add_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_play_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_progress_arc, LV_OPA_TRANSP, 0);
        lv_label_set_text(s_lbl_track, "");
        lv_label_set_text(s_lbl_artist, "");
        show_logo_centered();
        lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, 45);
        lv_label_set_text(s_lbl_status,
            "No active playback\n\n"
            "Play something on your\n"
            "phone or laptop");
        s_current_art_url[0] = '\0';
        s_current_track[0] = '\0';
        display_unlock();
        return;
    }

    s_has_track = true;
    s_is_playing = state->is_playing;

    // Only accept remote volume if not recently changed locally
    if (esp_timer_get_time() >= s_vol_local_until) {
        s_volume = state->volume;
    }

    // Update progress with smooth interpolation
    s_progress_ms = state->progress_ms;
    s_duration_ms = state->duration_ms;
    s_progress_update_at = esp_timer_get_time();

    // Hide status, move logo to bottom, show track info
    lv_obj_add_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
    show_logo_top();
    lv_label_set_text(s_lbl_track, state->track);
    lv_label_set_text(s_lbl_artist, state->artist);

    // Pause overlay
    if (!state->is_playing) {
        lv_obj_clear_flag(s_play_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_play_icon, LV_OBJ_FLAG_HIDDEN);
    }

    // Progress arc — interpolate from last server update
    if (s_duration_ms > 0 && !s_vol_visible) {
        int64_t elapsed_us = esp_timer_get_time() - s_progress_update_at;
        int interpolated_ms = s_progress_ms;
        if (s_is_playing) {
            interpolated_ms += (int)(elapsed_us / 1000);
        }
        if (interpolated_ms > s_duration_ms) interpolated_ms = s_duration_ms;
        int progress = (int)((int64_t)interpolated_ms * 1000 / s_duration_ms);
        lv_arc_set_value(s_progress_arc, progress);
        lv_obj_set_style_opa(s_progress_arc, LV_OPA_COVER, 0);
    }

    // Volume arc (silently)
    lv_arc_set_value(s_vol_arc, s_volume);

    // Fetch album art if URL or track changed
    bool track_changed = strcmp(s_current_track, state->track) != 0;
    bool art_changed = strcmp(s_current_art_url, state->art_url) != 0;
    if (track_changed) {
        strncpy(s_current_track, state->track, sizeof(s_current_track) - 1);
    }
    if ((art_changed || track_changed) && state->art_url[0] != '\0' && s_art_jpeg_buf) {
        strncpy(s_current_art_url, state->art_url, sizeof(s_current_art_url) - 1);

        // Show pulsating loading placeholder
        lv_obj_add_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_art_loading, LV_OBJ_FLAG_HIDDEN);
        // Pulse animation
        lv_anim_t pulse;
        lv_anim_init(&pulse);
        lv_anim_set_var(&pulse, s_art_loading);
        lv_anim_set_values(&pulse, LV_OPA_40, LV_OPA_80);
        lv_anim_set_duration(&pulse, 600);
        lv_anim_set_exec_cb(&pulse, fade_opa_cb);
        lv_anim_set_playback_duration(&pulse, 600);
        lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&pulse);

        display_unlock();

        int jpeg_len = spotify_api_fetch_art(state->art_url,
                                              s_art_jpeg_buf, ART_JPEG_BUF);
        if (jpeg_len > 0) {
            uint8_t *pixels = nullptr;
            int w = 0, h = 0;
            if (art_decode_jpeg(s_art_jpeg_buf, jpeg_len,
                                &pixels, &w, &h, ART_MAX_DIM) && pixels) {
                if (display_lock(200)) {
                    if (s_art_pixels) heap_caps_free(s_art_pixels);
                    s_art_pixels = pixels;

                    s_art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
                    s_art_dsc.header.w = static_cast<uint32_t>(w);
                    s_art_dsc.header.h = static_cast<uint32_t>(h);
                    s_art_dsc.header.stride = static_cast<uint32_t>(w * 2);
                    s_art_dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
                    s_art_dsc.data_size = static_cast<uint32_t>(w * h * 2);
                    s_art_dsc.data = s_art_pixels;

                    lv_image_set_src(s_art_img, &s_art_dsc);
                    // Scale 300px to ~160px (256 = 100%)
                    lv_image_set_scale(s_art_img, 137);
                    lv_obj_align(s_art_img, LV_ALIGN_CENTER, 0, -15);

                    // Stop loading pulse, hide placeholder, fade in art
                    lv_anim_delete(s_art_loading, fade_opa_cb);
                    lv_obj_add_flag(s_art_loading, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_style_opa(s_art_img, LV_OPA_TRANSP, 0);
                    lv_obj_clear_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);
                    fade_to(s_art_img, LV_OPA_COVER, 300);
                    display_unlock();
                } else {
                    heap_caps_free(pixels);
                }
            }
        }
        return;
    }

    if (s_art_pixels) {
        lv_obj_clear_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);
    }

    display_unlock();
}

void ui_show_wifi_setup(const char *ap_name) {
    if (!display_lock(1000)) return;

    // Hide all playback UI
    lv_obj_add_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_art_loading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_play_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_progress_arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_vol_arc, LV_OPA_TRANSP, 0);
    lv_label_set_text(s_lbl_track, "");
    lv_label_set_text(s_lbl_artist, "");
    lv_obj_add_flag(s_logo, LV_OBJ_FLAG_HIDDEN);

    // QR code with Spotify logo overlay
    char wifi_qr[128];
    snprintf(wifi_qr, sizeof(wifi_qr), "WIFI:T:nopass;S:%s;;", ap_name);
    create_branded_qr(s_bg, wifi_qr, 150, -35);

    // Instructions
    lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, 85);
    char msg[128];
    snprintf(msg, sizeof(msg), "Scan to join \"%s\"\nthen set up WiFi", ap_name);
    lv_label_set_text(s_lbl_status, msg);

    display_unlock();
}

void ui_show_spotify_setup(const char *device_ip) {
    if (!display_lock(1000)) return;

    // Hide all playback UI
    lv_obj_add_flag(s_art_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_art_loading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_play_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_progress_arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(s_vol_arc, LV_OPA_TRANSP, 0);
    lv_label_set_text(s_lbl_track, "");
    lv_label_set_text(s_lbl_artist, "");
    lv_obj_add_flag(s_logo, LV_OBJ_FLAG_HIDDEN);

    // QR code with Spotify logo overlay
    char url[128];
    snprintf(url, sizeof(url), "http://%s:8888/spotify", device_ip);
    create_branded_qr(s_bg, url, 150, -35);

    // Instructions
    lv_obj_clear_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, 85);
    lv_label_set_text(s_lbl_status,
        "Scan to set up\nSpotify on your knob");

    display_unlock();
}

void ui_show_volume(int volume) {
    if (!display_lock(50)) return;

    lv_arc_set_value(s_vol_arc, volume);

    // Fade: progress out, volume in
    if (!s_vol_visible) {
        fade_to(s_progress_arc, LV_OPA_TRANSP, 150);
        fade_to(s_vol_arc, LV_OPA_COVER, 150);
    }

    s_vol_visible = true;
    s_vol_hide_at = esp_timer_get_time() + (int64_t)VOL_DISPLAY_MS * 1000;

    display_unlock();
}
