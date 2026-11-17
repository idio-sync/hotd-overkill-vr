# Starts or stops the test copy of the game with the probe installed. Its PID goes to
# hovr\pid.txt so -Stop only ever ends the process this script started.
param(
    [string]$Game = 'C:\Users\Virtual\hotd-vr-game',
    [switch]$Stop,
    [switch]$Normal,  # start with a normal window instead of minimized
    [ValidateSet('', 'off', 'sim', 'openvr')] [string]$Mode = ''  # hovr.ini [vr] mode for this run (kept)
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
            Wait-Process -Id $id -Timeout 60 -ErrorAction SilentlyContinue
            Write-Output "stopped $id"
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
# openvr_api.dll (delay-loaded; only used with mode=openvr)
$ovr = Join-Path $PSScriptRoot '..\out\openvr_api.dll'
if (Test-Path $ovr) { Copy-Item $ovr (Join-Path $Game 'openvr_api.dll') -Force }

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
if ($Mode) {
    $lines = @(Get-Content $vrIni | Where-Object { $_ -notmatch '^\s*mode\s*=' })
    $i = [array]::IndexOf($lines, '[vr]')
    if ($i -lt 0) { $lines = @('[vr]') + $lines; $i = 0 }
    $rest = if ($i + 1 -lt $lines.Count) { $lines[($i + 1)..($lines.Count - 1)] } else { @() }
    Set-Content $vrIni (@($lines[0..$i]) + "mode=$Mode" + @($rest)) -Encoding ASCII
}
Remove-Item (Join-Path $hovr 'status.txt'), (Join-Path $hovr 'cmd.txt') -ErrorAction SilentlyContinue
Get-ChildItem $hovr -File | Where-Object Name -match '^(shot_\d+|frame_\d+)\.(png|txt)$' | Remove-Item

$style = if ($Normal -or -not $minimized) { 'Normal' } else { 'Minimized' }
$p = Start-Process (Join-Path $Game 'HOTD_NG.exe') -WorkingDirectory $Game -WindowStyle $style -PassThru
Set-Content $pidFile $p.Id
Write-Output "started PID $($p.Id)"
