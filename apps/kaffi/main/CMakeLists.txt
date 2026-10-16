set(SRCS
    "main.cpp"
    "kaffi/sanity_api.cpp"
    "kaffi/prefs.cpp"
    "kaffi/pending.cpp"
    "kaffi/ota.cpp"
    "ui/ui.cpp"
    "ui/images/img_flame.cpp"
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
