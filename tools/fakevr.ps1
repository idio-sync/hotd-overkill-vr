# Sends script lines to the fake SteamVR runtime (src/fakevr; installed with run.ps1 -OpenVR fake),
# e.g.  tools\fakevr.ps1 'head 0 1.65 0 30 0 0' 'wait 500' 'btn R trigger 150'
# Commands are listed at the top of src/fakevr/fakevr.cpp. Waits for the previous batch to be
# picked up (the runtime reads hovr\fakevr_cmd.txt from WaitGetPoses), then writes atomically.
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)] [string[]]$Lines,
    [string]$File,
    [string]$Game = 'C:\Users\Virtual\hotd-vr-game'
)
$ErrorActionPreference = 'Stop'
if ($File) { $Lines = @(Get-Content $File) + @($Lines | Where-Object { $_ }) }
$hovr = Join-Path $Game 'hovr'
$cmd = Join-Path $hovr 'fakevr_cmd.txt'
$tmp = Join-Path $hovr 'fakevr_cmd.tmp'
$deadline = (Get-Date).AddSeconds(10)
while ((Test-Path $cmd) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
if (Test-Path $cmd) { throw 'previous fakevr commands were not picked up (is the game calling WaitGetPoses?)' }
[IO.File]::WriteAllLines($tmp, $Lines, [Text.Encoding]::ASCII)
Move-Item $tmp $cmd -Force
