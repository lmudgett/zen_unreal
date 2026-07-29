# Headless botmatch test harness (x64 port).
# Runs a dedicated-server deathmatch with bots for a fixed duration, then
# summarizes bot activity from the log. Use after editing bot scripts:
#   1. Unreal.exe -make -remake=UnrealI          (rebuild scripts)
#   2. scripts\botmatch-test.ps1 [-Map DmAriza.unr] [-Seconds 150]
# Requires [UnrealI.DeathMatchGame] bMultiPlayerBots=True and InitialBots>0
# in System\Unreal.ini (bots only auto-spawn on a dedicated server when
# bMultiPlayerBots is set).
#
# Author: Len Mudgett
param(
    [string]$Map = "DmAriza.unr",
    [int]$Seconds = 150
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$SystemDir = Join-Path $Root "System"
$Exe = Join-Path $SystemDir "Unreal.exe"
$LogFile = Join-Path $SystemDir "Unreal.log"

if (Test-Path $LogFile) { Remove-Item $LogFile }

Write-Output "Running headless botmatch: $Map for $Seconds seconds..."
$p = Start-Process -FilePath $Exe -ArgumentList "$Map`?Game=UnrealI.DeathMatchGame",'-server','-log' -WorkingDirectory $SystemDir -PassThru
Start-Sleep -Seconds $Seconds
$crashed = $p.HasExited
if (-not $crashed) { Stop-Process -Id $p.Id -Force } else { Write-Output "WARNING: server exited early (code $($p.ExitCode)) - check for Critical below" }

Start-Sleep -Seconds 1
$log = Get-Content $LogFile

$criticals = @($log | Select-String -SimpleMatch "Critical:")
$kills     = @($log | Select-String -SimpleMatch "KILL:")
$scores    = @($log | Select-String -SimpleMatch "SCORE:")
$warnings  = @($log | Select-String -SimpleMatch "Warning:")

Write-Output ""
Write-Output "=== Botmatch summary ==="
Write-Output "Kills logged : $($kills.Count)"
Write-Output "Criticals    : $($criticals.Count)"
Write-Output "Warnings     : $($warnings.Count)"
Write-Output ""
if ($kills.Count -gt 0) {
    Write-Output "--- kill feed ---"
    $kills | ForEach-Object { $_.Line -replace '^ScriptLog: ', '' }
}
if ($scores.Count -gt 0) {
    Write-Output "--- final scores (per game end) ---"
    $scores | ForEach-Object { $_.Line -replace '^ScriptLog: ', '' }
}
if ($criticals.Count -gt 0) {
    Write-Output "--- CRITICALS ---"
    $criticals | Select-Object -First 15 | ForEach-Object { $_.Line }
    exit 1
}
if ($kills.Count -eq 0) {
    Write-Output "NOTE: no kills logged - bots may not be spawning or fighting; check the log."
    exit 2
}
Write-Output ""
Write-Output "PASS: bots fought without crashing."
