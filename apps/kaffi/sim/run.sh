#!/usr/bin/env bash
# Render the kaffi UI headlessly with the firmware's LVGL config.
#   apps/kaffi/sim/run.sh [people-photo-dir]
# Screenshots: apps/kaffi/build/sim/out/*.png
# People's photos are 120 px baseline JPEGs named as in scenarios.cpp; any
# missing one shows the person's initial instead.
set -euo pipefail
cd "$(dirname "$0")"
build=../build/sim
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$build" -j 4 > /dev/null
mkdir -p "$build/out" "$build/people"
people="$(cd "${1:-$build/people}" && pwd)"
"$build/kaffi_sim" "$people" "$build/out"
for f in "$build"/out/*.ppm; do magick "$f" "${f%.ppm}.png" && rm "$f"; done
