<#
.SYNOPSIS
  Drives the panels with real mouse and keyboard input: the generated Details panel, the Outliner, the Asset Browser
  and layouts.

.DESCRIPTION
  Works in a temporary project with its own settings folder. Each check reads the editor's --game-log, which reports
  true state, or a file on disk. Keep hands off the mouse. A stopped run exits with code 2 and proves nothing either way.
#>
param([string]$Editor = 'build-ux-windows-msvc-release\bin\engine_editor.exe', [string]$Output = 'real-input-run', [string]$Work = "$env:TEMP\engine-ux-phase3")
$ErrorActionPreference = 'Stop'
$WarningPreference = 'SilentlyContinue'
Import-Module (Join-Path $PSScriptRoot 'InputDriver.psm1') -Force

$Editor = (Resolve-Path $Editor).Path
$repo = Split-Path (Split-Path $PSScriptRoot)
New-Item -ItemType Directory -Force $Output | Out-Null
$Output = (Resolve-Path $Output).Path
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
$project = Join-Path $Work 'project'
$assets = Join-Path $project 'Assets'
New-Item -ItemType Directory -Force (Join-Path $assets 'textures'), (Join-Path $assets 'models') | Out-Null
Copy-Item (Join-Path $repo 'Samples\PhysicsShooter\Assets\models\WaterBottle.glb') (Join-Path $assets 'models\WaterBottle.glb')
Copy-Item (Join-Path $repo 'Samples\PhysicsShooter\Assets\textures\wood_base.jpg') (Join-Path $assets 'textures\wood_base.jpg')
Copy-Item (Join-Path $repo 'Tests\Assets\editor\meshes\cube.obj') (Join-Path $assets 'models\cube.obj')
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
function Get-Settled { Start-Sleep -Milliseconds 300; return Get-State }
function To-Vector { param([string]$Text) $p = $Text -split ','; return @([double]$p[0], [double]$p[1], [double]$p[2]) }
function Distance { param($A, $B) return [Math]::Sqrt(($A[0]-$B[0])*($A[0]-$B[0]) + ($A[1]-$B[1])*($A[1]-$B[1]) + ($A[2]-$B[2])*($A[2]-$B[2])) }
function Steps { param($State) return [int]($State.history -split '/')[0] }
function Chord { param([string]$Modifier, [string]$Key) Key-Down $Modifier; Press-Key $Key; Key-Up $Modifier; Start-Sleep -Milliseconds 250 }
function Modifier-Click { param([string]$Modifier, [string]$Name) $p = Get-WidgetPoint $Name; Move-To $p[0] $p[1]; Key-Down $Modifier; Start-Sleep -Milliseconds 60; Click-At $p[0] $p[1]; Key-Up $Modifier; Start-Sleep -Milliseconds 150 }
function Start-Run { param([string[]]$Extra = @())
    Remove-Item $log -ErrorAction SilentlyContinue
    Set-UiMap $map
    Start-Editor -Executable $Editor -Arguments (@('--project', "`"$project`"", '--game-log', "`"$log`"", '--config-dir', "`"$config`"", '--ui-map', "`"$map`"") + $Extra) -WorkingDirectory $Work -Log (Join-Path $Output 'phase3') | Out-Null
    Start-Sleep -Seconds 2
}
# Presses on a number field and moves the mouse, with a modifier if asked. $During runs with the button down.
function Scrub { param([string]$Name, [double]$Fx, [int]$Dx, [string]$Modifier, [scriptblock]$During)
    $p = Get-WidgetPoint $Name $Fx
    Move-To $p[0] $p[1]; [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 150
    if ($Modifier) { Key-Down $Modifier; Start-Sleep -Milliseconds 60 }
    Mouse-Press 'Left' $true; Start-Sleep -Milliseconds 80
    try {
        Move-Relative $Dx 0 250
        Start-Sleep -Milliseconds 120
        if ($During) { & $During }
    } finally { Mouse-Press 'Left' $false; if ($Modifier) { Key-Up $Modifier }; Start-Sleep -Milliseconds 300 }
}
function Type-Into { param([string]$Name, [string]$Value, [double]$Fx = 0.5)
    Click-Widget $Name $Fx; Start-Sleep -Milliseconds 250
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'
    Type-Text $Value; Start-Sleep -Milliseconds 100
    Press-Key 'Enter'; Start-Sleep -Milliseconds 300
}

Wait-ForIdleDesktop
Start-Run
try {
    # ---- Details: the panel is generated, and Transform is first ----
    Click-Widget 'Tab.Inspector'; Start-Sleep -Milliseconds 400
    Save-Capture (Join-Path $Output 'p3_01_details.jpg')
    Check 'The Details panel shows the selected entity from its tables' ((Test-Widget 'Details.Transform.position') -and (Test-Widget 'Details.Mesh Renderer.color')) 'Transform and Mesh Renderer rows are there'

    # ---- Scrubbing is live and one undo step; Shift is finer, Ctrl coarser ----
    $before = Get-Settled
    Scrub 'Details.Transform.position' 0.17 100 -During { $script:during = Get-State }
    $after = Get-Settled
    $plain = (To-Vector $after.primary)[0] - (To-Vector $before.primary)[0]
    Check 'Dragging a number moves the object while dragging' ((To-Vector $script:during.primary)[0] -ne (To-Vector $before.primary)[0]) "x $((To-Vector $before.primary)[0]), during $((To-Vector $script:during.primary)[0]), after $((To-Vector $after.primary)[0])"
    Check 'The whole drag is one undo step' ((Steps $after) -eq (Steps $before) + 1 -and (Steps $script:during) -eq (Steps $before)) "history $($before.history), during $($script:during.history), after $($after.history)"
    Chord 'Ctrl' 'Z'
    $state = Get-Settled
    Check 'One Ctrl+Z undoes all of it' ($state.primary -eq $before.primary) "primary $($state.primary)"
    $start = (To-Vector (Get-Settled).primary)[0]
    Scrub 'Details.Transform.position' 0.17 100 -Modifier 'Shift'
    $fine = (To-Vector (Get-Settled).primary)[0] - $start
    Chord 'Ctrl' 'Z'
    $start = (To-Vector (Get-Settled).primary)[0]
    Scrub 'Details.Transform.position' 0.17 100 -Modifier 'Ctrl'
    $coarse = (To-Vector (Get-Settled).primary)[0] - $start
    Chord 'Ctrl' 'Z'
    Check 'Shift scrubs finer and Ctrl coarser' ([Math]::Abs($fine) -lt [Math]::Abs($plain) * 0.3 -and [Math]::Abs($coarse) -gt [Math]::Abs($plain) * 3) ('fine {0:n3}, plain {1:n3}, coarse {2:n3}' -f $fine, $plain, $coarse)

    # ---- Click to type ----
    Type-Into 'Details.Transform.position' '2.5' 0.17
    $state = Get-Settled
    Check 'A click on a number lets you type it' ([Math]::Abs((To-Vector $state.primary)[0] - 2.5) -lt 1e-6) "primary $($state.primary)"

    # ---- Reset arrow only when modified; Show only modified ----
    Check 'A modified property has a reset arrow, and an untouched one has none' ((Test-Widget 'Arrow.Transform.position') -and -not (Test-Widget 'Arrow.Transform.scale')) 'arrow on position, none on scale'
    Click-Widget 'Details.OnlyModified'; Start-Sleep -Milliseconds 300
    Save-Capture (Join-Path $Output 'p3_02_only_modified.jpg')
    Check 'Show only modified hides the rest' ((Test-Widget 'Details.Transform.position') -and -not (Test-Widget 'Details.Transform.scale')) 'position stays, scale goes'
    Click-Widget 'Details.OnlyModified'; Start-Sleep -Milliseconds 300
    Click-Widget 'Arrow.Transform.position'; Start-Sleep -Milliseconds 300
    $state = Get-Settled
    Check 'The arrow resets it, and then goes away' ([Math]::Abs((To-Vector $state.primary)[0]) -lt 1e-6 -and -not (Test-Widget 'Arrow.Transform.position')) "primary $($state.primary)"

    # ---- Search ----
    Click-Widget 'Details.Search'; Start-Sleep -Milliseconds 150
    Type-Text 'rough'; Start-Sleep -Milliseconds 300
    Save-Capture (Join-Path $Output 'p3_03_search.jpg')
    $found = Test-Widget 'Details.Mesh Renderer.roughness'; $gone = Test-Widget 'Details.Transform.position'
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'; Press-Key 'Delete'
    Type-Text 'Surface'; Start-Sleep -Milliseconds 300
    $category = (Test-Widget 'Details.Mesh Renderer.roughness') -and (Test-Widget 'Details.Mesh Renderer.color') -and -not (Test-Widget 'Details.Transform.scale')
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'; Press-Key 'Delete'; Press-Key 'Enter'; Start-Sleep -Milliseconds 300
    Check 'Search finds a property by name, and by category' ($found -and -not $gone -and $category) "by name: $found, others hidden: $(-not $gone), by category: $category"

    # ---- Conditions: a spot light shows its cone ----
    Click-Widget 'Details.AddComponent'; Start-Sleep -Milliseconds 250
    Click-Widget 'Details.Add.Light'; Start-Sleep -Milliseconds 400
    Save-Capture (Join-Path $Output 'p3_04_light.jpg')
    $hiddenCone = -not (Test-Widget 'Details.Light.inner_cone_degrees')
    $hiddenRange = -not (Test-Widget 'Details.Light.range')
    Click-Widget 'Details.Light.type'; Start-Sleep -Milliseconds 250
    Click-ComboRow 'Details.Light.type' 2; Start-Sleep -Milliseconds 400
    $shownCone = Test-Widget 'Details.Light.inner_cone_degrees'
    Save-Capture (Join-Path $Output 'p3_05_spot.jpg')
    Check 'A directional light hides the cone and range; a spot light shows them' ($hiddenCone -and $hiddenRange -and $shownCone) "hidden then: $hiddenCone, $hiddenRange; shown now: $shownCone"
    Chord 'Ctrl' 'Z'; Chord 'Ctrl' 'Z'

    # ---- Multi-edit ----
    Click-Widget 'Create.Cube'; Start-Sleep -Milliseconds 300
    Click-Widget 'Create.Cube'; Start-Sleep -Milliseconds 300
    Click-Widget 'RowAt.1'; Start-Sleep -Milliseconds 250
    Type-Into 'Details.Transform.position' '4' 0.17
    Click-Widget 'RowAt.2'; Start-Sleep -Milliseconds 250
    Type-Into 'Details.Transform.position' '8' 0.17
    $count = [int](Get-Settled).entities
    Click-Widget 'RowAt.1'; Start-Sleep -Milliseconds 200
    Modifier-Click 'Ctrl' 'RowAt.2'
    $state = Get-Settled
    Start-Sleep -Milliseconds 200
    Save-Capture (Join-Path $Output 'p3_06_multiple_values.jpg')
    $before = Get-State
    Check 'Two selected entities are both in the panel' ($state.details_targets -eq '2') "targets $($state.details_targets)"
    Type-Into 'Details.Transform.position' '3' 0.5   # Y: both differ in X only, and share this one.
    $after = Get-Settled
    $second = To-Vector $after.primary
    Click-Widget 'RowAt.1'; Start-Sleep -Milliseconds 300
    $first = To-Vector (Get-Settled).primary
    Check 'An edit goes to all of them in one step, and leaves the axis that differs alone' ($first[1] -eq 3 -and $second[1] -eq 3 -and $first[0] -eq 4 -and $second[0] -eq 8 -and (Steps $after) -eq (Steps $before) + 1) "first $($first -join ','), second $($second -join ','), history $($before.history) to $($after.history)"

    # ---- Copy and paste a value ----
    Click-Widget 'RowAt.2'; Start-Sleep -Milliseconds 250
    Click-Widget 'Details.Transform.position' 0.5 0.5 -Right; Start-Sleep -Milliseconds 300
    Save-Capture (Join-Path $Output 'p3_07_property_menu.jpg')
    Click-Widget 'Details.Copy'; Start-Sleep -Milliseconds 200
    $copied = To-Vector (Get-Settled).primary
    Click-Widget 'RowAt.0'; Start-Sleep -Milliseconds 250
    Click-Widget 'Details.Transform.position' 0.5 0.5 -Right; Start-Sleep -Milliseconds 300
    Click-Widget 'Details.Paste'; Start-Sleep -Milliseconds 300
    $state = Get-Settled
    Check 'Right-click copies a vector and pastes it on another entity' ((Distance (To-Vector $state.primary) $copied) -lt 1e-6) "copied $($copied -join ','), now $($state.primary)"
    Click-Widget 'Details.Transform.position' 0.5 0.5 -Right; Start-Sleep -Milliseconds 300
    Click-Widget 'Details.Reset'; Start-Sleep -Milliseconds 300

    # ---- Lock ----
    Click-Widget 'RowAt.1'; Start-Sleep -Milliseconds 250
    Click-Widget 'Details.Lock'; Start-Sleep -Milliseconds 250
    $locked = (To-Vector (Get-Settled).primary)
    Click-Widget 'RowAt.2'; Start-Sleep -Milliseconds 300
    Type-Into 'Details.Transform.position' '6.5' 0.17
    $other = To-Vector (Get-Settled).primary
    Click-Widget 'Details.Lock'; Start-Sleep -Milliseconds 250
    Click-Widget 'RowAt.1'; Start-Sleep -Milliseconds 250
    $state = Get-Settled
    Check 'Lock keeps the panel on the same entity while the selection changes' ((To-Vector $state.primary)[0] -eq 6.5 -and $other[0] -ne 6.5) "locked entity x $((To-Vector $state.primary)[0]), selected one x $($other[0])"

    # ---- Favorites ----
    Click-Widget 'Star.Transform.scale'; Start-Sleep -Milliseconds 350
    Save-Capture (Join-Path $Output 'p3_08_favorites.jpg')
    $text = Get-Content (Join-Path $config 'editor.ini') -Raw
    Check 'The star saves a favorite' ($text -match 'details\.favorite\.Transform\.scale = 1') 'in editor.ini'

    # ---- A category remembers being collapsed ----
    Click-Widget 'Header.Mesh Renderer' 0.2; Start-Sleep -Milliseconds 350
    $collapsed = -not (Test-Widget 'Details.Mesh Renderer.color')
    $text = Get-Content (Join-Path $config 'editor.ini') -Raw
    Check 'A collapsed section is remembered' ($collapsed -and $text -match 'details\.open\.Mesh_Renderer = 0') "collapsed $collapsed"
    Click-Widget 'Header.Mesh Renderer' 0.2; Start-Sleep -Milliseconds 300

    # ---- Outliner: F2 ----
    Click-Widget 'RowAt.0'; Start-Sleep -Milliseconds 250
    Press-Key 'F2'; Start-Sleep -Milliseconds 300
    Save-Capture (Join-Path $Output 'p3_09_rename.jpg')
    Type-Text 'Hero'; Press-Key 'Enter'; Start-Sleep -Milliseconds 300
    Check 'F2 renames in the tree' (Test-Widget 'Row.Hero') 'a row called Hero'

    # ---- Search and filter ----
    Click-Widget 'Outliner.Search'; Start-Sleep -Milliseconds 150
    Type-Text 'her'; Start-Sleep -Milliseconds 300
    $filtered = (Test-Widget 'Row.Hero') -and -not (Test-Widget 'Row.Cube')
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'; Press-Key 'Delete'; Press-Key 'Enter'; Start-Sleep -Milliseconds 300
    Click-Widget 'Outliner.Chip.Light'; Start-Sleep -Milliseconds 300
    $none = -not (Test-Widget 'Row.Hero')
    Click-Widget 'Outliner.Chip.Light'; Start-Sleep -Milliseconds 300
    Check 'The tree searches and filters by type' ($filtered -and $none -and (Test-Widget 'Row.Hero')) "search: $filtered, Light chip: $none"

    # ---- Eye and lock ----
    $before = Get-Settled
    Click-Widget 'Eye.Hero'; Start-Sleep -Milliseconds 300
    $state = Get-Settled
    Save-Capture (Join-Path $Output 'p3_10_eye.jpg')
    Check 'The eye hides an entity, with no undo step' ([int]$state.hidden -ge 1 -and $state.history -eq $before.history) "hidden $($state.hidden), history $($state.history)"
    Click-Widget 'Eye.Hero'; Start-Sleep -Milliseconds 300
    Click-Widget 'Lock.Hero'; Start-Sleep -Milliseconds 300
    $state = Get-Settled
    Check 'The lock locks it' ($state.locked -eq '1') "locked $($state.locked)"
    Click-Widget 'Lock.Hero'; Start-Sleep -Milliseconds 300

    # ---- Group, copy and paste ----
    Click-Widget 'RowAt.0'; Start-Sleep -Milliseconds 250
    Modifier-Click 'Ctrl' 'RowAt.1'
    $before = Get-Settled
    Chord 'Ctrl' 'G'
    $state = Get-Settled
    Save-Capture (Join-Path $Output 'p3_11_grouped.jpg')
    Check 'Ctrl+G groups in one step' ([int]$state.entities -eq [int]$before.entities + 1 -and $state.selected -eq '1' -and (Steps $state) -eq (Steps $before) + 1 -and (Test-Widget 'Row.Group')) "entities $($before.entities) to $($state.entities), history $($before.history) to $($state.history)"
    Chord 'Ctrl' 'Z'
    $state = Get-Settled
    Check 'One Ctrl+Z ungroups' ([int]$state.entities -eq [int]$before.entities) "entities $($state.entities)"
    Click-Widget 'RowAt.0'; Start-Sleep -Milliseconds 250
    $before = Get-Settled
    Chord 'Ctrl' 'C'
    Chord 'Ctrl' 'V'
    $state = Get-Settled
    Check 'Ctrl+C and Ctrl+V add a copy' ([int]$state.entities -eq [int]$before.entities + 1 -and (Steps $state) -eq (Steps $before) + 1) "entities $($before.entities) to $($state.entities)"
    Chord 'Ctrl' 'Z'

    # ---- Folders ----
    Click-Widget 'Hierarchy.Empty' 0.5 0.8 -Right; Start-Sleep -Milliseconds 300
    Click-Widget 'Item.Outliner.NewFolder'; Start-Sleep -Milliseconds 400
    Type-Text 'Set Dressing'; Press-Key 'Enter'; Start-Sleep -Milliseconds 400
    $state = Get-Settled
    Check 'A new folder takes a name' ($state.folders -eq '1' -and (Test-Widget 'Folder.Set Dressing')) "folders $($state.folders)"
    $from = Get-WidgetPoint 'Row.Hero'; $to = Get-WidgetPoint 'Folder.Set Dressing'
    Drag-Between $from $to (Join-Path $Output 'p3_12_dragging_to_folder.jpg')
    $state = Get-Settled
    Save-Capture (Join-Path $Output 'p3_13_in_folder.jpg')
    Check 'Dragging a row to a folder files it' ($state.members -eq '1') "members $($state.members)"
    Chord 'Ctrl' 'Z'
    $state = Get-Settled
    Check 'One Ctrl+Z takes it back out' ($state.members -eq '0') "members $($state.members)"
    Click-Widget 'Folder.Set Dressing' 0.3; Start-Sleep -Milliseconds 250
    Press-Key 'Delete'; Start-Sleep -Milliseconds 300
    $count = [int](Get-Settled).entities
    $state = Get-Settled
    Check 'Delete removes the folder and nothing else' ($state.folders -eq '0' -and [int]$state.entities -ge 1) "folders $($state.folders), entities $($state.entities)"

    # ---- Asset Browser: thumbnails ----
    Start-Sleep -Seconds 3
    $state = Get-Settled
    Save-Capture (Join-Path $Output 'p3_14_browser.jpg')
    Check 'Models and images get thumbnails' ([int]$state.thumbnails -ge 3) "thumbnails $($state.thumbnails)"

    # ---- Filters and sources ----
    Click-Widget 'Asset.Chip.Image'; Start-Sleep -Milliseconds 300
    $chip = (Test-Widget 'Asset.textures/wood_base.jpg') -and -not (Test-Widget 'Asset.models/cube.obj')
    Click-Widget 'Asset.Chip.Image'; Start-Sleep -Milliseconds 300
    Click-Widget 'Asset.Search'; Start-Sleep -Milliseconds 150
    Type-Text 'bottle'; Start-Sleep -Milliseconds 300
    $search = (Test-Widget 'Asset.models/WaterBottle.glb') -and -not (Test-Widget 'Asset.models/cube.obj')
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'; Press-Key 'Delete'; Press-Key 'Enter'; Start-Sleep -Milliseconds 300
    Click-Widget 'Source.models'; Start-Sleep -Milliseconds 300
    $folder = (Test-Widget 'Asset.models/cube.obj') -and -not (Test-Widget 'Asset.textures/wood_base.jpg')
    Click-Widget 'Source.All'; Start-Sleep -Milliseconds 300
    Check 'Chips, search and the folder tree each narrow the view' ($chip -and $search -and $folder) "chip $chip, search $search, folder $folder"
    Click-Widget 'Asset.ViewToggle'; Start-Sleep -Milliseconds 400
    Save-Capture (Join-Path $Output 'p3_15_list.jpg')
    $listed = Test-Widget 'Asset.models/cube.obj'
    Click-Widget 'Asset.ViewToggle'; Start-Sleep -Milliseconds 300
    Check 'The list view shows the same assets' $listed 'cube.obj row'

    # ---- Tooltip, favorite ----
    Move-ToWidget 'Asset.models/WaterBottle.glb'; Start-Sleep -Milliseconds 900
    Save-Capture (Join-Path $Output 'p3_16_tooltip.jpg')
    Click-Widget 'Asset.models/cube.obj'; Start-Sleep -Milliseconds 250
    Click-Widget 'Asset.models/cube.obj' 0.5 0.5 -Right; Start-Sleep -Milliseconds 300
    Click-Widget 'Item.Asset.ToggleFavorite'; Start-Sleep -Milliseconds 300
    Click-Widget 'Source.Favorites'; Start-Sleep -Milliseconds 300
    $favorite = (Test-Widget 'Asset.models/cube.obj') -and -not (Test-Widget 'Asset.models/WaterBottle.glb')
    Click-Widget 'Source.All'; Start-Sleep -Milliseconds 300
    Check 'Favorites keeps what was starred' $favorite 'only cube.obj under Favorites'

    # ---- Drag an asset onto a Details field ----
    Click-Widget 'Create.Cube'; Start-Sleep -Milliseconds 300
    $before = Get-Settled
    $from = Get-WidgetPoint 'Asset.models/cube.obj'
    $to = Get-WidgetPoint 'Details.Mesh Renderer.mesh_asset' 0.5 0.5
    Drag-Between $from $to (Join-Path $Output 'p3_17_dragging_to_field.jpg')
    $state = Get-Settled
    Save-Capture (Join-Path $Output 'p3_18_field_assigned.jpg')
    Check 'An asset dragged onto a matching field is assigned in one step' ((Steps $state) -eq (Steps $before) + 1) "history $($before.history) to $($state.history)"

    # ---- Ctrl+B ----
    Click-Widget 'Asset.textures/wood_base.jpg'; Start-Sleep -Milliseconds 250
    $selected = (Get-Settled).selected_asset
    Move-ToWidget 'Hierarchy.Empty'
    Chord 'Ctrl' 'B'
    $state = Get-Settled
    Check 'Ctrl+B finds the selected entity''s asset' ($state.selected_asset -ne $selected) "$selected to $($state.selected_asset)"

    # ---- Drawer ----
    Chord 'Ctrl' 'Space'
    Start-Sleep -Milliseconds 500
    $state = Get-Settled
    Save-Capture (Join-Path $Output 'p3_19_drawer.jpg')
    Check 'Ctrl+Space opens the drawer' ($state.drawer -eq '1' -and (Test-Widget 'Panel.Asset Drawer')) "drawer $($state.drawer)"
    Press-Key 'Esc'; Start-Sleep -Milliseconds 500
    $state = Get-Settled
    Check 'Esc closes it, and keeps the selection' ($state.drawer -eq '0' -and [int]$state.selected -ge 1) "drawer $($state.drawer), selected $($state.selected)"
    Chord 'Ctrl' 'Space'; Start-Sleep -Milliseconds 500
    Chord 'Ctrl' 'Space'; Start-Sleep -Milliseconds 400
    Check 'Ctrl+Space closes it too' ((Get-Settled).drawer -eq '0') 'closed'

    # ---- Rename an asset that the scene uses ----
    Click-Widget 'Toolbar.ScenePath'; Start-Sleep -Milliseconds 150
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'
    Type-Text $scene; Press-Key 'Enter'; Start-Sleep -Milliseconds 200
    Move-ToWidget 'Hierarchy.Empty'
    Chord 'Ctrl' 'S'
    Click-Widget 'Asset.models/cube.obj'; Start-Sleep -Milliseconds 250
    Press-Key 'F2'; Start-Sleep -Milliseconds 400
    Save-Capture (Join-Path $Output 'p3_20_rename_asset.jpg')
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'
    Type-Text 'props/crate.obj'; Press-Key 'Enter'; Start-Sleep -Milliseconds 500
    Move-ToWidget 'Hierarchy.Empty'
    Chord 'Ctrl' 'S'
    $saved = Get-Content $scene -Raw
    Check 'Renaming an asset moves the file and updates the scene' ((Test-Path (Join-Path $assets 'props\crate.obj')) -and (Test-Path (Join-Path $assets 'props\crate.obj.meta')) -and -not (Test-Path (Join-Path $assets 'models\cube.obj')) -and $saved -match 'props/crate\.obj' -and $saved -notmatch 'models/cube\.obj') 'file, sidecar and scene all say props/crate.obj'

    # ---- Window menu and layouts ----
    Click-Widget 'Menu.Window'; Start-Sleep -Milliseconds 250
    Save-Capture (Join-Path $Output 'p3_21_window_menu.jpg')
    Click-Widget 'Item.Window.Console'; Start-Sleep -Milliseconds 400
    $closed = -not (Test-Widget 'Panel.Console')
    Click-Widget 'Menu.Window'; Start-Sleep -Milliseconds 250
    Click-Widget 'Item.Window.SaveLayout'; Start-Sleep -Milliseconds 400
    Type-Text 'No Console'; Press-Key 'Enter'; Start-Sleep -Milliseconds 400
    Check 'The Window menu closes a panel, and a layout saves under a name' ($closed -and (Test-Path (Join-Path $config 'layouts\No Console.ini'))) "closed $closed"

    # Tear a tab off, then reset.
    $tab = Get-WidgetPoint 'Tab.Lighting'
    $docked = Get-Widget 'Tab.Lighting'
    Drag-Between $tab @(0.45, 0.45) (Join-Path $Output 'p3_22_tearing_off.jpg')
    Start-Sleep -Milliseconds 300
    Save-Capture (Join-Path $Output 'p3_23_floating.jpg')
    $floating = Get-Widget 'Panel.Lighting'
    $tore = [Math]::Abs($floating.X0 - $docked.X0) -gt 50
    Click-Widget 'Menu.Window'; Start-Sleep -Milliseconds 250
    Click-Widget 'Item.Window.ResetLayout'; Start-Sleep -Milliseconds 600
    Save-Capture (Join-Path $Output 'p3_24_reset.jpg')
    $restored = (Test-Widget 'Panel.Console') -and (Test-Widget 'Tab.Lighting') -and (Get-Widget 'Tab.Lighting').X0 -gt 1000
    Check 'A tab tears off, and Reset Layout puts everything back' ($tore -and $restored) "tore $tore, restored $restored"

    Click-Widget 'Menu.Window'; Start-Sleep -Milliseconds 250
    Move-ToWidget 'Menu.Layouts'; Start-Sleep -Milliseconds 400
    Click-Widget 'Layout.No Console'; Start-Sleep -Milliseconds 700
    Save-Capture (Join-Path $Output 'p3_25_loaded.jpg')
    Check 'A saved layout loads, with the panels that were open' ((Test-Widget 'Panel.3D Viewport') -and (Test-Widget 'Panel.Scene Hierarchy') -and -not (Test-Widget 'Panel.Console')) 'the Console was closed when it was saved'

    # ---- The state survives a restart ----
    $exit = Stop-Editor -TimeoutMilliseconds 2500
    if ($null -eq $exit) { Kill-Editor }
    Start-Run @('--scene', "`"$scene`"")
    $state = Get-Settled
    Click-Widget 'Tab.Inspector'; Start-Sleep -Milliseconds 300
    Check 'The scene loads again with the renamed asset, nothing missing' ($state.dirty -eq '0') "dirty $($state.dirty)"
    Check 'A closed panel stays closed after a restart' (-not (Test-Widget 'Panel.Console')) 'the Console is still closed'
    Save-Capture (Join-Path $Output 'p3_26_reloaded.jpg')
    $exit = Stop-Editor -TimeoutMilliseconds 2500
    if ($null -eq $exit) { Kill-Editor }
    $results | Set-Content (Join-Path $Output 'phase3_results.txt') -Encoding utf8
} catch {
    Write-Host "RUN STOPPED: $_"
    Save-Capture (Join-Path $Output 'p3_stopped.jpg')
    $results | Set-Content (Join-Path $Output 'phase3_results_partial.txt') -Encoding utf8
    exit 2
} finally {
    Kill-Editor
}
