idf_component_register(
    SRCS
        "src/lights.cpp"
        "src/coaps.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_event
        knob_net
    PRIV_REQUIRES
        esp_timer
        json
        lwip
        mbedtls
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PUBLIC
    -std=gnu++20
    -fno-exceptions
    -fno-rtti
)
