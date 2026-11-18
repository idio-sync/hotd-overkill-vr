# Headset-view captures from the fake SteamVR runtime: runs script lines, then `capture NAME`, waits
# for it and writes NAME_pair.png (both eyes' composited views with the ground-truth marks, side by
# side, at -Size pixels per eye). Files land in <game>\hovr\fakevr\ (see src/fakevr/compositor.cpp):
# NAME_view0/1 (what the wearer sees), NAME_truth0/1 (plus controller rays, horizon, panel
# outlines), NAME_submit0/1 (the eye images as submitted), NAME_ov_<key> (overlay textures),
# NAME.txt (poses, overlays, pixel positions of the controller rays).
#   tools\fakevr_shot.ps1 turn30 'head 0 1.65 0 30 0 0' 'wait 300'
param(
    [Parameter(Position = 0, Mandatory = $true)] [string]$Name,
    [Parameter(Position = 1, ValueFromRemainingArguments = $true)] [string[]]$Lines,
    [string]$Game = 'C:\Users\Virtual\hotd-vr-game',
    [int]$Size = 900,
    [switch]$Plain  # pair the plain views instead of the truth images
)
$ErrorActionPreference = 'Stop'
$dir = Join-Path $Game 'hovr\fakevr'
$done = Join-Path $dir "$Name.done"
Remove-Item $done -ErrorAction SilentlyContinue
$all = @($Lines | Where-Object { $_ }) + "capture $Name"
& (Join-Path $PSScriptRoot 'fakevr.ps1') -Game $Game -Lines $all
$deadline = (Get-Date).AddSeconds(120)
while (-not (Test-Path $done) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
if (-not (Test-Path $done)) { throw "no capture $Name (is the game in mode=openvr with the fake runtime?)" }
$kind = if ($Plain) { 'view' } else { 'truth' }
$py = Join-Path $PSScriptRoot '..\.venv\Scripts\python.exe'
$a = Join-Path $dir "${Name}_${kind}0.png"
$b = Join-Path $dir "${Name}_${kind}1.png"
$out = Join-Path $dir "${Name}_pair.png"
& $py -I -c "import sys; from PIL import Image; a=Image.open(sys.argv[1]).convert('RGB'); b=Image.open(sys.argv[2]).convert('RGB'); s=int(sys.argv[4]); h=round(a.height*s/a.width); im=Image.new('RGB',(2*s+8,h),(40,40,40)); im.paste(a.resize((s,h),Image.LANCZOS),(0,0)); im.paste(b.resize((s,h),Image.LANCZOS),(s+8,0)); im.save(sys.argv[3])" $a $b $out $Size
Write-Output $out
