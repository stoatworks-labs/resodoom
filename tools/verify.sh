#!/bin/sh
#
# Everything, in three layers that fail for different reasons.
#
#   tools/verify.sh
#
# A WAD is needed for most of it and none can ship with this repo, so point
# RESODOOM_TEST_IWAD at one. Freedoom is free, complete and exactly what this
# was developed against:
#
#   https://freedoom.github.io/
#   RESODOOM_TEST_IWAD=~/wads/freedoom1.wad tools/verify.sh
#
# Without it the build and the no-WAD checks still run, and the rest is SKIPPED
# rather than quietly passing.

set -e

ROOT=$( cd "$( dirname "$0" )/.." && pwd )
BUILD="$ROOT/build-verify"

IWAD="${RESODOOM_TEST_IWAD:-}"

say() { printf '\n=== %s ===\n' "$1"; }

say "configure"
# A single architecture on purpose: this script is about correctness and runs
# often. `lipo` on the release build is what checks the universal one, and the
# build log is not evidence for that -- see AGENTS.md.
cmake -S "$ROOT" -B "$BUILD" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      > "$BUILD.log" 2>&1 || { tail -30 "$BUILD.log"; exit 1; }

say "build"
cmake --build "$BUILD" -j8 >> "$BUILD.log" 2>&1 || { tail -40 "$BUILD.log"; exit 1; }
echo "ok"

say "the engine is loadable and exports exactly one symbol"
# Hidden visibility everywhere except the one entry point. doomgeneric has
# several hundred file-scope globals and none of them belong in the host's
# symbol namespace.
EXPORTS=$( nm -gU "$BUILD/libresodoom_engine.dylib" 2>/dev/null \
           | grep -c "resodoom_engine_api" || true )
if [ "$EXPORTS" -ne 1 ]; then
    echo "FAIL: expected exactly one exported entry point, found $EXPORTS"
    exit 1
fi
LEAKED=$( nm -gU "$BUILD/libresodoom_engine.dylib" 2>/dev/null \
          | grep -cE " _(D_DoomMain|W_CacheLumpName|R_RenderPlayerView)$" || true )
if [ "$LEAKED" -ne 0 ]; then
    echo "FAIL: doomgeneric internals are visible in the engine's exports"
    exit 1
fi
echo "ok"

say "the bundle registers a plugin"
# CFFGLPluginInfo registers from a file-scope constructor that nothing
# references by name. In a static archive the linker may drop the whole
# translation unit, giving a bundle that loads, exports plugMain and reports
# that it contains no plugins.
if ! nm -gU "$BUILD/Resodoom.bundle/Contents/MacOS/Resodoom" 2>/dev/null \
     | grep -q plugMain; then
    echo "FAIL: the bundle does not export plugMain"
    exit 1
fi
echo "ok"

say "the engine is inside the bundle"
# A bundle without it loads fine and then refuses every WAD.
if [ ! -f "$BUILD/Resodoom.bundle/Contents/MacOS/libresodoom_engine.dylib" ]; then
    echo "FAIL: the engine was not staged into the bundle"
    exit 1
fi
echo "ok"

if [ -z "$IWAD" ]; then
    say "SKIPPED: everything that needs a WAD"
    echo "Set RESODOOM_TEST_IWAD to a WAD file to run the rest."
    echo "  https://freedoom.github.io/"
    exit 0
fi

if [ ! -f "$IWAD" ]; then
    echo "FAIL: RESODOOM_TEST_IWAD is set but '$IWAD' is not a file"
    exit 1
fi

say "resotest -- the engine, no graphics API"
"$BUILD/resotest" --iwad "$IWAD" --check 2>/dev/null | grep -E "^  (ok|FAIL)|checks,"
"$BUILD/resotest" --iwad "$IWAD" --check > /dev/null 2>&1 || exit 1

say "resogl -- the real plugin, headless CGL, 16:9"
"$BUILD/resogl" --iwad "$IWAD" --check 2>/dev/null | grep -E "^  (ok|FAIL)|checks,"
"$BUILD/resogl" --iwad "$IWAD" --check > /dev/null 2>&1 || exit 1

say "resogl -- the other letterbox branch, 1:1"
# Not redundant. A sign error in the Fit branch is invisible whenever the
# picture happens to be wider than the frame, and a square render is the
# cheapest way to make the other branch matter.
"$BUILD/resogl" --iwad "$IWAD" --check --size 720x720 2>/dev/null \
    | grep -E "^  (ok|FAIL)|checks,"
"$BUILD/resogl" --iwad "$IWAD" --check --size 720x720 > /dev/null 2>&1 || exit 1

say "all passed"
