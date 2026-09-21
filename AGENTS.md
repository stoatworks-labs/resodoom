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

**`-include` is processed before the file's first line.** The hook header is
force-included into every translation unit including the one that *implements*
the hooks, so `#define ..._HOOKS_IMPL`-style guards arrive too late and the
include guard then makes that file's own `#include` a no-op. Every hook calls
itself. `EngineImpl.c` undoes the macros with explicit `#undef`s as its first
real act; that is not tidiness, it is the fix.

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
  exercises is the shipped artefact. Twenty-three checks: the failure paths, that
  the engine never runs past its budget, that the undefined alpha is forced,
  that the frame is real picture rather than one flat colour, that a second
  `Open` in one copy is refused with the remedy, and that **two cold runs are
  byte-identical** — which is the check that catches the clock drifting
  anywhere near real time.
- **`resogl`** — the **real plugin class** through the real FFGL sequence in a
  headless CGL 4.1 core context. The only thing that catches a shader that will
  not compile or a uniform that does not resolve. Run at **two aspects**: a
  sign error in the Fit branch is invisible whenever the picture happens to be
  wider than the frame, and a square render is the cheapest way to make the
  other branch matter.
- **The symbol checks** — that the engine exports exactly one entry point and
  none of doomgeneric's internals, that the bundle exports `plugMain`, and that
  the engine was actually staged inside the bundle. A bundle missing the engine
  loads fine and then refuses every WAD.

**Two checks in `resogl` originally passed for the wrong reason**, and the
shape of that mistake is worth keeping in mind for anything added later:
Freedoom opens on a *static* title screen, so "Run off holds the picture still"
and "two instances differ" were both comparing identical still frames and would
have passed whatever the code did. Anything asserting about motion must first
wait for motion; anything asserting two instances differ must make them
genuinely different (they now warp to different levels), because the engine is
deterministic and two copies given the same input are *supposed* to match.

**Host verification is Allan's, not an agent's.** Driving the Resolume GUI from
a session is unreliable. **Nothing here has been loaded into Resolume.** The
three things most worth checking first: how the parameter groups land in the
inspector, whether a controller MIDI-maps onto the twelve controls usefully,
and whether a real commercial IWAD behaves like Freedoom does.

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
