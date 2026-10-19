#pragma once

#include "lvgl.h"

// Apple's SF Pro, cut per size at its optical size, and SF Pro Rounded for
// big numerals (scripts/gen_fonts.py). Each app compiles the ones it uses.
extern const lv_font_t sf_regular_16;
extern const lv_font_t sf_regular_18;
extern const lv_font_t sf_regular_20;
extern const lv_font_t sf_regular_22;
extern const lv_font_t sf_medium_13;
extern const lv_font_t sf_medium_15;
extern const lv_font_t sf_semibold_16;
extern const lv_font_t sf_semibold_20;
extern const lv_font_t sf_semibold_28;
extern const lv_font_t sf_rounded_regular_132; // digits, minus and degree sign
extern const lv_font_t
    sf_rounded_regular_200; // clock numerals 1, 2, 3, 6, 9 only
// Tabular digits, space and a colon centered on the figures.
extern const lv_font_t sf_rounded_semibold_52;
extern const lv_font_t sf_rounded_semibold_84;
extern const lv_font_t sf_rounded_black_200; // B, K, N and O only

extern const lv_font_t lucide_22;
extern const lv_font_t lucide_48; // artwork placeholder glyphs only
extern const lv_font_t lucide_18; // ICON_SPEAKER and ICON_TIMER only
extern const lv_font_t lucide_34; // ICON_LIGHTBULB(_OFF) only
extern const lv_font_t lucide_weather_20;

// Lucide icon codepoints (use with lucide_22 font)
// Encode PUA codepoints as UTF-8: 0xE0xx → 0xEE 0x80+hi 0x80+lo
#define ICON_ARROW_DOWN "\xEE\x81\x82"    // U+E042
#define ICON_ARROW_LEFT "\xEE\x81\x88"    // U+E048
#define ICON_ARROW_RIGHT "\xEE\x81\x89"   // U+E049
#define ICON_ARROW_UP "\xEE\x81\x8A"      // U+E04A
#define ICON_CHECK "\xEE\x81\xAC"         // U+E06C
#define ICON_CHEVRON_DOWN "\xEE\x81\xAD"  // U+E06D
#define ICON_CHEVRON_LEFT "\xEE\x81\xAE"  // U+E06E
#define ICON_CHEVRON_RIGHT "\xEE\x81\xAF" // U+E06F
#define ICON_CHEVRON_UP "\xEE\x81\xB0"    // U+E070
#define ICON_PAUSE "\xEE\x84\xAE"         // U+E12E
#define ICON_PLAY "\xEE\x84\xBC"          // U+E13C
#define ICON_REFRESH "\xEE\x85\x85"       // U+E145
#define ICON_SKIP_BACK "\xEE\x85\x9F"     // U+E15F
#define ICON_SKIP_FORWARD "\xEE\x85\xA0"  // U+E160
#define ICON_SPEAKER "\xEE\x85\xA6"       // U+E166
#define ICON_VOLUME "\xEE\x86\xAB"        // U+E1AB
#define ICON_X "\xEE\x86\xB2"             // U+E1B2
#define ICON_TIMER "\xEE\x87\xA0"         // U+E1E0, lucide_18 only
#define ICON_LIGHTBULB "\xEE\x87\x82"     // U+E1C2, lucide_34 only
#define ICON_LIGHTBULB_OFF "\xEE\x88\x88" // U+E208, lucide_34 only

// Artwork placeholder codepoints (use with lucide_48 font)
#define ICON_AIRPLAY "\xEE\x80\xB9"     // U+E039
#define ICON_MUSIC "\xEE\x84\xA2"       // U+E122
#define ICON_RADIO "\xEE\x85\x82"       // U+E142
#define ICON_TV "\xEE\x86\x95"          // U+E195
#define ICON_AUDIO_LINES "\xEE\x95\x9A" // U+E55A

// Weather icon codepoints (use with lucide_22 font)
#define ICON_SUN "\xEE\x85\xB8"             // U+E178
#define ICON_MOON "\xEE\x84\x9E"            // U+E11E
#define ICON_CLOUD "\xEE\x82\x88"           // U+E088
#define ICON_CLOUD_SUN "\xEE\x88\x96"       // U+E216
#define ICON_CLOUD_MOON "\xEE\x88\x95"      // U+E215
#define ICON_CLOUD_FOG "\xEE\x88\x94"       // U+E214
#define ICON_CLOUD_DRIZZLE "\xEE\x82\x8A"   // U+E08A
#define ICON_CLOUD_RAIN "\xEE\x82\x8E"      // U+E08E
#define ICON_CLOUD_RAIN_WIND "\xEE\x82\x8F" // U+E08F
#define ICON_CLOUD_LIGHTNING "\xEE\x82\x8C" // U+E08C
#define ICON_CLOUD_SNOW "\xEE\x82\x90"      // U+E090
#define ICON_CLOUD_HAIL "\xEE\x82\x8B"      // U+E08B
#define ICON_CLOUD_SUN_RAIN "\xEE\x8B\xBB"  // U+E2FB
#define ICON_CLOUD_MOON_RAIN "\xEE\x8B\xBA" // U+E2FA
#define ICON_SNOWFLAKE "\xEE\x85\xA5"       // U+E165
#define ICON_WIND "\xEE\x86\xB0"            // U+E1B0
#define ICON_DROPLETS "\xEE\x82\xB5"        // U+E0B5
#define ICON_SUNRISE "\xEE\x85\xB9"         // U+E179
#define ICON_SUNSET "\xEE\x85\xBA"          // U+E17A
#define ICON_UMBRELLA "\xEE\x86\x99"        // U+E199
