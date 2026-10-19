#!/usr/bin/env bash
# Host-side tests for the radio app's UI decisions, plus the shared Sonos
# component tests they build on.
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
sonos=../../../components/knob_sonos
c++ -std=c++20 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
  -I"$sonos/include" -I../main -o "$out/now_playing_test" now_playing_test.cpp \
  ../main/ui/now_playing.cpp ../main/ui/weather_kind.cpp ../main/ui/face_palette.cpp \
  "$sonos/src/sonos_group.cpp"
"$out/now_playing_test"
"$sonos/test/run.sh"
