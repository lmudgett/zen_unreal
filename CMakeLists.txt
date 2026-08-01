# Unreal Engine v200 (May 1998) — modern x64 port, cross-platform migration
# Windows: Visual Studio 2022 (MSVC v143), Windows SDK 10
# Linux/macOS: Clang/GCC + SDL (windowing/input) — staged migration in progress.
cmake_minimum_required(VERSION 3.21)
project(Unreal LANGUAGES C CXX)  # C for the vendored libxmp-lite

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This port targets 64-bit only.")
endif()

# Cross-platform migration: the engine core + renderer are portable; the
# windowing/input/audio backends are being moved off Win32 onto SDL. Until a
# given backend is ported, non-Windows builds omit it (see per-module guards).
if(WIN32)
    set(UNREAL_PLATFORM_WINDOWS TRUE)
    message(STATUS "Unreal: Windows platform (full feature set)")
else()
    message(STATUS "Unreal: ${CMAKE_SYSTEM_NAME} platform (cross-platform migration — see docs/CROSSPLATFORM.md)")
endif()

# SDL is the only windowing/input backend on every platform. The DirectX-based
# WinDrv client is retired to legacy/ (DirectX is fully removed from the port
# for cross-platform compatibility).

# All runtime binaries land in System/ next to Unreal.ini, as the engine expects.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/System)
set(CMAKE_PDB_OUTPUT_DIRECTORY     ${CMAKE_SOURCE_DIR}/System)
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${CMAKE_SOURCE_DIR}/System)
    set(CMAKE_PDB_OUTPUT_DIRECTORY_${cfg}     ${CMAKE_SOURCE_DIR}/System)
endforeach()

# C++14 suits this 1998 codebase best (keeps `register`, permissive template
# rules). ASM=0 / NOAMD3D disable the x86 inline-asm and 3DNow! paths on every
# platform (the C++ fallbacks are used).
set(CMAKE_CXX_STANDARD 14)
add_compile_definitions(
    ASM=0                       # no inline asm; use the C++ fallbacks
    NOAMD3D                     # disable AMD 3DNow! asm paths (C fallbacks used)
)
if(MSVC)
    # /Zc:forScope- allows the old `for(INT i;...)` scoping the code relies on.
    add_compile_options(/W3 /EHsc /Zc:forScope- /MP /GF /wd4996 /w34840)
    add_compile_definitions(WIN32 _WINDOWS _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
else()
    # Clang/GCC (Linux/macOS): the codebase leans on pre-standard for-scope and
    # permissive conversions; keep it building without drowning in 1998 warnings.
    add_compile_options(-fpermissive -w -fno-strict-aliasing)
endif()

# An Unreal "package" module: a DLL exporting <NAME>_API symbols.
# Extra arguments are additional modules/libs to link against.
function(add_unreal_package NAME)
    file(GLOB _src CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/src/${NAME}/Src/*.cpp)
    add_library(${NAME} SHARED ${_src})
    string(TOUPPER ${NAME} _upper)
    target_compile_definitions(${NAME} PRIVATE
        "${_upper}_API=__declspec(dllexport)")
    if(EXISTS ${CMAKE_SOURCE_DIR}/src/${NAME}/Inc)
        target_include_directories(${NAME} PUBLIC ${CMAKE_SOURCE_DIR}/src/${NAME}/Inc)
    endif()
    target_include_directories(${NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src/${NAME}/Src)
    if(NOT NAME STREQUAL "Core")
        target_link_libraries(${NAME} PRIVATE Core)
    endif()
    target_link_libraries(${NAME} PRIVATE ${ARGN})
endfunction()

# Per-platform system libraries the portable modules need.
if(UNREAL_PLATFORM_WINDOWS)
    set(UNREAL_CORE_LIBS winmm)
    set(UNREAL_NET_LIBS  ws2_32)
    set(OPENGL_LIB       opengl32)
else()
    set(UNREAL_CORE_LIBS ${CMAKE_DL_LIBS})  # dlopen for dynamic package loading
    set(UNREAL_NET_LIBS  "")                # POSIX sockets are in libc
    find_package(OpenGL REQUIRED)
    set(OPENGL_LIB       OpenGL::GL)
endif()

# --- Portable engine core (targets all platforms) --------------------------
# Core still has a Windows-only platform layer (UnPlat.cpp: file IO, threads,
# GUID, DLL loading) pending the split into per-platform files — see
# docs/CROSSPLATFORM.md. The timing primitives are already portable.
add_unreal_package(Core ${UNREAL_CORE_LIBS})
add_unreal_package(Engine)
add_unreal_package(Fire   Engine)
add_unreal_package(Render Engine)
add_unreal_package(IpDrv  Engine ${UNREAL_NET_LIBS})

# --- Windows-only backends (being migrated to SDL / portable audio) --------
# Window (Win32 widget library) and the Editor shell are the remaining Win32
# surface targeted for SDL/ImGui replacement. Gated so a non-Windows configure
# omits them until their portable equivalents land.
if(UNREAL_PLATFORM_WINDOWS)
    add_unreal_package(Window Engine comctl32 winmm) # x64 port: winmm for timeBeginPeriod (frame cap)
    add_unreal_package(Editor Engine Window user32 oleaut32 comctl32) # ucc/-make script compiler
endif()

# Tracker-music replayer for the rebuilt Galaxy audio driver (MIT, vendored;
# renders the retail .umx S3M/IT modules). Exact source list from the
# project's own cmake/libxmp-sources.cmake.
set(XMPDIR ${CMAKE_SOURCE_DIR}/src/ThirdParty/libxmp-lite)
add_library(xmplite STATIC
    ${XMPDIR}/src/virtual.c      ${XMPDIR}/src/format.c      ${XMPDIR}/src/period.c
    ${XMPDIR}/src/player.c       ${XMPDIR}/src/read_event.c  ${XMPDIR}/src/misc.c
    ${XMPDIR}/src/dataio.c       ${XMPDIR}/src/lfo.c         ${XMPDIR}/src/scan.c
    ${XMPDIR}/src/control.c      ${XMPDIR}/src/filter.c      ${XMPDIR}/src/effects.c
    ${XMPDIR}/src/flow.c         ${XMPDIR}/src/mixer.c       ${XMPDIR}/src/mix_all.c
    ${XMPDIR}/src/load_helpers.c ${XMPDIR}/src/load.c        ${XMPDIR}/src/filetype.c
    ${XMPDIR}/src/hio.c          ${XMPDIR}/src/smix.c        ${XMPDIR}/src/memio.c
    ${XMPDIR}/src/rng.c          ${XMPDIR}/src/win32.c
    ${XMPDIR}/src/loaders/common.c   ${XMPDIR}/src/loaders/itsex.c
    ${XMPDIR}/src/loaders/sample.c   ${XMPDIR}/src/loaders/xm_load.c
    ${XMPDIR}/src/loaders/mod_load.c ${XMPDIR}/src/loaders/s3m_load.c
    ${XMPDIR}/src/loaders/it_load.c)
target_compile_definitions(xmplite PRIVATE
    LIBXMP_CORE_PLAYER LIBXMP_STATIC _USE_MATH_DEFINES)
target_include_directories(xmplite
    PUBLIC  ${XMPDIR}/include/libxmp-lite
    PRIVATE ${XMPDIR}/src)
if(MSVC)
    target_compile_options(xmplite PRIVATE /w)  # third-party C89: silence W3 noise
else()
    target_compile_options(xmplite PRIVATE -w)
endif()

# --- SDL windowing backend -------------------------------------------------
include(FetchContent)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.12
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

add_unreal_package(SDLDrv Engine SDL3::SDL3)
add_unreal_package(OpenGLDrv Engine ${OPENGL_LIB} SDL3::SDL3)
target_compile_definitions(OpenGLDrv PRIVATE UNREAL_USE_SDL=1)
# Portable audio (Phase 3): SDL3 audio on every platform (XAudio2 retired with
# the Windows-only stack 2026-08-01) -- see UNREAL_SDL_AUDIO in Galaxy.cpp.
add_unreal_package(Galaxy Engine SDL3::SDL3 xmplite)
target_compile_definitions(Galaxy PRIVATE UNREAL_USE_SDL=1)
# Stage SDL3's shared library next to the game binaries.
add_custom_command(TARGET SDLDrv POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL3::SDL3> ${CMAKE_SOURCE_DIR}/System)

# --- Dear ImGui (Phase 5: cross-platform editor UI) -------------------------
# Vendored like SDL via FetchContent (docking branch release: dockable panel
# layout for the editor frame). ImGui ships no CMakeLists — we define a small
# static lib with the SDL3 + OpenGL3 backends.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5-docking
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)
if(MSVC)
    target_compile_options(imgui PRIVATE /w)
else()
    target_compile_options(imgui PRIVATE -w)
endif()

# The ImGui editor frame (portable UnrealEd shell). Static lib linked
# into the SDL launcher; hooks into SDLDrv (events) and OpenGLDrv
# (post-render overlay), drives the editor engine via Exec strings.
add_library(EdGui STATIC ${CMAKE_SOURCE_DIR}/src/EdGui/Src/EdGui.cpp)
target_include_directories(EdGui PUBLIC ${CMAKE_SOURCE_DIR}/src/EdGui/Inc)
target_link_libraries(EdGui PUBLIC Core Engine SDLDrv OpenGLDrv imgui)

# The portable SDL launcher (Phase 4).
add_subdirectory(src/Launch)                # Unreal / Unreal.exe

# ---------------------------------------------------------------------------
# Packaging — assemble a clean, self-contained, runnable release folder.
#
#   cmake --build build --config Release            # 1. build the binaries
#   cmake --install build --config Release --prefix dist   # 2. stage the folder
#
# or in one step with the convenience target:
#   cmake --build build --config Release --target dist
#
# The result mirrors the layout the game runs from:
#   <prefix>/System   release binaries + script packages (.u) + localization
#                     (.int) + config (.ini)
#   <prefix>/content  Maps / Music / Sounds / Textures
# Build artifacts (.pdb/.lib/.exp/.log/Crash.txt) are deliberately excluded, so
# the folder is a minimal distributable copy that can be zipped and run as-is.
# ---------------------------------------------------------------------------
# Editor ships too: retail Editor.u (and three retail maps that import from
# it — Dug, NaliC, Ruins) need Editor.dll's intrinsic classes to load, and
# it enables `-make` script rebuilds from a dist. (x64 port)
set(UNREAL_RUNTIME_TARGETS
    Core Engine Fire Render IpDrv Galaxy OpenGLDrv SDLDrv Unreal)
# SDL3's own shared library is staged into System/ by the post-build copy
# above (dist packaging of SDL3 itself: TODO with Linux packaging).
if(UNREAL_PLATFORM_WINDOWS)
    list(APPEND UNREAL_RUNTIME_TARGETS Window Editor)
endif()

# Release binaries only: RUNTIME installs the .exe and .dll; ARCHIVE (import
# .lib) is intentionally omitted so no build-only artifacts ship.
install(TARGETS ${UNREAL_RUNTIME_TARGETS}
    RUNTIME DESTINATION System
    LIBRARY DESTINATION System)

# App-local MSVC runtime (msvcp140/vcruntime140*): shipped beside the exe so a
# clean Windows install runs the game without the VC++ redistributable. The
# UCRT (api-ms-win-crt-*) is in-box on Windows 10+ and intentionally not
# shipped. SDL3.dll is staged by the SDLDrv post-build copy.
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION System)
    include(InstallRequiredSystemLibraries)
endif()

# Runtime data that lives in the source System/ (retail script packages,
# localization, config). Match only these extensions so the co-located build
# outputs in System/ are never picked up.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/System/
    DESTINATION System
    FILES_MATCHING
        PATTERN "*.u"
        PATTERN "*.int"
        PATTERN "*.ini")

# Game content.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/content/ DESTINATION content)

# Convenience target: `--target dist` builds the binaries (via the dependency
# below) and stages them into <source>/dist for the active configuration. The
# output folder is wiped first so no stale files survive from a prior package.
add_custom_target(dist
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_SOURCE_DIR}/dist
    COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --config $<CONFIG> --prefix ${CMAKE_SOURCE_DIR}/dist
    COMMENT "Staging clean release into ${CMAKE_SOURCE_DIR}/dist"
    VERBATIM)
add_dependencies(dist ${UNREAL_RUNTIME_TARGETS})
