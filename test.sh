#!/usr/bin/env bash
# test.sh — Build + lint check for monorepo apps. Run before pushing.
#
# Usage:
#   ./test.sh              Build the default app (radio)
#   ./test.sh radio        Build the radio app
#   ./test.sh <app>        Build a specific app
#   ./test.sh --all        Build all apps
#   ./test.sh --lint       Run clang-tidy lint only (requires prior build)
#
# Exit codes:
#   0 = all checks passed
#   1 = build or lint failed

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m'

cd "$(dirname "$0")"

APP=""
LINT=false
BUILD_ALL=false

for arg in "$@"; do
  case "$arg" in
    --all)  BUILD_ALL=true ;;
    --lint) LINT=true ;;
    -h|--help)
      echo "Usage: ./test.sh [<app>] [--all] [--lint]"
      echo "  (default)    Build the radio app"
      echo "  <app>        Build a specific app (e.g., radio, homekit)"
      echo "  --all        Build all apps in apps/"
      echo "  --lint       Run clang-tidy (requires prior build)"
      exit 0
      ;;
    -*) echo "Unknown option: $arg"; exit 1 ;;
    *)  APP="$arg" ;;
  esac
done

# Default to radio if no app specified
if [ -z "$APP" ] && [ "$BUILD_ALL" = false ]; then
  APP="radio"
fi

FAIL=0

# Apple's fonts and ringtone stay out of git; they're made here, on a Mac.
scripts/gen_assets.sh || FAIL=1

build_app() {
  local app_name="$1"
  local app_dir="apps/$app_name"

  if [ ! -d "$app_dir" ]; then
    echo -e "${RED}  ✗ App not found: $app_dir${NC}"
    return 1
  fi

  echo -e "${BOLD}▸ Building $app_name...${NC}"
  if ! command -v idf.py &>/dev/null; then
    echo -e "${YELLOW}  idf.py not found — source ESP-IDF first:${NC}"
    echo "    source ~/esp/esp-idf/export.sh"
    return 1
  fi

  pushd "$app_dir" > /dev/null
  idf.py set-target esp32s3 2>/dev/null || true
  if idf.py build; then
    echo -e "${GREEN}  ✓ $app_name build passed${NC}"
    echo ""
    idf.py size 2>/dev/null | grep -E "^(Total|Used)" || true
  else
    echo -e "${RED}  ✗ $app_name build failed${NC}"
    popd > /dev/null
    return 1
  fi
  popd > /dev/null
}

run_host_tests() {
  local app_name="$1"
  local runner="apps/$app_name/test/run.sh"
  [ -x "$runner" ] || return 0

  echo -e "${BOLD}▸ Running host tests ($app_name)...${NC}"
  if "$runner"; then
    echo -e "${GREEN}  ✓ Host tests passed${NC}"
  else
    echo -e "${RED}  ✗ Host tests failed${NC}"
    return 1
  fi
}

run_lint() {
  local app_name="$1"
  local app_dir="apps/$app_name"

  echo -e "${BOLD}▸ Running clang-tidy lint ($app_name)...${NC}"
  if [ ! -f "$app_dir/build/compile_commands.json" ]; then
    echo -e "${YELLOW}  No compile_commands.json — build $app_name first${NC}"
    return 1
  fi
  if [ -f scripts/lint.sh ]; then
    if BUILD_DIR="$app_dir/build" ./scripts/lint.sh; then
      echo -e "${GREEN}  ✓ Lint passed${NC}"
    else
      echo -e "${RED}  ✗ Lint issues found${NC}"
      return 1
    fi
  else
    echo -e "${YELLOW}  No lint script found${NC}"
  fi
}

if [ "$BUILD_ALL" = true ]; then
  for app_dir in apps/*/; do
    app_name=$(basename "$app_dir")
    run_host_tests "$app_name" || FAIL=1
    build_app "$app_name" || FAIL=1
    echo ""
  done
else
  run_host_tests "$APP" || FAIL=1
  build_app "$APP" || FAIL=1
fi

if ! $LINT; then
  # Auto-lint after build
  if [ "$BUILD_ALL" = true ]; then
    for app_dir in apps/*/; do
      app_name=$(basename "$app_dir")
      echo ""
      run_lint "$app_name" || FAIL=1
    done
  elif [ -n "$APP" ]; then
    echo ""
    run_lint "$APP" || FAIL=1
  fi
fi

if $LINT; then
  if [ "$BUILD_ALL" = true ]; then
    for app_dir in apps/*/; do
      app_name=$(basename "$app_dir")
      run_lint "$app_name" || FAIL=1
    done
  elif [ -n "$APP" ]; then
    run_lint "$APP" || FAIL=1
  fi
fi

echo ""
if [ "$FAIL" -eq 0 ]; then
  echo -e "${GREEN}${BOLD}All checks passed ✓${NC}"
else
  echo -e "${RED}${BOLD}Some checks failed ✗${NC}"
  exit 1
fi
