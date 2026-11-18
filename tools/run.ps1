# Starts or stops the test copy of the game with the probe installed. Its PID goes to
# hovr\pid.txt so -Stop only ever ends the process this script started.
param(
    [string]$Game = 'C:\Users\Virtual\hotd-vr-game',
    [switch]$Stop,
    [switch]$Normal,  # start with a normal window instead of minimized
    [switch]$NoStart, # install the DLLs and settings (-Mode, -OpenVR) without starting the game
    [ValidateSet('', 'off', 'sim', 'openvr')] [string]$Mode = '',  # hovr.ini [vr] mode for this run (kept)
    # Which openvr_api.dll the test copy gets (kept until changed): 'fake' = the fake SteamVR runtime
    # (out\fakevr, src/fakevr: tests without a headset; never starts SteamVR), 'real' = Valve's
    # (out\openvr_api.dll, for a headset session). Omitted: the one there stays. mode=openvr only
    # starts with the fake one installed, or with -OpenVR real given in the same call.
    [ValidateSet('', 'fake', 'real')] [string]$OpenVR = ''
)
$ErrorActionPreference = 'Stop'
$hovr = Join-Path $Game 'hovr'
$pidFile = Join-Path $hovr 'pid.txt'

if ($Stop) {
    if (Test-Path $pidFile) {
        $id = [int](Get-Content $pidFile)
        $p = Get-Process -Id $id -ErrorAction SilentlyContinue
        if ($p -and $p.ProcessName -eq 'HOTD_NG') {
            Stop-Process -Id $id -Force
            $deadline = (Get-Date).AddSeconds(60)
            while ((Get-Process -Id $id -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
            Write-Output $(if (Get-Process -Id $id -ErrorAction SilentlyContinue) { "process $id still exiting" } else { "stopped $id" })
        }
        else { Write-Output "process $id is not running" }
        Remove-Item $pidFile
    } else { Write-Output 'no pid file' }
    return
}

if ($Game -like '*steamapps*') { throw 'refusing to run in the Steam folder; use the test copy' }
$running = Get-Process HOTD_NG -ErrorAction SilentlyContinue
if ($running) { throw "HOTD_NG is already running (PID $($running.Id -join ', '))" }

New-Item -ItemType Directory -Force $hovr | Out-Null
Copy-Item (Join-Path $PSScriptRoot '..\out\XINPUT1_3.dll') (Join-Path $Game 'XINPUT1_3.dll') -Force
# openvr_api.dll (delay-loaded; only used with mode=openvr): the fake runtime or Valve's, on request.
$gameOvr = Join-Path $Game 'openvr_api.dll'
function Test-FakeVR([string]$path) {
    (Test-Path $path) -and ((Get-Item $path).VersionInfo.ProductName -eq 'hovr fakevr')
}
if ($OpenVR) {
    $src = if ($OpenVR -eq 'fake') { Join-Path $PSScriptRoot '..\out\fakevr\openvr_api.dll' } else { Join-Path $PSScriptRoot '..\out\openvr_api.dll' }
    if (-not (Test-Path $src)) { throw "missing $src (build.cmd)" }
    if (($OpenVR -eq 'fake') -ne (Test-FakeVR $src)) { throw "$src is not the $OpenVR openvr_api.dll" }
    Copy-Item $src $gameOvr -Force
    Write-Output "installed the $OpenVR openvr_api.dll"
}

# The probe redirects %APPDATA% to hovr\AppData; seed it once with a copy of the real save.
$save = Join-Path $hovr 'AppData\sega\typing of the dead overkill'
if (-not (Test-Path $save)) {
    New-Item -ItemType Directory -Force $save | Out-Null
    Copy-Item (Join-Path $env:APPDATA 'sega\typing of the dead overkill\*') $save -Exclude 'steam_autocloud.vdf'
    Write-Output "seeded $save from the real save"
}

$ini = Join-Path $hovr 'probe.ini'
if (-not (Test-Path $ini)) {
    Set-Content $ini "[probe]`r`nminimized=1`r`nmute=1`r`nwindowed=1`r`nfake_pad=1`r`ncap_fps=0`r`n" -Encoding ASCII
}
$minimized = (Get-Content $ini) -match '^minimized=1'

# The mod's own settings: hovr\hovr.ini [vr] (see src/mod/config.cpp). -Mode sets its mode line.
$vrIni = Join-Path $hovr 'hovr.ini'
if (-not (Test-Path $vrIni)) { Set-Content $vrIni "[vr]`r`nmode=off`r`n" -Encoding ASCII }
# mode=openvr with Valve's openvr_api.dll starts SteamVR: only with the fake runtime, or when this
# very call asked for the real one (-OpenVR real, a headset session).
$effective = $Mode
if (-not $effective) {
    $m = Get-Content $vrIni | Select-String '^\s*mode\s*=\s*(\w+)' | Select-Object -Last 1
    $effective = if ($m) { $m.Matches[0].Groups[1].Value } else { 'off' }
}
if ($effective -eq 'openvr' -and -not (Test-FakeVR $gameOvr) -and $OpenVR -ne 'real') {
    throw "refusing mode=openvr: $gameOvr is not the fake runtime (it would start SteamVR). Use -OpenVR fake for tests, or -OpenVR real for a headset session."
}
if ($Mode) {
    $lines = @(Get-Content $vrIni | Where-Object { $_ -notmatch '^\s*mode\s*=' })
    $i = [array]::IndexOf($lines, '[vr]')
    if ($i -lt 0) { $lines = @('[vr]') + $lines; $i = 0 }
    $rest = if ($i + 1 -lt $lines.Count) { $lines[($i + 1)..($lines.Count - 1)] } else { @() }
    Set-Content $vrIni (@($lines[0..$i]) + "mode=$Mode" + @($rest)) -Encoding ASCII
}
Remove-Item (Join-Path $hovr 'status.txt'), (Join-Path $hovr 'cmd.txt'), (Join-Path $hovr 'fakevr_cmd.txt') -ErrorAction SilentlyContinue
Get-ChildItem $hovr -File | Where-Object Name -match '^(shot_\d+|frame_\d+)\.(png|txt)$' | Remove-Item
if ($NoStart) {
    Write-Output "installed (mode=$effective, openvr_api.dll: $(if (Test-FakeVR $gameOvr) { 'fake' } elseif (Test-Path $gameOvr) { 'real' } else { 'none' })); not started"
    return
}

$style = if ($Normal -or -not $minimized) { 'Normal' } else { 'Minimized' }
$p = Start-Process (Join-Path $Game 'HOTD_NG.exe') -WorkingDirectory $Game -WindowStyle $style -PassThru
Set-Content $pidFile $p.Id
Write-Output "started PID $($p.Id) (mode=$effective$(if ($effective -eq 'openvr') { if (Test-FakeVR $gameOvr) { ', fake SteamVR runtime' } else { ', REAL SteamVR' } }))"
