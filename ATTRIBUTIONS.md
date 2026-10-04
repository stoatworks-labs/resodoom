# Attributions

Resodoom is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### doomgeneric

<https://github.com/ozkl/doomgeneric>  
Licence: GPL

The engine. doomgeneric descends from id Software's own release of the Doom source, which is GPL, and anything linked into this binary inherits it, which is why resodoom is GPL-2.0-only when the rest of the fleet is MIT. It is a pristine submodule pinned by commit; resodoom changes it by force-including source/engine/ResodoomHooks.h and by four patches in patches/ applied to a copy in the build tree: 0001 holds the projection at its 320-wide value so a wider buffer gains angle (true Hor+ widescreen), 0002 keeps a non-320x200 view at size 11, 0003 bounds the end-of-Episode-3 bunny scroll at 320 columns, and 0004 centres the 320-wide 2D layer. Each is an identity at 320x200. Four engines, 320, 384, 426 and 568 wide, are compiled from it, and source/engine/EngineImpl.c implements its callbacks. DOOM is a trademark of id Software LLC; no id Software content ships.

### stagehand — Stoatworks stagehand

<https://github.com/stoatworks-labs/stagehand>  
Licence: MIT  
Copyright: Stoatworks Labs

The MIT half, consumed as a submodule: loading a private copy of the engine library, paying out its clock against elapsed real time, the letterbox maths, texture presentation and the diagnostics log. It was split out of this repo so the GPL engine could not swallow it; the engine implements stagehand's generic source ABI, so the boundary is compiled and tested, and stagehand's files stay MIT inside the GPL-2.0 binary.

### Threading, letterbox maths and the private-copy loader — Stoatworks cartridge

<https://github.com/stoatworks-labs/cartridge>  
Licence: MIT  
Copyright: Stoatworks Labs

cartridge, a libretro frontend as an FFGL source, is the same shape, and is where the threading, the letterbox maths and several of the traps AGENTS.md records came from. Staging a uniquely named copy of the engine for each instance is the conclusion cartridge reached about libretro cores, for the same reason.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Freedoom

<https://freedoom.github.io/>  
Licence: BSD

What resodoom was developed against and what tools/verify.sh runs on: the headless checks were made with Freedoom Phase 1 0.13.0, and the published video's footage is Freedoom, so nothing of id Software's appears. It could legally be bundled and deliberately is not; the operator supplies a WAD.

### Crispy Doom

patches/0004 centres the 2D layer the way Crispy Doom does: V_DrawPatch adds DELTAWIDTH = (SCREENWIDTH - 320) / 2, and every caller must speak 320-space.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
