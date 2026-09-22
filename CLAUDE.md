# resodoom

**Doom as a live Resolume source.** Point it at a WAD and the game becomes an
FFGL layer: composite it, key it, run it through other effects, MIDI-map the
controls. With nothing mapped it plays Doom's attract demos forever.

C++17 + C11, CMake → universal `.bundle` (macOS). **GPL-2.0 — NOT the fleet's
usual MIT**, because doomgeneric descends from id's Doom source and anything
linked into this binary inherits it. Public.

The non-Doom half is a separate MIT repo, `stagehand`, consumed here as a
submodule: it owns the private-copy loading, the clock pacing, the letterbox
maths, the texture presentation and the diagnostics log. Before adding
anything to `source/`, ask whether it belongs there instead — and never move
GPL code in that direction.

Read `AGENTS.md` before changing the clock, the loading arrangement, or
anything about what may be shipped. **No WAD is ever committed here.**

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install into Resolume: `cmake --install build`
- Submodules: `git submodule update --init --recursive` (ffgl, doomgeneric,
  stagehand — the build stops with instructions if any is missing)
- Skip pieces: `-DRESODOOM_BUILD_PLUGIN=OFF`, `-DRESODOOM_BUILD_TOOLS=OFF`
  (these are cached — re-running `cmake -B build` without them keeps the old
  value, so pass `=ON` explicitly to turn one back on)
- Widescreen engine: `-DRESODOOM_SCREEN_WIDTH=426` (16:9; 384 = 16:10,
  568 = 21:9). Also cached. The 3D view is true Hor+; the 2D screens (title,
  menus, intermission) are still 320-wide art at the left edge.

## Verify
- Everything: `RESODOOM_TEST_IWAD=/path/to/freedoom1.wad tools/verify.sh`
  (without the variable it builds, checks symbols, and **skips** the rest)
- The engine, no GL: `./build/resotest --iwad W.wad --check`
- The real plugin in a real GL context: `./build/resogl --iwad W.wad --check`
- The other letterbox branch: `./build/resogl --iwad W.wad --check --size 720x720`
- A frame as a PPM: `./build/resotest --iwad W.wad --tics 500 --out /tmp/f.ppm`
- A PPM per frame: `./build/resotest --iwad W.wad --tics 400 --seq /tmp/f_`
- Straight into a level: `./build/resotest --iwad W.wad --warp 1 3 --skill 4`

`sips -s format png /tmp/f.ppm --out /tmp/f.png` to look at one.

## Parameters
`WAD`, `Mod WAD`, `Run`, `Restart`, `Speed`, `Skill`, `Episode`, `Map`,
`Scaling` (Fit / Fill / Stretch / Integer), `Pixel Aspect`, `Smoothing`, then
twelve controls in a **Controls** group — plain booleans, so Resolume
MIDI-maps, keyboard-maps and automates them like anything else.

## Notes
- **`doomgeneric_Create` does not loop.** It runs D_DoomMain plus one tick and
  returns; the host owns the loop.
- **The clock is virtual and is spent in `I_Sleep`.** Gating on the draw
  deadlocks; so does advancing on Grant. See AGENTS.md — both cost an hour.
- **A loaded engine copy runs Doom once.** A second `Start` is refused. Each
  plugin instance stages its own uniquely-named copy and dlopens that, which is
  also what makes two layers two games.
- **The engine thread needs an 8 MiB stack.** A pthread's 512 KiB default gives
  a SIGBUS in an innocent-looking leaf.
- Upstream is a **pristine submodule**. Changes are made by force-including
  `source/engine/ResodoomHooks.h`, which intercepts `exit`, the allocator and
  `SCREENWIDTH`. `EngineImpl.c` must `#undef` those macros — `-include` runs
  before line 1. An edit *inside* an upstream function cannot be intercepted;
  those live in `patches/`, applied to a copy in the build tree and never to
  the submodule. Each must be an **identity at 320x200** — see AGENTS.md.
- The engine implements **stagehand's generic source ABI**, not a bespoke one.
  Options arrive as a key/value list, so adding a setting needs no ABI change.
- **`pixelAspect` is pixel WIDTH over HEIGHT**, so Doom's is 5/6 and not 1.2.
  The reciprocal gives a 1.92 display aspect and stretches every face.
- **No FBO anywhere**, and no `ffglex::Scoped*` bindings — both are SDK traps.
- **There is no audio.** FFGL has no audio path; Doom runs `-nosound -nomusic`.
- macOS build must be universal. Verify with `lipo`, never the build log.
- `flat`, `active`, `filter`, `input`, `output`, `sample`, `common`, `patch`
  are GLSL reserved words. Shader errors surface only at runtime.
- Public repo. "Commit" = commit **and** push.

## Diagnostics
`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume). It also **redirects the process's stderr into the log**, because
Doom writes its failures there and then exits, and inside Resolume that output
otherwise goes nowhere. It covers the failures that all look identical from
outside ("the layer is black"): a WAD that could not be found, a WAD Doom
rejected, a missing engine library, an ABI mismatch, a shader that would not
compile.

    ~/Library/Logs/Resodoom/resodoom.YYYY-MM-DD.log
