set(SRCS
    "main.cpp"
    "alarm_sound.cpp"
    "weather.cpp"
    "fonts/sf_regular_18.c"
    "fonts/sf_regular_16.c"
    "fonts/sf_regular_22.c"
    "fonts/sf_semibold_28.c"
    "fonts/sf_rounded_regular_200.c"
    "fonts/sf_rounded_semibold_84.c"
    "fonts/sf_rounded_black_200.c"
    "fonts/sf_rounded_regular_132.c"
    "fonts/sf_medium_13.c"
    "fonts/sf_medium_15.c"
    "fonts/sf_semibold_16.c"
    "fonts/sf_semibold_20.c"
    "fonts/lucide_48.c"
    "fonts/lucide_18.c"
    "fonts/lucide_34.c"
    "ui/images/nrj.c"
    "ui/images/nrk_jazz.c"
    "ui/images/nrk_mp3.c"
    "ui/images/nrk_nyheter.c"
    "ui/images/nrk_p1.c"
    "ui/images/nrk_p2.c"
    "ui/images/nrk_p3.c"
    "ui/images/p4.c"
    "ui/images/p5.c"
    "ui/images/p9.c"
    "ui/images/radio_norge.c"
    "ui/images/radio_rock.c"
    "ui/activity.cpp"
    "ui/carousel.cpp"
    "ui/face_palette.cpp"
    "ui/clock_art.cpp"
    "ui/faces.cpp"
    "ui/weather_assets.cpp"
    "ui/weather_kind.cpp"
    "ui/now_playing.cpp"
    "ui/primitives.cpp"
    "ui/ui.cpp"
    "ui/timer_dial.cpp"
    "ui/timer_sheet.cpp"
    "ui/lights_page.cpp"
    "ui/splash.cpp"
    "ui/volume_ring.cpp"
    "ui/sheet.cpp"
    "ui/frost.cpp"
)

set(INCLUDE_DIRS
    "."
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    EMBED_FILES "sounds/alarm.mp3"
    REQUIRES
        knob_hal
        knob_net
        knob_storage
        knob_ui
        knob_sonos
        knob_timer
        knob_lights
        esp_event
        esp_timer
        esp_http_client
        json
        esp_wifi
        nvs_flash
    PRIV_REQUIRES
        esp_http_server
        esp_netif
        esp_psram
)

target_compile_options(${COMPONENT_LIB} PUBLIC
    -std=gnu++20
    -fno-exceptions
    -fno-rtti
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
