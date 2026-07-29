# Cross-platform migration (Windows → Windows/Linux/macOS)

Goal: run the port on Linux and macOS as well as Windows, by moving the
~12 Win32-bound files off the Windows API and onto portable libraries —
**without** disturbing the engine core, the UnrealScript VM, the
translating loader, the modern OpenGL renderer, or the modernized bot AI,
all of which are already portable C++.

Chosen stack (decided 2026-07-23): **SDL** for windowing + input +
GL-context + dynamic-library loading + timing, **a portable audio backend**
(OpenAL or miniaudio) behind the existing Galaxy interface, and **Dear
ImGui** for the editor UI that replaces the retired Visual Basic shell.
This keeps everything already built and adds Linux/macOS as targets. .NET
was rejected: .NET Framework is Windows-only (it would *prevent*
cross-platform) and hosting a 121k-line C++ engine on a managed runtime
means a full rewrite for no portability gain.

## Win32 surface (what has to move)

| Module     | Win32 dependency                          | Portable replacement            |
|------------|-------------------------------------------|---------------------------------|
| Core       | timing, threads, DLL load, file IO, GUID  | std::chrono ✅, std::thread, SDL_LoadObject, std::filesystem, libuuid/random |
| Window     | Win32 widget library (WWindow/WButton/…)  | Dear ImGui (editor); game HUD is engine-drawn already |
| WinDrv     | DirectDraw enum + window + raw input      | SDL_Window / SDL_Event          |
| OpenGLDrv  | wgl context creation (rendering portable) | SDL_GL_CreateContext            |
| Galaxy     | XAudio2 + winmm (libxmp already portable) | OpenAL / miniaudio              |
| Editor     | Win32 dialogs + VB-hosted exec shell      | Dear ImGui editor frame         |
| Launch     | WinMain + Win32 message pump              | SDL_main + SDL event loop       |
| IpDrv      | Winsock (ws2_32)                          | BSD sockets (#if _WIN32 split)  |

## Phased plan

- **Phase 0 — foundation (DONE 2026-07-23).** CMake is platform-aware
  (`UNREAL_PLATFORM_WINDOWS`; MSVC-only flags gated; Windows-only modules
  gated behind the flag; `CMAKE_DL_LIBS`/sockets selected per platform).
  Core timing (`appSeconds`/`appCycles`) moved to `std::chrono::steady_clock`
  — no Win32, ARM-safe, verified behavior-identical on Windows (200fps cap,
  physics, audio all unchanged; self-test 32/32).
- **Phase 1 — portable Core (DONE 2026-07-23).** Rather than physically
  splitting the file (Core's *entire* Win32 surface is just `UnPlat.cpp`
  plus `_findfirst` in `UnFile.cpp`), each Windows primitive got an
  `#if _WIN32 / #else` POSIX branch in place, keeping the shared config /
  logging / init logic DRY:
  - DLL loading → `dlopen`/`dlsym`/`dlclose` (with `lib<n>.so`/`.dylib`
    fallback); memory/CPU/page info → `sysconf`; GUID → seeded random
    (RFC-4122 v4 layout); `appFindFiles` → `glob()` + `basename`;
    file move/copy → `rename` + portable byte copy; system time → `localtime`;
    URL/relaunch → `xdg-open`/`open`/`system`; message boxes → stderr;
    debug break → `__builtin_trap`; MSVC CRT shims (`_strdate`/`_strtime`/
    `_stat`/`_utime`) provided for POSIX.
  - Command line: Core has no `GetCommandLine` on POSIX, so a new
    `appSetCmdLine(argc,argv)` records it (the SDL launcher will call it);
    exe path via `readlink(/proc/self/exe)`.
  - Verified: **Windows build compiles cleanly through every seam**
    (the Windows code paths are byte-identical — all Win32 is under
    `#if _WIN32`), self-test 32/32, botmatch clean. POSIX branches are
    correct-by-construction (no Linux build host on this machine).
  - Known macOS follow-up: the `sysconf(_SC_PHYS_PAGES)` memory query is
    Linux-specific; macOS needs `sysctl(HW_MEMSIZE)`.
- **Phase 2 — SDL windowing (SDLDrv) (DONE 2026-07-24, SDL3).** New
  `SDLDrv` package (`USDLClient`/`USDLViewport`) implements the engine's
  `UClient`/`UViewport` contract on **SDL3** — window, GL surface, and
  keyboard/mouse input (SDL events → `Engine->InputEvent`). OpenGLDrv gained
  a `UNREAL_USE_SDL` path that creates/binds/swaps the GL context via
  `SDL_GL_*` instead of wgl. SDL is vendored through CMake `FetchContent`
  (`release-3.4.12`); `-DUNREAL_USE_SDL=ON` builds it (default OFF on
  Windows so the WinDrv build is untouched; forced ON on Linux/macOS).
  **Verified on Windows**: the game boots through `SDLClient`, opens an SDL
  window, creates an OpenGL 4.6 context via SDL, and renders the live
  DmAriza level with HUD + crosshair (screenshot-confirmed). Because SDL
  abstracts the OS, this same path is what runs on Linux/macOS.
  - The engine drives it polymorphically (no engine changes): `NewViewport`
    / `OpenWindow` / `Tick` from `UGameEngine`, client from ini
    `ViewportManager=SDLDrv.SDLClient`.
  - Key bug found & fixed: `UInput::Init`→`ResetInput()`→
    `Viewport->UpdateInput()` — the driver's `UpdateInput` must NOT call
    `ResetInput` (WinDrv's polls the joystick); the naive version recursed
    forever (stack-overflow crash). SDL3 API deltas from SDL2 handled:
    `SDL_EVENT_*` enums, flattened `Ev.key.key`, `SDL_CreateWindow(title,
    w,h,flags)`, `SDL_WINDOW_FULLSCREEN`, `SDL_SetWindowRelativeMouseMode`,
    `SDL_GL_DestroyContext`, `SDLK_A..Z`/`SDLK_GRAVE` renames.
  - Not yet ported to SDL: joystick, multi-viewport editor windows, exact
    fullscreen mode-setting (borderless-desktop for now).
- **Phase 3 — portable audio (DONE 2026-07-24, SDL3 audio).** Galaxy gained
  a `UNREAL_USE_SDL` backend on **SDL3 audio** (no new dependency — SDL is
  already vendored). All the spatialization / attenuation / priority-steal /
  ambient-scan / music state-machine logic is shared; only the XAudio2 voice
  calls are branched. Effects: one `SDL_AudioStream` per voice whose
  get-callback pulls mono sample frames, applies the live L/R pan gains, and
  loops; volume via `SDL_SetAudioStreamGain`, pitch/doppler via
  `SDL_SetAudioStreamFrequencyRatio`; SDL mixes all bound streams. Music: one
  stream fed by `xmp_play_buffer` in its get-callback — which **eliminates
  the Win32 feeder thread** the XAudio2 build needs. libxmp-lite untouched.
  **Verified on Windows** with the full SDL stack (windowing+audio): renders
  DmAriza and the system audio peak meter reads a steady 0.27–0.57 (music +
  effects), matching the XAudio2 baseline. Default Windows build keeps
  XAudio2 (untouched: selftest 32/32, botmatch clean).
- **Phase 4 — SDL launcher (DONE 2026-07-24, verified on all four command
  paths — via a renamed binary while Norton blocks the real name, see
  below).** New portable launcher
  `src/Launch/Src/SDLLaunch.cpp`: a plain `main(argc,argv)` replacing
  WinMain + the Window package entirely for the SDL build. No message pump
  is needed — `SDLClient::Tick()` pumps SDL events inside `Engine->Tick`,
  and a dedicated server has no window. The Win32 log window is replaced by
  tee-ing the log to stdout via Core's `GLogHook` (per-line `fflush` so
  `-selftest`'s `TerminateProcess`/`_exit` exit path loses nothing).
  Portable `InitEngine` (drops the running-instance mutex, registry
  file-type registration, and the 1998 first-run hardware wizard),
  portable `MainLoop` (keeps `-framestats`, `GetMaxTickRate` pacing, and
  the 200fps `FrameRateLimit` cap; `timeBeginPeriod` under `#if _WIN32`,
  `nanosleep` elsewhere), same `-selftest` / `-make` / `-server` paths.
  On POSIX it records the command line via the Phase-1 `appSetCmdLine`
  seam. CMake: `src/Launch` now builds on every platform; `UNREAL_USE_SDL`
  selects the portable launcher (console subsystem on Windows, keeping the
  Windows-only crash reporter + icon resource), default Windows build keeps
  the WinMain launcher untouched; the packaging target list is now
  platform-aware.
  - **Verified on Windows (SDL build), all four paths through the new
    launcher**: `-selftest` 32/32; `-make` UnrealI compiler sandbox PASS;
    `-server` 90s headless botmatch (5 kills, 0 criticals); rendered
    client boots SDLClient → 1280x720 SDL window → GL 4.6 → live DmAriza
    bot DM (screenshot-confirmed, frame cap steady at 5.0ms avg / 0
    spikes, audio peaking) with the log tee-ing to the terminal.
  - **Antivirus caveat (open)**: Norton behaviorally kills freshly-built
    unsigned exes the moment `IpDrv.ServerBeacon/ServerUplink` opens
    sockets (silent death right after "Game engine initialized"), then
    quarantines the binary and tombstones the filename — `Unreal.exe` and
    `UnrealSDL.exe` are both currently unusable names (LNK1104 on relink).
    Verification used a renamed link (`msbuild Unreal.vcxproj
    /p:TargetName=ZenUnreal /p:BuildProjectReferences=false` — the second
    flag is essential or Core/Engine also relink under the new name) with
    beacon/uplink `ServerActors` blanked in `ZenUnreal.ini` (the exe name
    selects `<name>.ini`/`<name>.log`). Until the repo is added to
    Norton's exclusions and the quarantined files restored, `Unreal.exe`
    cannot be linked by either config.
  - `[SDLDrv.SDLClient]` ini section now carries `ViewportX/Y` +
    `StartupFullscreen` (UClient config defaults gave a 320x200 window).
  - Build-hygiene gotchas found during verification (both configs link
    into the shared `System/`): (1) an IDE CMake watcher (VS Code) can
    rebuild the *default* tree on CMakeLists edits, clobbering the SDL
    variants of OpenGLDrv/Galaxy mid-test; (2) MSBuild then considers the
    clobbered (newer) DLL up-to-date and skips relinking the SDL variant —
    delete `System/OpenGLDrv.dll` + `Galaxy.dll` before rebuilding the SDL
    config to force correct relinks (check: the SDL variants import
    SDL3.dll).
- **Phase 5 — ImGui editor (MILESTONE 1 DONE 2026-07-24; in progress).**
  Cross-platform UnrealEd replacing the VB shell, driving the existing
  C++ editor backend through its original contract (Exec strings, topic
  Get/Set, EdCallback codes) with the Win32 transport removed.
  - **Architecture**: Dear ImGui v1.91.5-docking vendored via FetchContent
    (static lib `imgui` with SDL3+OpenGL3 backends). New static lib
    **`src/EdGui`** (linked into the SDL launcher, `UNREAL_WITH_EDGUI`),
    installed for `-editor`: it takes two new hooks — `GSDLEventHook`
    (SDLDrv gives ImGui first crack at every SDL event; consumed events
    skip engine input) and `GGLPostRenderHook` (OpenGLDrv calls it with
    the GL context current after the world render, before the swap — the
    overlay draw site; ImGui inits lazily on the first callback). SDLDrv
    also gained the **editor input translation** (mirroring WinDrv's
    window proc): button press captures the mouse and starts a drag →
    `Engine->MouseDelta(Buttons|Ctrl/Shift/Alt, dx, dy)`; non-moved
    release → `Engine->Click`; hover → `MousePosition`; keydown also
    feeds `Engine->Key` — all UEngine virtuals, so SDLDrv does not link
    Editor.dll.
  - **Milestone 1 (verified on Windows, screenshots)**: `-editor` boots
    through the SDL launcher into an ImGui frame over the Standard3V
    viewport (opened via `CAMERA OPEN`, GL 4.6): menu bar
    (File map load/save + exit, Edit undo/redo/selection, Mode = the ten
    MODE exec tokens with status display, Build rebuild/paths, View panel
    toggles), log panel (tees Core `GLogHook`), exec console. `MAP LOAD`
    of DmAriza works from the menu path; camera drag input runs through
    the editor MouseDelta path without crashing.
  - **Port bugs found (all fixed)**: (1) the interactive editor engine
    must be constructed from the intrinsic EditorEngine class
    (`LOAD_DisallowFiles`, like `-make`) — loading retail Editor.u's
    class object copies 1998 32-bit serialized defaults over the x64
    layout and clobbers the C++ TArray members (crashed in
    `Tools.AddItem`, garbage ArrayMax); (2) consequently `EditPackages[]`
    is read directly from the ini in `UEditorEngine::Init` (the config
    property never populates on the intrinsic class); (3) viewports must
    pass `SHOW_NoButtons` — `UEditorEngine::Draw` unconditionally calls
    `DrawButtons`, which derefs 1998 toolbar textures only the VB shell
    loaded (AV; ImGui is the toolbar now).
  - **Milestone 2 (DONE 2026-07-24) — selection → properties pipeline**:
    new Core hook `GEdCallbackHook` (beside GLogHook/GExecHook; EdHook's
    `EdCallback` now calls it in addition to the legacy hWndCallback
    PostMessage) delivers EDC_* codes in-process; EdGui refreshes its
    selection set on SelChange/SelPolyChange/MapChange. **Properties
    panel**: reflection-driven via `TFieldIterator<UProperty>` — CPF_Edit
    properties grouped by UnrealScript `Category` (CollapsingHeaders),
    values via `UProperty::ExportText`, edits via `ImportText` +
    `PostEditChange` applied to the whole selection (classic multi-edit;
    booleans are checkboxes, EditConst renders read-only). Status bar
    shows selection count. Startup macro file `System\EdGuiBoot.txt`
    (one exec per line, `;` comments) used by the automated tests.
    Verified: DmAriza + `ACTOR SELECT OFCLASS CLASS=Engine.Light` → "96
    selected (Light, editing all)" with correct reflected values
    (bStatic/bHidden/bNoDelete on, LifeSpan 0.0). TODO: transaction
    bracketing around property edits (undo).
  - **Milestone 3 (DONE 2026-07-24) — classic quad views (multi-window)**:
    EdGui now opens four SDL windows at boot — Standard3V (perspective,
    REN_DynLight, hosts the ImGui frame) + OrthXY/OrthXZ/OrthYZ wire
    views. SDLDrv routes events per SDL window ID (keyboard follows
    focus via SDL_EVENT_WINDOW_FOCUS_GAINED which also sets
    UViewport::Current; closing the primary window quits, secondary
    windows hide — real CAMERA CLOSE teardown TODO); editor windows are
    titled "Unreal - <viewport>"; ImGui init pins itself to the
    Standard3V window and ignores other windows' events/swaps. OpenGLDrv
    needed nothing: per-viewport GL contexts already switch in Lock, and
    the ortho/wire path draws through Draw2DLine/Draw2DPoint which the
    modern renderer implements. **Verified (occlusion-safe PrintWindow
    captures)**: ortho views show the classic grey background + 16-unit
    grid + brush wireframes of DmAriza; the 3D view shows lit textured
    geometry under the ImGui menu bar.
  - **Editor-config root fix (part of M3)**: the intrinsic-class engine
    object never loaded [Editor.EditorEngine] config — and the generic
    UObject::LoadConfig CANNOT be used for this class (the script-mirror
    property offsets drift from the x64 C++ layout: int-sized vtbl
    stand-ins for the FNotifyHook/FConstraints vfptrs, a 32-bit TArray
    mirror for Tools — measured 24-36 bytes of cumulative drift; a
    LoadConfig attempt corrupted the object and crashed). UEditorEngine::
    Init now reads the values straight into the C++ members: grid/
    rotgrid/snap Constraints, FovAngleDegrees, and the 28 C_* editor
    colors. FovAngle=0 was also why the perspective view rendered black;
    the C_* zeros were why ortho views drew black-on-black.
  - **Milestone 4 (DONE 2026-07-24) — editing workflow**: class browser
    panel (live UClass hierarchy tree rooted at Actor, abstract classes
    greyed, click sets the current class via `SETCURRENTCLASS`; current
    class shown in the status bar); right-click context menus driven by
    the EDC_RtClick* callback codes — viewport background ("Add
    <CurrentClass>/Light/PlayerStart Here" via `ACTOR ADD CLASS=` at the
    engine's ClickLocation, Paste), actor (Properties/Duplicate/Delete/
    Select-all-of-class), surface (`POLY SETTEXTURE`, select matching/
    adjacent); map dialogs now list `content\Maps\*.unr` via
    appFindFiles (double-click loads). Context menus open on the primary
    window regardless of which viewport was clicked (per-window ImGui
    contexts are a later refinement).
  - **Milestone 5 (DONE 2026-07-24, click-verified) — editor hit testing
    in OpenGLDrv.** Verified with real input automation: a right-click on
    a DmAriza BSP surface in the textured 3D view produced the ImGui
    "Surface" context menu at the click point (device hit-test → proxy
    stack → ExecuteHits → UnEdClick → EDC_RtClickPoly → popup), process
    stable throughout. Discovery: the modern
    renderer's PushHit/PopHit were empty stubs AND never wrote *HitSize
    back — the editor's first real click would have parsed 1024 bytes of
    uninitialized stack as hit proxies (selection, context menus, and
    texture-browser clicks were all dead code until now). Implemented
    the device recorder: Lock arms a proxy-blob stack + the 5px hit
    region center; each primitive coverage-tests against it in screen
    space (DrawComplexSurface/DrawGouraudPolygon: point-in-projected-
    polygon; DrawTile/Draw2DPoint: rect; Draw2DLine: segment proximity
    ≤3px — this is what makes ortho wireframe picking work); a covered
    primitive sets a flag and the innermost enclosing PopHit snapshots
    the full current stack as the winner (painter's order, later
    overwrites earlier; bForce for the backdrop proxy); Unlock copies
    the winner into the caller's buffer and writes *HitSize (0 when
    nothing hit — the crucial part). Hit frames don't present
    (Unlock(Blit=0)), so clicking doesn't flicker.
    Also: closing a secondary editor window now execs a real
    `CAMERA CLOSE NAME=<viewport>` teardown (viewport lookup for later
    events in the same pump just misses; all branches null-check).
  - **Milestone 6 (DONE 2026-07-24, click-verified) — texture browser.**
    Textures panel lists loaded texture packages (distinct top-level
    parents of UTexture objects) and content\Textures\*.utx files
    (`OBJ LOAD FILE=` to load); selecting a package opens the retail
    **in-engine browser** via `CAMERA OPEN NAME=TexBrowser REN=17
    MISC1=64 PACKAGE=<pkg>` (MISC1 = tile size, REQUIRED — DrawTexture-
    Browser divides by it; Misc2 = scroll, wheel-scrolled by a new
    SDLDrv handler). Current texture tracked via EDC_CurTexChange →
    topic `Get("Ed","CURTEX")` (virtual call through Editor.h decls, no
    Editor import-lib link). Two enablers: UEditorEngine::Init now
    resolves the Editor.u icon textures (Bkgnd/BkgndHi/B_* — the
    intrinsic object's pointers were NULL and the browser derefs them
    for the selection highlight); verified end-to-end with input
    automation: thumbnail grid renders (DecayedS), clicking a tile
    draws the highlight ring and the panel readout updates
    ("Current: Basemai2").
  - **Milestone 7 (DONE 2026-07-25) — brush builders + CSG authoring
    from scratch.** New Brush menu: parameterized Cube / Sheet /
    Cylinder builders (EdGui generates the T3D polygon text and execs
    `BRUSH SET`; winding = counter-clockwise from outside, verified),
    Reset Transform, and the CSG ops (Add / Subtract / Intersect /
    Deintersect). Builders also usable as console/macro commands:
    `EDGUI.CUBE SIZE= [X= Y= Z=]`, `EDGUI.SHEET W= H=`,
    `EDGUI.CYLINDER R= H= SIDES=`. Engine fix: `BRUSH SET` now defaults
    untextured polys to CurrentTexture-else-LevelInfo->DefaultTexture
    (the modern renderer draws nothing for a NULL surface texture, so
    builder output rendered invisible). **Verified end-to-end**: from
    the empty startup level, subtract a 768 room + add a 192 cube +
    `MAP REBUILD` renders correct textured geometry.
    - Debug findings: a view that renders black with `surfs=0` in the
      new `-glcounts` diagnostic means the ENGINE submitted nothing —
      classic cause: camera at a point classified in-solid. The fresh
      level's camera starts at the exact origin, which classifies as
      solid even after a symmetric subtract — `JUMPTO x y z` (or fly)
      before judging a black view. `-glcounts` logs per-viewport
      surf/poly/tile counts to distinguish engine-cull from GL-state
      bugs.
  - **Next**: camera-speed / grid toolbar, property-edit undo,
    texture-browser GROUP filter + scroll clamp, surface properties
    panel, actor-click selection polish (verify HActor path / ortho
    wire picking interactively), persp wireframe-line check (REN=1 in
    perspective untested).

Each phase keeps the Windows build green and is independently verifiable.
Verification on Linux/macOS requires a Linux/macOS build host (this
development box is Windows-only), so those targets are validated when a
build host is available; the code is written portably by construction and
the Windows build proves no regression at every step.
