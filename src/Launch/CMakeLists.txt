# Unreal / Unreal.exe — the portable SDL launcher (cross-platform Phase 4):
# plain main() + engine tick loop; no Window package (log tees to stdout,
# SDLClient pumps SDL events inside Engine->Tick). Console subsystem on
# Windows so the stdout log is visible.
set(_launch_src Src/SDLLaunch.cpp Src/SelfTest.cpp)
if(UNREAL_PLATFORM_WINDOWS)
    # Crash reporter + icon resource are Windows-specific extras.
    list(APPEND _launch_src Src/LaunchCrash.cpp Src/Res/LaunchRes.rc)
endif()
add_executable(Unreal ${_launch_src})
target_link_libraries(Unreal PRIVATE Core Engine EdGui)
target_compile_definitions(Unreal PRIVATE UNREAL_WITH_EDGUI=1)
if(UNREAL_PLATFORM_WINDOWS)
    target_link_libraries(Unreal PRIVATE winmm) # timeBeginPeriod for the frame cap
endif()
target_include_directories(Unreal PRIVATE Src)
set_target_properties(Unreal PROPERTIES OUTPUT_NAME "Unreal")
