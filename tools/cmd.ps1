# Sends commands to the running probe (see src/probe/control.cpp), e.g.
#   tools\cmd.ps1 'pad A' 'wait 2000' 'shot menu'
# Waits for the previous batch to be picked up, then writes atomically via rename.
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)] [string[]]$Lines,
    [string]$File,
    [string]$Game = 'C:\Users\Virtual\hotd-vr-game'
)
$ErrorActionPreference = 'Stop'
if ($File) { $Lines = @(Get-Content $File) + @($Lines | Where-Object { $_ }) }
$hovr = Join-Path $Game 'hovr'
$cmd = Join-Path $hovr 'cmd.txt'
$tmp = Join-Path $hovr 'cmd.tmp'
$deadline = (Get-Date).AddSeconds(10)
while ((Test-Path $cmd) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
if (Test-Path $cmd) { throw 'previous commands were not picked up (is the game rendering?)' }
[IO.File]::WriteAllLines($tmp, $Lines, [Text.Encoding]::ASCII)
Move-Item $tmp $cmd -Force
