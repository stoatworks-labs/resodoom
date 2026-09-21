# resodoom

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The macOS build is
> verified end to end **headlessly**: the engine against a real WAD with no
> graphics API, and the real plugin class through the real FFGL sequence in a
> headless GL context, at two aspect ratios. **It has not been loaded into
> Resolume yet**, so how the parameter groups land in the inspector and whether
> a controller MIDI-maps onto the controls usefully are both unconfirmed. The
> Windows and Linux branches have never been built.

**Doom as a live Resolume source.**

Point it at a WAD and the game becomes a layer: composite it, key it, run it
through other effects, and MIDI-map the controls onto whatever is already on
the desk.

With nothing mapped it plays Doom's own attract demos forever, which is usually
what you want from a layer nobody is holding a controller for.

> **No game data is shipped with this plugin, and none ever will be.** You
> supply your own WAD, exactly as you would with any other source port.
> [Freedoom](https://freedoom.github.io/) is free, complete, BSD-licensed and
> what this was developed against.

---

<!-- downloads:start -->

## Download

**[v0.1.0](https://github.com/stoatworks-labs/resodoom/releases/tag/v0.1.0)** — prebuilt for macOS. Pick your platform:

<details>
<summary><b>macOS</b> — Universal (Apple Silicon + Intel)</summary>

| Build | Download | Size |
| --- | --- | --- |
| Universal (Apple Silicon + Intel) · .dmg disk image | [`resodoom-0.1.0-macos-universal.dmg`](https://github.com/stoatworks-labs/resodoom/releases/download/v0.1.0/resodoom-0.1.0-macos-universal.dmg) | 751 KB |
| Universal (Apple Silicon + Intel) · .zip archive | [`resodoom-macos-universal.zip`](https://github.com/stoatworks-labs/resodoom/releases/latest/download/resodoom-macos-universal.zip) | 693 KB |

</details>

All builds, checksums and release notes: [github.com/stoatworks-labs/resodoom/releases](https://github.com/stoatworks-labs/resodoom/releases).

macOS builds are signed and notarised by Apple, so they open normally — no Gatekeeper warning and no quarantine step.

<!-- downloads:end -->

## Building

```bash
git clone --recursive https://github.com/stoatworks-labs/resodoom
cd resodoom
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build      # drops the bundle into Resolume's plugin folder
```

`--recursive` matters: the FFGL SDK, doomgeneric and stagehand are all
submodules, and the build stops with instructions if any is missing.

## Using it

Drop `SW Resodoom` on a layer and set **WAD** to a `.wad` file. That is the
whole setup.

| Parameter | What it does |
| --- | --- |
| **WAD** | The game data. Required. |
| **Mod WAD** | An optional PWAD loaded on top. |
| **Run** | Off parks the game exactly where it stands, at no CPU cost. |
| **Restart** | Reloads from scratch. |
| **Speed** | Exponential, centred: mid-travel is 1.0x, the bottom is a stop. |
| **Skill** | Doom's five difficulties, or leave its own default. |
| **Episode** / **Map** | Warp straight into a level. Both at *Title / Demos* boots to the title screen and its attract demos. |
| **Scaling** | Fit, Fill, Stretch, or Integer (whole-number pixel multiples). |
| **Pixel Aspect** | On by default. Doom's 320x200 was always shown at 4:3. |
| **Smoothing** | Off by default, and it should stay off. |

The twelve **Controls** are plain boolean parameters, so Resolume MIDI-maps
them, keyboard-maps them and automates them off the timeline like anything
else. FFGL gives a plugin no keyboard access at all, so this is not a
convenience — it is the only input path that exists.

## Two layers, two games

Each instance loads its own private copy of the engine, so two layers are two
independent games rather than two views of one. `resogl` proves it by warping
two instances to different levels and comparing the pixels.

## Verifying

```bash
RESODOOM_TEST_IWAD=~/wads/freedoom1.wad tools/verify.sh
```

Three layers that fail for different reasons: the engine on the CPU
(`resotest`), the real plugin class in a headless GL context (`resogl`), and
the same again at a square aspect, because a sign error in the letterbox
branch is invisible whenever the picture happens to be the wider one.

## Licence

> ### This repo is GPL-2.0, not MIT.
>
> Every other Stoatworks plugin is MIT. This one is not, and it cannot be: it
> ships [doomgeneric](https://github.com/ozkl/doomgeneric), which descends from
> id Software's own release of the Doom source, and that is GPL. Anything
> linked into this binary inherits it.
>
> **If you are here to reuse code, you probably want
> [stagehand](https://github.com/stoatworks-labs/stagehand) instead** — the MIT
> half, split out for exactly this reason.

See [LICENSE](LICENSE) for the full text.

doomgeneric is a pristine submodule: everything this project changes about it
is done by force-including one header, so the submodule's `git diff` stays
empty and updating upstream is a version bump rather than a merge.

DOOM is a trademark of id Software LLC. This project is an unaffiliated FFGL
front end for a free source port, and ships no id Software content.

## The MIT half

The parts of this that are not about Doom live in a separate repo,
[stagehand](https://github.com/stoatworks-labs/stagehand), under **MIT**:
loading a private copy of a source library, paying out its clock against
elapsed real time, and presenting its frames as a letterboxed quad. It depends
on OpenGL and `libdl` and nothing else — not even on FFGL — and is useful
without Doom anywhere near it.

That split is real rather than cosmetic: this repo consumes stagehand as a
submodule and the engine here implements stagehand's generic source ABI, so
the boundary is compiled and tested rather than asserted. MIT code may be
linked into a GPL work, which is this direction; the combined binary released
here is GPL-2.0, and stagehand's files stay MIT and reusable anywhere.

What is **not** in stagehand, deliberately: `source/engine/EngineImpl.c`, the
platform layer that implements doomgeneric's callbacks. We wrote it, but it is
compiled into the GPL engine and is meaningless outside it, so calling it MIT
would be a technicality rather than an honest offer.
