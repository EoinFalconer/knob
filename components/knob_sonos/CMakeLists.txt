idf_component_register(
    SRCS
        "src/sonos.cpp"
        "src/sonos_control.cpp"
        "src/sonos_engine.cpp"
        "src/sonos_group.cpp"
        "src/soap.cpp"
        "src/track_info.cpp"
        "src/zone_topology.cpp"
        "src/discovery.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_event
        esp_http_client
        knob_storage
        knob_net
    PRIV_REQUIRES
        esp-tls
        esp_timer
        lwip
)

target_compile_options(${COMPONENT_LIB} PUBLIC
    -std=gnu++20
    -fno-exceptions
    -fno-rtti
)
