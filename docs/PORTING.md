# Unreal Engine v200 (May 1998) — x64 Modernization Port

This repository contains the Unreal Engine v200 source tree ported from
Visual C++ 5.0 / 32-bit Windows to **Visual Studio 2022 (MSVC v143) on
x64**, with all rendering migrated to a new **modern OpenGL** driver.

## Repository layout

| Path        | Contents                                                        |
|-------------|-----------------------------------------------------------------|
| `src/`      | All buildable engine modules (one directory per package/DLL)    |
| `src/OpenGLDrv/` | **New** modern OpenGL renderer (replaces all legacy drivers) |
| `System/`   | Runtime directory: built binaries, `Unreal.ini`, `.int` files   |
| `content/`  | Game assets (`Textures/`, `Maps/`, `Sounds/`, `Music/`)         |
| `legacy/`   | Retired code, not built: SoftDrv, GlideDrv, SglDrv, VB editor shell, installer, original Galaxy audio wrapper, `Unreal.dsw` |
| `docs/`     | This document, original `Help/` files                           |
| `Unreal - v200/` | Pristine gzipped source archive (do not modify)            |

## Building

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Binaries land in `System/`. Run `System\Unreal.exe -log`.

Modules built: Core, Engine, Fire, Render, Window, WinDrv, IpDrv,
Galaxy (stub), OpenGLDrv, Unreal.exe (Launch). Editor and the
UnrealScript packages (UnrealI) are not yet ported.

## Key porting decisions

* **x64 only.** `ASM=0` everywhere; the engine's own C++ fallback paths are
  used instead of the Pentium/MMX/3DNow! inline assembly (`NOAMD3D` defined
  globally). Fallbacks that were missing or bit-rotted were repaired
  (`appRound`, `appSeconds`/`appCycles` via QPC/`__rdtsc`, `FMipmap::Data`
  rename in Fire's C paths).
* **Rendering: OpenGL only.** SoftDrv (software rasterizer), GlideDrv
  (3dfx) and SglDrv (PowerVR) are retired to `legacy/`. The new
  `OpenGLDrv.OpenGLRenderDevice` requests a 4.6 compatibility-profile
  context, rebuilds the engine's projection with `glFrustum` (the engine
  supplies eye-space vertices), converts P8/RGBA32 mips on upload keyed by
  the engine's 64-bit cache IDs, and implements the multi-pass surface
  pipeline (base + 2x-modulated lightmap + additive fogmap, masked surfaces
  via alpha test + depth-equal passes). `Unreal.ini` now points all render
  device settings at it.
* **Audio: stub Galaxy.** The original Galaxy library only exists as a
  32-bit VC5 static lib, so `src/Galaxy` reimplements
  `UGalaxyAudioSubsystem` with the same class name and config properties
  but silent output. Replace with OpenAL/XAudio2 later without touching
  the engine.
* **No struct packing override.** The original used `/Zp4`; x64 uses
  natural alignment. Disk serialization is field-wise so package files are
  unaffected, but native class layouts differ from 1998 script offsets —
  `.u` packages must be rebuilt from the `Classes/` sources once the
  script compiler (Editor module) is ported. Shipped-1998 `.u` files also
  embed 32-bit in-memory sizes for bytecode (`ScriptSize`), which no
  longer match 64-bit pointer-widened bytecode; loading retail content
  will need a translating loader.

## Classes of x64 bugs fixed (grep for "x64 port" comments)

1. **Pointers stored in 32-bit integers.** Autoregistration chained
   objects through the 32-bit `LinkerIndex` (now uses the `HashNext`
   pointer); `_findfirst` handles in `long`; pointer alignment math in
   `Align()` / `FMemStack::PushBytes` through `INT`/`DWORD`; assorted
   `(INT)pointer` casts (new `PTRINT`/`PTRUINT` typedefs).
2. **Varargs stack-walking.** `appGetVarArgs` read the caller's stack —
   impossible on x64. `GET_VARARGS` now expands `va_start` in the caller.
3. **Objects passed to varargs.** `FString`/`UObject` values passed to
   `%s` "worked" on x86 because the first struct member was the char
   pointer; all sites now pass `*Str` / `GetName()` (warning C4840 is
   promoted to catch regressions).
4. **UnrealScript VM.** Bytecode embeds object pointers; in memory they
   are now pointer-width (`FFrame::ReadPtr`), matching what
   `UStruct::SerializeExpr` writes at load time.
5. **Win32 API width.** Window procs use `LRESULT`/`WPARAM`/`LPARAM`,
   dialogs use `INT_PTR`, `SIZE_T` working-set APIs, `HMENU` control IDs
   via `UINT_PTR`. HWND-in-DWORD command-line plumbing kept (USER handles
   are 32-bit significant by contract).
6. **VC5-era language relics.** Duplicate `operator<<` overloads
   (UBOOL==INT, FSphere's serializer taking `FPlane&`), member-function
   pointers without `&`, default-int declarations, `for`-scope reuse
   (kept, via `/Zc:forScope-`), `__p__amblksiz` CRT tuning removed.

## Debugging aid

`src/Launch/Src/LaunchCrash.cpp` installs an unhandled-exception filter
that writes a symbolized stack trace to `System\Crash.txt` using the
build's PDBs.

## Current status

`Unreal.exe -log` boots cleanly on x64: object system, config,
localization, stub audio, Windows client, DirectDraw enumeration,
lighting subsystem and the render device all initialize, and the engine
reaches `LoadMap: Entry` — failing only because the retail game content
(`Entry.unr`, `UnrealI.u`, …) is not present in this repository.

Next steps, in rough order:
1. Port the Editor module (script compiler) and rebuild the `.u`
   packages from `src/*/Classes` with a 64-bit `ucc`.
2. First real frame through OpenGLDrv once content loads.
3. Real audio backend behind the Galaxy stub interface.
4. Retail-content translating loader (32-bit `ScriptSize` fixup),
   if compatibility with 1998 packages is desired.

## Script compiler (ucc / `-make`) port — 2026-07-23

The Editor module now builds on x64 (`add_unreal_package(Editor Engine
Window ...)`), giving the port a working UnrealScript compiler:
`Unreal.exe -make` rebuilds any EditPackage whose `.u` is missing, and the
new `-make -remake=<Pkg>` flag force-recompiles a package whose `.u`
*does* load — the loaded package supplies meshes/textures/sounds whose
source art is not in the repo, so UnrealI rebuilds **code-only** against
retail content (all 487 classes, ~71k lines, verified in-game).

Load-bearing fixes, all tagged "x64 port":

1. **Bytecode emitter width** (`UnScrCom.h`): `FScriptWriter::operator<<
   (UObject*&)` emitted a 4-byte `(DWORD)` object reference; the VM reads
   pointer-width (mirror of the translating-loader read fix). Now
   `Serialize(&Res,sizeof(Res))`.
2. **`FPropertyBase` union init** (`UnScrCom.h`): the ctor initialized the
   union via 4-byte `BitMask(0)`, leaving the high half of the 8-byte
   `Enum`/`PropertyClass` pointers garbage → crash saving compiled
   classes. Init via `Enum(NULL)`.
3. **Intrinsic-size pin source** (`UnClass.h/.cpp`, `UnObj.cpp`): the
   LinkOffsets pin used the entry `PropertiesSize` as the C++ truth, but
   the compiler zeroes/overwrites `PropertiesSize` mid-parse, so compiled
   intrinsics (class Object!) shrank to script size and asserted on
   reload. New transient `UStruct::CppLayoutSize` captured at intrinsic
   registration, preserved across AllocateObject in-place replacement.
4. **FString varargs** (`UnEditor.cpp`): `Files(i)` passed by value to
   `appSprintf` `%s` (the x86 trick); now `*Files(i)`.
5. Sources import from `..\src\<Pkg>\Classes\*.uc`; usual x64 sweep in
   Editor (member-pointer syntax, `sizeof(T())` hit proxies, LRESULT
   window proc, HWND casts, `Min` template width).

Two long-standing runtime bugs found via the new bot/compile testing:

- **CleanupDestroyed flaky assert (fixed)**: v200's `Object.uc` names the
  outer property `Parent`, but the mirror-offset pin looked for the later
  spelling `Outer` — so `Parent` kept computed script offset 24, which
  overlays `MainFrame`/`Linker`, and the actor-cleanup RefLink walk read
  garbage there whenever a script stack existed. Deterministic on
  `Dig.unr` (needs 128+ deleted actors, hence the flakiness). Both
  spellings now pin to `STRUCT_OFFSET(UObject,Parent)`.
- **Bot pathing NaN crash (fixed)**: `APawn::SuggestJumpVelocity` divided
  by `vel.Size()` unguarded; jump destination directly above/below the
  pawn → 0/0 → NaN Location → the collision hash's `Location !=
  ColLocation` check trips on NaN!=NaN ("moved without proper hashing",
  ~30 s into any DmAriza botmatch). Guarded.

Status: `Core.u` and `UnrealI.u` rebuilt from source at package version
100 and shipped in `System/`; game boots, plays, and runs 4-bot
deathmatch (`?Game=UnrealI.DeathMatchGame`, `[UnrealI.DeathMatchGame]
InitialBots=4` in Unreal.ini) stably. This completes Phase 0 of the
bot-AI modernization plan; Phase 1 (perception & memory model) can now
edit `src/UnrealI/Classes/*.uc` and rebuild with
`Unreal.exe -make -remake=UnrealI`.

## Frame-pacing stutter fix — 2026-07-23

Measured with the new `-framestats` command-line diagnostic (frame/tick/
pump/swap/GL-block breakdown logged to Unreal.log): gameplay averaged
16.7 ms/frame but hitched 30–120 ms (exact vsync multiples) 1–3×/sec,
with the stall landing on arbitrary mid-frame GL calls — the modern
driver's deep present queue applies back-pressure wherever the command
stream happens to fill. Fix: `glFinish()` after `SwapBuffers` in
`OpenGLDrv::Unlock` drains the queue at the frame boundary; the CPU
frame is <1 ms so the lost CPU/GPU overlap is irrelevant. Verified: 0
spikes, max 18–20 ms over minutes of botmatch (was 4–16 spikes/5 s).

## Headless bot testing + polish — 2026-07-23

- **Headless botmatch harness**: `scripts\botmatch-test.ps1 [-Map X] [-Seconds N]`
  runs `Unreal.exe <map>?Game=UnrealI.DeathMatchGame -server -log` (dedicated
  server: no window, renderer, or audio; bot AI is fully server-side), then
  summarizes the log and exits nonzero on Criticals or zero bot activity.
  Enablers: `bMultiPlayerBots=True` + `InitialBots` in
  `[UnrealI.DeathMatchGame]` (bots only auto-spawn on a dedicated server with
  bMultiPlayerBots), and new `KILL:`/`SCORE:` log lines in
  `DeathMatchGame.uc` `Killed()`/`EndGame()` (first real script change built
  with `-remake=UnrealI`).
- **Bot skin warning spam fixed** (`BotInfo.uc Individualize`): the code
  re-resolved each bot's class-default skin name against the external
  `<Mesh>Skins` package — which always fails (those textures live inside
  UnrealI, or the default is None) — warning once per bot spawned. Now only
  explicitly configured `BotSkins[n]` entries are looked up.
- **Log window sized for modern displays** (`Window.h WTerminal::OpenWindow`):
  was hardcoded 512x256 with DEFAULT_GUI_FONT; now 900x600 scaled by display
  DPI (capped to the work area) with a DPI-scaled Consolas for the text.

## Bot AI modernization Phase 1: perception & memory — 2026-07-23

Implemented entirely in `Bots.uc` (ScriptedPawn/monsters untouched), gated
on `[UnrealI.Bots] bModernAI` (globalconfig; class default False = faithful
1998 behavior; shipped Unreal.ini enables it). `bModernAIDebug` logs
perception events for headless verification.

- **Reaction latency**: `SetEnemy` override arms a per-contact delay
  (`FMax(0.1, 0.5 - 0.13*skill + 0.15*FRand())` — ~0.5 s at skill 0, ~0.1 s
  at skill 3); `FireWeapon` holds fire inside the window (movement is
  unaffected, so it reads as human reaction, not lag). Taking damage from
  the enemy cuts the delay short.
- **Enemy belief**: position + velocity + confidence. Refreshed exactly at
  the moment sight is lost (`EnemyNotVisible` hooks in Attacking / Charging
  / TacticalMove — the engine keeps `LastSeenPos` current until that edge),
  at half confidence by heard noise, and fuzzily (±150 uu) by pain from an
  unseen attacker.
- **Predictive hunting**: `Hunting.BeginState` projects the belief along
  the last known velocity (horizon bounded by confidence, clamped to the
  first wall hit by a trace) and feeds it into the existing Hunting logic
  via `LastSeenPos` — bots cut toward where the enemy is going.

Verified headlessly with `scripts\botmatch-test.ps1`: toggle off =
regression-clean; toggle on = 75 perception events over 2 minutes
(acquisitions with 0.38–0.52 s reactions, loss-edge snapshots, projected
hunt destinations), 8 kills, zero Criticals.

## Bot AI modernization Phase 2: utility-based decisions — 2026-07-23

`ModernChooseAttackMode()` in `Bots.uc` replaces the 1998
`Attacking.ChooseAttackMode()` if/FRand cascade for DM combat (non-player
enemies and monsters keep the original router; same `bModernAI` gate).
Every option is an existing FSM state used as an action, scored 0..~1.5
from health, ammo fraction, `RelativeStrength`, `Aggressiveness`, skill,
and the Phase 1 enemy belief:

- **Engage** (MeleeAttack/RangedAttack) — needs readiness + a clear shot;
  wants health/ammo and a winnable matchup; high skill prefers moving.
- **Reposition** (TacticalMove) — strafe/fire-on-the-move baseline, keeps
  the original refire-timer cadence; picks up nearby items en route.
- **Retreat/resupply** (Retreating) — low health, low ammo, fear, or being
  outgunned; a new `RetreatUntil` window commits the bot to the resupply
  run for 3-5 s so it doesn't ping-pong back to Attacking (the 1998
  Retreating state only holds under ATTITUDE_Fear).
- **Hunt** (Hunting) — chases the Phase 1 projected position while the
  belief is fresh.
- **Stake out** (StakeOut) — covers the last seen spot at close range.

`bModernAIDebug` logs one score vector per decision
(`decide E=.. T=.. R=.. H=.. S=..`), making every choice explainable.
Verified headless: toggle off regression-clean; toggle on = 120 decisions
over 2 minutes exercising all five actions (R observed 0.10 healthy →
1.59 near-dead/dry with commit-and-return behavior), kills normal, zero
Criticals.

## Bot AI modernization Phase 3: aim & movement skill split — 2026-07-23

The single 0-3 `Skill` scalar is split into four independent axes in
`Bots.uc` — `AimSkill`, `MoveSkill`, `TacticSkill`, `ReactSkill` — each
derived in `ReSetSkill()` from Skill plus a stable per-bot personality bias
rolled once per life (so one bot is a great shot but a poor mover, another
the reverse; AdjustSkill dynamic difficulty shifts the base, biases keep
the personality). Same `bModernAI` gate.

- **Aim model** (full `AdjustAim` override, replaces
  `aimerror * (2.4 - 0.5*(skill+FRand()))`): error = per-weapon base ×
  convergence (wide on fresh contact, settles over ~2.5 s of tracking,
  anchored to the Phase 1 AcquireTime) × axis curve × perpendicular
  relative-velocity tracking difficulty. Leading is always on with
  proportional error (poor aim under/over-leads) instead of the
  bLeadTarget coin flip. The 1998 trace fallbacks, splash-at-feet,
  hold-fire, and turn-rate clamp are preserved. StakeOut's state-scoped
  aim-at-remembered-spot stays 1998 (convergence doesn't apply there).
- **Threat-based dodging** (`WarnTarget` override, replaces the flat
  `FRand() > 0.33*skill` gate): dodge propensity from the movement axis
  scaled by the shooter's `Weapon.AIRating`, with a MoveSkill-scaled
  minimum flight time and the original see-the-shooter FOV gate.
- **Axis plumbing**: ReactSkill drives the Phase 1 reaction delay,
  TacticSkill the Phase 2 engage/reposition scores, MoveSkill the
  strafe-vs-direct choice in TacticalMove.

Verified headless: off = regression-clean; on = distinct stable per-bot
profiles (e.g. aim 0.99/move 0.42 vs tactic 1.49/react 0.47) with the
best-aim bot visibly dominating a hitscan match 6-1; zero Criticals.

## Clean `-remake` output (resolve-existing imports) — 2026-07-23

Code-only makes emitted ~1,756 warnings for missing source art. Now:
`ImportObjectFromFile` (UnObj.cpp) resolves silently to the existing
object when the file is absent but the target (pre-loaded from the retail
package) exists; `meshImport` (UnMeshEd.cpp) keeps an existing mesh
silently when the model files are absent; `OBJ LOAD` (UnEdSrv.cpp) skips
silently when the target package already lives in memory;
`MESHMAP SETTEXTURE TEXTURE=Default` (a 1998 placeholder meaning "engine
default") no longer warns. A full UnrealI remake now emits 2 warnings —
both genuine dangling texture references (`Jflag21`, `Jb1exp1`) that were
broken in Epic's own build.

## Deathmatch stutter: vsync quantization — 2026-07-23

Combat still stuttered after the glFinish fix. `-framestats` (now with a
per-frame Draw timer in `UWindowsViewport::Repaint`) attributed every
spike to the render path: with vsync on, each frame carries a ~16.7 ms
sync wait, and the 5-15 ms of CPU render work that firefights add
(dynamic lights from muzzle flashes/explosions re-lighting surfaces)
pushed frames past the 60 Hz budget - each miss costing a whole extra
frame (25-55 ms clusters = the felt stutter).

Fix: `UseVSync=False` shipped in `[OpenGLDrv.OpenGLRenderDevice]`
(Unreal.ini + Default.ini). The existing per-frame `glFinish` keeps
pacing tight, DWM composition prevents tearing in borderless fullscreen,
and the quantization cliff disappears entirely. Measured in combat:
~0.9-1.0 ms/frame (~1000 fps), zero spikes >25 ms after level load
(max 12.6 ms), gameplay verified healthy at that rate. Set
`UseVSync=True` there to restore the 60 Hz cap if ever desired.

## Bot AI modernization Phase 4: map economy & team intel — 2026-07-23

In `Bots.uc`, same `bModernAI` gate:

- **Key-item respawn clocks**: each bot tracks up to 8 of the map's most
  valuable respawning pickups (MaxDesireability >= 0.75: armor, super
  items, top weapons). Beliefs come only from experience — taking an item
  itself (exact clock: now + RespawnTime, via an `AddInventory` hook),
  or seeing the spot (visible item = available now; visibly empty spot =
  believed taken, estimated half-cycle).
- **Timed pickups**: `BestTimedPickup()` scores items by desirability
  over travel + wait, considering anything available by arrival (plus a
  short camp window). Consumed in `Roaming.PickDestination` (when no
  visible item beats it) and `Retreating.PickDestination` (resupply runs
  head for the armor about to spawn), both falling back to the 1998 flow.
- **Team intel**: the previously vestigial `CallForHelp` /
  `HandleHelpMessageFrom` hooks now share enemy contacts — a fresh
  acquisition (and, as before, retreating) broadcasts to teammates, who
  adopt the reported enemy as a half-confidence belief.

## Frame cap + physics NaN guards — 2026-07-23

Uncapped (~1000 fps) frame rates put the 1998 physics float math outside
its designed regime: `physWalking`'s
`Velocity=(Location-OldLocation)/(deltaTime-remainingTime)` with a
near-zero denominator went astronomic then NaN (bot pawns NaN'd
mid-match), and `ActualDist/DesiredDist` could 0/0. Fixes: (1) both
divisions guarded in UnPhysic.cpp; (2) MainLoop now caps frames at
**200 fps** by default (the same cap the surviving community UE1 patches
use) via timeBeginPeriod(1) sleep + spin tail — ~5 ms deltas, well above
the old 60 Hz feel. `[Engine.Engine] FrameRateLimit=N` overrides;
`FrameRateLimit=0` uncaps. Window package now links winmm. Verified:
~195 fps in combat, clean windows, and repeated 150 s modern botmatches
(previously NaN-crashing) run clean, headless server included.

## Sound system audit (ambients overwhelming) — 2026-07-23

Audited the rebuilt Galaxy driver against `legacy/Galaxy/Src/Galaxy.cpp`.
Faithful: ambient start volume (`AmbientFactor*SoundVolume/255`), the 2x
update scale, linear `1-d/r` attenuation, despatialize-at-0.1r, +-7/8 pan
sweep, Doppler clamp, priority/steal, `AmbientFactor=0.6` ini respected.
`EFFECT_FACTOR` (dropped in the port) is 1.0 in the original — a no-op.

Two real defects found and fixed:

1. **Missing light-animation modulation** (the reported problem): the
   original passes ambient Brightness by reference through
   `Render->GlobalLighting()`, which applies the owner light's ANIMATED
   curve — flicker averages ~0.4x with dropouts, pulse sweeps 0.6+-0.39,
   blink/strobe gate to silence. The port only applied the static
   `LightBrightness/255`, so torch-type ambients played constantly at
   full volume, ~2-5x the original's time-average loudness. The
   GlobalLighting call is restored (guarded for headless).
2. **Voice start order**: source voices were `Start()`ed before
   volume/pan/pitch were applied, so every sound began with a sub-frame
   full-volume centered blip. Voices now start only after the first
   parameter application (`bStarted` flag).

Verified: near Dig.unr torches the output peak now fluctuates 0.13-0.92
(flicker modulation audible/measurable via IAudioMeterInformation);
music and effects unchanged; no regressions in boot or botmatch.

## Full-system review + isolated test suite — 2026-07-23

Prompted by the sound-audit find (a silently diverging subsystem), every
system now has an isolated test, wired into two tiers:

**Tier 1 — `Unreal.exe -selftest -log`** (SelfTest.cpp; no window,
renderer, audio device, or gameplay; exit code = failure count):
- Core: vector math (incl. the SafeNormal zero guard), FName add/find,
  TArray growth/removal, appSprintf varargs, appAtoi/appAtof.
- Layout invariants: sizeof(FVector/FRotator/UObject/AActor) locked
  (UObject=72, AActor=556) and the class-Object mirror pins verified
  against the loaded package (Parent=48/ObjectFlags=56/Name=60/Class=64
  — the regression guard for the CleanupDestroyed class of bug).
- Translating loader: EVERY retail package must load — 6 script
  packages, 46 texture, 9 sound, 32 music, 53 maps (146 total).
- Hardened WAV parser: every retail USound must parse.
- Script presence: ScriptedPawn/DeathMatchGame bytecode (summed across
  function children — class-level Script is empty by design) and the
  modern-AI Phase 1/3/4 properties on Bots.
- Teardown is skipped via TerminateProcess: the run loads all content
  outside the engine lifecycle and DLL-detach destructors crash on it.

**Tier 2 — `scripts\selftest.ps1`** (exit code = failed stages):
1. the engine self-test, 2. a throwaway-sandbox `-remake=UnrealI`
compiler round-trip (warning budget <=5, package >30MB), 3. a 90 s
headless botmatch (AI/physics/VM), 4. a rendered botmatch checking boot,
frame pacing (<10 ms avg) and a non-black screenshot, 5. the system
audio peak meter during that run.

Review status by system: Core/Engine loader+layout (tier 1), script
compiler (tier 2), bot AI+physics (tier 2 headless), renderer (tier 2
smoke + -framestats), audio (audited + metered), Fire/procedural
(exercised by rendered run). Not yet covered in isolation: IpDrv
networking beyond package load, savegames, and the interactive editor
shell (only the -make path is exercised).

## Dist packaging fix: ship Editor.dll — 2026-07-23

Running the new self-test FROM INSIDE a freshly staged dist caught a real
shipping bug: dist carried retail `Editor.u` (via the `*.u` staging rule)
but not `Editor.dll`, so Editor's intrinsic classes could not bind — and
three retail maps (Dug, NaliC, Ruins) import Editor objects, making them
unloadable in the shipped build (masked in the working tree, where
Editor.dll exists). Editor is now in UNREAL_RUNTIME_TARGETS, which also
gives dist users `-make`/`-remake` script rebuilding. Verified: the dist
self-test passes 32/32 (all 53 maps, 966 sounds) and the dist game boots.

## Cross-platform migration — Phase 0 foundation (2026-07-23)

Direction set (with the user): make the existing C++ engine cross-platform
(Windows/Linux/macOS) via SDL + portable audio + Dear ImGui — NOT .NET
(.NET Framework is Windows-only and would require a full managed rewrite
that loses portability). Full plan in docs/CROSSPLATFORM.md.

Phase 0 (done, Windows build unchanged):
- CMake is platform-aware: dropped the `if(NOT WIN32) FATAL`; MSVC-only
  flags/defines gated behind `if(MSVC)` with a Clang/GCC branch; the
  Win32-only backends (Window, WinDrv, Editor, Galaxy, OpenGLDrv, Launch)
  gated behind `UNREAL_PLATFORM_WINDOWS`; per-platform system libs
  (`winmm`/`ws2_32` vs `CMAKE_DL_LIBS`/libc sockets).
- Core timing moved off Win32: `appSeconds`/`appCycles` now use
  `std::chrono::steady_clock` (monotonic, high-res, ARM-safe) instead of
  QueryPerformanceCounter + `__rdtsc`. `appCycles` is now a nanosecond
  tick, so the empirical `GSecondsPerCycle` calibration converges to 1e-9
  and clock()/unclock() stat math is unchanged (the cosmetic "CPU Speed"
  readout now shows ~1000 MHz = nominal ns clock).
- Verified behavior-identical on Windows: self-test 32/32, 200fps frame
  cap holds (~5 ms frames), physics/audio/botmatch clean, dist selftest
  passes. The remaining QueryPerformanceCounter calls are confined to
  `appInit`'s Windows startup CPU-speed display (part of the Windows
  platform file, split in Phase 1).

## Cross-platform Phase 1 — portable Core (2026-07-23)

Core's entire Win32 surface turned out to be just two files — `UnPlat.cpp`
(the platform layer) and one function in `UnFile.cpp` (`_findfirst`). Every
Windows primitive now has an `#if _WIN32 / #else` POSIX branch in place
(keeps the shared config/logging/init logic DRY rather than duplicating it):
dlopen for DLL loading, sysconf for machine info, glob for file listing,
rename/byte-copy for move/copy, localtime for system time, seeded-random
GUIDs, xdg-open/open for URLs, stderr for message boxes, __builtin_trap for
debug break, plus `_strdate`/`_strtime`/`_stat`/`_utime` CRT shims. Timing
was already portable (Phase 0). `appCmdLine` gains a POSIX `appSetCmdLine`
seam for the future SDL launcher; exe path via readlink.

Windows is unaffected — all Win32 code sits under `#if _WIN32`, so the
Windows compile is byte-identical: clean build, self-test 32/32, botmatch
clean. POSIX branches are correct-by-construction (no Linux build host
available; a gcc container/WSL distro would validate them). macOS has one
known gap (sysconf memory query → sysctl). Next: Phase 2, SDL windowing.

## Cross-platform Phase 2 — SDL windowing (SDL3) — 2026-07-24

New `SDLDrv` package (`USDLClient`/`USDLViewport`) implements the engine's
`UClient`/`UViewport` contract on **SDL3**: window creation, an OpenGL
surface, and keyboard/mouse input (SDL events → `Engine->InputEvent`).
OpenGLDrv gained a `UNREAL_USE_SDL` build path that makes/binds/swaps the GL
context via `SDL_GL_*` in place of wgl. SDL is vendored via CMake
`FetchContent` (`release-3.4.12`); `-DUNREAL_USE_SDL=ON` enables it (OFF by
default on Windows — WinDrv stays default and untouched; ON on Linux/macOS).

The engine drives the new classes polymorphically, so **no engine changes**
were needed — the client is chosen by `[Engine.Engine]
ViewportManager=SDLDrv.SDLClient`, and `UGameEngine` calls
`NewViewport`/`OpenWindow`/`Tick` through the base interface.

Verified on Windows (proves the Linux path, since SDL abstracts the OS):
the game boots through `SDLClient`, opens an SDL window, creates an
**OpenGL 4.6 context via SDL**, and renders the live DmAriza deathmatch
level with HUD and crosshair (screenshot-confirmed). The default WinDrv
build is unaffected — after restoring `System/` from the default build:
selftest 32/32, botmatch clean.

The load-bearing bug: `UInput::Init` → `ResetInput()` →
`Viewport->UpdateInput()`, so the driver's `UpdateInput` must NOT call
`ResetInput` (WinDrv's polls the joystick). The first version did, recursing
to a stack-overflow crash. SDL3-vs-SDL2 API deltas handled: `SDL_EVENT_*`
event enums, flattened `Ev.key.key`, `SDL_CreateWindow(title,w,h,flags)`,
`SDL_WINDOW_FULLSCREEN`, `SDL_SetWindowRelativeMouseMode`,
`SDL_GL_DestroyContext`, and the `SDLK_a→SDLK_A` / `SDLK_BACKQUOTE→SDLK_GRAVE`
keycode renames. Deferred: joystick, multi-viewport editor windows, exact
fullscreen mode-setting. Next: Phase 3, portable audio.

## Map-data fix: TerraLift.unr semisolid collision hole — 2026-08-03

**The only deviation from retail map data in this repo.** Everything else in
`content/` is bit-identical to the 1998 release.

Player report: walking through the pink switch (`DSWITCH4`) and the rock
beside it in "Terraniux Underground" — which is `TerraLift.unr`
(`Terraniux.unr` is the *next* level; TerraLift's exit teleports to
`terraniux#entree`).

Root cause — a 1998 authoring defect, not a port bug:

- `Brush204`, the concave 14-poly semisolid rock at the `Mover0` button
  (-832,104,-64), renders correctly but its **baked collision hulls**
  (`Nodes`/`LeafHulls`, written by UnrealEd's rebuild in 1998) miss the
  pocket behind its front face: a ~108-unit-wide player-sized void
  (X -772..-664, Y up to ~96, Z -128..32).
- Semisolid brushes are composed into the *already-partitioned* BSP as
  "detail" brushes (`csgRebuild` second pass); their faces get fragmented by
  unrelated tree planes and can lose hull coverage — a known limitation of
  the era. A sweep of all 53 retail maps (`-semisolid` audit) found 0–3%
  of semisolid faces open per map (TerraLift: 9 of 2318) while plain solid
  geometry audits ~0% everywhere, so the mechanism itself is sound.
- The runtime collision code is exactly retail: `UnTrace.cpp`,
  `UnLevAct.cpp`, `UnDynBsp.cpp` diff **zero lines** against the v200
  originals. The hole reproduces identically in retail 1998.

Fix: **31 invisible `BlockAll` actors** (`BlockFix0..30`, cylinders r=6..48,
Z -128..32) filling the rock's interior at player heights — the same tool
the original mappers used 9 times elsewhere in this map. Generated by
ray-casting the brush mesh so no blocker pokes outside the visual surface,
imported via `MAP IMPORTADD` + `MAP SAVE` (no BSP rebuild; the semisolid
audit is byte-for-byte identical before/after). Verified: pawn-extent grid
scans show the pocket fully blocked, courtyard cells unchanged
cell-for-cell, level strings/teleporter intact, selftest 32/32 incl. all 54
maps loading. Other flagged faces in TerraLift were probed pawn-sized: not
enterable (slivers) or legit space — left alone, as were all other maps.

Diagnostics added for this investigation (all in `src/`, no gameplay
effect): `whereami` console command (position/zone/overlaps),
`-mapprobe=x0:y0:z0:x1:y1:z1` line probe, `-probegrid` ASCII occupancy
maps, `-probeview=x:y:z:pitch:yaw` pinned-camera screenshots,
`-probetrigger`/`-probeexec`, `-semisolid` all-map audit, and
`EDGUI.MOVERINFO`/`EDGUI.MOVERPROBE` mover collision dumps.
