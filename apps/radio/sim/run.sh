#!/usr/bin/env bash
# Render the radio UI headlessly with the firmware's LVGL config.
#   apps/radio/sim/run.sh [cover.jpg]
# Screenshots: apps/radio/build/sim/out/*.png
# Prints how much of the panel each interaction repaints; on the ESP32-S3 that
# is what costs CPU and SPI time, so keep frequent interactions small.
set -euo pipefail
cd "$(dirname "$0")"
build=../build/sim
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$build" -j 4 > /dev/null
mkdir -p "$build/out"
art="${1:-$build/art.jpg}"
if [ ! -f "$art" ]; then
  # tjpgd needs a baseline JPEG; any square cover works.
  magick -size 300x300 gradient:'#d0402a-#1d3f8f' -interlace none "$art"
fi
"$build/radio_sim" "$art" "$build/out"
for f in "$build"/out/*.ppm; do magick "$f" "${f%.ppm}.png" && rm "$f"; done
