# Every app builds every shared component, but only an app that turns on
# DTLS and PSK in mbedTLS (the radio) can talk to the gateway; the others
# leave this empty.
set(srcs)
if(CONFIG_MBEDTLS_SSL_PROTO_DTLS AND CONFIG_MBEDTLS_KEY_EXCHANGE_PSK)
    list(APPEND srcs "src/lights.cpp" "src/coaps.cpp")
endif()

idf_component_register(
    SRCS ${srcs}
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

if(srcs)
    target_compile_options(${COMPONENT_LIB} PUBLIC
        -std=gnu++20
        -fno-exceptions
        -fno-rtti
    )
endif()
