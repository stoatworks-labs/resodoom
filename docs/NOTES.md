# Notes

Working notes for this repo: status, decisions, and the traps that have
actually bitten. Written in the first person and dated by when each thing was
learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*resodoom — Doom as an FFGL source for Resolume. Built 2026-09-21. NOT RELEASED,
not yet a fleet repo, never loaded into Resolume.*

## Status, 2026-09-21

`~/dev/resodoom`, local git only — **no remote, not on GitHub, no tag**. Lives
in `~/dev` rather than `~/Projects/resolume` because it is not a fleet repo
yet, the same way occluder and songbook started.

Two commits: the engine, then the plugin. Everything below is verified
headlessly on an M4 Max against Freedoom Phase 1 0.13.0.

- `resotest` — 19 checks, 0 failures. Engine only, no graphics API.
- `resogl` — 14 checks, 0 failures, at **both** 1280x720 and 720x720.
- `tools/verify.sh` — all of the above plus symbol checks, passing.
- Universal `.bundle` built and `lipo`-confirmed x86_64 + arm64.
- **Installed** into `~/Documents/Resolume Arena/Extra Effects/Resodoom.bundle`.

**It has never been instantiated inside Resolume.** The three things worth
checking first are how the parameter groups land in the inspector, whether a
controller MIDI-maps onto the twelve controls usefully, and whether a real
commercial IWAD behaves the way Freedoom does.

Not registered on the website, not in `names.json`, no `StoatworksAbout.h`
block, no guide, no release, no demo, no video. Plugin ID **`RD01`** (free as
of the 2026-09-21 fleet sweep); display name **`SW Resodoom`** — 11 characters,
comfortably inside FFGL's unterminated 16-byte `PluginName` field.

## Why this exists when cartridge already does

cartridge hosts libretro cores, and PrBoom is a libretro core, so Doom was
*already* possible there. This earns its place by being self-contained: drop it
on a layer, pick a WAD, done — no core to download from a buildbot, no
architecture matching, no Gatekeeper quarantine dance. That is the entire
product difference and it should stay the pitch.

## The four things that cost real time

**`doomgeneric_Create` does not loop.** The name, and the fact that
`D_DoomMain` historically never returns, both say otherwise. It actually runs
D_DoomMain plus one tick and returns; every upstream reference port is
`Create(); while(1) Tick();`. I built the whole teardown path around longjmps
before reading `D_DoomLoop` and finding it ends in a single `doomgeneric_Tick()`.

**The virtual clock has exactly one correct gate, and I found the other two
first.** Both failures look identical — full startup banner, then nothing —
and neither names a clock:

- Gating in `DG_DrawFrame`: `TryRunTics` needs `I_GetTime` to move before it
  yields a tic, so a clock that moves after a draw waits for a draw that waits
  for the clock.
- Advancing on `Grant`: fixes the first tick, hangs on the second, because
  `TryRunTics` samples `entertic` on entry and its wait loop needs the clock to
  move *during* the call.

`I_Sleep` is the answer. It is where Doom itself yields.

**A pthread gets 512 KiB; Doom wants the main thread's 8 MiB.** SIGBUS on the
first instruction of `lumper_hook_malloc`, stack too corrupt for lldb to
unwind past frame 0. Nothing in that backtrace suggests "stack size".

**`-include` is processed before line 1 of the file that receives it.** So the
`#define LUMPER_HOOKS_IMPL` at the top of the hook implementation arrived
*after* the macros existed, and the include guard then turned that file's own
`#include` into a no-op — every hook called itself. This presented as the
*same* SIGBUS as the stack-size bug, at the same address, which is why I
"fixed" the stack size and saw no change. Two different bugs with one symptom.
`#undef` explicitly; do not try to be clever with guards.

## A loaded engine copy runs Doom exactly once

`Stop()` joins the thread and frees every tracked allocation, but doomgeneric's
several hundred globals still describe the finished run and mostly point at
freed memory. A second `Start` gets partway through D_DoomMain and then
produces no frame — indistinguishable from a bad WAD, and it cost three test
failures that all read "engine produced no frame for tic 0" before I worked out
they were runs *two, three and four*, not run one.

It is now refused explicitly, with "load a fresh copy" in the message, and
`resotest` asserts both the refusal and the wording.

The private-copy arrangement this forces is the same one cartridge needs for
libretro cores, and it was worth proving directly rather than assuming: a
throwaway program opened two copies and confirmed separate `api` pointers,
separate `Start` function addresses, two distinct zone allocations, and A at
tic 200 while B sat at tic 20.

## Two harness checks that passed for the wrong reason

Freedoom opens on a **static** title screen that holds for about five seconds.
Two `resogl` checks — "Run off holds the picture still" and "two instances are
at different points" — were comparing identical still frames, so both passed
whatever the code did. The first one I only noticed because the *second* one
started failing.

The fixes are worth generalising to anything added later:

- Anything asserting about motion has to **wait for motion first**
  (`AdvanceUntilMoving`), with Speed turned up so that is cheap.
- Anything asserting two instances differ has to make them **genuinely
  different**. The engine is deterministic, so two copies given the same input
  are *supposed* to render identically; they now warp to different levels,
  which cannot coincide. At 8x speed both instances had also drifted onto the
  static screen *between* attract demos, which is how the pixel comparison
  found them equal despite genuinely being separate games.

## Decisions worth not relitigating

- **GPL-2.0**, not the fleet's usual MIT. doomgeneric descends from id's
  source. Nothing to argue about.
- **No WAD ships**, including Freedoom, which legally could. 27 MB per IWAD,
  and a link to the project beats a stale copy of it.
- **No audio.** FFGL has no audio path. Doom is launched `-nosound -nomusic`
  rather than rendering samples nobody can hear.
- **No out-of-process build.** cartridge needs one because third-party cores
  vary wildly; here the engine is one pinned body of C with its `exit()`
  intercepted. If a WAD ever takes Resolume down, copy cartridge's shared
  memory transport.
- **320x200 1:1 out of the engine.** doomgeneric's own integer scaler becomes a
  straight copy and every scaling decision belongs to the plugin's shader.
- **`-nogui` is not optional.** Without it `I_Error` pops a CoreFoundation
  alert on macOS — a modal dialog, owned by Resolume, over a live output.

## Next, if this is taken further

1. Load it in Resolume and check the inspector and MIDI mapping.
2. The five fleet registration tables, a `StoatworksAbout.h` block, and a guide
   before any tag — see `fleet-first-release`.
3. A gamepad would be a better controller than twelve MIDI-mapped booleans, and
   HID needs no entitlement. It is additive, not a replacement.
4. Quake's software renderer fits this ABI almost exactly. That is an argument
   for a sibling repo, not a mode here.
