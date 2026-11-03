<#
.SYNOPSIS
  Drives viewport navigation, the gizmo, snapping, placement and view helpers with real mouse and keyboard input.

.DESCRIPTION
  Works in a temporary project with its own settings folder. Each check reads the editor's --game-log, which reports
  true state, or a file on disk. Keep hands off the mouse. A stopped run exits with code 2 and proves nothing either way.
#>
param([string]$Editor = 'build-ux-windows-msvc-release\bin\engine_editor.exe', [string]$Output = 'real-input-run', [string]$Work = "$env:TEMP\engine-ux-phase2")
$ErrorActionPreference = 'Stop'
$WarningPreference = 'SilentlyContinue'
Import-Module (Join-Path $PSScriptRoot 'InputDriver.psm1') -Force

$Editor = (Resolve-Path $Editor).Path
$repo = Split-Path (Split-Path $PSScriptRoot)
New-Item -ItemType Directory -Force $Output | Out-Null
$Output = (Resolve-Path $Output).Path
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
$project = Join-Path $Work 'project'
New-Item -ItemType Directory -Force (Join-Path $project 'Assets') | Out-Null
Copy-Item (Join-Path $repo 'Tests\Assets\editor\meshes\cube.obj') (Join-Path $project 'Assets\cube.obj')
$log = Join-Path $Work 'state.log'
$map = Join-Path $Work 'ui-map.txt'
$config = Join-Path $Work 'config'
$scene = Join-Path $Work 'main.scene'
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
            if ($stream.Length -gt 6000) { [void]$stream.Seek(-6000, 'End') }
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
function Get-Settled { Start-Sleep -Milliseconds 250; return Get-State }
function To-Vector { param([string]$Text) $p = $Text -split ','; return @([double]$p[0], [double]$p[1], [double]$p[2]) }
function Distance { param($A, $B) return [Math]::Sqrt(($A[0]-$B[0])*($A[0]-$B[0]) + ($A[1]-$B[1])*($A[1]-$B[1]) + ($A[2]-$B[2])*($A[2]-$B[2])) }
function Steps { param($State) return [int]($State.history -split '/')[0] }
function Chord { param([string]$Modifier, [string]$Key) Key-Down $Modifier; Press-Key $Key; Key-Up $Modifier; Start-Sleep -Milliseconds 200 }
function Start-Run { param([string[]]$Extra = @())
    Remove-Item $log -ErrorAction SilentlyContinue
    Set-UiMap $map
    Start-Editor -Executable $Editor -Arguments (@('--project', "`"$project`"", '--game-log', "`"$log`"", '--config-dir', "`"$config`"", '--ui-map', "`"$map`"") + $Extra) -WorkingDirectory $Work -Log (Join-Path $Output 'phase2') | Out-Null
    Start-Sleep -Seconds 2
}
# Puts the mouse over the view, away from the gizmo and the overlays, and lets it register.
function Over-View { param([double]$Fx = 0.85, [double]$Fy = 0.75) Move-ToWidget 'Panel.3D Viewport' $Fx $Fy; Start-Sleep -Milliseconds 120 }
function Click-View { param([double]$Fx = 0.85, [double]$Fy = 0.75) Click-Widget 'Panel.3D Viewport' $Fx $Fy; Start-Sleep -Milliseconds 200 }
# Holds a button (and a modifier) over the view while a script block runs.
function With-Button { param([string]$Button, [scriptblock]$While, [string]$Modifier)
    if ($Modifier) { Key-Down $Modifier; Start-Sleep -Milliseconds 80 }
    Mouse-Press $Button $true; Start-Sleep -Milliseconds 120
    try { & $While } finally { Mouse-Press $Button $false; if ($Modifier) { Key-Up $Modifier }; Start-Sleep -Milliseconds 250 }
}
# Drags the gizmo's center by this many counts. $During runs with the button down.
function Drag-Gizmo { param([int]$Dx, [int]$Dy, [scriptblock]$During, [string]$Modifier)
    Move-ToWidget 'Gizmo.Center'; Start-Sleep -Milliseconds 200
    if ($Modifier) { Key-Down $Modifier; Start-Sleep -Milliseconds 80 }
    Mouse-Press 'Left' $true; Start-Sleep -Milliseconds 100
    try {
        Move-Relative $Dx $Dy 400
        Start-Sleep -Milliseconds 150
        if ($During) { & $During }
    } finally { Mouse-Press 'Left' $false; if ($Modifier) { Key-Up $Modifier }; Start-Sleep -Milliseconds 300 }
}

Wait-ForIdleDesktop
Start-Run
try {
    # ---- Fly ----
    Over-View
    $before = Get-Settled
    $cursor = Get-CursorPosition
    $during = $null
    With-Button 'Right' {
        $script:during = Get-Settled
        Move-Relative 80 -30 250
        Start-Sleep -Milliseconds 120
        $script:looked = Get-State
        Hold-Key @('W') 500
        Start-Sleep -Milliseconds 120
        $script:flown = Get-State
        Hold-Key @('E') 300
        Start-Sleep -Milliseconds 120
        $script:raised = Get-State
        Save-Capture (Join-Path $Output 'p2_01_flying.jpg')
    }
    $after = Get-Settled
    $now = Get-CursorPosition
    Check 'RMB enters fly mode' ($script:during.mode -eq '3') "mode $($script:during.mode)"
    Check 'RMB drag looks around' ([double]$script:looked.yaw -ne [double]$before.yaw -and [double]$script:looked.pitch -ne [double]$before.pitch) "yaw $($before.yaw) to $($script:looked.yaw), pitch $($before.pitch) to $($script:looked.pitch)"
    Check 'W flies forward' ((Distance (To-Vector $script:looked.camera) (To-Vector $script:flown.camera)) -gt 1) ('moved {0:n2} m' -f (Distance (To-Vector $script:looked.camera) (To-Vector $script:flown.camera)))
    Check 'E flies up' ((To-Vector $script:raised.camera)[1] -gt (To-Vector $script:flown.camera)[1] + 0.3) "y $((To-Vector $script:flown.camera)[1]) to $((To-Vector $script:raised.camera)[1])"
    Check 'Releasing RMB ends it' ($after.mode -eq '0') "mode $($after.mode)"
    Check 'The cursor comes back where it was' ([Math]::Abs($now[0] - $cursor[0]) -le 2 -and [Math]::Abs($now[1] - $cursor[1]) -le 2) "$($cursor -join ',') then $($now -join ',')"

    # ---- Speed and field of view ----
    $speed = [double](Get-State).speed
    With-Button 'Right' { Scroll-Wheel 3; Start-Sleep -Milliseconds 200; Save-Capture (Join-Path $Output 'p2_02_speed_overlay.jpg') }
    $state = Get-Settled
    Check 'RMB + wheel changes the camera speed' ([double]$state.speed -gt $speed) "speed $speed to $($state.speed)"
    Check 'The speed is saved' ((Test-Path (Join-Path $config 'editor.ini')) -and ((Get-Content (Join-Path $config 'editor.ini') -Raw) -match 'camera\.speed = ')) 'editor.ini'
    With-Button 'Right' { Hold-Key @('Z') 400; Start-Sleep -Milliseconds 60; $script:wide = Get-State }
    Start-Sleep -Milliseconds 800
    $state = Get-State
    Check 'Z widens the view while flying, and it returns' ([double]$script:wide.fov -gt 65 -and [Math]::Abs([double]$state.fov - 60) -lt 1) "fov $($script:wide.fov), then $($state.fov)"

    # ---- Alt navigation ----
    Click-Widget 'Row.Cube'; Start-Sleep -Milliseconds 200
    Over-View
    $before = Get-Settled
    With-Button 'Left' { $script:mode = (Get-Settled).mode; Move-Relative 70 25 250 } -Modifier 'Alt'
    $after = Get-Settled
    Check 'Alt+LMB orbits' ($script:mode -eq '5' -and [double]$after.yaw -ne [double]$before.yaw) "mode $($script:mode), yaw $($before.yaw) to $($after.yaw)"
    # After Alt, keys still reach the editor and not a menu.
    Press-Key 'E'
    $state = Get-Settled
    Check 'Letting go of Alt does not take the keyboard to the menu bar' ($state.operation -eq '1') "operation $($state.operation)"
    Press-Key 'W'
    $before = Get-Settled
    With-Button 'Right' { $script:mode = (Get-Settled).mode; Move-Relative 80 0 250 } -Modifier 'Alt'
    $after = Get-Settled
    Check 'Alt+RMB dollies' ($script:mode -eq '6' -and [double]$after.distance -lt [double]$before.distance) "distance $($before.distance) to $($after.distance)"
    $before = Get-Settled
    With-Button 'Middle' { $script:mode = (Get-Settled).mode; Move-Relative 60 20 250 } -Modifier 'Alt'
    $after = Get-Settled
    Check 'Alt+MMB pans' ($script:mode -eq '7' -and $after.camera -ne $before.camera) "camera $($before.camera) to $($after.camera)"
    $before = Get-Settled
    With-Button 'Middle' { Move-Relative -60 -20 250 }
    $after = Get-Settled
    Check 'MMB pans' ($after.camera -ne $before.camera) "camera $($before.camera) to $($after.camera)"
    $before = Get-Settled
    Scroll-Wheel -2; Start-Sleep -Milliseconds 200
    $after = Get-Settled
    Check 'The wheel zooms' ([double]$after.distance -gt [double]$before.distance) "distance $($before.distance) to $($after.distance)"
    $before = Get-Settled
    With-Button 'Left' { Move-Relative 20 -60 300; $script:mode = (Get-State).mode }
    $after = Get-Settled
    Check 'LMB drag walks and keeps the selection' ($script:mode -eq '2' -and $after.camera -ne $before.camera -and $after.selected -eq $before.selected) "mode $($script:mode), camera $($before.camera) to $($after.camera), selected $($after.selected)"
    $before = Get-Settled
    With-Button 'Right' { Mouse-Press 'Left' $true; Start-Sleep -Milliseconds 100; $script:mode = (Get-Settled).mode; Move-Relative 0 -40 200; Mouse-Press 'Left' $false; Start-Sleep -Milliseconds 80 }
    $after = Get-Settled
    Check 'LMB + RMB moves up' ($script:mode -eq '4' -and (To-Vector $after.camera)[1] -gt (To-Vector $before.camera)[1]) "mode $($script:mode), y $((To-Vector $before.camera)[1]) to $((To-Vector $after.camera)[1])"

    # ---- F eases ----
    $start = Get-Settled
    Press-Key 'F' 20
    $seen = @{}
    for ($i = 0; $i -lt 12; $i++) { $seen[(Get-State).camera] = 1; Start-Sleep -Milliseconds 30 }
    $state = Get-Settled
    Check 'F frames the selection with a short ease' ($seen.Count -ge 3 -and (Distance (To-Vector $state.camera) (To-Vector $state.primary)) -lt 0.01) "$($seen.Count) in-between positions, ended at $($state.camera)"

    # ---- Click selects; empty space deselects ----
    Click-View 0.97 0.95
    $state = Get-Settled
    Check 'Clicking empty space deselects' ($state.selected -eq '0') "selected $($state.selected)"
    $p = Get-WidgetPoint 'Panel.3D Viewport' 0.5 0.5
    Click-At $p[0] $p[1]; Start-Sleep -Milliseconds 250
    $state = Get-Settled
    Check 'Clicking the object in the middle of the view selects it' ($state.selected -eq '1') "selected $($state.selected)"

    # ---- Gizmo modes ----
    Over-View
    Press-Key 'E'; $e = (Get-Settled).operation
    Press-Key 'R'; $r = (Get-Settled).operation
    Press-Key 'W'; $w = (Get-Settled).operation
    Press-Key 'Space'; $space = (Get-Settled).operation
    Press-Key 'W'
    $before = (Get-Settled).space
    Chord 'Ctrl' 'Grave'
    $after = (Get-Settled).space
    Check 'W, E, R and Space choose the gizmo' ($e -eq '1' -and $r -eq '2' -and $w -eq '0' -and $space -eq '1') "E $e, R $r, W $w, Space $space"
    Check 'Ctrl+` switches world and local' ($before -ne $after) "space $before to $after"

    # ---- The gizmo: snapping, readout, cancel ----
    Click-Widget 'Overlay.Grid'; Start-Sleep -Milliseconds 250
    Click-Widget 'Overlay.Grid.Value'; Start-Sleep -Milliseconds 250
    Save-Capture (Join-Path $Output 'p2_03_grid_presets.jpg')
    Click-Widget 'Overlay.Grid.0.5'; Start-Sleep -Milliseconds 300
    $state = Get-Settled
    Check 'The overlay turns snapping on and chooses a size' ($state.snap_grid -eq '1' -and [double]$state.grid_size -eq 0.5) "snap $($state.snap_grid), size $($state.grid_size)"
    $start = To-Vector (Get-Settled).primary
    Drag-Gizmo 140 -40 { Save-Capture (Join-Path $Output 'p2_04_gizmo_readout.jpg') }
    $end = To-Vector (Get-Settled).primary
    $offs = @(0, 1, 2) | ForEach-Object { $d = ($end[$_] - $start[$_]) / 0.5; [Math]::Abs($d - [Math]::Round($d)) }
    Check 'A snapped drag moves in whole grid steps' ((Distance $start $end) -gt 0.4 -and ($offs | Measure-Object -Maximum).Maximum -lt 0.01) "moved from $($start -join ',') to $($end -join ',')"
    Click-Widget 'Overlay.Grid'; Start-Sleep -Milliseconds 250
    Over-View
    Press-Key 'LeftBracket'; $smaller = [double](Get-Settled).grid_size
    Press-Key 'RightBracket'; $larger = [double](Get-Settled).grid_size
    Check '[ and ] step the grid size' ($smaller -lt 0.5 -and $larger -eq 0.5) "$smaller then $larger"

    $before = Get-Settled
    Drag-Gizmo 90 30 { Press-Key 'Esc' }
    $after = Get-Settled
    Check 'Esc cancels a drag and leaves no step' ($after.primary -eq $before.primary -and $after.history -eq $before.history -and $after.selected -eq '1') "primary $($after.primary), history $($before.history) to $($after.history)"

    # ---- Alt+drag copies ----
    $before = Get-Settled
    Drag-Gizmo 110 20 -Modifier 'Alt'
    $after = Get-Settled
    Check 'Alt+drag copies and moves the copy, in one step' ([int]$after.entities -eq [int]$before.entities + 1 -and (Steps $after) -eq (Steps $before) + 1 -and $after.primary -ne $before.primary) "entities $($before.entities) to $($after.entities), history $($before.history) to $($after.history)"
    Save-Capture (Join-Path $Output 'p2_05_alt_copied.jpg')
    Press-Key 'Esc'; Start-Sleep -Milliseconds 100
    Click-Widget 'RowAt.0'; Start-Sleep -Milliseconds 200

    # ---- Shift+drag: the camera follows ----
    $before = Get-Settled
    Drag-Gizmo 80 -20 -Modifier 'Shift'
    $after = Get-Settled
    $moved = Distance (To-Vector $before.primary) (To-Vector $after.primary)
    $followed = Distance (To-Vector $before.camera) (To-Vector $after.camera)
    # The cursor moved about 80 pixels: far more than a few meters would mean the object ran away from it.
    Check 'Shift+drag takes the camera along, by what the mouse moved' ($moved -gt 0.1 -and $moved -lt 4 -and [Math]::Abs($moved - $followed) -lt 0.02) ('object {0:n3} m, camera {1:n3} m' -f $moved, $followed)

    # ---- V lands on a vertex. Zoomed out, so the other cube's corners are near its center on screen ----
    Over-View
    Scroll-Wheel -14; Start-Sleep -Milliseconds 300
    Click-Widget 'RowAt.1'; Start-Sleep -Milliseconds 300
    $other = To-Vector (Get-Settled).primary
    $there = Get-WidgetPoint 'Gizmo.Center'
    Click-Widget 'RowAt.0'; Start-Sleep -Milliseconds 300
    Move-ToWidget 'Gizmo.Center'; Start-Sleep -Milliseconds 200
    Mouse-Press 'Left' $true; Start-Sleep -Milliseconds 100
    try {
        Move-Relative 12 4 100
        Move-To $there[0] $there[1]; [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 120
        Key-Down 'V'; Start-Sleep -Milliseconds 300
        Save-Capture (Join-Path $Output 'p2_06_vertex_snap.jpg')
    } finally { Mouse-Press 'Left' $false; Key-Up 'V'; Start-Sleep -Milliseconds 300 }
    $state = Get-Settled
    $at = To-Vector $state.primary
    $corner = @(0, 1, 2) | ForEach-Object { [Math]::Abs([Math]::Abs($at[$_] - $other[$_]) - 0.5) }
    Check 'Holding V lands on a corner of the other cube' (($corner | Measure-Object -Maximum).Maximum -lt 0.01) "at $($at -join ','), other cube at $($other -join ',')"

    # ---- End. Lifted first, and away from the other cube, so there is only the ground below ----
    Press-Key 'F' 20; Start-Sleep -Milliseconds 400
    Drag-Gizmo 240 -160
    $start = To-Vector (Get-Settled).primary
    Over-View
    Press-Key 'End'
    $after = Get-Settled
    $end = To-Vector $after.primary
    Check 'End drops it onto what is below' ($start[1] -gt 0.6 -and [Math]::Abs($end[0] - $start[0]) -lt 1e-3 -and [Math]::Abs($end[2] - $start[2]) -lt 1e-3 -and [Math]::Abs($end[1] - 0.5) -lt 1e-3) "y $($start[1]) to $($end[1])"
    Save-Capture (Join-Path $Output 'p2_07_on_floor.jpg')
    Press-Key 'F' 20; Start-Sleep -Milliseconds 400

    # ---- Views ----
    Over-View
    $before = Get-Settled
    Chord 'Alt' 'J'
    $top = Get-Settled
    Save-Capture (Join-Path $Output 'p2_08_top.jpg')
    Chord 'Alt' 'H'
    $front = Get-Settled
    Save-Capture (Join-Path $Output 'p2_09_front.jpg')
    Chord 'Alt' 'K'
    $side = Get-Settled
    Save-Capture (Join-Path $Output 'p2_10_side.jpg')
    Check 'Alt+J, Alt+H and Alt+K give orthographic views' ($top.ortho -eq '1' -and [double]$top.pitch -gt 1.5 -and $front.ortho -eq '1' -and [Math]::Abs([double]$front.yaw) -lt 0.001 -and $side.ortho -eq '1' -and [double]$side.yaw -gt 1.5) "top pitch $($top.pitch), front yaw $($front.yaw), side yaw $($side.yaw)"
    $panned = Get-State
    With-Button 'Right' { Move-Relative 50 0 200 }
    $state = Get-Settled
    Check 'RMB pans an orthographic view and does not turn it' ($state.yaw -eq $panned.yaw -and $state.camera -ne $panned.camera) "camera $($panned.camera) to $($state.camera)"
    Chord 'Alt' 'G'
    $state = Get-Settled
    Check 'Alt+G goes back to perspective, at the same angle' ($state.ortho -eq '0' -and $state.yaw -eq $before.yaw -and $state.pitch -eq $before.pitch) "yaw $($state.yaw), pitch $($state.pitch)"

    # ---- Game view and maximize ----
    Over-View
    Press-Key 'G'
    $state = Get-Settled
    Save-Capture (Join-Path $Output 'p2_11_game_view.jpg')
    Check 'G hides the editor overlays' ($state.game_view -eq '1') "game_view $($state.game_view)"
    Press-Key 'G'
    Press-Key 'F11'
    $state = Get-Settled
    Start-Sleep -Milliseconds 300
    Save-Capture (Join-Path $Output 'p2_12_maximized.jpg')
    $full = Get-Widget 'Panel.3D Viewport'
    Update-Window
    $size = Get-WindowSize
    Check 'F11 fills the window' ($state.maximized -eq '1' -and $full.X0 -lt 2 -and $full.Y0 -lt 2 -and $full.X1 -ge $size[0] - 2) "maximized $($state.maximized), panel $($full.X0),$($full.Y0) to $($full.X1),$($full.Y1) in $($size -join 'x')"
    Over-View 0.5 0.9
    Press-Key 'F11'
    $state = Get-Settled
    Start-Sleep -Milliseconds 300
    Save-Capture (Join-Path $Output 'p2_13_restored.jpg')
    $docked = Get-Widget 'Panel.3D Viewport'
    $tab = Get-Widget 'Tab.3D Viewport'
    Check 'F11 again puts it back in its place in the layout' ($state.maximized -eq '0' -and $docked.X0 -gt 50 -and $docked.Y0 -gt 50) "panel $($docked.X0),$($docked.Y0), tab at $($tab.X0)"

    # ---- View modes ----
    Click-Widget 'View.Mode'; Start-Sleep -Milliseconds 300
    Save-Capture (Join-Path $Output 'p2_14_view_mode.jpg')
    Press-Key 'Esc'; Start-Sleep -Milliseconds 150
    Over-View
    $before = Get-Settled
    Chord 'Alt' '2'
    $after = Get-Settled
    Check 'Alt+2 is held back, not a crash or a wrong mode' ($after.ortho -eq $before.ortho -and $after.selected -eq $before.selected) 'see p2_14_view_mode: Wireframe and Unlit are grey'

    # ---- Hide ----
    Press-Key 'H'
    $state = Get-Settled
    Check 'H hides the selection and clears it, without an undo step or a dirty mark of its own' ([int]$state.hidden -ge 1 -and $state.selected -eq '0' -and $state.history -eq $after.history) "hidden $($state.hidden), selected $($state.selected), history $($state.history)"
    Save-Capture (Join-Path $Output 'p2_15_hidden.jpg')
    Chord 'Ctrl' 'H'
    $state = Get-Settled
    Check 'Ctrl+H shows everything again' ($state.hidden -eq '0') "hidden $($state.hidden)"

    # ---- Dragging a model from the Asset Browser ----
    $before = Get-Settled
    $from = Get-WidgetPoint 'Asset.cube.obj'
    $to = Get-WidgetPoint 'Panel.3D Viewport' 0.3 0.8
    $size = Get-WindowSize
    Move-To $from[0] $from[1]; [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 150
    Mouse-Press 'Left' $true; Start-Sleep -Milliseconds 120
    Move-Relative 0 -20 120
    Move-Relative ([int](($to[0] - $from[0]) * $size[0])) ([int](($to[1] - $from[1]) * $size[1]) + 20) 400
    Start-Sleep -Milliseconds 200
    Save-Capture (Join-Path $Output 'p2_16_dragging_asset.jpg')
    Mouse-Press 'Left' $false; Start-Sleep -Milliseconds 400
    $after = Get-Settled
    Check 'A model dragged to the view lands where the cursor points' ([int]$after.entities -eq [int]$before.entities + 1 -and [int]$after.selected -eq 1 -and (Distance (To-Vector $after.primary) @(0, 0.5, 0)) -gt 0.5) "entities $($before.entities) to $($after.entities), at $($after.primary)"
    Save-Capture (Join-Path $Output 'p2_17_asset_dropped.jpg')

    # ---- Bookmarks, saved beside the scene ----
    Click-Widget 'Toolbar.ScenePath'; Start-Sleep -Milliseconds 150
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'
    Type-Text $scene; Start-Sleep -Milliseconds 80; Press-Key 'Enter'; Start-Sleep -Milliseconds 200
    Move-ToWidget 'Hierarchy.Empty'
    Chord 'Ctrl' 'S'
    $saved = (Get-FileHash $scene).Hash
    Over-View
    $mark = Get-Settled
    Chord 'Ctrl' '3'
    Scroll-Wheel 5; Start-Sleep -Milliseconds 200
    With-Button 'Middle' { Move-Relative 70 40 250 }
    $away = Get-Settled
    Press-Key '3' 20
    $seen = @{}
    for ($i = 0; $i -lt 10; $i++) { $seen[(Get-State).distance] = 1; Start-Sleep -Milliseconds 30 }
    $back = Get-Settled
    Check 'Ctrl+3 stores a view, and 3 eases back to it' ($away.camera -ne $mark.camera -and $back.camera -eq $mark.camera -and $back.distance -eq $mark.distance -and $seen.Count -ge 2) "$($seen.Count) in-between distances, back at $($back.camera)"
    Check 'A bookmark lives beside the scene and does not touch it' ((Test-Path "$scene.editor") -and (Get-FileHash $scene).Hash -eq $saved -and $back.dirty -eq '0') ((Get-Content "$scene.editor" -Raw) -replace "`r?`n", ' | ')
    $exit = Stop-Editor
    Check 'The editor closes cleanly' ($exit -eq 0) "exit $exit"

    Start-Run @('--scene', "`"$scene`"")
    Over-View
    $state = Get-Settled
    $speedAfter = [double]$state.speed
    Press-Key '3' 20
    $state = Get-Settled
    Check 'The bookmark and the camera speed survive a restart' ($state.camera -eq $mark.camera -and $speedAfter -gt $speed) "camera $($state.camera), speed $speedAfter"
    $exit = Stop-Editor
    if ($null -eq $exit) { Kill-Editor }
    $results | Set-Content (Join-Path $Output 'phase2_results.txt') -Encoding utf8
} catch {
    Write-Host "RUN STOPPED: $_"
    Save-Capture (Join-Path $Output 'p2_stopped.jpg')
    $results | Set-Content (Join-Path $Output 'phase2_results_partial.txt') -Encoding utf8
    exit 2
} finally {
    Kill-Editor
}
