@echo off
REM ============================================================================
REM  build-dist.bat - Build the SDL (ZenUnreal) game and (re)create dist\.
REM
REM  Creates a dist\ folder and writes a self-contained runnable tree into it;
REM  if dist\ already exists its contents are overwritten. Contents:
REM    dist\System   = release DLLs + SDL3.dll + ZenUnreal.exe + *.u/*.int/*.ini
REM    dist\content  = Maps/Music/Sounds/Textures (mirrored from content\)
REM  Then runs the 32-check selftest.
REM
REM  Run:  build-dist.bat   (double-click, or from a terminal in the repo root)
REM  Needs Visual Studio 2022 and a configured build-sdl\ CMake tree
REM  (cmake -S . -B build-sdl -DUNREAL_USE_SDL=ON). Does NOT reconfigure CMake;
REM  if you edited CMakeLists.txt, run that cmake command once first.
REM ============================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

REM --- Locate MSBuild (common path, else vswhere) ---------------------------
set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "!VSWHERE!" for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe"`) do set "MSBUILD=%%i"
)
if not exist "%MSBUILD%" ( echo ERROR: MSBuild.exe not found. Edit the MSBUILD path near the top of this script. & exit /b 1 )
echo Using MSBuild: %MSBUILD%

REM --- Close a running game so its files aren't locked ----------------------
taskkill /f /im ZenUnreal.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul

REM --- Build every module ---------------------------------------------------
echo.
echo === Building modules (Release x64) ===
for %%M in (Core Engine Fire Render IpDrv Galaxy OpenGLDrv SDLDrv Editor Window WinDrv) do (
  echo   %%M
  "%MSBUILD%" "build-sdl\%%M.vcxproj" -p:Configuration=Release -p:Platform=x64 -m -nologo -clp:ErrorsOnly
  if errorlevel 1 ( echo. & echo BUILD FAILED: %%M & exit /b 1 )
)
echo   ZenUnreal.exe
"%MSBUILD%" "build-sdl\src\Launch\Unreal.vcxproj" -p:Configuration=Release -p:Platform=x64 -p:TargetName=ZenUnreal -p:BuildProjectReferences=false -m -nologo -clp:ErrorsOnly
if errorlevel 1 ( echo. & echo LINK FAILED & exit /b 1 )

REM --- Validate the source BEFORE touching the existing dist ----------------
REM (A crash can truncate System\ZenUnreal.ini to 0 bytes, and the .u script
REM  packages can drift out of System\ -- either produces a dist that fails at
REM  startup with ConfigNotFound / load-package errors. Refuse to build one.)
echo.
echo === Validating source ===
findstr /b /c:"GameEngine=" "System\ZenUnreal.ini" >nul 2>&1
if errorlevel 1 (
  echo ERROR: System\ZenUnreal.ini is missing/empty/invalid ^(no "GameEngine=" line^).
  echo        A crash may have truncated it. Restore a good ini before building a dist
  echo        ^(e.g. rebuild it from System\Unreal.ini^). Existing dist left untouched.
  exit /b 1
)
set "NU="
for /f %%C in ('dir /b "System\*.u" 2^>nul ^| find /c /v ""') do set "NU=%%C"
if not "%NU%"=="6" (
  echo ERROR: expected 6 script packages ^(*.u^) in System\, found "%NU%".
  echo        Restore them ^(git checkout, or rebuild via -make^) before building a dist.
  echo        Existing dist left untouched.
  exit /b 1
)
echo   ini OK ^(has GameEngine^), %NU% script packages present

REM --- Create / overwrite dist ----------------------------------------------
echo.
echo === Creating dist (overwriting if it exists) ===
REM Kill a game that may still hold dist\System, then wait for handles to release.
taskkill /f /im ZenUnreal.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul
if not exist "dist" mkdir "dist"
if exist "dist\System" rmdir /s /q "dist\System" 2>nul
if exist "dist\System" ( ping -n 3 127.0.0.1 >nul & rmdir /s /q "dist\System" 2>nul )
if not exist "dist\System" mkdir "dist\System"
REM Savegames write to SavePath=..\Save (relative to System); keep existing saves, create if absent.
if not exist "dist\Save" mkdir "dist\Save"

set "COPYERR="
for %%D in (Core Engine Fire Render IpDrv Galaxy OpenGLDrv SDLDrv Editor Window WinDrv) do (
  copy /y "System\%%D.dll" "dist\System\" >nul || set "COPYERR=1"
)
copy /y "System\SDL3.dll"     "dist\System\" >nul || set "COPYERR=1"
copy /y "System\ZenUnreal.exe" "dist\System\" >nul || set "COPYERR=1"
copy /y "System\*.u"          "dist\System\" >nul
copy /y "System\*.int"        "dist\System\" >nul
copy /y "System\ZenUnreal.ini" "dist\System\" >nul
if defined COPYERR ( echo. & echo ERROR: one or more System files failed to copy & exit /b 1 )

echo   copying content ...
if not exist "dist\content" mkdir "dist\content"
xcopy "%~dp0content" "%~dp0dist\content" /e /i /y /d /q >nul
if errorlevel 4 ( echo. & echo ERROR: content copy failed & exit /b 1 )

for /f %%C in ('dir /b "dist\System\*.dll" 2^>nul ^| find /c /v ""') do set "NDLL=%%C"
echo   staged !NDLL! DLLs + ZenUnreal.exe + content

REM --- Verify ---------------------------------------------------------------
echo.
echo === Selftest ===
pushd "dist\System"
".\ZenUnreal.exe" -selftest -log > "%TEMP%\up4_selftest.log" 2>&1
popd
findstr /C:"ALL PASSED" /C:"FAILED" "%TEMP%\up4_selftest.log" || echo (result not found; see %TEMP%\up4_selftest.log)

echo.
echo === DONE:  dist\System\ZenUnreal.exe ===
endlocal
exit /b 0
