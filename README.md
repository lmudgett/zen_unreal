# Unreal (v200, May 1998) — Modern x64 Port

A working 64-bit port of the Unreal Engine v200 codebase (retail Unreal, May 1998) that builds with modern toolchains, runs the full game, and is partway through a cross-platform (SDL3) migration with an in-progress ImGui-based level editor.

## Status

- **Game runs**: menus, input, audio, fullscreen, high resolutions. CD-free. Frame pacing fixed (~200 fps cap, configurable via `FrameRateLimit`).
- **UnrealScript compiler works**: `ucc -make` and `-remake=<Package>` rebuild `.u` packages from source; `Core.u` and `UnrealI.u` ship rebuilt.
- **Modernized bot AI**: perception/memory, utility-based decision scoring, per-personality skill axes, map economy (pickup respawn clocks, team intel). Toggle with `bModernAI`; verified in headless botmatches including TeamGame.
- **Cross-platform migration** (C++ / SDL3): portable Core (POSIX branches in place), SDL3 windowing + input (`SDLDrv`), SDL3 audio in Galaxy, and a portable SDL launcher. Windows builds default to the original WinDrv path; configure with `-DUNREAL_USE_SDL=ON` to build the SDL client on Windows. See `docs/CROSSPLATFORM.md`.
- **ImGui editor** (`src/EdGui`, SDL build): quad viewports (textured 3D + ortho wireframes), reflection-based property editing, class/texture browsers, brush builders + CSG, surface properties panel, hit-tested right-click context menus.

## Layout

| Path | Purpose |
|---|---|
| `src/` | All ported engine source — the only tree that gets edited |
| `Unreal - v200/`, `legacy/` | Read-only reference baselines; never modified |
| `System/` | Runtime output: binaries, `.u` packages, `Unreal.ini` |
| `content/` | Game content (maps, textures, sounds, music) |
| `scripts/` | Test and packaging automation (PowerShell) |
| `docs/` | `PORTING.md`, `CROSSPLATFORM.md` |
| `dist/` | Distributable build produced by `build-dist.bat` |

Engine modules under `src/`: `Core`, `Engine`, `Render`, `Fire`, `Galaxy` (audio), `IpDrv` (networking), `OpenGLDrv`, `WinDrv` / `SDLDrv` (windowing), `Window`, `Editor`, `EdGui` (ImGui editor UI), `UnrealI` (game script sources), `Launch`, `ThirdParty`.

## Building

Requirements: CMake ≥ 3.21, 64-bit toolchain. Windows: Visual Studio 2022 (MSVC v143) + Windows SDK 10. The SDL build fetches SDL3 (and Dear ImGui for the editor) automatically via CMake FetchContent.

```powershell
# Default Windows build (WinDrv, unchanged classic path)
cmake -B build
cmake --build build --config Release

# SDL3 build (cross-platform path; required for the ImGui editor)
cmake -B build-sdl -DUNREAL_USE_SDL=ON
cmake --build build-sdl --config Release
```

Binaries land in `System/` next to `Unreal.ini`, as the engine expects. Note the two build trees write different driver DLLs into the same `System/` directory — rebuild the tree you intend to run.

## Running & testing

```powershell
System\Unreal.exe                 # play the game
System\Unreal.exe -selftest       # built-in test suite (32 checks: core, package loads, WAV parsing)
System\ucc.exe -make              # rebuild UnrealScript packages
scripts\selftest.ps1              # full 5-stage suite: selftest, compiler, headless bots, renderer, audio meter
scripts\botmatch-test.ps1         # headless dedicated-server botmatch with kill/score log parsing
build-dist.bat                    # build the shippable dist/ (validates before overwriting)
```

Useful diagnostic flags: `-framestats` (frame pacing), `-glcounts` (renderer surf/poly counters).

## Troubleshooting

- **Antivirus quarantine**: some AV products (Norton in particular) kill fresh unsigned executables on their first socket open and blacklist the filename. Add the repo directory to your AV exclusions before first run.
- **Black 3D view in the editor**: usually camera-inside-solid on a fresh level — use `JUMPTO` to a real location; `-glcounts` showing `surfs=0` confirms it.

## Porting notes

The port targets 64-bit only. Inline x86 assembly and 3DNow! paths are disabled (`ASM=0`, `NOAMD3D`) in favor of the original C++ fallbacks. C++14 with permissive flags keeps the 1998 idioms (old `for`-scope, `register`) compiling. See `docs/PORTING.md` for the full history and `docs/CROSSPLATFORM.md` for the migration plan and per-phase status.
