set(SRCS
    "main.cpp"
    "kaffi/sanity_api.cpp"
    "kaffi/prefs.cpp"
    "kaffi/pending.cpp"
    "kaffi/ota.cpp"
    "ui/ui.cpp"
    "ui/avatars.cpp"
    "ui/standing.cpp"
    "fonts/sf_regular_16.c"
    "fonts/sf_regular_18.c"
    "fonts/sf_regular_22.c"
    "fonts/sf_medium_15.c"
    "fonts/sf_semibold_28.c"
    "fonts/sf_rounded_semibold_48.c"
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
        esp_netif
        esp_http_client
        esp_https_ota
        app_update
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

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
