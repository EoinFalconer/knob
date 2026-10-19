#!/usr/bin/env bash
# Host-side tests for knob_sonos: real control/SOAP/parsing code against a
# fake Sonos household behind an esp_http_client double.
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
c++ -std=c++20 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
  -Istubs -I../include -I../src -o "$out/sonos_test" sonos_test.cpp \
  ../src/sonos_control.cpp ../src/sonos_engine.cpp ../src/sonos_group.cpp \
  ../src/soap.cpp ../src/track_info.cpp ../src/zone_topology.cpp
"$out/sonos_test"
