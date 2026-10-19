#!/usr/bin/env bash
# Makes the assets that are Apple's and so stay out of git: the SF fonts and
# the timer's ringtone, from this Mac's own copies. Anything already made is
# kept. test.sh and flash.sh run this before building.
#   scripts/gen_assets.sh [--force]
set -euo pipefail
cd "$(dirname "$0")/.."

if [ "$(uname)" != "Darwin" ]; then
  echo "gen_assets: needs a Mac (Apple's fonts and ringtones)" >&2
  exit 1
fi

for tool in uv npx ffmpeg lame; do
  if ! command -v "$tool" &>/dev/null; then
    echo "gen_assets: $tool not found (brew install uv node ffmpeg lame)" >&2
    exit 1
  fi
done

uv run --quiet --with fonttools scripts/gen_fonts.py "$@"

# Encoded without a bit reservoir or an Info header, so copies of it play
# back to back as one longer file (see apps/radio/main/alarm_sound.cpp).
alarm=apps/radio/main/sounds/alarm.mp3
tone=/System/Library/PrivateFrameworks/ToneLibrary.framework/Versions/A/Resources/Ringtones/Breaking-EncoreInfinitum.m4r
if [ ! -f "$alarm" ] || [ "${1:-}" = "--force" ]; then
  mkdir -p "$(dirname "$alarm")"
  ffmpeg -v error -i "$tone" -f wav - | lame --quiet --nores -t -b 96 --resample 32 - "$alarm"
  echo "radio: alarm.mp3"
fi
