# A headless knob for an app's UI (sim.h). In the app's sim/CMakeLists.txt:
#   include(${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/sim.cmake)
#   knob_sim(radio_sim SOURCES ... INCLUDES ...)
# The app folder is the sim folder's parent. Its fonts (main/fonts/*.c) and the
# shared knob_ui sources come along.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(SIM ${CMAKE_CURRENT_LIST_DIR})
set(APP ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(COMPONENTS ${SIM}/../components)
set(LVGL ${APP}/managed_components/lvgl__lvgl)
set(SDKCONFIG_DIR ${APP}/build/config)

if(NOT EXISTS ${LVGL}/lvgl.h OR NOT EXISTS ${SDKCONFIG_DIR}/sdkconfig.h)
  message(FATAL_ERROR "Build the firmware once first: idf.py build in ${APP}")
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
target_include_directories(lvgl PUBLIC ${SIM})
target_compile_options(lvgl PRIVATE -w)

function(knob_sim name)
  cmake_parse_arguments(ARG "" "" "SOURCES;INCLUDES" ${ARGN})
  file(GLOB FONTS ${APP}/main/fonts/*.c ${COMPONENTS}/knob_ui/src/*.c)
  set(UI ${COMPONENTS}/knob_ui/src)
  add_executable(${name}
    ${SIM}/sim_main.cpp
    ${SIM}/sim_platform.cpp
    ${UI}/activity.cpp
    ${UI}/art_decoder.cpp
    ${UI}/art_tint.cpp
    ${UI}/backdrop.cpp
    ${UI}/caption.cpp
    ${UI}/carousel.cpp
    ${UI}/frost.cpp
    ${UI}/primitives.cpp
    ${UI}/pill.cpp
    ${UI}/sheet.cpp
    ${UI}/sheet_list.cpp
    ${UI}/squircle.cpp
    ${UI}/ui_pages.cpp
    ${UI}/volume_ring.cpp
    ${UI}/tjpgd/tjpgd.c
    ${FONTS}
    ${ARG_SOURCES})
  # Stubs first so they shadow the ESP-IDF headers.
  target_include_directories(${name} PRIVATE
    ${SIM}/stubs
    ${SIM}
    ${APP}/main
    ${COMPONENTS}/knob_hal/include
    ${COMPONENTS}/knob_ui/include
    ${COMPONENTS}/knob_ui/src
    ${COMPONENTS}/knob_net/include
    ${ARG_INCLUDES})
  target_compile_definitions(${name} PRIVATE SIMULATOR=1)
  target_link_libraries(${name} PRIVATE lvgl)
endfunction()
