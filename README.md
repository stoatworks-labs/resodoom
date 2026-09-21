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

## Building

```bash
git clone --recursive https://github.com/stoatworks-labs/resodoom
cd resodoom
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build      # drops the bundle into Resolume's plugin folder
```

`--recursive` matters: the FFGL SDK and doomgeneric are both submodules, and
the build stops with instructions if they are missing.

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

**GPL-2.0**, because it links [doomgeneric](https://github.com/ozkl/doomgeneric),
which descends from id Software's own release of the Doom source. See
[LICENSE](LICENSE). Upstream is a pristine submodule — everything this project
changes about it is done by force-including one header, so the submodule's
`git diff` stays empty.

DOOM is a trademark of id Software LLC. This project is an unaffiliated FFGL
front end for a free source port, and ships no id Software content.
