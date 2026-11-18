# Installs House of the Dead: Overkill VR into The Typing of the Dead: Overkill (Steam), or removes it.
# A development and test tool, not in the release zip: players extract the zip into the game folder
# (README, "Install") and the mod writes hovr\hovr.ini itself at its first start.
#
#   powershell -ExecutionPolicy Bypass -File install_game.ps1 -Game "C:\Program Files (x86)\Steam\steamapps\common\Typing of the Dead Overkill"
#   powershell -ExecutionPolicy Bypass -File install_game.ps1 -Game "<same folder>" -Uninstall [-Purge]
#
# Adds to the game folder: XINPUT1_3.dll (the mod), openvr_api.dll (SteamVR's API) and hovr\
# (hovr.ini settings, hovr.log, installed.txt). Nothing else in the game folder or your save is
# changed. It checks first that HOTD_NG.exe is the one build the mod supports, and refuses to install
# over an XINPUT1_3.dll or openvr_api.dll that it did not put there. Uninstall removes exactly the
# files it installed (checked by hash, or by the mod's marker for XINPUT1_3.dll) and keeps
# hovr\hovr.ini and the log unless -Purge is given.
#
# Play: start SteamVR, then the game from Steam as usual. Holding Shift while the game starts plays
# flat (the mod does nothing). [vr] mode=off in hovr\hovr.ini turns VR off for good.
param(
    [Parameter(Mandatory)] [string]$Game,
    [switch]$Uninstall,
    [switch]$Purge,
    [string]$From   # where XINPUT1_3.dll / openvr_api.dll are (default: next to this script, else the repo's out\)
)
$ErrorActionPreference = 'Stop'

# The one supported build (Steam build 569304, RC14); src/mod/buildcheck.cpp checks the same file.
$KnownExeSize = 6345784
$KnownExeSha256 = 'F9DB505A3835AF7CEF136D4C2F9E2DF819698BD1183A033AF6B5A2F90275BB03'
$Files = @('XINPUT1_3.dll', 'openvr_api.dll')  # exact case: some Steam library drives are case-sensitive

if (-not $From) {
    $From = if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'XINPUT1_3.dll')) { $PSScriptRoot } else { Join-Path (Split-Path $PSScriptRoot -Parent) 'out' }
}
$Game = (Resolve-Path -LiteralPath $Game).Path
$exe = Join-Path $Game 'HOTD_NG.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "no HOTD_NG.exe in $Game (point -Game at the game's folder)" }
if (Get-Process HOTD_NG -ErrorAction SilentlyContinue) { throw 'close the game first' }

$hovr = Join-Path $Game 'hovr'
$record = Join-Path $hovr 'installed.txt'
if (-not $Uninstall -and (Get-Item -LiteralPath (Join-Path $From 'openvr_api.dll')).VersionInfo.ProductName -eq 'hovr fakevr') {
    throw "$From\openvr_api.dll is the test runtime (fakevr), not Valve's"
}

function Get-Sha256([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }

function Test-Ours([string]$path) {
    # The mod's DLL carries its log line about the supported build (every build of it has).
    $ascii = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($path))
    return $ascii.Contains('HOTD_NG.exe is not the supported build (Steam 569304)')
}

function Read-Record {
    $r = @{}
    if (Test-Path -LiteralPath $record) {
        foreach ($line in Get-Content -LiteralPath $record) {
            $parts = $line -split '\s+', 2
            if ($parts.Count -eq 2) { $r[$parts[1]] = $parts[0] }
        }
    }
    return $r
}

if ($Uninstall) {
    $installed = Read-Record
    foreach ($f in $Files) {
        $path = Join-Path $Game $f
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $hash = Get-Sha256 $path
        $byHash = $installed.ContainsKey($f) -and $installed[$f] -eq $hash
        $byMarker = $f -eq 'XINPUT1_3.dll' -and (Test-Ours $path)
        if ($byHash -or $byMarker) {
            Remove-Item -LiteralPath $path
            Write-Output "removed $f"
        } else {
            Write-Output "kept ${f}: not the file this installer put there"
        }
    }
    Remove-Item -LiteralPath $record -ErrorAction SilentlyContinue
    if ($Purge) {
        if (Test-Path -LiteralPath $hovr) { Remove-Item -LiteralPath $hovr -Recurse; Write-Output 'removed hovr\ (settings and log)' }
    } elseif (Test-Path -LiteralPath $hovr) {
        Write-Output 'kept hovr\ (your settings and the log; -Purge removes it)'
    }
    Write-Output "uninstalled from $Game"
    return
}

# Install.
$size = (Get-Item -LiteralPath $exe).Length
$sha = Get-Sha256 $exe
if ($size -ne $KnownExeSize -or $sha -ne $KnownExeSha256) {
    throw "HOTD_NG.exe in $Game is not the supported build (Steam build 569304): $size bytes, SHA-256 $sha. Nothing installed."
}
foreach ($f in $Files) {
    if (-not (Test-Path -LiteralPath (Join-Path $From $f))) { throw "missing $f in $From (build first: build.cmd)" }
}
if (-not (Test-Ours (Join-Path $From 'XINPUT1_3.dll'))) { throw "$From\XINPUT1_3.dll is not this mod's DLL" }
$installed = Read-Record
foreach ($f in $Files) {
    $path = Join-Path $Game $f
    if (-not (Test-Path -LiteralPath $path)) { continue }
    $hash = Get-Sha256 $path
    $ours = ($installed.ContainsKey($f) -and $installed[$f] -eq $hash) -or ($f -eq 'XINPUT1_3.dll' -and (Test-Ours $path)) -or
            ($hash -eq (Get-Sha256 (Join-Path $From $f)))
    if (-not $ours) {
        throw "$path is already there and is not from this mod (another mod?). Remove it yourself first; nothing installed."
    }
}
New-Item -ItemType Directory -Force $hovr | Out-Null
# The record (SHA-256 and name of each file) is written before the copies, so an interrupted install
# can still be uninstalled.
$lines = @($Files | ForEach-Object { "$(Get-Sha256 (Join-Path $From $_)) $_" })
[IO.File]::WriteAllLines($record, [string[]]$lines, [Text.Encoding]::ASCII)
foreach ($f in $Files) {
    Copy-Item -LiteralPath (Join-Path $From $f) (Join-Path $Game $f) -Force
    Write-Output "installed $f"
}

$cfg = Join-Path $hovr 'hovr.ini'
if (-not (Test-Path -LiteralPath $cfg)) {
    $default = @((Join-Path $From 'hovr.default.ini'), (Join-Path $PSScriptRoot 'hovr.default.ini')) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $default) { throw 'missing hovr.default.ini next to the installer' }
    Copy-Item -LiteralPath $default $cfg
    Write-Output "wrote default settings to $cfg (mode=openvr)"
} else {
    Write-Output "kept your settings in $cfg"
}
if (Test-Path -LiteralPath (Join-Path $hovr 'probe.ini')) {
    Write-Output "note: $hovr\probe.ini exists, so the mod runs its development harness there (save redirected into hovr\AppData). Delete probe.ini for normal play."
}
Write-Output "installed into $Game"
Write-Output 'Start SteamVR, then the game. Hold Shift while it starts to play without VR. Your save is not touched by the installer.'
