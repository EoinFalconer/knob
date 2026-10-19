idf_component_register(
    SRCS
        "src/timer.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_event
        esp_timer
        nvs_flash
        knob_voice
        knob_net
)

target_compile_options(${COMPONENT_LIB} PUBLIC
    -std=gnu++20
    -fno-exceptions
    -fno-rtti
)
