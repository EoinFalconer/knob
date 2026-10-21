set(srcs
    "src/ui_pages.cpp"
    "src/squircle.cpp"
    "src/art_decoder.cpp"
    "src/tjpgd/tjpgd.c"
    "src/lucide_18.c"
    "src/lucide_22.c"
    "src/primitives.cpp"
    "src/pill.cpp"
    "src/toast.cpp"
    "src/activity.cpp"
    "src/volume_ring.cpp"
    "src/carousel.cpp"
    "src/art_tint.cpp"
    "src/backdrop.cpp"
    "src/caption.cpp"
)
# Sheets frost a snapshot of the screen behind them, so only apps that turn
# on LVGL's snapshot get them.
if(CONFIG_LV_USE_SNAPSHOT)
    list(APPEND srcs "src/sheet.cpp" "src/frost.cpp" "src/sheet_list.cpp")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "include"
    REQUIRES
        knob_hal
    PRIV_REQUIRES
        esp_psram
)

target_compile_options(${COMPONENT_LIB} PUBLIC
    -std=gnu++20
    -fno-exceptions
    -fno-rtti
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)

# The fonts are the app's (fonts.h), compiled in its main component, which
# links before this one: link it again after, or a font only drawn from here
# is never pulled in.
target_link_libraries(${COMPONENT_LIB} INTERFACE __idf_main)
