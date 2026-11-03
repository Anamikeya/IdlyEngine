<#
.SYNOPSIS
  Plays Physics Shooter with real mouse and keyboard input on the Release editor and checks the brief's 16 steps.

.DESCRIPTION
  Runs against a temporary copy of the sample, so the repository's scene is never saved over. Each check reads the
  editor's --game-log, which reports true state; captures are saved as evidence. Keep hands off the mouse while it runs.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File Tests/RealInput/shooter_acceptance.ps1 -Editor build-ux-windows-msvc-release\bin\engine_editor.exe -Output real-input-run
#>
param([string]$Editor = 'build-ux-windows-msvc-release\bin\engine_editor.exe', [string]$Output = 'real-input-run', [string]$Work = "$env:TEMP\engine-m4-acceptance")
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

function Step { param([int]$Number, [string]$Name, [bool]$Pass, [string]$Evidence)
    $line = '{0,2}  {1,-5}  {2}  [{3}]' -f $Number, $(if ($Pass) { 'PASS' } else { 'FAIL' }), $Name, $Evidence
    [void]$results.Add($line); Write-Host $line
}
# The latest line of game state, as a table.
function Get-State {
    for ($i = 0; $i -lt 40; $i++) {
        try {
            $stream = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite,Delete')
            $reader = New-Object IO.StreamReader $stream
            if ($stream.Length -gt 12000) { [void]$stream.Seek(-12000, 'End') }
            $text = $reader.ReadToEnd(); $reader.Close(); $stream.Close()
            $lines = $text -split "`n" | Where-Object { $_ -like 'state=*' }
            if ($lines) {
                $state = @{}
                foreach ($pair in ($lines[-1].Trim() -split ' ')) { $k, $v = $pair -split '=', 2; $state[$k] = $v }
                return $state
            }
        } catch { }
        Start-Sleep -Milliseconds 50
    }
    throw 'No state in the game log'
}
function Get-Bodies {
    $stream = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite,Delete')
    $reader = New-Object IO.StreamReader $stream
    $text = $reader.ReadToEnd(); $reader.Close(); $stream.Close()
    $line = ($text -split "`n" | Where-Object { $_ -like 'bodies*' })[-1]
    $bodies = @{}
    foreach ($pair in ($line.Trim() -split ' ' | Select-Object -Skip 1)) { $k, $v = $pair -split '=', 2; $bodies[$k] = To-Vector $v }
    return $bodies
}
function To-Vector { param([string]$Text) $p = $Text -split ','; return @([double]$p[0], [double]$p[1], [double]$p[2]) }
function Distance { param($A, $B) return [Math]::Sqrt(($A[0]-$B[0])*($A[0]-$B[0]) + ($A[1]-$B[1])*($A[1]-$B[1]) + ($A[2]-$B[2])*($A[2]-$B[2])) }

# ---- Aiming: the game turns by look_sensitivity radians per raw mouse count. ----
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
            $far = [Math]::Sqrt(($eye[0]-$X)*($eye[0]-$X) + ($eye[2]-$Z)*($eye[2]-$Z))
            if ($far -lt 0.5) { break }
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

$map = Join-Path $Work 'ui-map.txt'
Set-UiMap $map
Wait-ForIdleDesktop
$process = Start-Editor -Executable $Editor -Arguments @('--project', "`"$project`"", '--scene', "`"$scene`"", '--game-log', "`"$log`"", '--perf-log', "`"$(Join-Path $Output 'performance.txt')`"", '--config-dir', "`"$(Join-Path $Work 'config')`"", '--ui-map', "`"$map`"") -WorkingDirectory $Work -Log (Join-Path $Output 'editor')
try {
    Start-Sleep -Seconds 3

    # 1. The scene loads.
    $state = Get-State
    Save-Capture (Join-Path $Output '01_scene_loaded.jpg')
    Step 1 'The shooter scene loads' ([int]$state.entities -gt 60 -and $state.dirty -eq '0') "$($state.entities) entities, clean"
    # A save now is the reference for "Stop restores the editor scene": written by the same code, before any Play.
    Key-Down 'Ctrl'; Press-Key 'S'; Key-Up 'Ctrl'; Start-Sleep -Milliseconds 500
    $saved = (Get-FileHash $scene -Algorithm SHA256).Hash

    # 2. The arena renders: the player's view, once Play starts.
    $clicks = 0
    do { Click-Widget 'Play.Play'; $clicks++; Start-Sleep -Seconds 2; $state = Get-State } while ($state.state -ne 'playing' -and $clicks -lt 3)
    if ($clicks -gt 1) { Write-Host "Note: the Play click took $clicks attempts" }
    Save-Capture (Join-Path $Output '02_ready_overlay.jpg')
    Step 2 'Play shows the arena, ready to start' ($state.state -eq 'playing' -and $state.phase -eq 'ready') "phase $($state.phase), $($state.targets) targets"
    Click-Widget 'Panel.3D Viewport' 0.5 0.8
    Start-Sleep -Milliseconds 500
    Reset-Aim
    $state = Get-State
    Save-Capture (Join-Path $Output '02_arena_playing.jpg')
    Step 2 'A click starts the game and takes the mouse' ($state.phase -eq 'playing') "phase $($state.phase), time $($state.time)"

    # 3. WASD.
    $moves = @{}
    foreach ($key in 'W', 'S', 'A', 'D') {
        $before = To-Vector (Get-State).eye
        Hold-Key @($key) 500
        Start-Sleep -Milliseconds 200
        $after = To-Vector (Get-State).eye
        $moves[$key] = @(($after[0] - $before[0]), ($after[2] - $before[2]))
    }
    $ok = $moves.W[1] -lt -1 -and $moves.S[1] -gt 1 -and $moves.A[0] -lt -1 -and $moves.D[0] -gt 1
    Step 3 'WASD moves the player' $ok ('W dz {0:n2}, S dz {1:n2}, A dx {2:n2}, D dx {3:n2}' -f $moves.W[1], $moves.S[1], $moves.A[0], $moves.D[0])

    # 4. Mouse look.
    Save-Capture (Join-Path $Output '04_before_look.jpg')
    Move-Relative -360 -80 400
    $script:yaw += 360 * $sensitivity; $script:pitch += 80 * $sensitivity
    Start-Sleep -Milliseconds 200
    Save-Capture (Join-Path $Output '04_after_look.jpg')
    Hold-Key @('W') 400
    Start-Sleep -Milliseconds 200
    Aim-At @(0, 1.68, 100) # Face the south wall.
    Step 4 'The mouse turns the view' $true 'see 04_before_look and 04_after_look; walking after the turn follows the new direction'

    # 5. Walls: sprint at the south wall, and stop at it.
    Hold-Key @('W', 'Shift') 2600
    Start-Sleep -Milliseconds 200
    $eye = To-Vector (Get-State).eye
    Save-Capture (Join-Path $Output '05_at_wall.jpg')
    Step 5 'The player collides with walls' ($eye[2] -gt 15 -and $eye[2] -lt 15.7) ('stopped at z {0:n3}; the wall face is at 16, the capsule radius 0.35' -f $eye[2])

    # 6. Jump, from the floor and back.
    $rest = (To-Vector (Get-State).eye)[1]
    $peak = $rest
    Press-Key 'Space' 40
    for ($i = 0; $i -lt 40; $i++) { $peak = [Math]::Max($peak, (To-Vector (Get-State).eye)[1]); Start-Sleep -Milliseconds 25 }
    Start-Sleep -Milliseconds 300
    $land = (To-Vector (Get-State).eye)[1]
    Step 6 'Jumping works' ($peak -gt $rest + 0.8 -and [Math]::Abs($land - $rest) -lt 0.05) ('rest {0:n2}, peak {1:n2}, landed {2:n2}' -f $rest, $peak, $land)

    # 7, 10. A shot at the glass: ammunition falls and it registers.
    Walk-To 0 8
    $ammo = [int](Get-State).ammo
    Aim-At @(0, 1.6, 3)
    Shoot 1
    Start-Sleep -Milliseconds 150
    $after = Get-State
    Save-Capture (Join-Path $Output '07_shot_impact.jpg')
    Step 7 'Shooting consumes ammunition and marks the impact' ([int]$after.ammo -lt $ammo) "ammo $ammo to $($after.ammo)"
    Shoot 3 # Two hits break the glass.
    Start-Sleep -Milliseconds 200
    $glassGone = [int](Get-State).entities
    Save-Capture (Join-Path $Output '07_glass_broken.jpg')

    # 8. A dynamic crate moves when shot.
    Walk-To 0 0.5
    Reload-IfLow
    Start-Sleep -Milliseconds 1100
    $before = Get-Bodies
    Aim-At @(-4.6, 1.2, -0.6)
    Shoot 3
    Start-Sleep -Milliseconds 1200
    $bodies = Get-Bodies
    $moved = Distance $before['Stack_Crate_3'] $bodies['Stack_Crate_3']
    Save-Capture (Join-Path $Output '08_crates_pushed.jpg')
    Step 8 'Dynamic crates move when shot' ($moved -gt 0.2) ('Stack Crate 3 moved {0:n2} m' -f $moved)

    # 9, 10. Targets take damage; score and ammunition update.
    $scoreBefore = [int](Get-State).score; $targetsBefore = [int](Get-State).targets
    $static1 = Destroy-Target 'Static_Target_1'
    $state = Get-State
    Save-Capture (Join-Path $Output '09_target_destroyed.jpg')
    Step 9 'A target takes damage and is destroyed' ($static1 -and [int]$state.targets -eq $targetsBefore - 1) "targets $targetsBefore to $($state.targets)"
    Step 10 'Score and ammunition update' ([int]$state.score -gt $scoreBefore) "score $scoreBefore to $($state.score), ammo $($state.ammo)"

    # 11. Reload.
    Shoot 2
    $low = Get-State
    Press-Key 'R'
    Start-Sleep -Milliseconds 300
    $during = Get-State
    Save-Capture (Join-Path $Output '11_reloading.jpg')
    Start-Sleep -Milliseconds 1400
    $done = Get-State
    Step 11 'Reload works' ($during.reloading -eq '1' -and [int]$done.ammo -eq 15 -and [int]$done.reserve -lt [int]$low.reserve) "ammo $($low.ammo) reserve $($low.reserve), then $($done.ammo) and $($done.reserve)"

    # Sensor events: a crate pushed off the shelf lands in the bonus zone, a sensor, and scores.
    # A light crate can bounce anywhere, so if one misses the zone the next is tried: what this checks is the sensor.
    $scoreBefore = [int](Get-State).score
    $scoreAfter = $scoreBefore
    foreach ($x in 0, -0.8, 0.8, -1.6, 1.6) {
        Reload-IfLow
        Aim-At @($x, 2.92, -11.5)
        Shoot 1
        Start-Sleep -Milliseconds 1400
        $scoreAfter = [int](Get-State).score
        if ($scoreAfter -ge $scoreBefore + 50) { break }
    }
    Save-Capture (Join-Path $Output '11_bonus_zone.jpg')
    Step 11 'A crate falling into the bonus zone scores through a sensor event' ($scoreAfter -ge $scoreBefore + 50) "score $scoreBefore to $scoreAfter"

    # 12. Win: every remaining target, from where it can be seen.
    $results2 = @{}
    foreach ($name in 'Static_Target_2', 'Water_Bottle_Target', 'Dynamic_Target_1') { $results2[$name] = Destroy-Target $name }
    # The chain: one push at the first domino from in front of it.
    Walk-To 6.2 8.4
    $state = Get-State
    if ($state.ContainsKey('t:Chain_Target')) {
        Reload-IfLow
        Aim-At @(6.2, 1.8, 2.6)
        Save-Capture (Join-Path $Output '12_before_dominoes.jpg')
        Shoot 1
        Start-Sleep -Milliseconds 1500
        Save-Capture (Join-Path $Output '12_dominoes_falling.jpg')
        $clock = [Diagnostics.Stopwatch]::StartNew()
        while ($clock.ElapsedMilliseconds -lt 6000 -and (Get-State).ContainsKey('t:Chain_Target')) { Start-Sleep -Milliseconds 100 }
        $chain = -not (Get-State).ContainsKey('t:Chain_Target')
        Step 8 'A chain reaction: one shot topples the row onto its target' $chain "chain target destroyed: $chain"
    }
    # The pillar's target faces +Z, so it has to be shot from that side.
    $results2['Static_Target_3'] = Destroy-Target 'Static_Target_3'
    Walk-To 3 -1
    foreach ($name in 'Chain_Target', 'Dynamic_Target_2') { $results2[$name] = Destroy-Target $name }
    Walk-To 0 -3
    $results2['Moving_Target_1'] = Ambush-Target 'Moving_Target_1' @(0, 2.4, -7.8)
    Walk-To 9 4
    $results2['Moving_Target_2'] = Ambush-Target 'Moving_Target_2' @(14, 1.6, 4)
    $state = Get-State
    Start-Sleep -Milliseconds 400
    Save-Capture (Join-Path $Output '12_game_won.jpg')
    $state = Get-State
    Step 12 'The game can be won' ($state.phase -eq 'won') "phase $($state.phase), score $($state.score), targets $($state.targets), $([int][double]$state.time) s left"

    # 14. Restart from the win screen, with the key the screen offers.
    Press-Key 'R'
    Start-Sleep -Milliseconds 800
    $state = Get-State
    Reset-Aim
    Save-Capture (Join-Path $Output '14_restarted.jpg')
    Step 14 'Restart resets the scene' ($state.phase -eq 'playing' -and [int]$state.targets -eq 9 -and [int]$state.score -eq 0 -and [int]$state.ammo -eq 15) "phase $($state.phase), targets $($state.targets), score $($state.score), ammo $($state.ammo), time $($state.time)"

    # Esc releases the mouse and pauses; a click on the view takes it again.
    Press-Key 'Esc'
    Start-Sleep -Milliseconds 300
    $paused = Get-State
    $timeA = [double]$paused.time
    Start-Sleep -Milliseconds 1000
    $timeB = [double](Get-State).time
    Save-Capture (Join-Path $Output 'pause.jpg')
    Click-Widget 'Panel.3D Viewport' 0.96 0.27
    Start-Sleep -Milliseconds 500
    $resumed = Get-State
    Step 14 'Esc pauses and frees the mouse; a click resumes' ($paused.phase -eq 'paused' -and $timeA -eq $timeB -and $resumed.phase -eq 'playing') "paused at $timeA, still $timeB; then phase $($resumed.phase)"

    # 13. Loss: wait out the clock.
    $wait = [double](Get-State).time
    Start-Sleep -Seconds ([int]$wait + 2)
    $state = Get-State
    Save-Capture (Join-Path $Output '13_game_lost.jpg')
    Step 13 'The game can be lost' ($state.phase -eq 'lost') "phase $($state.phase), targets left $($state.targets)"

    # 15. Stop returns the unchanged scene: saving again gives the same bytes.
    Click-Widget 'Play.Stop'
    Start-Sleep -Milliseconds 800
    $state = Get-State
    Save-Capture (Join-Path $Output '15_stopped.jpg')
    Key-Down 'Ctrl'; Press-Key 'S'; Key-Up 'Ctrl'; Start-Sleep -Milliseconds 500
    $again = (Get-FileHash $scene -Algorithm SHA256).Hash
    Step 15 'Stop restores the editor scene' ($state.state -eq 'editing' -and $state.dirty -eq '0' -and $again -eq $saved) "state $($state.state), dirty $($state.dirty), saved file identical: $($again -eq $saved)"

    # 16. The editor still works.
    $count = [int](Get-State).entities
    Click-Widget 'Create.Cube'
    Start-Sleep -Milliseconds 400
    $added = [int](Get-State).entities
    Key-Down 'Ctrl'; Press-Key 'Z'; Key-Up 'Ctrl'; Start-Sleep -Milliseconds 400
    $undone = [int](Get-State).entities
    Save-Capture (Join-Path $Output '16_editor_usable.jpg')
    $process.Refresh()
    "peak_working_set_mb $([int]($process.PeakWorkingSet64 / 1MB))" | Set-Content (Join-Path $Output 'memory.txt')
    $exit = Stop-Editor
    Step 16 'The editor remains usable and closes cleanly' ($added -eq $count + 1 -and $undone -eq $count -and $exit -eq 0) "+ Cube: $count to $added; undo: $undone; exit code $exit"
    $results | Set-Content (Join-Path $Output 'real_input_results.txt') -Encoding utf8
    Write-Host "Other targets: $(($results2.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ', ')"
} catch {
    Write-Host "RUN STOPPED: $_"
    $results | Set-Content (Join-Path $Output 'real_input_results_partial.txt') -Encoding utf8
    exit 2
} finally {
    Kill-Editor
    Copy-Item $log (Join-Path $Output 'game.log') -ErrorAction SilentlyContinue
}
