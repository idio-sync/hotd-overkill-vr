# Builds the release zip: out\dist\hotd-overkill-vr-<version>.zip, laid out to be extracted
# straight into the game folder (next to HOTD_NG.exe): the mod's DLL, Valve's openvr_api.dll, the
# README, the licence and the third-party notes, all at the top level. No hovr.ini (the mod writes
# the default settings at its first start, so extracting a newer zip over an old install keeps the
# player's settings) and no installer script (tools\install_game.ps1 stays a development tool). No
# game files: the mod reads the game's own assets at runtime and ships none of them. Next to it:
# hotd-overkill-vr-<version>-pdb.zip (XINPUT1_3.pdb, for crash dumps; not for players) and
# hotd-overkill-vr-<version>.sha256 (SHA-256 of both zips, sha256sum format).
#
#   powershell -File tools\package.ps1 [-NoBuild] [-Version 0.4.0]
param(
    [switch]$NoBuild,
    [string]$Version
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'out'

if (-not $NoBuild) {
    # Judge the build by its exit code: under Windows PowerShell 5.1 with 'Stop', any line a tool
    # writes to stderr (vcvars' own notes, compiler chatter) would otherwise abort the script.
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & cmd /c (Join-Path $root 'build.cmd') 2>&1 | ForEach-Object { "$_" } }
    finally { $ErrorActionPreference = $eap }
    if ($LASTEXITCODE -ne 0) { throw "build failed (build.cmd exit code $LASTEXITCODE)" }
}
if (-not $Version) {
    $hash = (& git -C $root rev-parse --short HEAD).Trim()
    $dirty = if (& git -C $root status --porcelain -- src tools CMakeLists.txt) { '-dirty' } else { '' }
    $Version = "m4-$(Get-Date -Format yyyyMMdd)-$hash$dirty"
}

$name = "hotd-overkill-vr-$Version"
$stage = Join-Path $out "dist\$name"
$zip = Join-Path $out "dist\$name.zip"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse }
New-Item -ItemType Directory -Force $stage | Out-Null

$files = [ordered]@{
    'XINPUT1_3.dll'  = Join-Path $out 'XINPUT1_3.dll'
    'openvr_api.dll' = Join-Path $out 'openvr_api.dll'
    'README.md'      = Join-Path $root 'README.md'
    'LICENSE'        = Join-Path $root 'LICENSE'
}
# Ship Valve's openvr_api.dll, never the test runtime (out\fakevr, ProductName 'hovr fakevr').
$real = Join-Path $root 'third_party\openvr\bin\win32\openvr_api.dll'
if ((Get-FileHash -LiteralPath $files['openvr_api.dll']).Hash -ne (Get-FileHash -LiteralPath $real).Hash -or
    (Get-Item -LiteralPath $files['openvr_api.dll']).VersionInfo.ProductName -eq 'hovr fakevr') {
    throw "$($files['openvr_api.dll']) is not Valve's openvr_api.dll ($real)"
}
foreach ($k in $files.Keys) {
    if (-not (Test-Path -LiteralPath $files[$k])) { throw "missing $($files[$k])" }
    Copy-Item -LiteralPath $files[$k] (Join-Path $stage $k)
}

$openvrLicense = Get-Content -LiteralPath (Join-Path $root 'third_party\openvr\LICENSE') -Raw
$notes = @"
House of the Dead: Overkill VR ($Version)
A VR mod for the House of the Dead: Overkill mode of The Typing of the Dead: Overkill (Steam app
246580, build 569304). You need your own copy of the game; this package contains no game files.

Install: extract this zip into the game's folder, next to HOTD_NG.exe (Steam > right-click The
Typing of the Dead: Overkill > Manage > Browse local files). The first start writes the settings
to hovr\hovr.ini. Uninstall: delete XINPUT1_3.dll, openvr_api.dll and the hovr folder.

Contents
- XINPUT1_3.dll     the mod (a stand-in XInput DLL the game loads from its own folder)
- openvr_api.dll    Valve's OpenVR API (32-bit 2.15.6), used to talk to SteamVR; licence below
- README.md         install, settings, controls, the headset checklist and known issues
- LICENSE           the mod's licence (MIT)
- LICENSES.txt      this file

Third-party licence: OpenVR SDK (openvr_api.dll)
-------------------------------------------------
$openvrLicense
"@
[IO.File]::WriteAllText((Join-Path $stage 'LICENSES.txt'), $notes, [Text.Encoding]::ASCII)

if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Output "packaged $zip"
Get-ChildItem -LiteralPath $stage | ForEach-Object { '  {0,-18} {1,10:N0} bytes' -f $_.Name, $_.Length }

# Debug symbols for the mod's DLL (crash dumps from players), kept out of the players' zip.
$pdb = Join-Path $out 'XINPUT1_3.pdb'
$pdbZip = Join-Path $out "dist\$name-pdb.zip"
if (Test-Path -LiteralPath $pdbZip) { Remove-Item -LiteralPath $pdbZip }
if (-not (Test-Path -LiteralPath $pdb)) { throw "missing $pdb (build RelWithDebInfo)" }
Compress-Archive -LiteralPath $pdb -DestinationPath $pdbZip
Write-Output "packaged $pdbZip"

$sums = Join-Path $out "dist\$name.sha256"
$lines = @($zip, $pdbZip | ForEach-Object { "$((Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLower())  $(Split-Path $_ -Leaf)" })
[IO.File]::WriteAllText($sums, ($lines -join "`n") + "`n", [Text.Encoding]::ASCII)
Write-Output "wrote $sums"
