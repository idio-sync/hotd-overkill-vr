# The local check before tagging a release (CI cannot do this part: it has no game). Builds, runs the
# unit tests and the release package check, then plays the TEST COPY of the game (never the Steam
# folder) with the fake SteamVR runtime and checks, from the mod's and the runtime's logs and from
# headset-view captures:
#   start-up      OpenVR up at the fake headset's size, the gun controller's render model and tip
#   menus         boot -> Naked Terror by pointing the controller at the flat screen and pulling the
#                 trigger (tools\scripts\to_naked_terror_fakevr.txt), stereo gameplay, level-start recentre
#   stereo        a capture: both eyes submitted at the recommended size without errors, with disparity
#   aim           the hit marker against the controller's ray (tools\fakevr_check.py), two poses: the
#                 marker on the barrel line (a constant few cm above the pointing axis), never beside it
#   buttons       trigger fires (a shot from the hand, haptic pulses), A reload, B switch weapon, grip grenade
#   recentre      the stick held 1 s, and resuming from the pause menu (other hand's B), each facing
#                 where the head now looks
#   quit          SteamVR's quit event closes the game
# At the end (also after a failure) the game this script started is stopped by its PID and the test
# copy goes back to mode=sim with Valve's openvr_api.dll (checked by hash). The summary is printed
# and written to out\release_check.txt; the exit code is 0 only if every check passed.
#
#   powershell -ExecutionPolicy Bypass -File tools\release_check.ps1 [-SkipBuild]
param(
    [string]$Game = 'C:\Users\Virtual\hotd-vr-game',
    [switch]$SkipBuild,   # reuse out\ as it is (the unit tests and package check still run)
    [int]$MenuTimeout = 300
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'out'
$hovr = Join-Path $Game 'hovr'
$py = Join-Path $root '.venv\Scripts\python.exe'
$probeLog = Join-Path $hovr 'probe.log'
$fakeLog = Join-Path $hovr 'fakevr.log'
$valveDll = Join-Path $root 'third_party\openvr\bin\win32\openvr_api.dll'
$started = Get-Date

$results = New-Object System.Collections.Generic.List[object]
function Add-Result([string]$name, [bool]$ok, [string]$detail) {
    $results.Add([pscustomobject]@{ Name = $name; Ok = $ok; Detail = $detail })
    Write-Host ('[{0}] {1}: {2}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name, $detail)
}

# Runs a command line in a child process; output to the console, result = exit code. Judged by the
# exit code only, so stderr chatter cannot abort this script (Windows PowerShell 5.1 + 'Stop').
function Invoke-Child([string]$exe, [string[]]$arguments) {
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $exe @arguments 2>&1 | ForEach-Object { Write-Host "    $_" } }
    finally { $ErrorActionPreference = $eap }
    return $LASTEXITCODE
}
function Invoke-Ps([string]$script, [string[]]$arguments) {
    return Invoke-Child 'powershell' (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot $script)) + $arguments)
}

# Logs the game is still writing: read with sharing, as lines.
function Read-Log([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return @() }
    $fs = [IO.File]::Open($path, 'Open', 'Read', 'ReadWrite, Delete')
    try { $text = (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
    return @($text -split "`r?`n")
}
function Count-Log([string]$path, [string]$regex) { @(Read-Log $path | Where-Object { $_ -match $regex }).Count }

$script:gamePid = 0
function Test-GameAlive { $script:gamePid -and (Get-Process -Id $script:gamePid -ErrorAction SilentlyContinue) }

# Waits until $path has more than $after lines matching $regex; returns the newest one, or $null.
function Wait-Log([string]$path, [string]$regex, [int]$seconds, [int]$after = 0) {
    $deadline = (Get-Date).AddSeconds($seconds)
    do {
        $m = @(Read-Log $path | Where-Object { $_ -match $regex })
        if ($m.Count -gt $after) { return $m[-1] }
        if (-not (Test-GameAlive)) { return $null }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    return $null
}

function Send-Mod([string[]]$lines) { & (Join-Path $PSScriptRoot 'cmd.ps1') -Game $Game -Lines $lines }
function Send-Fake([string[]]$lines) { & (Join-Path $PSScriptRoot 'fakevr.ps1') -Game $Game -Lines $lines }

# One fake-runtime capture with the controller at a pose, checked by fakevr_check.py.
function Test-Aim([string]$name, [string]$pose) {
    & (Join-Path $PSScriptRoot 'fakevr_shot.ps1') -Game $Game -Name $name -Lines @("ctl R $pose", 'wait 800') | Out-Null
    $capture = Join-Path $hovr "fakevr\$name"
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $check = @(& $py -I (Join-Path $PSScriptRoot 'fakevr_check.py') $capture 2>&1 | ForEach-Object { "$_" }) }
    finally { $ErrorActionPreference = $eap }
    $check | ForEach-Object { Write-Host "    $_" }
    return [pscustomobject]@{ Capture = $capture; Lines = $check; Txt = @(Get-Content -LiteralPath "$capture.txt") }
}

function Restore-TestCopy {
    if (Test-Path -LiteralPath (Join-Path $hovr 'pid.txt')) { Invoke-Ps 'run.ps1' @('-Game', $Game, '-Stop') | Out-Null }
    $code = Invoke-Ps 'run.ps1' @('-Game', $Game, '-Mode', 'sim', '-OpenVR', 'real', '-NoStart')
    $dll = Join-Path $Game 'openvr_api.dll'
    $hashOk = (Test-Path -LiteralPath $dll) -and ((Get-FileHash -LiteralPath $dll).Hash -eq (Get-FileHash -LiteralPath $valveDll).Hash)
    $mode = (Get-Content -LiteralPath (Join-Path $hovr 'hovr.ini') | Select-String '^\s*mode\s*=\s*(\w+)' | Select-Object -Last 1).Matches[0].Groups[1].Value
    $alive = [bool](Test-GameAlive)
    Add-Result 'restore' ($code -eq 0 -and $hashOk -and $mode -eq 'sim' -and -not $alive) `
        "test copy mode=$mode, openvr_api.dll $(if ($hashOk) { "is Valve's (SHA-256 matches)" } else { 'is NOT Valve''s' }), game $(if ($alive) { "PID $script:gamePid still running" } else { 'not running' })"
}

# --- preflight ---
if ($Game -like '*steamapps*') { throw 'refusing to run in the Steam folder; use the test copy' }
if (-not (Test-Path -LiteralPath (Join-Path $Game 'HOTD_NG.exe'))) { throw "no test copy at $Game" }
if (-not (Test-Path -LiteralPath (Join-Path $hovr 'probe.ini'))) { throw "$hovr\probe.ini is missing: without the development harness the game would play with the real save" }
if (-not (Test-Path -LiteralPath $py)) { throw "missing $py (the Python venv with numpy and Pillow)" }
$other = Get-Process HOTD_NG -ErrorAction SilentlyContinue
if ($other) { throw "HOTD_NG is already running (PID $($other.Id -join ', ')); not touching it" }

$commit = (& git -C $root rev-parse --short HEAD).Trim()
$dirty = if (& git -C $root status --porcelain -- src tools tests CMakeLists.txt build.cmd) { ' (uncommitted changes)' } else { '' }
Write-Host "release check of $commit$dirty, test copy $Game"

# --- build, unit tests, package ---
if ($SkipBuild) {
    $code = Invoke-Child 'ctest' @('--test-dir', $out, '--output-on-failure')
    Add-Result 'unit tests' ($code -eq 0) "ctest exit code $code (build skipped)"
} else {
    $code = Invoke-Child 'cmd' @('/c', (Join-Path $root 'build.cmd'), 'test')
    Add-Result 'build + unit tests' ($code -eq 0) "build.cmd test exit code $code"
}
$code = Invoke-Ps 'package.ps1' @('-NoBuild', '-Version', 'release-check')
if ($code -eq 0) { $code = Invoke-Ps 'check_package.ps1' @((Join-Path $out 'dist\hotd-overkill-vr-release-check.zip')) }
Add-Result 'package' ($code -eq 0) 'package.ps1 + check_package.ps1 (file list, Valve''s openvr_api.dll, no fakevr)'

$buildOk = -not ($results | Where-Object { -not $_.Ok })
try {
    if (-not $buildOk) { throw 'build, tests or package failed; the game checks were not run' }

    # --- start-up ---
    Remove-Item -LiteralPath $probeLog, $fakeLog -ErrorAction SilentlyContinue
    $code = Invoke-Ps 'run.ps1' @('-Game', $Game, '-Mode', 'openvr', '-OpenVR', 'fake')
    if ($code -ne 0) { throw "run.ps1 failed ($code)" }
    $script:gamePid = [int](Get-Content -LiteralPath (Join-Path $hovr 'pid.txt'))
    $up = Wait-Log $probeLog 'vr: OpenVR up, recommended (\d+)x(\d+) per eye' 90
    $tip = Wait-Log $probeLog "vr: gun controller model 'oculus_quest2_controller_right': tip at" 60
    $fake = Count-Log $fakeLog "fakevr: the fake SteamVR runtime .*loaded by pid $script:gamePid\b"
    $dev = Count-Log $probeLog 'harness \(this run\): development; hooks for VR; save redirected'
    Add-Result 'start-up' ([bool]$up -and $up -match '2064x2016' -and [bool]$tip -and $fake -eq 1 -and $dev -eq 1) `
        "PID $script:gamePid; $(if ($up) { $up.Substring($up.IndexOf('vr:')) } else { 'no OpenVR up line' }); tip $(if ($tip) { 'found' } else { 'missing' }); fake runtime $(if ($fake) { 'loaded' } else { 'NOT loaded' }); save redirected $(if ($dev) { 'yes' } else { 'NO' })"
    if (-not $up) { throw 'the game did not start in openvr mode' }

    # --- menus by pointing, to gameplay ---
    Send-Mod @(Get-Content -LiteralPath (Join-Path $PSScriptRoot 'scripts\to_naked_terror_fakevr.txt'))
    $at = Wait-Log $probeLog 'log: at gameplay' $MenuTimeout
    $stereo = Wait-Log $probeLog 'stereo: gameplay - stereo eyes \+ HUD panel' 30
    $lvl = Wait-Log $probeLog 'vr: recentre \(level start\)' 30
    $seen = Count-Log $probeLog 'input: controllers seen'
    Add-Result 'menus -> gameplay' ([bool]$at -and [bool]$stereo -and [bool]$lvl -and $seen -gt 0) `
        "Naked Terror reached by pointing + trigger: $(if ($at) { 'yes' } else { 'NO' }); stereo gameplay $(if ($stereo) { 'yes' } else { 'no' }); level-start recentre $(if ($lvl) { 'yes' } else { 'no' }); controllers drove the cursor $(if ($seen) { 'yes' } else { 'no' })"
    if (-not $stereo) { throw 'gameplay not reached' }
    Start-Sleep -Seconds 3

    # --- stereo capture and aim ---
    Send-Mod @('vr marker 1')
    $a1 = Test-Aim 'rc_neutral' '0.18 1.40 -0.35 0 -5 0'
    $a2 = Test-Aim 'rc_edge' '0.25 1.40 -0.30 10 12 0'
    $eyes = @($a1.Txt | Where-Object { $_ -match '^eye \d: pose .* view 2064x2016; submit 4128x2016 .*error 0\)' })
    $px = @($a1.Lines | Where-Object { $_ -match '^eye \d: marker at pixel' } | ForEach-Object { [double]($_ -replace '^eye \d: marker at pixel (\S+) .*$', '$1') })
    $disparity = if ($px.Count -eq 2) { [math]::Abs($px[0] - $px[1]) } else { 0 }
    Add-Result 'stereo capture' ($eyes.Count -eq 2 -and $disparity -gt 2) `
        "$($eyes.Count)/2 eyes submitted 2064x2016 (4128x2016 target) without errors; marker disparity $([math]::Round($disparity, 1)) px ($($a1.Capture)_pair.png)"
    foreach ($a in @($a1, $a2)) {
        $ray = @($a.Lines | Where-Object { $_ -match 'controller ray: the marker is (\S+) m along it, (\S+) cm off it \((\S+) cm right, (\S+) cm up\), (\S+) deg off' })
        $miss = @($a.Lines | Where-Object { $_ -match 'eye rays miss each other by (\S+) mm' })
        if ($ray.Count -eq 1 -and $miss.Count -eq 1) {
            $null = $ray[0] -match 'the marker is (\S+) m along it, (\S+) cm off it \((\S+) cm right, (\S+) cm up\), (\S+) deg off'
            $along, $off, $right, $upcm, $deg = $Matches[1..5] | ForEach-Object { [double]$_ }
            $null = $miss[0] -match 'miss each other by (\S+) mm'
            $mm = [double]$Matches[1]
            # The barrel sits a constant ~4.4 cm above the pointing axis (gun_grip); left/right is a bug.
            Add-Result "aim $(Split-Path $a.Capture -Leaf)" ($off -le 6 -and [math]::Abs($right) -le 1 -and $mm -le 5) `
                "marker $along m along the ray, $off cm off ($right right, $upcm up), $deg deg from the tip; eye rays meet within $mm mm"
        } else {
            Add-Result "aim $(Split-Path $a.Capture -Leaf)" $false "fakevr_check.py found no marker: $($a.Lines -join ' | ')"
        }
    }
    Send-Mod @('vr marker 0')

    # --- buttons ---
    Send-Fake @('haptics')
    Start-Sleep -Milliseconds 500
    $shots = Count-Log $probeLog '^\[.*\] shot( \(projectile\))?: '
    $hapt = Count-Log $fakeLog 'haptics: \d+ pulses since the last report'
    $checks = @(
        @{ Name = 'trigger fires'; Btn = 'trigger'; Regex = '^\[.*\] shot( \(projectile\))?: ' },
        @{ Name = 'A reloads'; Btn = 'a'; Regex = 'input: reload pressed \(mmb\)' },
        @{ Name = 'B switches weapon'; Btn = 'b'; Regex = 'input: switch weapon pressed \(TAB\)' },
        @{ Name = 'grip throws a grenade'; Btn = 'grip'; Regex = 'input: grenade pressed \(rmb\)' })
    $btnOk = @()
    $btnBad = @()
    foreach ($c in $checks) {
        $before = Count-Log $probeLog $c.Regex
        Send-Fake @("btn R $($c.Btn) 150", 'wait 400')
        if (Wait-Log $probeLog $c.Regex 10 $before) { $btnOk += $c.Name } else { $btnBad += $c.Name }
        Start-Sleep -Milliseconds 1500
    }
    Send-Fake @('haptics')
    $h = Wait-Log $fakeLog 'haptics: (\d+) pulses since the last report' 10 $hapt
    $pulses = if ($h -and $h -match 'haptics: (\d+) pulses') { [int]$Matches[1] } else { 0 }
    Add-Result 'buttons' ($btnBad.Count -eq 0 -and $pulses -gt 0) `
        "ok: $($btnOk -join ', ')$(if ($btnBad) { "; NOT seen: $($btnBad -join ', ')" }); $pulses haptic pulse(s) for the shot"

    # --- recentre: button held, and resuming from the pause menu ---
    $rc = Count-Log $probeLog 'vr: recentred at .* heading (\S+) deg'
    Send-Fake @('look 30 0 0 300', 'wait 500', 'hold R stick', 'wait 1400', 'release R stick')
    $btnLine = Wait-Log $probeLog 'vr: recentre \(button held\)' 15
    $head1 = Wait-Log $probeLog 'vr: recentred at .* heading (\S+) deg' 10 $rc
    $deg1 = if ($head1 -and $head1 -match 'heading (\S+) deg') { [double]$Matches[1] } else { [double]::NaN }
    Start-Sleep -Seconds 1
    $paused = Count-Log $probeLog 'game state: level [0-9]+ cutscene 0 paused 1'
    $rc = Count-Log $probeLog 'vr: recentred at .* heading (\S+) deg'
    Send-Fake @('btn L b 150')
    $p = Wait-Log $probeLog 'game state: level [0-9]+ cutscene 0 paused 1' 10 $paused
    Send-Fake @('wait 1500', 'look -20 0 0 300', 'wait 800', 'btn L b 150')
    $resume = Wait-Log $probeLog 'vr: recentre \(gameplay resumed after a flat screen\)' 20
    $head2 = Wait-Log $probeLog 'vr: recentred at .* heading (\S+) deg' 10 $rc
    $deg2 = if ($head2 -and $head2 -match 'heading (\S+) deg') { [double]$Matches[1] } else { [double]::NaN }
    Add-Result 'recentre' ([bool]$btnLine -and [math]::Abs($deg1 - 30) -le 2 -and [bool]$p -and [bool]$resume -and [math]::Abs($deg2 + 20) -le 2) `
        "stick held: $(if ($btnLine) { "heading $deg1 deg (head at 30)" } else { 'no recentre' }); pause $(if ($p) { 'opened' } else { 'NOT opened' }), resume: $(if ($resume) { "heading $deg2 deg (head at -20)" } else { 'no recentre' })"
    Send-Fake @('look 0 0 0')

    # --- quit through SteamVR ---
    Send-Fake @('event quit')
    $q = Wait-Log $probeLog 'vr: SteamVR asks the game to quit' 15
    $deadline = (Get-Date).AddSeconds(45)
    while ((Test-GameAlive) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }
    $gone = -not (Test-GameAlive)
    $ack = Count-Log $fakeLog 'AcknowledgeQuit_Exiting'
    Add-Result 'quit' ([bool]$q -and $gone -and $ack -gt 0) `
        "quit event $(if ($q) { 'answered' } else { 'NOT answered' }), AcknowledgeQuit_Exiting $(if ($ack) { 'called' } else { 'not called' }), game $(if ($gone) { 'exited' } else { 'still running' })"
}
catch {
    Add-Result 'game checks' $false "stopped: $($_.Exception.Message)"
}
finally {
    Restore-TestCopy
}

# --- summary ---
$failed = @($results | Where-Object { -not $_.Ok })
$summary = @(
    "release_check $(Get-Date -Format 'yyyy-MM-dd HH:mm') - commit $commit$dirty - $(if ($failed) { "FAILED ($($failed.Count) of $($results.Count))" } else { "PASSED ($($results.Count) checks)" })"
    "test copy $Game, fake SteamVR runtime; took $([int]((Get-Date) - $started).TotalSeconds) s"
    ''
) + @($results | ForEach-Object { '{0}  {1,-24} {2}' -f $(if ($_.Ok) { 'PASS' } else { 'FAIL' }), $_.Name, $_.Detail })
New-Item -ItemType Directory -Force $out | Out-Null
[IO.File]::WriteAllLines((Join-Path $out 'release_check.txt'), [string[]]$summary, [Text.Encoding]::ASCII)
Write-Host ''
$summary | ForEach-Object { Write-Host $_ }
Write-Host "(written to $(Join-Path $out 'release_check.txt'))"
exit $(if ($failed) { 1 } else { 0 })
