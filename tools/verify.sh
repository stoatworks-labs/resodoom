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

# The check lines are filtered with no ^ anchor on purpose. Doom prints to
# stdout too, some of it without a newline ("This appears to be v1.8."), so a
# check can land mid-line -- and anchoring hides exactly those, silently. The
# exit status is what decides pass or fail; this is only what gets shown.

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

# One engine per aspect, the same list as CMakeLists.txt's
# RESODOOM_ENGINE_WIDTHS and Controls.h's kEngineAspects. The 320 engine keeps
# the historical name.
WIDTHS="320 384 426 568"
engine_name() {
    if [ "$1" = 320 ]; then echo "libresodoom_engine.dylib"
    else echo "libresodoom_engine_$1.dylib"; fi
}

say "every engine is loadable and exports exactly one symbol"
# Hidden visibility everywhere except the one stagehand entry point.
# doomgeneric has several hundred file-scope globals and none of them belong in
# the host's symbol namespace.
for W in $WIDTHS; do
    ENGINE="$BUILD/$( engine_name "$W" )"
    EXPORTS=$( nm -gU "$ENGINE" 2>/dev/null \
               | grep -c "stagehand_source_api" || true )
    if [ "$EXPORTS" -ne 1 ]; then
        echo "FAIL: the $W engine exports $EXPORTS entry points, not exactly one"
        exit 1
    fi
    LEAKED=$( nm -gU "$ENGINE" 2>/dev/null \
              | grep -cE " _(D_DoomMain|W_CacheLumpName|R_RenderPlayerView)$" || true )
    if [ "$LEAKED" -ne 0 ]; then
        echo "FAIL: doomgeneric internals are visible in the $W engine's exports"
        exit 1
    fi
done
echo "ok ($WIDTHS)"

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

say "every engine is inside the bundle, where it can be signed"
# A bundle without the 320 engine loads fine and then refuses every WAD; one
# without another engine quietly falls back to 4:3 for that aspect. They have
# to be in Contents/Frameworks and not Contents/MacOS: macOS signs inside-out
# and the fleet's signing pass skips MacOS/*, so an engine there is never
# signed and signing the bundle fails naming it.
for W in $WIDTHS; do
    if [ ! -f "$BUILD/Resodoom.bundle/Contents/Frameworks/$( engine_name "$W" )" ]; then
        echo "FAIL: the $W engine was not staged into the bundle"
        exit 1
    fi
done
echo "ok ($WIDTHS)"

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

for W in $WIDTHS; do
    say "resotest -- the $W engine, no graphics API"
    # Held to its own width: --expect-width is what stops an engine built at
    # the wrong width from grading itself.
    ENGINE="$BUILD/$( engine_name "$W" )"
    "$BUILD/resotest" --iwad "$IWAD" --check --engine "$ENGINE" --expect-width "$W" \
        2>/dev/null | grep -E "  (ok|FAIL)  |checks,"
    "$BUILD/resotest" --iwad "$IWAD" --check --engine "$ENGINE" --expect-width "$W" \
        > /dev/null 2>&1 || exit 1
done

say "resogl -- the real plugin, headless CGL, 16:9, and the Aspect picker"
"$BUILD/resogl" --iwad "$IWAD" --check 2>/dev/null | grep -E "  (ok|FAIL)  |checks,"
"$BUILD/resogl" --iwad "$IWAD" --check > /dev/null 2>&1 || exit 1

say "resogl -- the other letterbox branch, 1:1"
# Not redundant. A sign error in the Fit branch is invisible whenever the
# picture happens to be wider than the frame, and a square render is the
# cheapest way to make the other branch matter.
"$BUILD/resogl" --iwad "$IWAD" --check --size 720x720 2>/dev/null \
    | grep -E "  (ok|FAIL)  |checks,"
"$BUILD/resogl" --iwad "$IWAD" --check --size 720x720 > /dev/null 2>&1 || exit 1

say "all passed"
