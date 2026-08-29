# ZenUnreal — Unreal (v200, May 1998) Modern x64 Port

A working 64-bit port of the Unreal Engine v200 codebase (retail Unreal, May 1998) that builds with modern toolchains, runs the full game, and is partway through a cross-platform (SDL3) migration with an in-progress Dear ImGui level editor. The shipped launcher is **`ZenUnreal.exe`**.

> **Note**: this repository contains retail game content and original reference source as baselines — it must stay **private**. The retail CD image is excluded (`Unreal - v200/CD/` is local-only; GitHub's 100 MB file limit wouldn't allow it anyway).

## What works

### Game
- Full game runs: menus, input, audio, fullscreen, arbitrary resolutions, CD-free.
- Frame pacing fixed for modern hardware: `glFinish`-based pacing, VSync off by default, configurable `FrameRateLimit` (200 fps default). The cap matters — at ~1000 fps the original 1998 float physics breaks down (NaN guards added in `physWalking` regardless).
- Audio restored faithfully: ambient light-flicker modulation, voice start-order fix (torches were 2–5× too loud in naive ports).

### Renderer & gameplay fidelity
The port started out rendering a visibly different game from the 1998 original. These are the corrections, each traced back to what the retail Glide/software renderers actually did:
- **Light map brightness** — v200 light maps are *quarter*-bright (64 = "texture as painted", 127 = 2× overbright). Glide reached 4× via a 2× upload scale *and* a 2× modulate blend; this driver only had the blend, so every lit surface drew at half brightness and dim rooms went to exact black. The missing 2× is now baked in at upload.
- **Movers with no baked lighting** — a mover lights from its own brush's raytraced data. Stairs raytraced while sunk in the floor ship with no light data at all and drew solid black once risen; such brushes now take the leaf-permeating-light path the engine already uses for `bDynamicLightMover`.
- **Water & liquids** — translucent wavy/auto-panning surfaces with glints; standing sheets shaded as waterfalls (strands, gaps, foot froth); underwater `EndFlash` warp. Slime gets a per-pixel GLSL warp shader (per-vertex tessellation was tried first and strobed on mip flicker).
- **Sky** — cloud churn: sky sheets auto-pan and warp at 1/8 game time. Sky frames are detected via the frame zone's `ASkyZoneInfo`, and sky surfaces are binned by normal rather than centroid.
- **Effect sprites** — the near-black dither floor is gated out of translucent textures (bilinear + additive stacking turned it into bright rectangles over rocket smoke), small mips sample mip 0 only, and modulated textures are excluded from the brightness gamma bake — that last one was the hard-edged pale square at NyLeve's waterfall.
- **Coronas** — lens flares occlude against real geometry via a `GetPixelDepth` depth readback instead of always drawing on top.
- **Doors** — `TriggerToggle` doors no longer open and shut again in a single approach above ~60 fps (`BeginTouch` used a box extent while `UnTouch` used a cylinder, so a grazing approach fired `Touch` twice).
- **Slope sliding** — the player stands still on ordinary walkable slopes; sliding now requires a low `ZoneGroundFriction` (the ice knob), as intended.

### UnrealScript compiler
- `ucc -make` rebuilds all script packages from source; `ucc -remake=<Package>` does a code-only rebuild of one package.
- `Core.u` and `UnrealI.u` ship rebuilt from source. Import-resolution work brought `-remake` warnings from 1756 down to 2.

### Modernized bot AI (`bModernAI` toggle in `Bots.uc`)
Four phases, all verified in headless botmatches (including TeamGame):
1. **Perception & memory** — bots track beliefs about enemies instead of omniscient queries.
2. **Utility scorer** — weighted decision scoring replaces the original ad-hoc state jumps.
3. **Skill axes & personalities** — per-bot aim/reaction/aggression axes.
4. **Map economy** — key-item respawn clocks, timed pickup contention, team intel sharing via `CallForHelp`.

### Cross-platform migration (C++ / SDL3)
Staged migration off Win32; Windows classic build stays untouched throughout:
- **Core** is portable: the entire Win32 surface (`UnPlat.cpp`, `_findfirst`) has `#if _WIN32 / #else` POSIX branches (dlopen, glob, chrono timing, etc.). Windows output stayed byte-identical.
- **SDLDrv**: SDL3 windowing/input backend (`USDLClient`/`USDLViewport`), with an SDL GL-context path in OpenGLDrv. SDL3 is fetched automatically via CMake FetchContent.
- **Galaxy audio on SDL3**: per-voice `SDL_AudioStream` callbacks, music via libxmp callback — no feeder thread. Verified against the XAudio2 path with a peak meter.
- **Portable launcher**: plain `main()` in `SDLLaunch.cpp`, log to stdout. All four run paths verified: `-selftest`, `ucc -make`, headless `-server` botmatch, and rendered client (1280x720, GL 4.6, ~5 ms frames, audio).

### ImGui editor (SDL build)
Eight milestones done, each verified interactively:
- Boots, loads maps; docking UI with menu/log/console panels.
- Reflection-based **Properties panel** (multi-edit via `ExportText`/`ImportText`).
- **Quad viewports** — 4 SDL windows (textured 3D + gridded ortho wireframes) with per-window input routing.
- **Class browser**, right-click **context menus** (actor add at click location, actor/poly ops), map open dialog.
- Renderer-level **hit testing** (the x64 port originally had empty stubs — clicks would have parsed garbage), including line-proximity picking in ortho views.
- **Texture browser** (in-engine browser camera, wheel scroll, click-select).
- **Brush builders + CSG** — Cube/Sheet/Cylinder via T3D, Add/Subtract/Intersect; builds a playable room from scratch.
- **Surface properties panel** — tri-state flag checkboxes, pan/scale/rotate/align, all transaction-bracketed.

## Layout

| Path | Purpose |
|---|---|
| `src/` | All ported engine source — the only tree that gets edited |
| `Unreal - v200/`, `legacy/` | Read-only reference baselines; never modified |
| `System/` | Runtime output: binaries, `.u` packages, `*.ini` |
| `content/` | Game content (maps, textures, sounds, music) |
| `scripts/` | Test automation (PowerShell) |
| `docs/` | `PORTING.md` (x64 port history), `CROSSPLATFORM.md` (migration log) |
| `dist/` | Self-contained distributable produced by `build-dist.bat` (untracked) |

Engine modules under `src/`: `Core`, `Engine`, `Render`, `Fire`, `Galaxy` (audio), `IpDrv` (networking), `OpenGLDrv`, `SDLDrv` (windowing), `Window`, `Editor`, `EdGui` (ImGui editor UI), `UnrealI` (game script sources), `Launch`, `ThirdParty` (libxmp-lite). The DirectX-based `WinDrv` client is retired to `legacy/` — SDL is the only windowing backend, and DirectX is fully removed from the port.

## Building

Requirements: CMake ≥ 3.21, 64-bit toolchain. Windows: Visual Studio 2022 (MSVC v143) + Windows SDK 10. SDL3 and Dear ImGui are fetched automatically by CMake.

```powershell
# SDL3 build (the only build flavor — SDL is the sole windowing backend)
cmake -S . -B build-sdl
cmake --build build-sdl --config Release

# One-shot: build SDL modules, link ZenUnreal.exe, validate, stage dist\, selftest
build-dist.bat
```

Notes:
- Binaries land in `System/` next to the ini files, as the engine expects.
- The launcher links under the name `ZenUnreal` via `-p:TargetName=ZenUnreal -p:BuildProjectReferences=false` (see `build-dist.bat`); the exe base name selects `<name>.ini` / `<name>.log`, so the config file is `System\ZenUnreal.ini`.
- `build-dist.bat` validates the source tree (non-truncated ini, all 6 `.u` packages) **before** overwriting an existing `dist\`, and finishes by running the selftest from inside the dist.

## Running & testing

```powershell
System\ZenUnreal.exe              # play the game (SDL build)
System\ZenUnreal.exe -selftest    # built-in suite: 32 checks (core layout pins, all 146 packages load, WAV parse)
System\ucc.exe -make              # rebuild UnrealScript packages
scripts\selftest.ps1              # full 5-stage suite: selftest, compiler, headless bots, renderer, audio meter
scripts\botmatch-test.ps1         # headless dedicated-server botmatch with KILL/SCORE log parsing
```

Diagnostic flags: `-framestats` (frame pacing report), `-glcounts` (renderer surf/poly counters), `-log` (tee log to terminal).

Beyond those, most fidelity fixes above shipped with a `-probe*` flag that reproduces the symptom or dumps the state it was diagnosed from — the fastest way back into a problem is usually the probe that found it. They come in two shapes: switches (`-probesurfs`, `-probeglstate`, `-probesounds`, `-probenochurn`, `-probenofall`, `-probenofogmap`, `-probeflatmod`, `-probetexdump`) and filters taking a substring (`-probelightmap=<texture>`, `-probelight=<name>`, `-probeactors[=<class>]`, `-probemesh=<name>`, `-probedoor=<name>`, `-probemusic[=<pkg.obj>]`, `-probeview=<x:y:z:pitch:yaw>`). Filters match on word boundaries. Run with `-log` to see the output live.


## Porting notes

- 64-bit only (`CMakeLists.txt` enforces it). Inline x86 assembly and 3DNow! paths are disabled (`ASM=0`, `NOAMD3D`); the original C++ fallbacks are used.
- C++14 with permissive flags keeps 1998 idioms compiling (`/Zc:forScope-` for old `for`-scope, `register`, pre-standard conversions).
- Script/C++ struct layout is fragile on x64: `UObject` field offsets are pinned and checked by the selftest (`Parent=48 Flags=56 Name=60 Class=64`), because UnrealScript-mirrored classes assume exact offsets.
- Varargs differ on x64: use `GET_VARARGS`, not `appGetVarArgs`, in ported code.
- See `docs/PORTING.md` for the full port history and `docs/CROSSPLATFORM.md` for the migration log, including verified milestones and known traps.

## Troubleshooting

- **Antivirus quarantine**: some AV products (Norton in particular) behaviorally kill freshly built unsigned exes the moment they open a network socket, quarantine the binary, and blacklist the *filename* (subsequent relinks fail with LNK1104). Add the repo directory to your AV exclusions. This is why the launcher ships under its own name with beacon/uplink `ServerActors` blanked in `ZenUnreal.ini`.
- **Black 3D view in the editor**: usually camera-inside-solid on a fresh level — `-glcounts` showing `surfs=0` confirms it; `JUMPTO` a real location.
- **`ConfigNotFound` at startup**: a crash can truncate the active ini to 0 bytes. Restore `System\ZenUnreal.ini` from git; `build-dist.bat` refuses to stage a dist from a broken tree for this reason.
