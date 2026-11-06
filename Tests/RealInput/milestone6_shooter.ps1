<#
.SYNOPSIS
  Milestone 6 slice 5 with real mouse and keyboard input: the Physics Shooter with its scripted vault, won and lost.

.DESCRIPTION
  Plays a copy of the sample on the Release editor. The vault by the start, through the west wall, is scripts: its gate
  (Scripts/Gate.lua) holds the player in the arena until the targets of its room, the west wing (the pillar's target and
  Enemy 1), have fallen, then slides into the wall; inside, the button (Scripts/Button.lua) gives 300 points and 20
  seconds when pressed with E from within reach, once. The run shoots those two, walks in, presses E, then destroys the
  rest and wins; restarts, and loses by waiting out the clock; and Stop gives back the scene as it was. Each check reads
  the editor's --game-log, which reports true state, the scripted entities' positions and the script errors since Play
  began. Keep hands off the mouse. A stopped run exits with code 2 and proves nothing.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File Tests/RealInput/milestone6_shooter.ps1 -Editor build-ux-windows-msvc-release\bin\engine_editor.exe -Output real-input-run
#>
param([string]$Editor = 'build-ux-windows-msvc-release\bin\engine_editor.exe', [string]$Output = 'real-input-run', [string]$Work = "$env:TEMP\engine-m6-shooter")
$ErrorActionPreference = 'Stop'
$WarningPreference = 'SilentlyContinue'
Import-Module (Join-Path $PSScriptRoot 'InputDriver.psm1') -Force

$Editor = (Resolve-Path $Editor).Path
$repo = Split-Path (Split-Path $PSScriptRoot)
New-Item -ItemType Directory -Force $Output | Out-Null
$Output = (Resolve-Path $Output).Path
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force $Work | Out-Null
$project = Join-Path $Work 'project'
Copy-Item -Recurse (Join-Path $repo 'Samples\PhysicsShooter') $project
$log = Join-Path $Work 'game.log'
$scene = Join-Path $project 'physics_shooter.scene'
$results = New-Object System.Collections.ArrayList
$code = 0

function Check { param([string]$Name, [bool]$Pass, [string]$Evidence)
    $line = '{0,-5}  {1}  [{2}]' -f $(if ($Pass) { 'PASS' } else { 'FAIL' }), $Name, $Evidence
    [void]$results.Add($line); Write-Host $line
}
# The latest line of game state, as a table.
function Get-State {
    for ($i = 0; $i -lt 40; $i++) {
        try {
            $stream = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite,Delete')
            $reader = New-Object IO.StreamReader $stream
            if ($stream.Length -gt 16000) { [void]$stream.Seek(-16000, 'End') }
            $text = $reader.ReadToEnd(); $reader.Close(); $stream.Close()
            $lines = $text -split "`n" | Where-Object { $_ -like 'state=*' }
            if ($lines) {
                $state = @{}
                foreach ($pair in (@($lines)[-1].Trim() -split ' ')) { $k, $v = $pair -split '=', 2; $state[$k] = $v }
                return $state
            }
        } catch { }
        Start-Sleep -Milliseconds 50
    }
    throw 'No state in the game log'
}
function To-Vector { param([string]$Text) $p = $Text -split ','; return @([double]$p[0], [double]$p[1], [double]$p[2]) }
function Distance { param($A, $B) return [Math]::Sqrt(($A[0]-$B[0])*($A[0]-$B[0]) + ($A[1]-$B[1])*($A[1]-$B[1]) + ($A[2]-$B[2])*($A[2]-$B[2])) }
function Wait-For { param([scriptblock]$Done, [int]$Seconds = 10)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalSeconds -lt $Seconds) { if (& $Done) { return $true }; Start-Sleep -Milliseconds 50 }
    return $false
}
function Gate-At { return To-Vector (Get-State)['s:Vault_Gate'] }
function Button-At { return To-Vector (Get-State)['s:Vault_Button'] }

# ---- Aiming and moving, as the shooter's acceptance run does: the game turns by look_sensitivity radians a count. ----
$script:yaw = 0.0; $script:pitch = 0.0
$sensitivity = 0.0022
function Reset-Aim { $script:yaw = 0.0; $script:pitch = 0.0 }
function Aim-At { param([double[]]$Point, [int]$Milliseconds = 120)
    $eye = To-Vector (Get-State).eye
    $dx = $Point[0] - $eye[0]; $dy = $Point[1] - $eye[1]; $dz = $Point[2] - $eye[2]
    $wantYaw = [Math]::Atan2(-$dx, -$dz); $wantPitch = [Math]::Atan2($dy, [Math]::Sqrt($dx*$dx + $dz*$dz))
    $turn = $script:yaw - $wantYaw
    while ($turn -gt [Math]::PI) { $turn -= 2 * [Math]::PI }; while ($turn -lt -[Math]::PI) { $turn += 2 * [Math]::PI }
    $countsX = [int][Math]::Round($turn / $sensitivity); $countsY = [int][Math]::Round(($script:pitch - $wantPitch) / $sensitivity)
    Move-Relative $countsX $countsY $Milliseconds
    $script:yaw -= $countsX * $sensitivity; $script:pitch -= $countsY * $sensitivity
    Start-Sleep -Milliseconds 60
}
function Walk-To { param([double]$X, [double]$Z, [int]$TimeoutMilliseconds = 7000)
    $eye = To-Vector (Get-State).eye
    Aim-At @($X, $eye[1], $Z)
    Key-Down 'W'
    $clock = [Diagnostics.Stopwatch]::StartNew()
    try {
        while ($clock.ElapsedMilliseconds -lt $TimeoutMilliseconds) {
            $eye = To-Vector (Get-State).eye
            if ([Math]::Sqrt(($eye[0]-$X)*($eye[0]-$X) + ($eye[2]-$Z)*($eye[2]-$Z)) -lt 0.5) { break }
            Start-Sleep -Milliseconds 30
        }
    } finally { Key-Up 'W' }
    Start-Sleep -Milliseconds 250
}
function Shoot { param([int]$Shots = 1)
    Mouse-Button $true; Start-Sleep -Milliseconds (40 + 125 * ($Shots - 1) + 30); Mouse-Button $false; Start-Sleep -Milliseconds 80
}
function Reload-IfLow { if ([int](Get-State).ammo -lt 6) { Press-Key 'R'; Start-Sleep -Milliseconds 1500 } }
# Shoots until the named target is gone. Returns whether it went.
function Destroy-Target { param([string]$Name, [int]$Tries = 12)
    for ($i = 0; $i -lt $Tries; $i++) {
        $state = Get-State
        if (-not $state.ContainsKey("t:$Name")) { return $true }
        Reload-IfLow
        Aim-At (To-Vector $state["t:$Name"]) 100
        Shoot 2
        Start-Sleep -Milliseconds 150
    }
    return -not (Get-State).ContainsKey("t:$Name")
}
# A target on a path: aim at a point it crosses, and fire as it arrives.
function Ambush-Target { param([string]$Name, [double[]]$Point, [int]$Tries = 4)
    for ($i = 0; $i -lt $Tries; $i++) {
        $state = Get-State
        if (-not $state.ContainsKey("t:$Name")) { return $true }
        Reload-IfLow
        Aim-At $Point
        $clock = [Diagnostics.Stopwatch]::StartNew()
        while ($clock.ElapsedMilliseconds -lt 15000) {
            $state = Get-State
            if (-not $state.ContainsKey("t:$Name")) { return $true }
            if ((Distance (To-Vector $state["t:$Name"]) $Point) -lt 0.7) { Shoot 2; break }
            Start-Sleep -Milliseconds 15
        }
        Start-Sleep -Milliseconds 400
    }
    return -not (Get-State).ContainsKey("t:$Name")
}
function Standing { return @((Get-State).Keys | Where-Object { $_ -like 't:*' } | ForEach-Object { $_.Substring(2) }) }

$map = Join-Path $Work 'ui-map.txt'
Set-UiMap $map
Wait-ForIdleDesktop
$process = Start-Editor -Executable $Editor -Arguments @('--project', "`"$project`"", '--scene', "`"$scene`"", '--game-log', "`"$log`"", '--config-dir', "`"$(Join-Path $Work 'config')`"", '--ui-map', "`"$map`"") -WorkingDirectory $Work -Log (Join-Path $Output 'editor')
try {
    Start-Sleep -Seconds 3
    $state = Get-State
    Check 'The shooter loads, with the vault' ([int]$state.entities -gt 80 -and $state.dirty -eq '0') "$($state.entities) entities, clean"
    # A save now is the reference for "Stop gives back the scene": written by the same code, before any Play.
    Key-Down 'Ctrl'; Press-Key 'S'; Key-Up 'Ctrl'; Start-Sleep -Milliseconds 500
    $saved = (Get-FileHash $scene -Algorithm SHA256).Hash

    $clicks = 0
    do { Click-Widget 'Play.Play'; $clicks++; Start-Sleep -Seconds 2; $state = Get-State } while ($state.state -ne 'playing' -and $clicks -lt 3)
    Click-Widget 'Panel.3D Viewport' 0.5 0.8
    Start-Sleep -Milliseconds 500
    Reset-Aim
    $state = Get-State
    $closed = Gate-At
    Check 'Play runs the vault''s two scripts, and the game starts' ($state.phase -eq 'playing' -and $state.scripts -eq '2' -and $state.script_errors -eq '0' -and [Math]::Abs($closed[2] - 12) -lt 1e-6) "phase $($state.phase), scripts $($state.scripts), errors $($state.script_errors), gate at z $($closed[2])"

    # ---- The gate holds the player in the arena ----
    Walk-To -12.5 12
    Aim-At @(-16.5, 1.6, 12)
    Save-Capture (Join-Path $Output 'm6_s5_01_gate_shut.jpg')
    Walk-To -20 12 2500
    $eye = To-Vector (Get-State).eye
    Check 'The shut gate stops the player at the doorway' ($eye[0] -gt -16.4 -and $eye[0] -lt -15.5) ('stopped at x {0:n3}; the gate''s face is at -16.35, the capsule''s radius 0.35' -f $eye[0])

    # ---- Its room's targets: the pillar's and Enemy 1 ----
    Walk-To -12 10
    $pillar = Destroy-Target 'Static_Target_3'
    $enemy = Destroy-Target 'Enemy_1' 16
    Aim-At @(-16.5, 1.6, 12) 80
    $opening = Wait-For { (Gate-At)[2] -gt 12.4 } 4
    Start-Sleep -Milliseconds 300
    $state = Get-State
    Save-Capture (Join-Path $Output 'm6_s5_02_gate_opening.jpg')
    Check 'Once the west wing''s two targets fall, the gate opens and says so' ($pillar -and $enemy -and $opening -and $state.event -eq 'The_vault_is_open') "pillar $pillar, enemy $enemy, gate at z $((To-Vector $state['s:Vault_Gate'])[2]), message $($state.event)"
    $open = Wait-For { [Math]::Abs((Gate-At)[2] - 15) -lt 1e-6 } 4
    Start-Sleep -Milliseconds 500
    $at = Gate-At
    Save-Capture (Join-Path $Output 'm6_s5_03_gate_open.jpg')
    Check 'It slides 3 m into the wall and stays there' ($open -and [Math]::Abs($at[0] - $closed[0]) -lt 1e-9 -and [Math]::Abs($at[2] - 15) -lt 1e-6) ('gate at {0:n3}, {1:n3}, {2:n3}' -f $at[0], $at[1], $at[2])

    # ---- Into the vault, and E at the button ----
    Walk-To -15 12
    Walk-To -21.3 12
    $state = Get-State
    $eye = To-Vector $state.eye
    Aim-At (Button-At)
    Save-Capture (Join-Path $Output 'm6_s5_04_in_the_vault.jpg')
    Check 'Inside, within reach of the button, its hint shows' ($eye[0] -lt -20.5 -and $state.event -eq "Press_E_for_the_vault's_prize") ('at x {0:n2}; message {1}' -f $eye[0], $state.event)
    $before = Get-State
    $rest = Button-At
    Press-Key 'E'
    Start-Sleep -Milliseconds 300
    $after = Get-State
    $pushed = Button-At
    Save-Capture (Join-Path $Output 'm6_s5_05_prize.jpg')
    $gain = [double]$after.time - [double]$before.time
    Check 'E gives the prize: 300 points and 20 seconds, and the button sinks in' ([int]$after.score -eq [int]$before.score + 300 -and $gain -gt 19 -and $gain -lt 20.01 -and $after.event -eq '+300__Vault,_and_20_seconds' -and [Math]::Abs($pushed[0] - ($rest[0] - 0.08)) -lt 1e-6) ('score {0} to {1}, time {2:n2} to {3:n2}, button x {4:n3} to {5:n3}' -f $before.score, $after.score, [double]$before.time, [double]$after.time, $rest[0], $pushed[0])
    Start-Sleep -Milliseconds 600
    Press-Key 'E'
    Start-Sleep -Milliseconds 400
    $again = Get-State
    Check 'Only once' ([int]$again.score -eq [int]$after.score) "score $($after.score), then $($again.score)"

    # ---- The rest of the targets, to win ----
    Walk-To -15 12
    Walk-To 0 8
    Aim-At @(0, 1.6, 3)
    Shoot 3 # The glass barrier.
    Start-Sleep -Milliseconds 300
    Walk-To 0 0.5
    $left = @{}
    foreach ($name in 'Static_Target_1', 'Static_Target_2', 'Water_Bottle_Target', 'Dynamic_Target_1') { $left[$name] = Destroy-Target $name }
    $left['Enemy_2'] = Destroy-Target 'Enemy_2' 16
    Walk-To 6.2 8.4
    if ((Get-State).ContainsKey('t:Chain_Target')) {
        Reload-IfLow
        Aim-At @(6.2, 1.8, 2.6)
        Shoot 1
        [void](Wait-For { -not (Get-State).ContainsKey('t:Chain_Target') } 6)
    }
    Walk-To 3 -1
    foreach ($name in 'Chain_Target', 'Dynamic_Target_2', 'Enemy_3') { $left[$name] = Destroy-Target $name 16 }
    Walk-To 0 -3
    $left['Moving_Target_1'] = Ambush-Target 'Moving_Target_1' @(0, 2.4, -7.8)
    Walk-To 9 4
    $left['Moving_Target_2'] = Ambush-Target 'Moving_Target_2' @(14, 1.6, 4)
    # Anything still standing, from three meters in front of it.
    for ($pass = 0; $pass -lt 2 -and (Get-State).phase -eq 'playing'; $pass++) {
        foreach ($name in Standing) {
            $p = To-Vector (Get-State)["t:$name"]
            if (-not $p) { continue }
            Walk-To ([Math]::Max(-14, [Math]::Min(14, $p[0]))) ([Math]::Max(-14, [Math]::Min(14, $p[2] + 3.2)))
            if ($name -like 'Moving_Target_*') { [void](Ambush-Target $name (To-Vector (Get-State)["t:$name"])) } else { [void](Destroy-Target $name 16) }
        }
    }
    Start-Sleep -Milliseconds 400
    $state = Get-State
    Save-Capture (Join-Path $Output 'm6_s5_06_won.jpg')
    Check 'The game is won' ($state.phase -eq 'won' -and $state.targets -eq '0') "phase $($state.phase), score $($state.score), $([int][double]$state.time) s left"

    # ---- Restart: the vault is shut again ----
    Press-Key 'R'
    Start-Sleep -Milliseconds 900
    Reset-Aim
    $state = Get-State
    $again = Gate-At
    Check 'Restart starts over, with the gate shut and its scripts run afresh' ($state.phase -eq 'playing' -and $state.targets -eq '12' -and $state.score -eq '0' -and $state.scripts -eq '2' -and [Math]::Abs($again[2] - 12) -lt 1e-6) "phase $($state.phase), targets $($state.targets), score $($state.score), gate at z $($again[2])"

    # ---- Lost: the clock runs out ----
    $wait = [double]$state.time
    Start-Sleep -Seconds ([int]$wait + 2)
    $state = Get-State
    Save-Capture (Join-Path $Output 'm6_s5_07_lost.jpg')
    Check 'The game is lost when the clock runs out' ($state.phase -eq 'lost' -and [int]$state.targets -eq 12) "phase $($state.phase), targets $($state.targets)"
    Check 'No script failed in the whole run' ($state.script_errors -eq '0') "script errors $($state.script_errors)"

    # ---- Stop gives back the scene ----
    Click-Widget 'Play.Stop'
    $stopped = Wait-For { (Get-State).state -eq 'editing' } 5
    Start-Sleep -Milliseconds 500
    Key-Down 'Ctrl'; Press-Key 'S'; Key-Up 'Ctrl'; Start-Sleep -Milliseconds 500
    $state = Get-State
    $same = (Get-FileHash $scene -Algorithm SHA256).Hash -eq $saved
    Check 'Stop gives back the scene as it was: saved again, the same bytes' ($stopped -and $state.dirty -eq '0' -and $same) "state $($state.state), identical $same"

    $exit = Stop-Editor
    Check 'The editor closes cleanly' ($exit -eq 0) "exit code $exit"
    Write-Host "Targets on the way to the win: $(($left.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ', ')"
} catch {
    Write-Host "STOPPED: $_"
    [void]$results.Add("STOPPED  $_")
    $code = 2
} finally {
    Kill-Editor
    $results | Set-Content -Encoding utf8 (Join-Path $Output 'real_input_results.txt')
    Copy-Item $log (Join-Path $Output 'game.log') -ErrorAction SilentlyContinue
}
if ($code -eq 0 -and ($results | Where-Object { $_ -like 'FAIL*' })) { $code = 1 }
exit $code
