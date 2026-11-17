# Runs probe commands, then saves a screenshot NAME.png and a half-size NAME_small.png.
#   tools\step.ps1 NAME 'cursor 330 686' 'click' 'wait 2000'
param(
    [Parameter(Position = 0, Mandatory = $true)] [string]$Name,
    [Parameter(Position = 1, ValueFromRemainingArguments = $true)] [string[]]$Lines,
    [string]$Game = 'C:\Users\Virtual\hotd-vr-game'
)
$ErrorActionPreference = 'Stop'
$hovr = Join-Path $Game 'hovr'
$png = Join-Path $hovr "$Name.png"
Remove-Item $png -ErrorAction SilentlyContinue
$all = @($Lines | Where-Object { $_ }) + "shot $Name"
& (Join-Path $PSScriptRoot 'cmd.ps1') -Game $Game -Lines $all
$deadline = (Get-Date).AddSeconds(60)
while (-not (Test-Path $png) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
if (-not (Test-Path $png)) { throw "no screenshot $Name" }
Start-Sleep -Milliseconds 300
$py = Join-Path $PSScriptRoot '..\.venv\Scripts\python.exe'
& $py -I -c "import sys; from PIL import Image; im=Image.open(sys.argv[1]).convert('RGB'); im.resize((im.width//2, im.height//2)).save(sys.argv[2])" $png (Join-Path $hovr "${Name}_small.png")
Write-Output (Join-Path $hovr "${Name}_small.png")
