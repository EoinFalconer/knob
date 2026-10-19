cmake_minimum_required(VERSION 3.16)
project(radio_sim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(RADIO ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(COMPONENTS ${RADIO}/../../components)
set(LVGL ${RADIO}/managed_components/lvgl__lvgl)
set(SDKCONFIG_DIR ${RADIO}/build/config)

if(NOT EXISTS ${LVGL}/lvgl.h OR NOT EXISTS ${SDKCONFIG_DIR}/sdkconfig.h)
  message(FATAL_ERROR "Build the firmware once first: cd apps/radio && idf.py build")
endif()

# A use-after-free in UI code freezes the device instead of crashing it, so
# the sim always runs under AddressSanitizer to catch it on the host.
add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
add_link_options(-fsanitize=address)

# LVGL exactly as the firmware configures it (via its generated sdkconfig.h).
# Optional modules compile to nothing unless the firmware's Kconfig enables them.
file(GLOB_RECURSE LVGL_SRC ${LVGL}/src/*.c)
add_library(lvgl STATIC ${LVGL_SRC})
target_include_directories(lvgl PUBLIC ${LVGL} ${LVGL}/src ${SDKCONFIG_DIR})
target_compile_definitions(lvgl PUBLIC
  LV_CONF_KCONFIG_EXTERNAL_INCLUDE="sim_conf.h"
  LV_CONF_SKIP
  LV_LVGL_H_INCLUDE_SIMPLE)
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(lvgl PRIVATE -w)

file(GLOB FONTS ${RADIO}/main/fonts/*.c ${COMPONENTS}/knob_ui/src/*.c)
file(GLOB IMAGES ${RADIO}/main/ui/images/*.c)

add_executable(radio_sim
  sim_main.cpp
  sim_platform.cpp
  scenarios.cpp
  ${RADIO}/main/ui/ui.cpp
  ${RADIO}/main/ui/activity.cpp
  ${RADIO}/main/ui/carousel.cpp
  ${RADIO}/main/ui/face_palette.cpp
  ${RADIO}/main/ui/clock_art.cpp
  ${RADIO}/main/ui/faces.cpp
  ${RADIO}/main/ui/weather_assets.cpp
  ${RADIO}/main/ui/weather_kind.cpp
  ${RADIO}/main/ui/timer_dial.cpp
  ${RADIO}/main/ui/timer_sheet.cpp
  ${RADIO}/main/ui/lights_page.cpp
  ${RADIO}/main/ui/splash.cpp
  ${RADIO}/main/ui/now_playing.cpp
  ${RADIO}/main/ui/primitives.cpp
  ${RADIO}/main/ui/volume_ring.cpp
  ${RADIO}/main/ui/sheet.cpp
  ${RADIO}/main/ui/frost.cpp
  ${COMPONENTS}/knob_ui/src/ui_pages.cpp
  ${COMPONENTS}/knob_ui/src/art_decoder.cpp
  ${COMPONENTS}/knob_ui/src/squircle.cpp
  ${COMPONENTS}/knob_ui/src/tjpgd/tjpgd.c
  ${COMPONENTS}/knob_sonos/src/sonos_group.cpp
  ${FONTS}
  ${IMAGES})

# Stubs first so they shadow the ESP-IDF headers.
target_include_directories(radio_sim PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${RADIO}/main
  ${COMPONENTS}/knob_hal/include
  ${COMPONENTS}/knob_ui/include
  ${COMPONENTS}/knob_ui/src
  ${COMPONENTS}/knob_sonos/include
  ${COMPONENTS}/knob_storage/include
  ${COMPONENTS}/knob_net/include
  ${COMPONENTS}/knob_timer/include
  ${COMPONENTS}/knob_lights/include)
target_compile_definitions(radio_sim PRIVATE SIMULATOR=1)
target_link_libraries(radio_sim PRIVATE lvgl)
