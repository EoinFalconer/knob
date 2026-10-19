cmake_minimum_required(VERSION 3.16)
project(radio_sim LANGUAGES C CXX)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/sim.cmake)

file(GLOB IMAGES ${APP}/main/ui/images/*.c)

knob_sim(radio_sim
  SOURCES
    radio_platform.cpp
    scenarios.cpp
    ${APP}/main/ui/ui.cpp
    ${APP}/main/ui/face_palette.cpp
    ${APP}/main/ui/clock_art.cpp
    ${APP}/main/ui/faces.cpp
    ${APP}/main/ui/weather_assets.cpp
    ${APP}/main/ui/weather_kind.cpp
    ${APP}/main/ui/timer_dial.cpp
    ${APP}/main/ui/timer_sheet.cpp
    ${APP}/main/ui/lights_page.cpp
    ${APP}/main/ui/splash.cpp
    ${APP}/main/ui/now_playing.cpp
    ${COMPONENTS}/knob_sonos/src/sonos_group.cpp
    ${IMAGES}
  INCLUDES
    ${COMPONENTS}/knob_sonos/include
    ${COMPONENTS}/knob_storage/include
    ${COMPONENTS}/knob_timer/include
    ${COMPONENTS}/knob_lights/include)
