#!/usr/bin/env bash
# Host-side unit tests, including NVS/HTTP doubles for delivery retries.
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
c++ -std=c++20 -Wall -Wextra -Werror -o "$out/pending_ring_test" pending_ring_test.cpp
"$out/pending_ring_test"
c++ -std=c++20 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
  -Istubs -I../main -o "$out/delivery_test" delivery_test.cpp \
  ../main/kaffi/pending.cpp ../main/kaffi/sanity_api.cpp
"$out/delivery_test"
