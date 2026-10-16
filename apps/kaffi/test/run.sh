#!/usr/bin/env bash
# Host-side unit tests for kaffi logic that has no ESP-IDF dependency.
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)
c++ -std=c++20 -Wall -Wextra -Werror -o "$out/pending_ring_test" pending_ring_test.cpp
"$out/pending_ring_test"
