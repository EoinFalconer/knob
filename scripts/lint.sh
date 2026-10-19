#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

# Find clang-tidy: prefer brew llvm, then PATH
if command -v /opt/homebrew/opt/llvm/bin/clang-tidy &>/dev/null; then
    CT=/opt/homebrew/opt/llvm/bin/clang-tidy
elif command -v clang-tidy &>/dev/null; then
    CT=clang-tidy
else
    echo "error: clang-tidy not found"
    echo "  brew install llvm"
    exit 1
fi

EXTRA_ARGS=()
if [[ "$(uname)" == "Darwin" ]] && xcrun --show-sdk-path &>/dev/null; then
    EXTRA_ARGS+=(--extra-arg="--sysroot=$(xcrun --show-sdk-path)")
fi

# Host clang has no Xtensa backend, so approximate the target:
# 1. -D__XTENSA__ keeps IDF headers on their xtensa branches (its absence
#    routes them to riscv paths, and to GCC-only quote-include tricks).
# 2. --target=i386 ELF: 32-bit pointers like xtensa, ELF sections like IDF's
#    EXT_RAM_BSS_ATTR (Mach-O rejects their names), and xtensa inline-asm
#    constraints like "a"/"=a" happen to be valid x86 constraints (asm bodies
#    aren't assembled in syntax-only mode). On arm64 they'd be rejected.
# 3. The xtensa toolchain's newlib/libstdc++ headers must win over the host
#    SDK so libc types match the firmware's.
EXTRA_ARGS+=(
    --extra-arg="-D__XTENSA__"
    --extra-arg="--target=i386-unknown-linux-gnu"
    --extra-arg="-fno-blocks"
    --extra-arg="-Wno-extern-c-compat"
)
XTENSA_ROOT=$(ls -d "$HOME"/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/xtensa-esp-elf 2>/dev/null | sort | tail -1)
if [ -n "$XTENSA_ROOT" ]; then
    GXX_VER=$(ls "$XTENSA_ROOT/include/c++" | sort | tail -1)
    EXTRA_ARGS+=(
        --extra-arg="-isystem$XTENSA_ROOT/include/c++/$GXX_VER"
        --extra-arg="-isystem$XTENSA_ROOT/include/c++/$GXX_VER/xtensa-esp-elf"
        --extra-arg="-isystem$XTENSA_ROOT/include"
    )
fi

# BUILD_DIR may point at an app build (e.g. apps/kaffi/build); default to the
# repo-root build for the simulator layout.
BUILD_DIR="${BUILD_DIR:-build}"
DB="$BUILD_DIR/compile_commands.json"
if [ ! -f "$DB" ]; then
    echo "error: $DB not found — run idf.py build or cmake -B build -S sim first"
    exit 1
fi

# Host clang chokes on xtensa-gcc-only flags, so lint against a sanitized
# copy of the compile database with those flags stripped.
CLEAN_DB_DIR="$BUILD_DIR/lint"
mkdir -p "$CLEAN_DB_DIR"
python3 - "$DB" "$CLEAN_DB_DIR/compile_commands.json" <<'PY'
import json, re, sys
BAD = re.compile(
    r"^(-mlongcalls|-mdisable-hardware-atomics|-fno-tree-switch-conversion"
    r"|-fstrict-volatile-bitfields|-fno-shrink-wrap)$"
)
with open(sys.argv[1]) as f:
    db = json.load(f)
for entry in db:
    if "command" in entry:
        entry["command"] = " ".join(
            a for a in entry["command"].split() if not BAD.match(a)
        )
    if "arguments" in entry:
        entry["arguments"] = [a for a in entry["arguments"] if not BAD.match(a)]
with open(sys.argv[2], "w") as f:
    json.dump(db, f)
PY

# Only lint the app's own main/ sources that have an entry in the database.
# Generated assets (lv_font_conv fonts, LVGLImage.py images) are skipped —
# they're machine output, not code we maintain.
mapfile -t SRCS < <(
    python3 -c "
import json, sys, os
root = os.getcwd()
app_dir = os.path.dirname(os.path.abspath('$DB'))
main_dir = os.path.join(os.path.dirname(app_dir), 'main')
GENERATED = (os.sep + 'fonts' + os.sep, os.sep + 'images' + os.sep)
with open('$DB') as f:
    db = json.load(f)
for entry in db:
    p = entry.get('file', '')
    if not os.path.isabs(p):
        p = os.path.join(entry.get('directory', ''), p)
    p = os.path.normpath(p)
    if p.startswith(main_dir + os.sep) and not any(g in p for g in GENERATED):
        print(os.path.relpath(p, root))
" | sort -u
)

if [ ${#SRCS[@]} -eq 0 ]; then
    echo "no main/ source files found in $DB"
    exit 0
fi

echo "running $($CT --version | head -1)"
echo "linting ${#SRCS[@]} files..."
echo ""

FAIL=0
for f in "${SRCS[@]}"; do
    # `|| RC=$?` keeps set -e from aborting before diagnostics are printed
    RC=0
    OUTPUT=$($CT -p "$CLEAN_DB_DIR" --quiet "${EXTRA_ARGS[@]}" "$f" 2>&1) || RC=$?
    FILTERED=$(echo "$OUTPUT" | grep -v "warnings\? generated\.\|errors\? generated\." || true)
    if [ -n "$FILTERED" ]; then
        echo "$FILTERED"
    fi
    if [ "$RC" -ne 0 ]; then
        FAIL=1
    fi
done

if [ "$FAIL" -ne 0 ]; then
    echo ""
    echo "lint: issues found"
    exit 1
fi

echo "lint: clean"
