set(SRCS
    "main.cpp"
    "spotify_setup.cpp"
    "spotify/json_parse.cpp"
    "spotify/spotify_auth.cpp"
    "spotify/spotify_api.cpp"
    "ui/ui.cpp"
    "fonts/sf_regular_16.c"
    "fonts/sf_regular_18.c"
    "fonts/sf_regular_22.c"
    "fonts/sf_medium_15.c"
    "fonts/sf_semibold_28.c"
    "fonts/lucide_48.c"
    "ui/images/spotify_logo_32.c"
    "ui/images/spotify_logo_64.c"
)

set(INCLUDE_DIRS
    "."
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES
        knob_hal
        knob_net
        knob_setup
        knob_storage
        knob_ui
        esp_event
        esp_timer
        esp_wifi
        esp_http_client
        esp_http_server
        esp-tls
        nvs_flash
        lwip
    PRIV_REQUIRES
        esp_psram
)

target_compile_options(${COMPONENT_LIB} PUBLIC
    -std=gnu++20
    -fno-exceptions
    -fno-rtti
)
