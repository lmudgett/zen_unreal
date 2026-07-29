# Full-system test suite (x64 port). Each subsystem is exercised in the most
# isolated form available:
#   1. Engine self-test  - Core, layout pins, translating loader over ALL
#                          retail content, WAV parser, script classes
#                          (no window/renderer/audio/gameplay).
#   2. Script compiler   - code-only remake of UnrealI in a throwaway sandbox.
#   3. Bot AI + physics  - headless dedicated-server botmatch (no rendering).
#   4. Renderer          - rendered botmatch: boot check, frame pacing, and a
#                          screenshot that must not be black.
#   5. Audio             - system peak meter sampled during the rendered run.
# Exit code = number of failed stages.
#
# Author: Len Mudgett
param([switch]$SkipCompiler)
$ErrorActionPreference = "Continue"
$Root = Split-Path -Parent $PSScriptRoot
$SystemDir = Join-Path $Root "System"
$Exe = Join-Path $SystemDir "Unreal.exe"
$LogFile = Join-Path $SystemDir "Unreal.log"
$Failed = 0

function Stage([string]$Name, [bool]$Ok, [string]$Detail) {
    if ($Ok) { Write-Output ("STAGE PASS: {0} {1}" -f $Name, $Detail) }
    else     { Write-Output ("STAGE FAIL: {0} {1}" -f $Name, $Detail); $script:Failed++ }
}

# --- 1. Engine self-test ----------------------------------------------------
Write-Output "=== [1/5] Engine self-test (isolated: no window/render/audio) ==="
if (Test-Path $LogFile) { Remove-Item $LogFile }
$p = Start-Process -FilePath $Exe -ArgumentList '-selftest','-log' -WorkingDirectory $SystemDir -PassThru
if (-not $p.WaitForExit(300000)) { Stop-Process -Id $p.Id -Force; Stage "engine-selftest" $false "(timeout)" }
else {
    $summary = (Select-String -Path $LogFile -Pattern 'SelfTest: =+ (ALL PASSED|FAILED)[^=]*' | Select-Object -Last 1).Matches.Value
    Stage "engine-selftest" ($p.ExitCode -eq 0) "($summary exit=$($p.ExitCode))"
    Select-String -Path $LogFile -Pattern 'SelfTest: FAIL' | ForEach-Object { Write-Output "  $($_.Line)" }
}

# --- 2. Script compiler round-trip -------------------------------------------
if ($SkipCompiler) {
    Write-Output "=== [2/5] Compiler: SKIPPED ==="
} else {
    Write-Output "=== [2/5] Script compiler (isolated sandbox remake of UnrealI) ==="
    $SB = Join-Path $env:TEMP "unreal-selftest-sandbox"
    if (Test-Path $SB) { Remove-Item $SB -Recurse -Force }
    New-Item -ItemType Directory -Force (Join-Path $SB "System") | Out-Null
    Copy-Item (Join-Path $SystemDir "*.dll") (Join-Path $SB "System")
    Copy-Item (Join-Path $SystemDir "Unreal.exe") (Join-Path $SB "System")
    Copy-Item (Join-Path $SystemDir "Unreal.ini") (Join-Path $SB "System")
    Copy-Item (Join-Path $SystemDir "*.int") (Join-Path $SB "System")
    Copy-Item (Join-Path $SystemDir "*.u") (Join-Path $SB "System")
    foreach ($pkg in @("Core","Engine","Editor","Fire","IpDrv","UnrealI")) {
        New-Item -ItemType Directory -Force (Join-Path $SB "src\$pkg") | Out-Null
        Copy-Item (Join-Path $Root "src\$pkg\Classes") (Join-Path $SB "src\$pkg\Classes") -Recurse
    }
    $p = Start-Process -FilePath (Join-Path $SB "System\Unreal.exe") -ArgumentList '-make','-remake=UnrealI','-log' -WorkingDirectory (Join-Path $SB "System") -PassThru
    if (-not $p.WaitForExit(300000)) { Stop-Process -Id $p.Id -Force; Stage "compiler" $false "(timeout)" }
    else {
        $elog = Join-Path $SB "System\Editor.log"
        $success = @(Select-String -Path $elog -Pattern 'Success: Compiled').Count
        $criticals = @(Select-String -Path $elog -Pattern 'Critical:').Count
        $warnings = @(Select-String -Path $elog -Pattern 'Warning').Count
        $uSize = (Get-Item (Join-Path $SB "System\UnrealI.u")).Length
        Stage "compiler" (($success -ge 1) -and ($criticals -eq 0) -and ($warnings -le 5) -and ($uSize -gt 30MB)) "(compiles=$success criticals=$criticals warnings=$warnings UnrealI.u=$([math]::Round($uSize/1MB,1))MB)"
    }
    Remove-Item $SB -Recurse -Force -ErrorAction SilentlyContinue
}

# --- 3. Headless bot AI + physics --------------------------------------------
Write-Output "=== [3/5] Bot AI + physics (headless dedicated server) ==="
$bm = powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "botmatch-test.ps1") -Seconds 90
$bm | ForEach-Object { Write-Output "  $_" }
Stage "headless-botmatch" ($LASTEXITCODE -eq 0) "(exit=$LASTEXITCODE)"

# --- 4 + 5. Renderer smoke + audio meter -------------------------------------
Write-Output "=== [4/5] Renderer + [5/5] audio (rendered botmatch) ==="
if (Test-Path $LogFile) { Remove-Item $LogFile }
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDevEnum { }
[Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDeviceEnumerator { int NotImpl1(); int GetDefaultAudioEndpoint(int f, int r, out IMMDevice d); }
[Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IMMDevice { int Activate(ref Guid iid, int ctx, IntPtr p, [MarshalAs(UnmanagedType.IUnknown)] out object o); }
[Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAudioMeterInformation { int GetPeakValue(out float p); }
public class Meter {
    public static float Peak() {
        var e = (IMMDeviceEnumerator)(new MMDevEnum()); IMMDevice d; e.GetDefaultAudioEndpoint(0, 0, out d);
        var iid = typeof(IAudioMeterInformation).GUID; object o; d.Activate(ref iid, 1, IntPtr.Zero, out o);
        float pk; ((IAudioMeterInformation)o).GetPeakValue(out pk); return pk;
    }
}
"@
$p = Start-Process -FilePath $Exe -ArgumentList 'DmAriza.unr?Game=UnrealI.DeathMatchGame','-log','-framestats' -WorkingDirectory $SystemDir -PassThru
Start-Sleep -Seconds 20
$peaks = @()
for ($i = 0; $i -lt 15; $i++) { $peaks += [Meter]::Peak(); Start-Sleep -Milliseconds 400 }
$shot = Join-Path $env:TEMP "unreal-selftest-shot.png"
$b = New-Object System.Drawing.Bitmap(1920,1080); $g = [System.Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen(0,0,0,0,$b.Size); $b.Save($shot,[System.Drawing.Imaging.ImageFormat]::Png)
# mean brightness of a sample grid
$sum = 0.0; $n = 0
for ($x = 40; $x -lt 1920; $x += 160) { for ($y = 40; $y -lt 1080; $y += 120) {
    $c = $b.GetPixel($x,$y); $sum += ($c.R + $c.G + $c.B)/3.0; $n++ } }
$bright = $sum / $n
$g.Dispose(); $b.Dispose()
Start-Sleep -Seconds 5
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Seconds 1
$booted = @(Select-String -Path $LogFile -Pattern 'Game engine initialized').Count
$criticals = @(Select-String -Path $LogFile -Pattern 'Critical:').Count
$avgLine = (Select-String -Path $LogFile -Pattern 'frames avg=([0-9.]+)ms' | Select-Object -Last 1)
$avgMs = 0.0
if ($avgLine) { $avgMs = [double]$avgLine.Matches.Groups[1].Value }
Stage "renderer-boot" (($booted -ge 1) -and ($criticals -eq 0)) "(booted=$booted criticals=$criticals)"
Stage "renderer-pacing" (($avgMs -gt 0) -and ($avgMs -lt 10)) "(avg=${avgMs}ms)"
Stage "renderer-picture" ($bright -gt 4) "(mean brightness=$([math]::Round($bright,1)))"
$pk = ($peaks | Measure-Object -Maximum).Maximum
Stage "audio-output" ($pk -gt 0.02) "(max peak=$([math]::Round($pk,3)))"

Write-Output ""
if ($Failed -eq 0) { Write-Output "=== ALL STAGES PASSED ===" } else { Write-Output "=== $Failed STAGE(S) FAILED ===" }
exit $Failed
