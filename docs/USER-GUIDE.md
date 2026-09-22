# resodoom user guide

resodoom is **Doom as a live Resolume source**. It runs inside Arena or Avenue as an FFGL
generator: point it at a WAD and the game becomes a layer — composite it, key it, run it through
other effects, and MIDI-map the controls onto whatever is already on the desk.

With nothing mapped it plays Doom's own attract demos for ever, which is usually what you want
from a layer nobody is holding a controller for.

> **No game data is included, and none ever will be.** You supply your own WAD, exactly as you
> would with any other source port. [Freedoom](https://freedoom.github.io/) is free, complete and
> what this was developed against.

> **This plugin is GPL-2.0, not MIT like the rest of the Stoatworks plugins.** It ships an engine
> descended from id Software's own release of the Doom source, and anything linked into that
> binary inherits its licence. If you are here to reuse code rather than to use the plugin, the
> parts that are not about Doom live in [stagehand](https://github.com/stoatworks-labs/stagehand),
> which is MIT.

> **Before you rely on this:** the macOS build is verified end to end **headlessly** — the engine
> against a real WAD with no graphics API, and the real plugin through the real FFGL sequence in a
> headless GL context, at two aspect ratios. **It has not been driven inside Resolume yet**, so how
> the parameter groups land in the inspector and whether a controller MIDI-maps onto the controls
> usefully are both unconfirmed. The **Windows and Linux branches have never been built**, and no
> binary is published for either.
>
> This codebase was created with AI assistance, directed and reviewed by a human author.

---

## Getting a WAD

A WAD is Doom's data file: the levels, the sprites, the sounds, the lot. The plugin has none and
loads whatever you point it at.

- **[Freedoom](https://freedoom.github.io/)** is a complete free replacement, BSD-licensed, and
  the two phases are two full games. Download it, unzip it, and you have `freedoom1.wad` and
  `freedoom2.wad`.
- **The shareware `doom1.wad`** is the original first episode and may be redistributed intact.
- **A commercial IWAD** you own — `doom.wad`, `doom2.wad`, `tnt.wad`, `plutonia.wad` — works, and
  stays yours.

Put it anywhere you like. The plugin gives you a real file picker.

## Running it

1. Drop **SW Resodoom** on a layer.
2. Set **WAD** to your `.wad` file.

That is the whole setup. The game starts, and with nothing mapped it runs its attract demos.

**Mod WAD** loads a PWAD on top, which is how community levels and total conversions work. Some
need a specific IWAD; the plugin passes it straight through to the engine, so the rules are the
engine's rules.

## Picking a level

**Episode** and **Map** both default to *Title / Demos*, which boots to the title screen and its
attract demos. Set both and the game warps straight into that level instead, which is what you
want when you have found a map that looks good under a key.

**Skill** sets the difficulty. It only matters once something is actually playing the game: on
Nightmare the monsters respawn, which on a layer left running is either a problem or the point.

Changing any of these reloads the game from scratch, because Doom takes them on its command line
and there is no way to warp a running engine without going through its menus. **Restart** does the
same thing deliberately.

## Making it look right

**Aspect** decides the shape of the game itself, and on a widescreen composition it is the setting
that matters most.

| | |
| --- | --- |
| **Auto** | Matches the composition. The default. |
| **4:3** | The original 320x200 picture. |
| **16:10** | Wider. |
| **16:9** | Widescreen, for the usual HD and 4K outputs. |
| **21:9** | Ultrawide, for LED strips and wide screens. |

Anything wider than 4:3 is true widescreen: the view keeps its height and shows more of the level to
either side, rather than stretching the 4:3 picture or cropping it. Menus, the title screen and the
intermissions are the original artwork, centred with black either side.

Doom's picture width is fixed when the engine is built, so the plugin carries one engine per aspect,
and **changing Aspect restarts the game** -- but only when it needs a different engine. Choosing
the one already running does nothing, and nor does Resolume re-sending the value when a
composition loads. Auto follows the composition's resolution; changing that restarts the game too
if the new shape wants a different engine.

**Scaling** decides how the picture then meets your composition.

| | |
| --- | --- |
| **Fit** | The whole picture, letterboxed or pillarboxed if its shape differs. The default. With Aspect on Auto there is usually nothing to box. |
| **Fill** | Covers the frame, cropping the overhang. |
| **Stretch** | Ignores the aspect entirely. |
| **Integer** | Whole-number pixel multiples, so one Doom pixel is an exact square block. Falls back to Fit if the frame is too small. |

**Pixel Aspect** is on by default and should usually stay on. Doom's 320x200 was always displayed
at 4:3, so its pixels are 1.2 times taller than they are wide. Turn it off and you get the raw
buffer, with every face in the game noticeably squashed.

**Smoothing** is off by default and should usually stay off. Linear filtering on a 320-pixel-wide
picture blown up to a 4K output is a blurred mess, and large hard-edged pixels are the entire
reason this content looks like itself.

## Speed, pause and the clock

**Speed** is exponential and centred: mid-travel is exactly 1.0x, and the bottom of the fader is a
hard stop rather than a very slow game.

**Run** off parks the game exactly where it stands, and costs nothing while it is off — the engine
blocks rather than spinning.

Both work because the plugin hands Doom its clock rather than letting it read a real one. A
practical consequence: if the composition stalls or the layer is not drawn for a while, the game
does not bank that time and then fast-forward through it. It simply missed it, which is what you
want in a show.

## The controls

FFGL gives a plugin **no keyboard access at all**. There is no key path in the API, so the twelve
controls are plain boolean parameters — which turns out to be the more useful shape anyway:
Resolume MIDI-maps them, keyboard-maps them and automates them off the timeline exactly like any
other parameter.

So a controller becomes a Doom pad with no special support, and a playthrough can be keyframed as
a visual.

| Control | What it does |
| --- | --- |
| **Forward**, **Back** | Walk |
| **Turn Left**, **Turn Right** | Turn |
| **Strafe Left**, **Strafe Right** | Sidestep |
| **Fire** | Fire |
| **Use** | Open doors, press switches |
| **Run** | Hold to move faster |
| **Menu** | Escape — opens Doom's own menu |
| **Confirm** | Enter — chooses in that menu |
| **Automap** | Tab |

**Menu** and **Confirm** are how you reach everything this plugin does not expose: saving,
loading, changing a weapon binding, and so on. Doom's own menus are still in there.

## Two layers, two games

Each instance loads its own private copy of the engine, so two layers are two independent games
rather than two views of one. Different WADs, different levels, different players.

## What it deliberately does not do

- **No sound.** FFGL has no audio path — a Resolume plugin returns a texture and nothing else. The
  game is launched with sound and music off rather than rendering samples nobody can hear.
- **No mouse look**, and no save-state buttons. Doom's own menus handle saving.
- **No bundled WAD**, including Freedoom, which legally could be bundled. A link to the project
  beats a stale copy of it, and 27 MB per IWAD inside a plugin bundle is not a kindness.

## If the layer stays black

Every interesting failure here looks the same from outside, so there is a log:

    ~/Library/Logs/Resodoom/resodoom.YYYY-MM-DD.log

It carries the plugin's own account **and the engine's**, which is usually the one that names the
real problem — a line like `IWAD file '...' not found!` comes straight from Doom.

The common causes, in order:

1. **No WAD selected**, or the file moved since the composition was saved.
2. **The WAD is not one Doom accepts.** A PWAD selected as the main **WAD** will not work; it goes
   in **Mod WAD** with a real IWAD above it.
3. **A Mod WAD that needs a different IWAD.** The log carries the engine's complaint.
4. **The game is paused** — check **Run**, and check **Speed** is not at the bottom of its travel.

## Licensing

This plugin is **GPL-2.0**. The source is at
[github.com/stoatworks-labs/resodoom](https://github.com/stoatworks-labs/resodoom), and the
licence obliges us to keep it that way.

DOOM is a trademark of id Software LLC. This is an unaffiliated front end for a free source port
and ships no id Software content. What you play, you supply.
