<#
.SYNOPSIS
    Build a clean Release of the Unreal x64 port and stage a self-contained,
    runnable folder.

.DESCRIPTION
    One command for a from-scratch release:
      1. wipes the CMake build tree (fresh configure -- no stale objects),
      2. configures + builds every runtime target in Release,
      3. stages the release binaries + script packages (.u) + localization
         (.int) + config (.ini) + content into <repo>\dist.

    The resulting <repo>\dist folder mirrors the layout the game runs from
    (System\ + content\) and contains no build artifacts, so it can be zipped
    and run on another machine as-is:  dist\System\Unreal.exe

.PARAMETER Prefix
    Output folder for the staged release. Default: <repo>\dist

.PARAMETER Config
    Build configuration to package. Default: Release

.PARAMETER KeepBuild
    Skip wiping the build tree (incremental build instead of clean).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\package-release.ps1
#>
[CmdletBinding()]
param(
    [string]$Prefix,
    [string]$Config = "Release",
    [switch]$KeepBuild
)

$ErrorActionPreference = "Stop"
$RepoRoot  = Split-Path -Parent $PSScriptRoot
$BuildDir  = Join-Path $RepoRoot "build"
if( -not $Prefix ) { $Prefix = Join-Path $RepoRoot "dist" }

# Locate cmake: PATH first, then the Visual Studio bundled copy.
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if( -not $cmake ) {
    $cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
if( -not (Test-Path $cmake) ) { throw "cmake not found on PATH or in the VS2022 install." }

Write-Host "== Unreal x64 port -- clean release packaging ==" -ForegroundColor Cyan
Write-Host "   cmake  : $cmake"
Write-Host "   config : $Config"
Write-Host "   output : $Prefix"

# 1. Clean the build tree for a true from-scratch build.
if( -not $KeepBuild -and (Test-Path $BuildDir) ) {
    Write-Host "-- wiping build tree $BuildDir"
    Remove-Item $BuildDir -Recurse -Force
}

# 2. Configure (x64) and build all runtime targets in the chosen config.
Write-Host "-- configuring"
& $cmake -S $RepoRoot -B $BuildDir -G "Visual Studio 17 2022" -A x64
if( $LASTEXITCODE -ne 0 ) { throw "cmake configure failed" }

Write-Host "-- building ($Config)"
& $cmake --build $BuildDir --config $Config
if( $LASTEXITCODE -ne 0 ) { throw "build failed" }

# 3. Stage the clean release folder.
Write-Host "-- staging release into $Prefix"
if( Test-Path $Prefix ) { Remove-Item $Prefix -Recurse -Force }
& $cmake --install $BuildDir --config $Config --prefix $Prefix
if( $LASTEXITCODE -ne 0 ) { throw "install/stage failed" }

$size = "{0:N0} MB" -f ((Get-ChildItem $Prefix -Recurse -File | Measure-Object Length -Sum).Sum/1MB)
Write-Host ""
Write-Host "== Done. Clean release staged ($size) ==" -ForegroundColor Green
Write-Host "   Run it with:  $Prefix\System\Unreal.exe"
