# Checks a release zip from tools\package.ps1: exactly the expected files, all at the top level (the
# zip is extracted straight into the game folder; no hovr.ini, so an update never replaces the
# player's settings, and no installer script), Valve's openvr_api.dll (by hash, never the fakevr
# test runtime), the mod's XINPUT1_3.dll with the default settings it writes at its first start
# (tools\hovr.default.ini) built in, and the licence. Prints the file list with sizes and SHA-256;
# -Annotate also writes it as a GitHub Actions notice.
#
#   powershell -File tools\check_package.ps1 out\dist\hotd-overkill-vr-<version>.zip [-Annotate]
param(
    [Parameter(Mandatory)] [string]$Zip,
    [switch]$Annotate
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$expected = @('LICENSE', 'LICENSES.txt', 'openvr_api.dll', 'README.md', 'XINPUT1_3.dll')

Add-Type -AssemblyName System.IO.Compression.FileSystem
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("hovr-check-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $tmp | Out-Null
try {
    [IO.Compression.ZipFile]::ExtractToDirectory((Resolve-Path -LiteralPath $Zip).Path, $tmp)
    $entries = @(Get-ChildItem -LiteralPath $tmp -Recurse -File | ForEach-Object { $_.FullName.Substring($tmp.Length + 1) })
    $problems = @()
    $missing = @($expected | Where-Object { $entries -notcontains $_ })
    $extra = @($entries | Where-Object { $expected -notcontains $_ })
    if ($missing) { $problems += "missing: $($missing -join ', ')" }
    if ($extra) { $problems += "unexpected: $($extra -join ', ')" }

    $ovr = Join-Path $tmp 'openvr_api.dll'
    $valve = Join-Path $root 'third_party\openvr\bin\win32\openvr_api.dll'
    if (Test-Path -LiteralPath $ovr) {
        if ((Get-Item -LiteralPath $ovr).VersionInfo.ProductName -eq 'hovr fakevr') { $problems += 'openvr_api.dll is the fakevr test runtime' }
        if ((Get-FileHash -LiteralPath $ovr).Hash -ne (Get-FileHash -LiteralPath $valve).Hash) { $problems += "openvr_api.dll differs from Valve's ($valve)" }
    }
    $mod = Join-Path $tmp 'XINPUT1_3.dll'
    if (Test-Path -LiteralPath $mod) {
        $ascii = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($mod))
        if (-not $ascii.Contains('HOTD_NG.exe is not the supported build (Steam 569304)')) { $problems += 'XINPUT1_3.dll is not the mod' }
        # The first-run settings: tools\hovr.default.ini as the DLL carries it (LF line ends).
        $ini = ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'hovr.default.ini'))) -replace "`r`n", "`n"
        if (-not $ascii.Contains($ini)) { $problems += 'XINPUT1_3.dll does not carry tools\hovr.default.ini (the first-run settings)' }
    }
    $lic = Join-Path $tmp 'LICENSE'
    if ((Test-Path -LiteralPath $lic) -and -not (Get-Content -LiteralPath $lic -Raw).StartsWith('MIT License')) { $problems += 'LICENSE is not the MIT licence' }

    $list = @(Get-ChildItem -LiteralPath $tmp -File | Sort-Object Name | ForEach-Object {
        '{0,-18} {1,10} bytes  {2}' -f $_.Name, $_.Length, (Get-FileHash -LiteralPath $_.FullName).Hash.Substring(0, 16)
    })
    Write-Output "$(Split-Path $Zip -Leaf):"
    $list | ForEach-Object { "  $_" }
    if ($Annotate) {
        # One annotation, lines joined with %0A (the workflow-command newline).
        $msg = ($list | ForEach-Object { $_ -replace '\s+', ' ' }) -join '%0A'
        Write-Output "::notice title=$(Split-Path $Zip -Leaf)::$msg"
    }
    if ($problems) {
        $problems | ForEach-Object { Write-Output "package check FAILED: $_" }
        exit 1
    }
    Write-Output 'package check passed (exact top-level file list, Valve''s openvr_api.dll, the mod with its default settings, MIT licence)'
}
finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
