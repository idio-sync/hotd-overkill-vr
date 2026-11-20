# Drives the gun accuracy test (docs/superpowers/specs/2026-11-19-gun-accuracy-design.md) in the
# TEST COPY with the fake SteamVR runtime: firing scenarios per weapon under `vr shotlog`, read
# afterwards by tools/gun_accuracy.py. Measuring only: nothing here changes how the game plays,
# except the dev-only weapon unlock below (memory only, never written by this script to a save).
#
# Phases (one game session: boot, loadout, play; then stop):
#   -Phase boot      run.ps1 -Mode openvr -OpenVR fake; at the main menu every weapon is made
#                    available and purchased in player 1's profile in memory (profile =
#                    [[0x99B748]+0x228]; +0x5C available mask, +0x60 purchased mask, bit = weapon
#                    id: 1 magnum, 2 hand cannon, 4 pump, 5 auto shotgun, 6 SMG, 8 rifle, 9/10
#                    crossbow and Gwendolyn, 11 minigun - the ids the game's own "Unlock Everything"
#                    script function unlocks, 0x424450), then menus to Naked Terror's loadout screen;
#                    a capture of it: <game>\hovr\gunacc_loadout_screen.png.
#   -Phase loadout   -Clicks 'R1,R1,R2,L2' : arrow clicks on the loadout (R/L = right/left arrow,
#                    1/2 = the slot), then a new capture. Repeat until the two weapons are the ones wanted.
#   -Phase play      -Weapons 'magnum,pump' (slot 1, slot 2 as on the loadout; one is fine): Start,
#                    cutscenes skipped, `vr god 1`, `vr shotlog 1`; per weapon (B switches to the
#                    second): still (neutral and edge pose, and held on a zombie found by `vr
#                    handseek`), the mesh check, sweeps at ~60 and ~180 deg/s, then turn (firing
#                    on through the level, re-aimed at zombies so it moves on). Each under its own
#                    `vr shotlog tag`. Waits for the end marker in the log.
#   -Phase stop      run.ps1 -Stop (this script's game only, by PID).
#
#   powershell -ExecutionPolicy Bypass -File tools\gun_accuracy.ps1 -Phase boot
#   ... -Phase loadout -Clicks 'R2,R2'      (check hovr\gunacc_loadout_screen.png)
#   ... -Phase play -Weapons 'magnum,pump'
#   ... -Phase stop
#   .venv\Scripts\python -I tools\gun_accuracy.py C:\Users\Virtual\hotd-vr-game\hovr\probe.log --summary
# The test copy's save is not written by this script; the game may write it (back it up first).
param(
    [ValidateSet('boot', 'loadout', 'play', 'stop')] [string]$Phase = 'boot',
    [string]$Clicks = '',
    [string]$Weapons = '',
    [double]$TurnSeconds = 120,   # per weapon
    [int]$StillShots = 30,        # trigger pulls per still pose
    [string]$Scenarios = 'still,sweep,turn',  # play: which scenario groups
    [switch]$InLevel,             # play: the level is already running (no Start, no cutscene skips)
    [string]$Game = 'C:\Users\Virtual\hotd-vr-game'
)
$ErrorActionPreference = 'Stop'
$hovr = Join-Path $Game 'hovr'
$probeLog = Join-Path $hovr 'probe.log'
if ($Game -like '*steamapps*') { throw 'refusing to run in the Steam folder; use the test copy' }
if (-not (Test-Path -LiteralPath (Join-Path $hovr 'probe.ini'))) { throw "$hovr\probe.ini is missing: without the development harness the game would use the real save" }

function Read-Log {
    if (-not (Test-Path -LiteralPath $probeLog)) { return @() }
    $fs = [IO.File]::Open($probeLog, 'Open', 'Read', 'ReadWrite, Delete')
    try { $text = (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
    return @($text -split "`r?`n")
}
function Get-GamePid {
    $f = Join-Path $hovr 'pid.txt'
    if (-not (Test-Path -LiteralPath $f)) { return 0 }
    $id = [int](Get-Content -LiteralPath $f)
    if (Get-Process -Id $id -ErrorAction SilentlyContinue) { return $id } else { return 0 }
}
# Waits for a log line matching $regex after $after earlier matches; returns it or throws.
function Wait-Log([string]$regex, [int]$seconds, [int]$after = 0) {
    $deadline = (Get-Date).AddSeconds($seconds)
    do {
        $m = @(Read-Log | Where-Object { $_ -match $regex })
        if ($m.Count -gt $after) { return $m[-1] }
        if (-not (Get-GamePid)) { throw 'the game is not running' }
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)
    throw "timed out waiting for /$regex/"
}
function Send-Mod([string[]]$lines) { & (Join-Path $PSScriptRoot 'cmd.ps1') -Game $Game -Lines $lines }
$script:marker = 0
# Sends lines followed by a unique log marker and waits for it.
function Invoke-Mod([string[]]$lines, [int]$seconds) {
    $script:marker++
    $m = "gunacc mark $PID-$script:marker"
    Send-Mod (@($lines) + "log $m")
    Wait-Log ([regex]::Escape("log: $m")) $seconds | Out-Null
}
function Click([int]$x, [int]$y) { @("fakevr aimov R hovr.screen $x $y", 'wait 300', 'fakevr btn R trigger 150') }

# Fire styles: semi = one trigger pull per $ms, $mag pulls then a reload; auto = hold the trigger
# $ms, release, reload.
$styles = @{
    magnum      = @{ kind = 'semi'; ms = 250; mag = 6 }
    gold_magnum = @{ kind = 'semi'; ms = 250; mag = 6 }
    handcannon  = @{ kind = 'semi'; ms = 450; mag = 5 }
    pump        = @{ kind = 'semi'; ms = 600; mag = 6 }
    autoshotgun = @{ kind = 'semi'; ms = 250; mag = 8 }
    crossbow    = @{ kind = 'semi'; ms = 700; mag = 5 }
    smg         = @{ kind = 'auto'; ms = 2000 }
    rifle       = @{ kind = 'semi'; ms = 350; mag = 10 }  # a 3-round burst per pull
    minigun     = @{ kind = 'auto'; ms = 3000 }
}
$reload = @('fakevr release R trigger', 'fakevr btn R a 150', 'wait 1800')
# About $seconds of firing in the weapon's style (reloads included).
function Fire([hashtable]$s, [double]$seconds) {
    $l = @()
    $t = 0
    while ($t -lt $seconds * 1000) {
        if ($s.kind -eq 'auto') {
            $l += 'fakevr hold R trigger', "wait $($s.ms)"
            $t += $s.ms
        } else {
            for ($k = 0; $k -lt $s.mag; $k++) { $l += 'fakevr btn R trigger 60', "wait $($s.ms)"; $t += $s.ms }
        }
        $l += $reload
        $t += 1950
    }
    return $l
}
# Trigger pulls while the controller sweeps between yaw -25 and +25 at $degs deg/s ($n legs).
function Sweep([hashtable]$s, [double]$degs, [int]$legs) {
    $pose = '0.18 1.40 -0.35'
    $ms = [int](50 / $degs * 1000)
    $l = @("fakevr ctl R $pose -25 -5 0", 'wait 300')
    for ($i = 0; $i -lt $legs; $i++) {
        $to = if ($i % 2 -eq 0) { 25 } else { -25 }
        $l += "fakevr ctl R $pose $to -5 0 $ms"
        if ($s.kind -eq 'auto') {
            $l += 'fakevr hold R trigger', "wait $ms"
            if ($i % 4 -eq 3) { $l += $reload }
        } else {
            $step = [math]::Min($s.ms, [math]::Max(120, [int]($ms / 3)))
            for ($w = 0; $w + $step -le $ms; $w += $step) { $l += 'fakevr btn R trigger 60', "wait $step" }
            if ($i % 2 -eq 1) { $l += $reload }  # between legs, with the controller still
        }
    }
    $l += 'fakevr release R trigger', $reload
    return $l
}

switch ($Phase) {
    'stop' {
        & (Join-Path $PSScriptRoot 'run.ps1') -Game $Game -Stop
    }
    'boot' {
        if (Get-GamePid) { throw 'the test copy is already running (run -Phase stop first)' }
        Remove-Item -LiteralPath $probeLog -ErrorAction SilentlyContinue
        & (Join-Path $PSScriptRoot 'run.ps1') -Game $Game -Mode openvr -OpenVR fake
        Wait-Log 'vr: OpenVR up' 120 | Out-Null
        Invoke-Mod @('wait 30000', 'log gunacc main menu', 'peek [va:0x99B748]+0x228 1') 120
        $lines = Read-Log
        $i = [array]::LastIndexOf($lines, ($lines | Where-Object { $_ -match 'cmd: peek \[va:0x99B748\]\+0x228' } | Select-Object -Last 1))
        if ($lines[$i + 1] -notmatch '^\[.*\]\s+([0-9A-Fa-f]{8}): ([0-9a-f]{8})') { throw "no profile pointer in the log: $($lines[$i + 1])" }
        $prof = [Convert]::ToUInt32($Matches[2], 16)
        if ($prof -eq 0) { throw 'no profile yet' }
        $masks = '0x{0:X8}' -f ($prof + 0x5C)
        Invoke-Mod @("peek $masks 2") 30
        $lines = Read-Log
        $j = [array]::LastIndexOf($lines, ($lines | Where-Object { $_ -match "cmd: peek $masks 2" } | Select-Object -Last 1))
        $avail = [Convert]::ToUInt32(($lines[$j + 1] -replace '^\[.*\]\s+[0-9A-Fa-f]{8}: ([0-9a-f]{8}).*$', '$1'), 16)
        $bought = [Convert]::ToUInt32(($lines[$j + 2] -replace '^\[.*\]\s+[0-9A-Fa-f]{8}: ([0-9a-f]{8}).*$', '$1'), 16)
        $all = 0xF76
        Write-Output ("profile 0x{0:X8}: available 0x{1:X} purchased 0x{2:X} -> both 0x{3:X} (memory only)" -f $prof, $avail, $bought, ($avail -bor $bought -bor $all))
        Invoke-Mod @(("poke $masks 0x{0:X}" -f ($avail -bor $all)), ("poke 0x{0:X8} 0x{1:X}" -f ($prof + 0x60), ($bought -bor $all)), "peek $masks 2",
            'log gunacc weapons unlocked in memory') 30
        # Main menu -> House of the Dead: Overkill -> Story -> Naked Terror (the loadout screen).
        Invoke-Mod (@(Click 1524 1010) + 'wait 4000' + (Click 310 1018) + 'wait 4000' + (Click 958 1016) + 'wait 4000' + (Click 1170 888) +
            'wait 2500' + 'vr handoff gunacc_loadout') 120
        Write-Output "at the loadout: $hovr\gunacc_loadout_screen.png"
    }
    'loadout' {
        $l = @()
        foreach ($c in ($Clicks -split ',' | Where-Object { $_ })) {
            if ($c -notmatch '^([LR])([12])$') { throw "bad click '$c' (R1, L1, R2, L2)" }
            $x = if ($Matches[1] -eq 'R') { 1168 } else { 742 }
            $y = if ($Matches[2] -eq '1') { 547 } else { 889 }
            $l += (Click $x $y) + 'wait 900'
        }
        Invoke-Mod ($l + 'wait 500' + 'vr handoff gunacc_loadout') 120
        Write-Output "loadout capture: $hovr\gunacc_loadout_screen.png"
    }
    'play' {
        $list = @($Weapons -split ',' | Where-Object { $_ })
        foreach ($w in $list) { if (-not $styles.ContainsKey($w)) { throw "unknown weapon '$w' ($($styles.Keys -join ', '))" } }
        $want = @($Scenarios -split ',' | Where-Object { $_ })
        if (-not $InLevel) {
            $before = @(Read-Log | Where-Object { $_ -match 'stereo: gameplay - stereo eyes' }).Count
            Invoke-Mod (@(Click 1624 1100) + 'wait 25000' + 'fakevr btn L b 150' + 'wait 10000' + 'fakevr btn L b 150' + 'wait 8000') 180
            Wait-Log 'stereo: gameplay - stereo eyes' 60 $before | Out-Null
        }
        Invoke-Mod @('vr god 1', 'vr marker 0', 'fakevr ctl R 0.18 1.40 -0.35 0 -5 0', 'wait 1000', 'vr shotlog 1', 'vr gun', 'vr measure') 60
        for ($n = 0; $n -lt $list.Count; $n++) {
            $w = $list[$n]
            $s = $styles[$w]
            if ($n -gt 0) { Invoke-Mod @('fakevr release R all', 'fakevr btn R b 150', 'wait 2500', 'vr gun') 60 }
            $pullsPerSec = if ($s.kind -eq 'auto') { 10 } else { 1000 / ($s.ms + 1950 / $s.mag) }
            $stillSec = [math]::Max(8, $StillShots / $pullsPerSec)
            $l = @("log gunacc weapon $w", 'vr gunmeshcheck')
            if ($want -contains 'still') {
            $l += 'vr shotlog tag still_neutral', 'fakevr ctl R 0.18 1.40 -0.35 0 -5 0', 'wait 600'
            $l += Fire $s $stillSec
            $l += 'vr shotlog tag still_edge', 'fakevr ctl R 0.25 1.40 -0.30 10 12 0', 'wait 600'
            $l += Fire $s $stillSec
            $l += 'vr shotlog tag still_seek', 'vr handseek', 'wait 600', 'vr measure'
            $l += Fire $s ($stillSec / 2)
            }
            if ($want -contains 'sweep') {
                $l += 'vr shotlog tag sweep60'
                $l += Sweep $s 60 14
                $l += 'vr shotlog tag sweep180'
                $l += Sweep $s 180 40
                $l += 'vr gunmeshcheck'
            }
            Invoke-Mod $l 900
            if ($want -notcontains 'turn') { continue }
            # Turn: firing on through the level, re-aimed at the nearest zombie (kills move the rail on),
            # then a burst as soon as the rail turns (the vignette starts: ~50 deg/s), if it does within 4 s.
            $l = @('vr shotlog tag turn', 'fakevr ctl R 0.18 1.40 -0.35 0 -5 0')
            $burst = if ($s.kind -eq 'auto') { @('fakevr hold R trigger', 'wait 1500', 'fakevr release R trigger') }
                     else { @(1..[math]::Max(3, [int](1500 / [math]::Min($s.ms, 200))) | ForEach-Object { 'fakevr btn R trigger 40', "wait $([math]::Min($s.ms, 200))" }) }
            $chunks = [math]::Ceiling($TurnSeconds / 8)
            for ($k = 0; $k -lt $chunks; $k++) {
                $l += 'vr handseek', 'wait 300'
                $l += Fire $s 3
                $l += @('vr comfort wait 0.12 4000') + $burst + $reload + 'vr comfort end'
            }
            $l += 'fakevr release R all', 'vr comfort stats 300', 'vr measure', "log gunacc weapon $w done"
            Invoke-Mod $l ([int]($TurnSeconds * 3 + 300))
        }
        Invoke-Mod @('vr shotlog tag end', 'vr shotlog 0') 60
        Write-Output "played: $($list -join ', ')"
    }
}
