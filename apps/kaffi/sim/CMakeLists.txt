cmake_minimum_required(VERSION 3.16)
project(kaffi_sim LANGUAGES C CXX)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../sim/sim.cmake)

knob_sim(kaffi_sim
  SOURCES
    kaffi_platform.cpp
    scenarios.cpp
    ${APP}/main/ui/ui.cpp
    ${APP}/main/ui/avatars.cpp
    ${APP}/main/ui/standing.cpp)
