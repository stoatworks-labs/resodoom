# resodoom — orientation for another LLM (or a newcomer)

**What it is:** Doom running inside Resolume as an FFGL source. The game
becomes a layer you can composite, key, MIDI-map and run through other effects.
C++17 and C11, CMake, universal macOS `.bundle`. **GPL-2.0 — the one repo in
the fleet that is not MIT.**

**The non-Doom half lives elsewhere.**
[stagehand](https://github.com/stoatworks-labs/stagehand) is an MIT library,
consumed here as a submodule, holding everything that is not about Doom:
loading a private copy of a source library, paying out its clock, the letterbox
maths, the texture presentation and the log. This repo's engine implements
stagehand's generic source ABI, so the boundary is compiled and tested rather
than asserted.

Code may move from here to stagehand only if it is genuinely Doom-free.
Nothing may move the other way.

`CLAUDE.md` is the command reference — build, install, verify. This file is the
*why*: read it before touching the clock, the loading arrangement, or anything
to do with what may be shipped.

---

## The one idea

**Doom's clock is a thing this plugin hands out, not a thing it reads.**

A libretro frontend has to fight the difference between the console's rate and
the composition's. Here there is no second clock at all: `DG_GetTicksMs` returns
a number the plugin controls, the consumer releases a budget of tics, and the
engine spends it. Speed, pause, single-stepping in a test and bit-exact
determinism are then all the same mechanism rather than four features.

Everything follows from that:

- Pausing costs nothing. The engine blocks on its budget instead of spinning.
- Speed is exact rather than approximate, because it scales the budget.
- Two cold runs are byte-identical, which is what makes a pixel digest a
  reasonable thing for `resotest` to assert against.
- The **double-render trap does not apply.** Resolume renders the same
  composition frame to the output, the preview monitor and the clip thumbnail;
  a generator that advanced once per `ProcessOpenGL` would run at two or three
  times speed *only while the preview is open* — which is how coinop's ball
  ended up moving at double speed. Releasing *elapsed wall time* instead means
  two calls a microsecond apart release a microsecond of Doom between them.

---

## The traps

Ordered by how much time they cost.

**`doomgeneric_Create` does not loop, despite the name and despite appearances.**
It runs the whole of D_DoomMain *plus one tick* and returns; the host owns the
loop from there (`Create(); while(1) Tick();` is what every upstream reference
port does). Building the engine around "D_DoomMain never returns" produces a
teardown path made of longjmps that is not needed, and an engine thread that
exits after one frame while the consumer waits forever for a second.

**The budget must be spent in `I_Sleep`, and the two obvious alternatives both
deadlock.** Each presents identically: Doom prints its entire startup banner
and then nothing happens.

- *Gate in `DG_DrawFrame`, one frame per granted tic.* `TryRunTics` will not
  produce a tic until `I_GetTime` moves, so a clock that only moves after a
  draw is waiting for a draw that is waiting for the clock.
- *Advance the clock on `Grant`.* This fixes the first tick and hangs on the
  second: `TryRunTics` reads `entertic` on entry and its wait loop needs the
  clock to advance *during* the call, which a clock moved from another thread
  between calls never does.

`I_Sleep` is where Doom itself says "I have nothing to do" — gating there means
blocking exactly where it would have idled, and the clock advances in the
middle of `TryRunTics` where that loop expects it to.

**A loaded copy of the engine can run Doom exactly once.** `Close()` joins the
thread and frees every allocation, but it cannot put doomgeneric's several
hundred file-scope globals back: the wad list, the zone pointers, the game
state machine and a long tail of "already initialised" flags all still describe
the run that just ended, and most now point at freed memory. A second `Open`
gets partway through D_DoomMain and then produces no frame, which from outside
is indistinguishable from a WAD that failed to load. It is refused, with the
remedy in the message.

**Two instances of one loaded image share one game.** `dlopen` keys on path, so
opening a library that is already loaded returns the *same* image with a bumped
refcount rather than a second set of globals. Two layers would share one player
and one framebuffer. Every instance therefore stages its own uniquely-named
copy of the library and opens that — the same conclusion cartridge reached
about libretro cores, for the same reason. It also happens to be the answer to
the previous trap: changing WAD throws the image away and takes a fresh one.

**A pthread gets 512 KiB where the main thread gets 8 MiB**, and Doom was
written for the latter — `R_RenderBSPNode` recurses through the map's BSP tree.
The failure is a SIGBUS on the *first instruction* of whatever function
overran, with a stack too far gone for the debugger to unwind past frame 0: a
backtrace that points at an innocent leaf and says nothing about stacks.

**Any buffer that is not 320x200 must run at view size 11.** `R_FillBackScreen`
skips the view border only when `scaledviewwidth == SCREENWIDTH`, and the
default view size of 10 makes the view 320 wide *whatever the buffer is* — so at
320 the test passes by coincidence, and at any other width the border is drawn
around a view that was inset horizontally but not vertically. The first border
patch lands at y=-3 and `V_DrawPatch`'s bounds check turns it into an `I_Error`
*before the first frame is published*:

    Bad V_DrawPatch x=53 y=-3 patch.width=8 patch.height=3

From outside: the title screen renders, and the layer goes black the moment the
game enters a level and stays black, reason only in the log. `EngineImpl.c`
forces `screenblocks = 11` for any non-classic geometry, before
`doomgeneric_Create` — which renders a frame of its own before returning, so
setting it afterwards is too late.

**That alone is not enough, because the operator can undo it.** Doom's Options
menu has a Screen Size slider, and the mapped Menu, Confirm and Turn controls
reach it; one notch down re-enters the crash mid-show. `patches/0002` clamps
inside `R_SetViewSize`, the one function every route to a new view size passes
through. The slider still moves; the view does not.

**A wider buffer alone is Vert−, not widescreen.** Doom's horizontal field of
view is fixed at 90 degrees and its scale is derived from half the *actual* view
width (`projection = centerxfrac`), so 426 columns show the same 90 degrees as
320 did, magnified, with the top and bottom cropped. It fills a 16:9 canvas and
shows less of the world. `patches/0001` holds the projection at the value a
320-wide view gives and derives `focallength` from it, so the extra columns
become extra *angle*: 106 degrees across at 426, vertical unchanged. The four
lines it touches are the places `centerxfrac` was being used as a **scale**;
where it is the screen **centre** — the angle mapping's anchor, sprite and
weapon positions — it is left alone, and those were already right.

**The 2D layer is 320-wide art in 320-wide coordinates, and a blanket shift
breaks it.** `patches/0004` does what Crispy Doom does: `V_DrawPatch` adds
`DELTAWIDTH = (SCREENWIDTH - 320) / 2` and every caller must speak 320-space.
Most already did. Eleven did not — they centred on the real `SCREENWIDTH`
(intermission titles, stats, par time, the finale's END text), so the shift
would have landed each one 53px off-centre — and several bounds checks
(`M_WriteText`, `F_TextWrite`, hu_lib) would have let a long line of text run
into `V_DrawPatch`'s own range check and an `I_Error`. Those moved to
`ORIGWIDTH`. Three callers were already in *screen* space (the pause graphic,
the automap marks, the view border) and subtract the shift. Map every draw
call before adding a patch that moves them.

**A full-screen page must black the strips beside it**, or they keep whatever
was drawn last — after the first attract demo, slices of the 3D view beside
the title page. Six places draw a page, and the first survey found four: the
help screens are drawn from `m_menu.c` with `V_DrawPatchDirect`. So
`V_DrawPatch` recognises a page instead — a 320x200 patch at the origin is one
by definition — and clears first. The test that proves it is the one page
drawn over live content: `resotest --warp 1 1 --keys 27,175,175,175,175,13`,
Read This! over E1M1, whose sides must be black and not the level.

`wi_stuff.c`'s fake screen-sized patch is **Chocolate Doom reproducing
vanilla's MAP33 crash on purpose**. It is left alone, and still crashes.

**One engine per aspect, because the width cannot be changed at runtime.**
`SCREENWIDTH` sizes static arrays all through the renderer, so an engine is
compiled for one width. The plugin ships four — 320, 384, 426, 568 — and the
Aspect parameter picks one; Auto takes the closest to the canvas by the log of
the ratio, which treats twice as wide and half as wide as equally far. The
list lives twice, `RESODOOM_ENGINE_WIDTHS` and `kEngineAspects`, and
`verify.sh` checks every engine reaches the bundle. Two decisions that look
arbitrary and are not:

- **Auto ignores Pixel Aspect.** It matches the shape Doom is meant to be seen
  at, so turning Pixel Aspect off never swaps the engine — which would restart
  the game underneath the operator.
- **Only a change of ENGINE reloads.** Resolume re-sends every value on
  composition load and on undo; picking the aspect already running must do
  nothing. The plugin remembers what it *chose*, not what loaded — if the 426
  engine is missing and 320 stood in, comparing against 320 would reload on
  every re-sent value and fall back identically each time. **And the request
  is SET OR CLEARED, in its own flag** (`mPendingAspectLoad`): a MIDI knob
  swept through 4:3 and back before the next frame must withdraw the reload
  its first step asked for. Folded into `mPendingLoad` it could not — that
  flag also carries WAD, Skill and re-init requests — and the game restarted
  for a choice that ended where it began. CodeRabbit caught it on #3; the
  check for it failed first, then passed with the fix.

**Resolume addresses parameters by NAME, so every name must be unique and
must never change.** A saved composition stores `<Param name="Scaling" …>`,
and a MIDI, keyboard or OSC mapping targets
`…/video/source/swresodoom/scaling` — lower case, spaces removed, from the 16
characters FFGL hands the host (seen in Arena 7.27.1, 2026-09-23). 0.2.0
shipped the Sprint control named **"Run"**, like the pause switch, and Arena
treated the two as one: one OSC address, selecting either selected both, and
on reopening a composition both saved values landed on the pause switch — a
paused layer came back running. `resogl` now reduces every name to its address
and fails on a clash; it failed on 0.2.0. A rename is not free either, since
compositions and mappings hold the old name, which is why Sprint moved and not
the pause switch: whatever 0.2.0 saved as "Run" still lands on Run.

New parameters still go at the end — Aspect sits after the twelve controls, not
beside Scaling — because FFGL's own ABI is by index and not every host is
Resolume. For Resolume alone the position would not matter.

**A GL re-initialisation used to leave the layer black.** `DeInitGL` closes the
engine, and nothing reopened it: the host re-sends the same WAD path, so
`SetTextParameter` saw no change. `InitGL` now requests a load whenever a WAD
is already chosen. That viewport is the ONLY size the plugin reads,
deliberately: a host renders the same instance at other sizes for previews and
thumbnails, and following the size bound at draw time would swap engines and
restart the game mid-show.

**It is the size of the clip, not of the composition.** Arena gives each clip
a Width and Height when it is created — saved with the clip — and resizing the
composition changes neither: it re-initialises nothing and fits the old size
into the new frame with the clip's Resize mode (Fill by default, so a 16:9
clip in a square composition is centre-cropped). Auto therefore matches the
composition a clip was added to. A copy keeps its original's size; only a clip
added after the resize picks again. Checked by resizing 1920×1080 to
1080×1080: the running layer and a pasted copy stayed on the 16:9 engine, a
fresh clip chose 4:3.

**`-include` is processed before the file's first line.** The hook header is
force-included into every translation unit including the one that *implements*
the hooks, so `#define ..._HOOKS_IMPL`-style guards arrive too late and the
include guard then makes that file's own `#include` a no-op. Every hook calls
itself. `EngineImpl.c` undoes the macros with explicit `#undef`s as its first
real act; that is not tidiness, it is the fix.

**Interception reaches names, not function bodies — which is why `patches/`
exists.** Everything the hook header does is redefine something upstream
*refers to*: `exit`, the allocator, `SCREENWIDTH` (by including `i_video.h`
first to trip its include guard, then redefining). An edit *inside* an upstream
function — the widescreen projection is four such lines — cannot be expressed
that way, so it is carried as a patch applied to a **copy** in the build tree.
The submodule stays byte-identical to upstream, which is the property the
pristine rule exists for. Four silent traps in that machinery, all fixed and
all commented in `CMakeLists.txt`:

- The patch step deletes the tree it patches, so it must not run *from* it.
- **`git apply` skips any path its repository ignores** — `build/` is
  gitignored, so inside this checkout it prints "Skipped patch", exits 1, and
  looks exactly like a corrupt patch. `GIT_CEILING_DIRECTORIES` stops git
  finding a repository at all.
- **A plain `file(GLOB)` never sees a new patch.** It is evaluated once at
  configure time, so a patch added later is not a dependency and is never
  applied — the build succeeds and the change is simply absent.
  `CONFIGURE_DEPENDS` re-checks it.
- **A deleted patch stays applied.** It stops being a dependency, nothing looks
  out of date, and its changes sit in the build tree indefinitely. The patch
  *set* is written to a file that only changes when the set does, and the
  patch step depends on that too.

Patches stack in name order, so each is generated against the tree with every
earlier one applied, not against pristine upstream.

A patch must be an **identity at 320x200**. Check it the only way that means
anything — hash a warped frame from the classic build before and after:

    ./build/resotest --iwad W.wad --warp 1 1 --tics 120 --out before.ppm
    # apply the patch, rebuild
    ./build/resotest --iwad W.wad --warp 1 1 --tics 120 --out after.ppm
    shasum before.ppm after.ppm   # must match

**The X in XRGB8888 is undefined, not zero,** and Doom leaves stale bits there.
Copied through, Resolume gets a mostly-transparent layer, which against a dark
composition reads as "the plugin does nothing". Alpha is forced to 255 in the
same pass that flips the picture.

**Three conflicting ideas of row 0.** Doom's framebuffer is top-left origin; GL
textures (and FFGL) are bottom-left; PPM is top-down. The picture is upside
down in exactly one of the three places you look if any one is wrong. The
engine flips once, so a published frame goes straight into a texture, and
only the PPM writers un-flip.

**A ranged parameter cannot have a ranged default.** `SetParamInfo` clamps an
`FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange`
can only be called afterwards. Every host parameter is 0..1 and the conversions
live in `Controls.h`.

**An unhandled TEXT or FILE parameter kills the plugin on an instantiate
sweep.** `SetTextParameter` handles the two it owns and returns `FF_FAIL` for
anything else rather than falling through.

**Every `ffglex::Scoped*` binding clears to 0 on scope exit — it does not
restore.** The render path uses plain `glUseProgram` and `glBindTexture` and
puts state back by hand. **No FBO is allocated anywhere**: `FFGLFBO::Initialise`
allocates under a `ScopedTextureBinding` whose destructor clears the binding,
and `FFGLFBO::Release` leaks its colour texture. A source draws a textured quad
and needs neither.

**The plugin registers itself from a file-scope constructor.** `CFFGLPluginInfo`
is never referenced by name, so in a STATIC archive the linker may drop the
whole translation unit — giving a bundle that loads, exports `plugMain`, and
reports that it contains no plugins. `SourcePlugin.cpp` is listed directly in
the MODULE target for that reason.

**`flat`, `active`, `filter`, `input`, `output`, `sample`, `common` and `patch`
are GLSL reserved words.** A shader that will not compile surfaces only at
runtime, as a plugin that appears to do nothing. That is what Diag and `resogl`
are for.

---

## Licensing — read this before adding anything

**GPL-2.0, and that is not negotiable.** doomgeneric descends from id
Software's release of the Doom source, which is GPL. Anything linked into this
binary inherits it. That is fine for a public repo and it is why this one is
not MIT like the rest of the fleet.

**stagehand is MIT and stays MIT.** MIT may be linked into a GPL work, which is
the direction used here; the combined binary released from this repo is
GPL-2.0, and stagehand's own files keep their licence and can be reused
anywhere. The reverse does not hold, so no GPL code may be moved into it.

**`source/engine/EngineImpl.c` stays here, deliberately.** We wrote it and
could in principle offer it under any terms, but it implements doomgeneric's
callbacks, is compiled into the GPL engine and is meaningless on its own.
Labelling it MIT would be a technicality rather than an honest offer, and it
would invite somebody to think they could use it without the obligations that
actually come with it.

**No WAD, no PWAD and no game content is committed here, ever.** The plugin
loads what the operator points it at. `.gitignore` blocks `*.wad`, `*.pk3`,
`*.deh` and `wads/` for that reason, and the About text says so to the
operator.

- **Freedoom** is BSD-licensed and could legally be bundled. It is deliberately
  not: 27 MB per IWAD in a plugin bundle, and pointing at the project is better
  for everyone than shipping a stale copy of it.
- **The shareware `doom1.wad`** may be redistributed intact but not modified.
  Also not shipped, same reasoning.
- **Commercial IWADs** are the operator's own and must stay that way.
- **DOOM is a trademark of id Software LLC.** The name is used the way every
  source port uses it — descriptively, for an unaffiliated front end. The
  plugin ships no id content and claims no endorsement.

If anyone proposes bundling a WAD "just to make it easier to try", the answer
is a link to this section.

---

## Checking your work

`tools/verify.sh` runs the lot. It needs `RESODOOM_TEST_IWAD` pointing at a WAD
and **skips loudly** rather than quietly passing without one.

- **`resotest`** — the engine on the CPU, no graphics API anywhere. It loads
  the engine through the same private-copy dlopen the plugin uses, so what it
  exercises is the shipped artefact — **every** shipped artefact: `verify.sh`
  runs it once per engine with `--engine` and `--expect-width`, and the second
  is what keeps the geometry check honest. Taking an engine's word for its own
  width and asserting it back would pass one built at the wrong width; handed
  the wrong engine on purpose, the check fails, which is how it was confirmed
  to be a real one. Twenty-three checks: the failure paths, that
  the engine never runs past its budget, that the undefined alpha is forced,
  that the frame is real picture rather than one flat colour, that a second
  `Open` in one copy is refused with the remedy, and that **two cold runs are
  byte-identical** — which is the check that catches the clock drifting
  anywhere near real time.
- **`resogl`** — the **real plugin class** through the real FFGL sequence in a
  headless 4.1 core context: CGL on macOS, WGL behind a hidden window on
  Windows. The only thing that catches a shader that will not compile or a
  uniform that does not resolve. Run at **two aspects**: a sign error in the
  Fit branch is invisible whenever the picture happens to be wider than the
  frame, and a square render is the cheapest way to make the other branch
  matter. The checks that predate the Aspect parameter pin it to 4:3, so they
  test what they always did whatever canvas they run on; a third case —
  picture and frame already the same aspect, bars on neither axis — belongs to
  the wider engines. **The Aspect checks read which engine is running from
  outside**, by the shape Fit gives its picture: each engine's aspect is
  different, so the ink extents name it without reaching into the plugin.
  They cover Auto on one canvas per engine, a fixed choice overriding Auto, a
  swap on a running layer, and — the ones that matter mid-show — that
  choosing the aspect a layer already runs does *not* restart the game, nor
  does a change undone before the next frame. Those are caught the only way a
  restart shows from outside: the picture going
  back to the title page after the game had moved on. It was confirmed real by
  breaking the plugin to reload on every change and watching it fail.
  **The Fit checks measure the presented quad, not the art, and that only
  works because Doom never emits pure black.** `gammatable[0]` starts at 1, so
  palette black leaves the engine as (1,1,1) and counts as ink. On a
  widescreen title page the art ends three-quarters of the way across and the
  rest is that near-black, which is why ink reads 0.998 there — the buffer is
  edge to edge, even though the picture in it is not. `--check --out F.ppm`
  writes the exact frame the Fit checks measured. Two more things only a host
  would otherwise show: **every parameter name reduces to its own address**
  (lower case, no spaces, first 16 characters — how Resolume maps them), and
  **constructing the plugin leaves stderr alone** while opening an engine
  takes it. Both failed against 0.2.0 before they passed.
- **The symbol checks** — that each engine exports exactly one entry point and
  none of doomgeneric's internals, that the bundle exports `plugMain`, and that
  every engine was actually staged inside the bundle. A bundle missing the 320
  engine loads fine and then refuses every WAD; one missing another falls back
  to 4:3 for that aspect and says so in the log, which nobody would notice
  without this check.

**Two checks in `resogl` originally passed for the wrong reason**, and the
shape of that mistake is worth keeping in mind for anything added later:
Freedoom opens on a *static* title screen, so "Run off holds the picture still"
and "two instances differ" were both comparing identical still frames and would
have passed whatever the code did. Anything asserting about motion must first
wait for motion; anything asserting two instances differ must make them
genuinely different (they now warp to different levels), because the engine is
deterministic and two copies given the same input are *supposed* to match.

**On Windows**, both suites build under MSVC and run — first done 2026-09-22 on
winlab, which is a QEMU VM with no GPU. Its stock `opengl32.dll` is a GL 1.1
rasteriser with no shader entry points; `resogl` refuses it with a message
rather than crashing. Resolume Arena ships Mesa llvmpipe as `opengl32.dll` plus
`libgallium_wgl.dll` in its install folder, and copying those two beside
`resogl.exe` gives a real 4.1 core context. The Windows-only traps found on the
way are commented where they live: FFGL.h defines `NOUSER` before including
`windows.h` (so anything needing `CreateWindowExA` must include `windows.h`
*first*), the SDK's `boolean` collides with Doom's unless `WIN32_LEAN_AND_MEAN`,
and `<stdatomic.h>` needs `/experimental:c11atomics`.

**In Resolume Arena (7.27.1, macOS, Apple Silicon, 2026-09-23)** — the shipped
0.2.0 bundle, driven from a session through the GUI. Confirmed: it registers
as `SW Resodoom`, `RD01`, a source; the inspector shows the settings, a
collapsible **Controls** group, **Aspect** as one row of five buttons, then
**About** with the right version; Auto on 1920×1080 runs the 16:9 engine edge
to edge; 21:9 and back swap engines; **choosing 16:9 while Auto already runs
the 16:9 engine does not restart the game**, and a Restart afterwards logs
`Aspect fixed`, which proves the value really arrived; a fresh clip on a
1080×1080 composition runs 4:3, letterboxed; three layers are three games;
Menu opens Doom's menu and Run off parks the game. Found: the duplicate
"Run" name, stderr taken at the startup scan, and the clip-size behaviour —
all above, and all fixed or documented.

Two things about driving Arena from a session. **Its option rows ignore an
accessibility press**: the button lights up and the value never reaches the
plugin (buttons and checkboxes do work), so a test of Aspect needs a real
click. And a real click on a stuck event button only releases it — the plugin
acts on the rising edge, so press twice.

**In Arena on Windows (7.27.1 on winlab, Mesa llvmpipe, 2026-09-23)** — this
branch's MSVC build through the fleet's Arena gate (`plugin-bench/arena`):
**9 passed, 0 failed, 1 skipped**. It loads and registers, all 29 parameters
match name, order, type, range and default (Sprint and Run apart), the WAD
fixture loads, Doom renders, Arena's log is clean and Arena survives. **The
four engine DLLs can sit beside `Resodoom.dll` in Extra Effects**: Arena tries
each as an FFGL and a VST plugin at its scan, finds neither, and says nothing
("loaded 1 plugin(s)" and not one error line), so the layout the plugin looks
for needs no change for a Windows release. The skip is the controls pass: the
probe refuses to measure while any file control is empty, and Mod WAD is
optional. **The release's own CI-built zip passed the same gate on 2026-09-23**
(release.yml's first Windows job, a `workflow_dispatch` run before v0.2.2):
9/0/1 again, the five DLLs deployed flat. Windows releases ship from v0.2.2.
Still unverified on Windows: a real GPU, Avenue, the installer on a real
machine, MIDI/keyboard mapping. The twelve controls are marked inert in the expectation anyway —
during the attract demos any key opens Doom's menu, so a frame comparison would
pass all twelve for that one reason. The expectation stays out of
plugin-bench: its WAD fixture is a local path, and no WAD goes in a repo.

Still unconfirmed: whether a hardware controller MIDI-maps onto the twelve
controls usefully, and whether a real commercial IWAD behaves like Freedoom
and the shareware `doom1.wad` do.

---

## Things deliberately not done

- **No audio.** FFGL has no audio path — a Resolume plugin returns a texture
  and nothing else. Doom is launched with `-nosound -nomusic` rather than
  rendering samples nobody can hear. Opening our own device would produce sound
  that is not on Resolume's clock and not in Resolume's mixer, which is worse
  than silence for a show.
- **No out-of-process build.** cartridge has one because a libretro core is
  third-party code of wildly varying quality; here the engine is one known body
  of C, pinned by commit, with its `exit()` intercepted. If a WAD is ever found
  that takes Resolume down anyway, the shared-memory transport in cartridge is
  the pattern to copy.
- **No save states, no mouse look, no config UI.** Doom's own menus are
  reachable through the Menu and Confirm controls, which is enough.
- **No hosting of other engines.** Quake's software renderer would fit the same
  ABI almost exactly, and that is an argument for a sibling repo rather than a
  mode here.

Related: [cartridge](https://github.com/stoatworks-labs/cartridge) (a libretro
frontend as an FFGL source — the same shape, and where the threading, the
letterbox maths and several of these traps came from), coinop (the fleet's
other stateful generator, and where the double-render trap was found),
old-cathode (the intended downstream).
