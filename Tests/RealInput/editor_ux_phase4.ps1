<#
.SYNOPSIS
  Drives the workflow keys with real mouse and keyboard input: play, simulate, eject, the command palette, quick open
  and the console.

.DESCRIPTION
  Runs against a temporary copy of Physics Shooter with its own settings folder. Each check reads the editor's --game-log,
  which reports true state, or a file on disk. Keep hands off the mouse. A stopped run exits with code 2 and proves
  nothing either way.
#>
param([string]$Editor = 'build-ux-windows-msvc-release\bin\engine_editor.exe', [string]$Output = 'real-input-run', [string]$Work = "$env:TEMP\engine-ux-phase4")
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
$scene = Join-Path $project 'physics_shooter.scene'
$log = Join-Path $Work 'state.log'
$map = Join-Path $Work 'ui-map.txt'
$config = Join-Path $Work 'config'
$results = New-Object System.Collections.ArrayList

function Check { param([string]$Name, [bool]$Pass, [string]$Evidence)
    $line = '{0,-5}  {1}  [{2}]' -f $(if ($Pass) { 'PASS' } else { 'FAIL' }), $Name, $Evidence
    [void]$results.Add($line); Write-Host $line
}
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
                foreach ($pair in (@($lines)[-1].Trim() -split ' ')) { $k, $v = $pair -split '=', 2; $state[$k] = $v }
                return $state
            }
        } catch { }
        Start-Sleep -Milliseconds 50
    }
    throw 'No state in the log'
}
function Get-Settled { Start-Sleep -Milliseconds 350; return Get-State }
function To-Vector { param([string]$Text) $p = $Text -split ','; return @([double]$p[0], [double]$p[1], [double]$p[2]) }
function Distance { param($A, $B) return [Math]::Sqrt(($A[0]-$B[0])*($A[0]-$B[0]) + ($A[1]-$B[1])*($A[1]-$B[1]) + ($A[2]-$B[2])*($A[2]-$B[2])) }
function Chord { param([string[]]$Modifiers, [string]$Key)
    foreach ($m in $Modifiers) { Key-Down $m }
    Start-Sleep -Milliseconds 40
    Press-Key $Key
    foreach ($m in $Modifiers) { Key-Up $m }
    Start-Sleep -Milliseconds 250
}
function Start-Run {
    Remove-Item $log -ErrorAction SilentlyContinue
    Set-UiMap $map
    Start-Editor -Executable $Editor -Arguments @('--project', "`"$project`"", '--scene', "`"$scene`"", '--game-log', "`"$log`"", '--config-dir', "`"$config`"", '--ui-map', "`"$map`"") -WorkingDirectory $Work -Log (Join-Path $Output 'phase4') | Out-Null
    Start-Sleep -Seconds 3
}
# Types a line into the palette, or quick open, that has just been opened.
function Pick { param([string]$Text) Type-Text $Text; Start-Sleep -Milliseconds 200 }
function Palette-Run { param([string]$Text) Chord 'Ctrl','Shift' 'P'; Pick $Text; Press-Key 'Enter'; Start-Sleep -Milliseconds 300 }
function Console-Line { param([string]$Text) Type-Text $Text; Start-Sleep -Milliseconds 80; Press-Key 'Enter'; Start-Sleep -Milliseconds 250 }
function Over-View { Move-ToWidget 'Panel.3D Viewport' 0.5 0.6 }

Wait-ForIdleDesktop
Start-Run
try {
    $state = Get-State
    Check 'The scene loads' ([int]$state.entities -gt 60 -and $state.state -eq 'editing') "$($state.entities) entities"
    Chord 'Ctrl' 'S'; Start-Sleep -Milliseconds 400
    $saved = (Get-FileHash $scene -Algorithm SHA256).Hash
    Over-View

    # ---- Alt+P plays, with the player's view ----
    Chord 'Alt' 'P'; Start-Sleep -Seconds 1
    $state = Get-State
    $player_camera = $state.view_camera
    Save-Capture (Join-Path $Output 'p4_01_play.jpg')
    Check 'Alt+P starts Play through the scene''s camera' ($state.state -eq 'playing' -and $state.phase -eq 'ready' -and [int]$player_camera -ne 0 -and $state.simulating -eq '0') "state $($state.state), phase $($state.phase), camera $player_camera"
    Click-Widget 'Panel.3D Viewport' 0.5 0.8; Start-Sleep -Milliseconds 500
    $state = Get-State
    Check 'A click starts the game and takes the mouse' ($state.phase -eq 'playing' -and $state.captured -eq '1') "phase $($state.phase), captured $($state.captured)"

    # ---- While the game has the keyboard, editor keys do nothing ----
    $entities = $state.entities
    Chord 'Ctrl' 'P'; Chord 'Ctrl','Shift' 'P'; Press-Key 'Grave'; Chord 'Ctrl' 'D'
    $state = Get-Settled
    Check 'Editor keys do nothing while the game has the keyboard' ($state.picker -eq '0' -and $state.entities -eq $entities -and $state.captured -eq '1' -and $state.phase -eq 'playing') "picker $($state.picker), entities $($state.entities), captured $($state.captured)"

    # ---- Shift+F1 frees the mouse without pausing ----
    Chord 'Shift' 'F1'
    $freed = Get-Settled
    $timeA = [double]$freed.time
    Start-Sleep -Milliseconds 900
    $timeB = [double](Get-State).time
    Check 'Shift+F1 frees the mouse and the game runs on' ($freed.captured -eq '0' -and $freed.phase -eq 'playing' -and $freed.state -eq 'playing' -and $timeB -lt $timeA) "captured $($freed.captured), phase $($freed.phase), time $timeA then $timeB"
    Click-Widget 'Panel.3D Viewport' 0.5 0.8; Start-Sleep -Milliseconds 400
    $state = Get-State
    Check 'A click on the view takes the mouse again' ($state.captured -eq '1' -and $state.phase -eq 'playing') "captured $($state.captured), phase $($state.phase)"

    # ---- F8 ejects while a key is held: the player stops walking, and the game carries on ----
    Key-Down 'W'; Start-Sleep -Milliseconds 400
    Press-Key 'F8'
    Key-Up 'W'
    $ejected = Get-Settled
    Save-Capture (Join-Path $Output 'p4_02_ejected.jpg')
    $eyeA = To-Vector $ejected.eye; $timeA = [double]$ejected.time; $ticksA = [int]$ejected.tick
    Start-Sleep -Milliseconds 700
    $later = Get-State
    $eyeB = To-Vector $later.eye
    Check 'F8 ejects to the editor camera, and the game carries on' ($ejected.ejected -eq '1' -and $ejected.captured -eq '0' -and [int]$ejected.view_camera -eq 0 -and [int]$later.tick -gt $ticksA -and [double]$later.time -lt $timeA) "ejected $($ejected.ejected), captured $($ejected.captured), camera $($ejected.view_camera), ticks $ticksA to $($later.tick)"
    Check 'The player does not walk on by itself after ejecting' ((Distance $eyeA $eyeB) -lt 0.01) "moved $([Math]::Round((Distance $eyeA $eyeB), 4)) m"

    # ---- Ejected, the editor works on the live scene ----
    Move-ToWidget 'Panel.3D Viewport' 0.5 0.5
    $yaw = [double]$later.yaw
    Mouse-Press 'Right' $true; Start-Sleep -Milliseconds 120
    try { Move-Relative 120 0 250 } finally { Mouse-Press 'Right' $false }
    $state = Get-Settled
    Check 'The editor camera moves while ejected' ([Math]::Abs([double]$state.yaw - $yaw) -gt 0.1 -and $state.ejected -eq '1') "yaw $yaw to $($state.yaw)"
    Chord 'Ctrl' 'P'; Pick 'crate'; Press-Key 'Enter'; Start-Sleep -Milliseconds 400
    $state = Get-State
    Check 'An entity can be selected while ejected' ([int]$state.selected -eq 1 -and $state.primary) "selected $($state.selected), at $($state.primary)"
    Click-Widget 'Tab.Inspector'; Start-Sleep -Milliseconds 350
    Check 'Details shows the live entity' (Test-Widget 'Details.Transform.position') 'Transform row is there'
    Save-Capture (Join-Path $Output 'p4_03_ejected_details.jpg')

    # ---- F8 goes back ----
    Press-Key 'F8'
    $state = Get-Settled
    Check 'F8 possesses again and takes the mouse' ($state.ejected -eq '0' -and $state.captured -eq '1' -and $state.view_camera -eq $player_camera -and $state.phase -eq 'playing') "ejected $($state.ejected), captured $($state.captured), camera $($state.view_camera)"

    # ---- Esc: pause, then stop ----
    Press-Key 'Esc'
    $paused = Get-Settled
    $timeA = [double]$paused.time
    Start-Sleep -Milliseconds 700
    $timeB = [double](Get-State).time
    Save-Capture (Join-Path $Output 'p4_04_paused.jpg')
    Check 'The first Esc frees the mouse and pauses' ($paused.phase -eq 'paused' -and $paused.state -eq 'paused' -and $paused.captured -eq '0' -and $timeA -eq $timeB) "state $($paused.state), phase $($paused.phase), time $timeA then $timeB"
    Over-View
    Press-Key 'Esc'
    $state = Get-Settled
    Check 'The second Esc stops' ($state.state -eq 'editing' -and $state.dirty -eq '0') "state $($state.state), dirty $($state.dirty)"
    Chord 'Ctrl' 'S'; Start-Sleep -Milliseconds 400
    $again = (Get-FileHash $scene -Algorithm SHA256).Hash
    Check 'The scene is restored exactly on stop' ($again -eq $saved) "saved file identical: $($again -eq $saved)"

    # ---- Alt+S simulates, with the editor's view ----
    Over-View
    Chord 'Alt' 'S'; Start-Sleep -Milliseconds 700
    $sim = Get-State
    Save-Capture (Join-Path $Output 'p4_05_simulate.jpg')
    $ticksA = [int]$sim.tick; $timeA = [double]$sim.time
    Start-Sleep -Milliseconds 600
    $later = Get-State
    Check 'Alt+S runs the scene without the player' ($sim.state -eq 'playing' -and $sim.simulating -eq '1' -and $sim.ejected -eq '1' -and [int]$sim.view_camera -eq 0 -and $sim.phase -eq 'ready' -and [int]$later.tick -gt $ticksA -and [double]$later.time -eq $timeA) "simulating $($sim.simulating), camera $($sim.view_camera), phase $($sim.phase), ticks $ticksA to $($later.tick), time $timeA then $($later.time)"
    Click-Widget 'Panel.3D Viewport' 0.5 0.5; Start-Sleep -Milliseconds 350
    $state = Get-State
    Check 'A click while simulating picks instead of starting the game' ($state.phase -eq 'ready' -and $state.captured -eq '0') "phase $($state.phase), captured $($state.captured)"
    Press-Key 'F8'
    $state = Get-Settled
    Check 'F8 turns a simulation into a run' ($state.simulating -eq '0' -and $state.ejected -eq '0' -and $state.view_camera -eq $player_camera -and $state.phase -eq 'ready') "simulating $($state.simulating), camera $($state.view_camera), phase $($state.phase)"
    Press-Key 'Esc'
    $state = Get-Settled
    Check 'Esc with the mouse free stops at once' ($state.state -eq 'editing' -and $state.simulating -eq '0' -and $state.dirty -eq '0') "state $($state.state), dirty $($state.dirty)"
    Chord 'Alt' 'S'; Start-Sleep -Milliseconds 500
    Press-Key 'Esc'
    $state = Get-Settled
    Check 'Esc stops a simulation' ($state.state -eq 'editing') "state $($state.state)"

    # ---- An open drawer takes Esc first, even while running ----
    Chord 'Alt' 'S'; Start-Sleep -Milliseconds 500
    Chord 'Ctrl' 'Space'
    $open = Get-Settled
    Press-Key 'Esc'
    $closed = Get-Settled
    Check 'Esc closes the drawer before it stops anything' ($open.drawer -eq '1' -and $closed.drawer -eq '0' -and $closed.state -eq 'playing') "drawer $($open.drawer) to $($closed.drawer), state $($closed.state)"
    Over-View
    Press-Key 'Esc'
    $state = Get-Settled
    Check 'The next Esc stops' ($state.state -eq 'editing') "state $($state.state)"

    # ---- Esc in the editor still deselects ----
    Chord 'Ctrl' 'P'; Pick 'crate'; Press-Key 'Enter'; Start-Sleep -Milliseconds 400
    $before = Get-State
    Press-Key 'Esc'
    $state = Get-Settled
    Check 'Esc outside Play deselects, and stops nothing' ([int]$before.selected -eq 1 -and [int]$state.selected -eq 0 -and $state.state -eq 'editing') "selected $($before.selected) to $($state.selected)"

    # ---- The command palette ----
    Over-View
    Chord 'Ctrl','Shift' 'P'; Pick 'game view'
    $state = Get-State
    Save-Capture (Join-Path $Output 'p4_06_palette.jpg')
    $operation = $state.operation
    Check 'Ctrl+Shift+P opens the palette, with what was typed filtering it' ($state.picker -eq '1' -and (Test-Widget 'Picker.Input') -and (Test-Widget 'Picker.Item.0')) "picker $($state.picker)"
    Press-Key 'Enter'
    $state = Get-Settled
    Check 'Enter runs the command' ($state.game_view -eq '1' -and $state.picker -eq '0') "game_view $($state.game_view), picker $($state.picker)"
    Chord 'Ctrl','Shift' 'P'; Start-Sleep -Milliseconds 200
    Save-Capture (Join-Path $Output 'p4_07_recent.jpg')
    Press-Key 'Enter'
    $state = Get-Settled
    Check 'The last command is first when nothing is typed' ($state.game_view -eq '0') "game_view $($state.game_view)"
    Chord 'Ctrl','Shift' 'P'; Pick 'r w e top'
    $state = Get-State
    Check 'Letters typed in the palette do not fire shortcuts' ($state.operation -eq $operation -and $state.picker -eq '1') "operation $($state.operation), was $operation"
    Press-Key 'Esc'
    $state = Get-Settled
    Check 'Esc closes the palette' ($state.picker -eq '0' -and $state.state -eq 'editing') "picker $($state.picker)"
    Chord 'Ctrl','Shift' 'P'; Pick 'view'
    Press-Key 'Down'; Press-Key 'Down'; Start-Sleep -Milliseconds 150
    $state = Get-State
    Check 'Down moves the choice' ($state.picker_index -eq '2') "index $($state.picker_index)"
    Press-Key 'Up'; Start-Sleep -Milliseconds 150
    $state = Get-State
    Check 'Up moves it back' ($state.picker_index -eq '1') "index $($state.picker_index)"
    Press-Key 'Esc'; Start-Sleep -Milliseconds 200
    Chord 'Ctrl','Shift' 'P'; Pick 'game view'
    Click-Widget 'Picker.Item.0'; Start-Sleep -Milliseconds 300
    $state = Get-Settled
    Check 'A click runs a command too' ($state.game_view -eq '1' -and $state.picker -eq '0') "game_view $($state.game_view)"
    Palette-Run 'game view'
    Chord 'Ctrl','Shift' 'P'; Start-Sleep -Milliseconds 150
    Click-Widget 'Panel.Scene Hierarchy' 0.5 0.95; Start-Sleep -Milliseconds 350
    $state = Get-State
    Check 'Clicking elsewhere closes the palette' ($state.picker -eq '0') "picker $($state.picker)"
    Chord 'Ctrl','Shift' 'P'; Pick 'step'; Press-Key 'Enter'
    $state = Get-Settled
    Check 'A command that cannot run now does nothing' ($state.state -eq 'editing' -and $state.picker -eq '0') "state $($state.state), picker $($state.picker)"
    Chord 'Ctrl','Shift' 'P'; Pick 'zzzzqqq'
    Save-Capture (Join-Path $Output 'p4_08_nothing.jpg')
    Press-Key 'Enter'
    $state = Get-Settled
    Check 'Enter with no match just closes' ($state.state -eq 'editing' -and $state.picker -eq '0' -and -not (Test-Widget 'Picker.Input')) "picker $($state.picker)"

    # ---- Quick open ----
    Chord 'Ctrl' 'P'; Pick 'crate'
    $state = Get-State
    Save-Capture (Join-Path $Output 'p4_09_quick_open.jpg')
    Check 'Ctrl+P opens quick open' ($state.picker -eq '2' -and (Test-Widget 'Picker.Item.0')) "picker $($state.picker)"
    Press-Key 'Enter'
    $state = Get-Settled
    Check 'Enter selects the entity and frames it' ([int]$state.selected -eq 1 -and $state.picker -eq '0') "selected $($state.selected), target $($state.camera)"
    $asset = $state.selected_asset
    Chord 'Ctrl' 'P'; Pick 'wood_base'; Press-Key 'Enter'
    $state = Get-Settled
    Save-Capture (Join-Path $Output 'p4_10_asset.jpg')
    Check 'Enter on an asset shows it in the browser' ($state.selected_asset -ne $asset -and $state.selected_asset -ne '0' -and $state.picker -eq '0') "asset $asset to $($state.selected_asset)"

    Chord 'Ctrl' 'P'; Pick 'wood_crate.material'; Press-Key 'Enter'
    $state = Get-Settled
    Check 'Enter on a material opens it in the Material Editor' ($state.picker -eq '0' -and (Test-Widget 'Material.BaseColor.Fields')) "asset $($state.selected_asset)"
    Save-Capture (Join-Path $Output 'p4_10_material.jpg')

    # ---- The console: autocomplete, values, errors, history ----
    Over-View
    Press-Key 'Grave'; Start-Sleep -Milliseconds 300
    Type-Text 'r.'; Start-Sleep -Milliseconds 250
    $state = Get-State
    $suggested = Test-Widget 'Console.Suggestions'
    Save-Capture (Join-Path $Output 'p4_11_suggestions.jpg')
    Check 'The backtick is not typed, and suggestions appear' ($state.console_typed -eq '2' -and $suggested) "typed $($state.console_typed) characters, suggestions $suggested"
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'; Press-Key 'Delete'
    Type-Text 'camera.spe'; Press-Key 'Tab'; Start-Sleep -Milliseconds 120
    $state = Get-State
    Check 'Tab completes the name' ($state.console_typed -eq '13') "$($state.console_typed) characters, camera.speed and a space"
    Console-Line '7'
    $state = Get-State
    Check 'A variable can be set' ([double]$state.speed -eq 7) "speed $($state.speed)"
    $lines = [int]$state.console_lines
    Console-Line 'camera.speed fast'
    $state = Get-State
    Check 'A bad value is an error and changes nothing' ([double]$state.speed -eq 7 -and [int]$state.console_lines -gt $lines) "speed $($state.speed), lines $lines to $($state.console_lines)"
    Console-Line 'camera.speed 9'
    Console-Line 'camera.speed 3'
    Press-Key 'Up'; Press-Key 'Up'; Press-Key 'Enter'; Start-Sleep -Milliseconds 250
    $state = Get-State
    Check 'Up brings back earlier lines' ([double]$state.speed -eq 9) "speed $($state.speed)"
    Press-Key 'Up'; Press-Key 'Down'; Start-Sleep -Milliseconds 120
    $state = Get-State
    Check 'Down comes back to an empty line' ($state.console_typed -eq '0') "typed $($state.console_typed)"
    Console-Line 'stat fps'
    Console-Line 'stat timings'
    $state = Get-State
    Save-Capture (Join-Path $Output 'p4_12_stats.jpg')
    Check 'stat fps and stat timings show over the view' ($state.stat_fps -eq '1' -and $state.stat_timings -eq '1' -and (Test-Widget 'Stats')) "fps $($state.stat_fps), timings $($state.stat_timings)"
    # What it shows is frames drawn, not trips round the loop, which carry on while Filament skips frames.
    $first = Get-State; $watch = [Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Milliseconds 2000
    $last = Get-State
    $measured = ([double]$last.frames_drawn - [double]$first.frames_drawn) / $watch.Elapsed.TotalSeconds
    $shown = 1000 / [double]$last.stat_frame_ms
    Check 'stat fps matches the frames drawn' ($measured -gt 0 -and [Math]::Abs($shown - $measured) -lt 0.3 * $measured) "shows $([Math]::Round($shown)), drew $([Math]::Round($measured)) a second"
    Console-Line 'stat none'
    $state = Get-State
    Check 'stat none hides them' ($state.stat_fps -eq '0' -and $state.stat_timings -eq '0') "fps $($state.stat_fps), timings $($state.stat_timings)"
    $colliders = $state.colliders
    Console-Line 'show collision'
    $state = Get-State
    Check 'show collision toggles collider outlines' ($state.colliders -ne $colliders) "$colliders to $($state.colliders)"
    Console-Line 'show collision'
    Console-Line 'r.bloom 0'
    $state = Get-State
    Check 'r.bloom sets the real flag' ($state.bloom -eq '0') "bloom $($state.bloom)"
    Console-Line 'r.bloom on'
    $lines = [int](Get-State).console_lines
    Console-Line 'help'
    $state = Get-State
    Check 'help lists what the console knows' ([int]$state.console_lines -gt $lines + 20) "lines $lines to $($state.console_lines)"
    Console-Line 'Viewport.Top'
    $state = Get-State
    Check 'A registered command runs by its ID' ($state.ortho -eq '1') "ortho $($state.ortho)"
    Console-Line 'Viewport.Perspective'
    Press-Key 'Grave'
    $state = Get-Settled
    Check 'The backtick leaves the console' ($state.console_typed -eq '0' -and $state.console_shown -eq '1') "typed $($state.console_typed), shown $($state.console_shown)"

    # ---- The key closes a panel it opened ----
    Palette-Run 'window console'
    $state = Get-State
    Check 'The panel can be closed' ($state.console_shown -eq '0') "shown $($state.console_shown)"
    Press-Key 'Grave'
    $opened = Get-Settled
    Press-Key 'Grave'
    $closed = Get-Settled
    Check 'A closed console opens on the key, and closes again' ($opened.console_shown -eq '1' -and $closed.console_shown -eq '0') "shown $($opened.console_shown), then $($closed.console_shown)"
    Press-Key 'Grave'; Start-Sleep -Milliseconds 200
    Palette-Run 'game view'
    $state = Get-Settled
    Check 'The palette opens over the console' ($state.game_view -eq '1') "game_view $($state.game_view)"
    Over-View
    Palette-Run 'game view'

    # ---- Recently used, after a restart ----
    $exit = Stop-Editor
    Check 'The editor closes cleanly' ($exit -eq 0) "exit code $exit"
    Start-Run
    Over-View
    Chord 'Ctrl','Shift' 'P'; Start-Sleep -Milliseconds 200; Press-Key 'Enter'
    $state = Get-Settled
    Check 'Recently used commands survive a restart' ($state.game_view -eq '1') "game_view $($state.game_view)"
    Save-Capture (Join-Path $Output 'p4_13_after_restart.jpg')
    $exit = Stop-Editor
    Check 'And it closes cleanly again' ($exit -eq 0) "exit code $exit"
    $results | Set-Content (Join-Path $Output 'phase4_results.txt') -Encoding utf8
} catch {
    Write-Host "RUN STOPPED: $_"
    $results | Set-Content (Join-Path $Output 'phase4_results_partial.txt') -Encoding utf8
    Kill-Editor
    exit 2
} finally {
    Kill-Editor
}
