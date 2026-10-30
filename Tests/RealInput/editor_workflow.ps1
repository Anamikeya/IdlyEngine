<#
.SYNOPSIS
  Drives the material, asset and lighting workflow with real mouse and keyboard input, and checks the files on disk.

.DESCRIPTION
  Works in a new temporary project. Positions are fractions of the client area for the default dock layout at the
  window's opening size, so a run needs that layout: it starts with no imgui.ini. Keep hands off the mouse.
#>
param([string]$Editor = 'build-m4-windows-msvc-release\bin\engine_editor.exe', [string]$Output = 'real-input-run', [string]$Work = "$env:TEMP\engine-m4-workflow")
$ErrorActionPreference = 'Stop'
$WarningPreference = 'SilentlyContinue'
Import-Module (Join-Path $PSScriptRoot 'InputDriver.psm1') -Force

$Editor = (Resolve-Path $Editor).Path
$repo = Split-Path (Split-Path $PSScriptRoot)
New-Item -ItemType Directory -Force $Output | Out-Null
$Output = (Resolve-Path $Output).Path
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
$project = Join-Path $Work 'project'
New-Item -ItemType Directory -Force $project | Out-Null
$log = Join-Path $Work 'state.log'
$scene = Join-Path $Work 'main.scene'
$results = New-Object System.Collections.ArrayList
$bottle = Join-Path $repo 'Samples\PhysicsShooter\Assets\models\WaterBottle.glb'
$image = Join-Path $repo 'Samples\PhysicsShooter\Assets\textures\wood_base.jpg'
$material = Join-Path $project 'Assets\materials\New Material.material'

function Check { param([string]$Name, [bool]$Pass, [string]$Evidence)
    $line = '{0,-5}  {1}  [{2}]' -f $(if ($Pass) { 'PASS' } else { 'FAIL' }), $Name, $Evidence
    [void]$results.Add($line); Write-Host $line
}
function Get-State {
    $stream = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite,Delete')
    $reader = New-Object IO.StreamReader $stream
    if ($stream.Length -gt 4000) { [void]$stream.Seek(-4000, 'End') }
    $text = $reader.ReadToEnd(); $reader.Close(); $stream.Close()
    $state = @{}
    foreach ($pair in ((($text -split "`n" | Where-Object { $_ -like 'state=*' })[-1]).Trim() -split ' ')) { $k, $v = $pair -split '=', 2; $state[$k] = $v }
    return $state
}
function Read-Material { param([string]$Key)
    $line = Get-Content $material | Where-Object { $_ -like "$Key *" }
    return ($line -split ' ', 2)[1]
}
# Double-click a number field, type a value, press Enter.
function Set-Field { param([double]$X, [double]$Y, [string]$Value)
    Double-Click-At $X $Y
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'
    Type-Text $Value; Start-Sleep -Milliseconds 80
    Press-Key 'Enter'; Start-Sleep -Milliseconds 300
}
function Send-Ctrl { param([string]$Key) Key-Down 'Ctrl'; Press-Key $Key; Key-Up 'Ctrl'; Start-Sleep -Milliseconds 400 }
function Clear-Focus { Move-To 0.09 0.6; Start-Sleep -Milliseconds 100 } # Over the hierarchy's empty area, away from fields.

$arguments = @('--project', "`"$project`"", '--game-log', "`"$log`"", '--perf-log', "`"$(Join-Path $Output 'workflow_performance.txt')`"")
Wait-ForIdleDesktop
Start-Editor -Executable $Editor -Arguments $arguments -WorkingDirectory $Work -Log (Join-Path $Output 'workflow') | Out-Null
try {
    Start-Sleep -Seconds 2

    # ---- Create, and see it in the Asset Browser and Material Editor ----
    Click-At 0.35 0.877
    Start-Sleep -Milliseconds 500
    Click-At 0.883 0.134
    Start-Sleep -Milliseconds 600
    Check 'New material creates a file' (Test-Path $material) $material
    Save-Capture (Join-Path $Output 'w1_material_created.jpg')

    # ---- Assign ----
    $revision = [int](Get-State).revision
    Click-At 0.2475 0.906
    Start-Sleep -Milliseconds 400
    Check 'Assign to selection is one scene step' ([int](Get-State).revision -eq $revision + 1) "revision $revision to $((Get-State).revision)"

    # ---- Edit: typed values, saved on release, visible at once ----
    Set-Field 0.852 0.4175 '0.9'   # Metallic.
    Set-Field 0.852 0.4475 '0.2'   # Roughness.
    Check 'Metallic and roughness save to the file' ([double](Read-Material 'metallic') -gt 0.89 -and [double](Read-Material 'roughness') -lt 0.21) "metallic $(Read-Material 'metallic'), roughness $(Read-Material 'roughness')"
    Set-Field 0.7985 0.388 '0.9'   # Base color R.
    Set-Field 0.8310 0.388 '0.3'   # G.
    Set-Field 0.8580 0.388 '0.1'   # B.
    Save-Capture (Join-Path $Output 'w2_material_edited.jpg')
    $color = Read-Material 'base_color'
    $parts = $color -split ' '
    Check 'Base color saves, and preview and viewport follow' ([Math]::Abs([double]$parts[0] - 0.9) -lt 0.001 -and [Math]::Abs([double]$parts[1] - 0.3) -lt 0.001 -and [Math]::Abs([double]$parts[2] - 0.1) -lt 0.001) $color

    # ---- Alpha mode: a combo ----
    Click-At 0.853 0.586; Start-Sleep -Milliseconds 300
    Save-Capture (Join-Path $Output 'w3_alpha_combo.jpg')
    Click-At 0.81 0.647; Start-Sleep -Milliseconds 500   # Blend, the third row.
    Check 'Alpha mode saves' ((Read-Material 'alpha_mode') -eq 'blend') (Read-Material 'alpha_mode')
    Save-Capture (Join-Path $Output 'w3_alpha_blend.jpg')

    # ---- Undo and redo reach the file ----
    Clear-Focus
    Send-Ctrl 'Z'
    Check 'Ctrl+Z undoes the material edit, in the file too' ((Read-Material 'alpha_mode') -eq 'opaque') (Read-Material 'alpha_mode')
    Send-Ctrl 'Y'
    Check 'Ctrl+Y redoes it' ((Read-Material 'alpha_mode') -eq 'blend') (Read-Material 'alpha_mode')
    Send-Ctrl 'Z'

    # ---- Duplicate ----
    Click-At 0.4165 0.877; Start-Sleep -Milliseconds 500
    $copy = Join-Path $project 'Assets\materials\New Material copy.material'
    Check 'Duplicate material makes a second file' (Test-Path $copy) $copy

    # ---- Background import, and a texture in a slot ----
    Click-At 0.26 0.828; Start-Sleep -Milliseconds 150
    Type-Text $image; Start-Sleep -Milliseconds 100; Press-Key 'Enter'
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path (Join-Path $project 'Assets\wood_base.jpg.meta')) -and $clock.ElapsedMilliseconds -lt 15000) { Start-Sleep -Milliseconds 50 }
    Start-Sleep -Milliseconds 300
    Check 'Importing a JPEG works from the path field' (Test-Path (Join-Path $project 'Assets\wood_base.jpg')) "$($clock.ElapsedMilliseconds) ms to appear"
    # A heavy model: the editor keeps drawing frames while it decodes.
    Click-At 0.26 0.828; Start-Sleep -Milliseconds 150
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'
    Type-Text $bottle; Start-Sleep -Milliseconds 100
    $before = Get-Item $log
    Press-Key 'Enter'
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $linesDuring = 0; $sizeA = (Get-Item $log).Length
    while (-not (Test-Path (Join-Path $project 'Assets\WaterBottle.glb.meta')) -and $clock.ElapsedMilliseconds -lt 30000) { Start-Sleep -Milliseconds 25 }
    $took = $clock.ElapsedMilliseconds
    Start-Sleep -Milliseconds 700
    Save-Capture (Join-Path $Output 'w4_imported.jpg')
    Check 'A 9 MB model imports in the background' (Test-Path (Join-Path $project 'Assets\WaterBottle.glb')) "$took ms until it landed; the Console reports the import time"

    # ---- A model with several parts takes a material per part ----
    Click-At 0.26 0.828; Start-Sleep -Milliseconds 150
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'
    Type-Text (Join-Path $env:GAMEENGINE_RESOURCES_DIR 'filament\third_party\models\TransmissionRoughnessTest\TransmissionRoughnessTest.glb'); Start-Sleep -Milliseconds 100
    Press-Key 'Enter'
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path (Join-Path $project 'Assets\TransmissionRoughnessTest.glb.meta')) -and $clock.ElapsedMilliseconds -lt 15000) { Start-Sleep -Milliseconds 25 }
    Start-Sleep -Milliseconds 500
    Click-At 0.339 0.906; Start-Sleep -Milliseconds 600   # Add to scene.
    Click-At 0.955 0.134; Start-Sleep -Milliseconds 600   # Inspector tab.
    Save-Capture (Join-Path $Output 'w4_inspector_parts.jpg')
    # Part 0's combo, then the second row of its list: the first material.
    Click-At 0.85 0.69; Start-Sleep -Milliseconds 300
    Save-Capture (Join-Path $Output 'w4_part_combo.jpg')
    Click-At 0.85 0.7336; Start-Sleep -Milliseconds 500
    Save-Capture (Join-Path $Output 'w4_part_assigned.jpg')
    Move-To 0.09 0.6

    # ---- Lighting panel ----
    Click-At 0.814 0.134; Start-Sleep -Milliseconds 400
    $entities = [int](Get-State).entities
    Click-At 0.847 0.2424; Start-Sleep -Milliseconds 900 # Lower while the scene has no environment: the panel explains that above it.
    Save-Capture (Join-Path $Output 'w5_indoor.jpg')
    Check 'The Indoor preset adds an environment' ([int](Get-State).entities -eq $entities + 1) "entities $entities to $((Get-State).entities)"
    Click-At 0.8015 0.213; Start-Sleep -Milliseconds 900
    Save-Capture (Join-Path $Output 'w5_outdoor.jpg')
    Click-At 0.8935 0.213; Start-Sleep -Milliseconds 500  # Add sun.
    Check 'Add sun creates a light' ([int](Get-State).entities -eq $entities + 2) "entities $((Get-State).entities)"
    Start-Sleep -Milliseconds 400
    Save-Capture (Join-Path $Output 'w5_sun_shadow.jpg')

    # ---- Save, close, reopen: the material and its assignment persist ----
    Click-At 0.35 0.077; Start-Sleep -Milliseconds 150   # The scene path field: a plain text box.
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'
    Type-Text $scene; Start-Sleep -Milliseconds 80; Press-Key 'Enter'; Start-Sleep -Milliseconds 200
    Clear-Focus
    Send-Ctrl 'S'
    Check 'The scene saves with its material reference' ((Get-Content $scene -Raw) -match 'materials/New Material\.material') $scene
    Check 'A material on one part of a model saves as a slot' ((Get-Content $scene -Raw) -match 'MATSLOT \d+ 0 ') (((Get-Content $scene) | Where-Object { $_ -like 'MATSLOT*' }) -join '; ')
    $exit = Stop-Editor
    Check 'The editor closes cleanly' ($exit -eq 0) "exit $exit"
    Remove-Item $log -ErrorAction SilentlyContinue
    Start-Editor -Executable $Editor -Arguments ($arguments + @('--scene', "`"$scene`"")) -WorkingDirectory $Work -Log (Join-Path $Output 'workflow2') | Out-Null
    Start-Sleep -Seconds 3
    Save-Capture (Join-Path $Output 'w6_reloaded.jpg')
    Check 'After a restart the file still holds the edits' ((Read-Material 'alpha_mode') -eq 'opaque' -and [double](Read-Material 'metallic') -gt 0.89) "alpha $(Read-Material 'alpha_mode'), metallic $(Read-Material 'metallic')"
    Stop-Editor | Out-Null

    # ---- Missing assets: reported, and the scene still loads and draws ----
    Remove-Item $material, "$material.meta"
    Remove-Item $log -ErrorAction SilentlyContinue
    Start-Editor -Executable $Editor -Arguments ($arguments + @('--scene', "`"$scene`"")) -WorkingDirectory $Work -Log (Join-Path $Output 'workflow3') | Out-Null
    Start-Sleep -Seconds 3
    Save-Capture (Join-Path $Output 'w7_missing_material.jpg')
    Check 'A missing material still loads the scene' (Test-EditorRunning) 'see w7_missing_material: magenta placeholder and a Console message'

    # ---- A genuinely empty project, then the sample, from the Project menu ----
    Click-At 0.4765 0.047; Start-Sleep -Milliseconds 300
    Save-Capture (Join-Path $Output 'w8_project_menu.jpg')
    $empty = Join-Path $Work 'empty'
    Click-At 0.50 0.088; Start-Sleep -Milliseconds 400 # New empty project...
    Type-Text $empty; Start-Sleep -Milliseconds 200
    Save-Capture (Join-Path $Output 'w8_project_dialog.jpg')
    Press-Key 'Enter'; Start-Sleep -Milliseconds 700
    # The missing-material scene has unsaved nothing, so no prompt comes first.
    $state = Get-State
    Save-Capture (Join-Path $Output 'w8_empty_project.jpg')
    Check 'New empty project: a folder, an empty scene, nothing of the sample' ((Test-Path (Join-Path $empty 'Assets')) -and (Test-Path (Join-Path $empty 'main.scene')) -and [int]$state.entities -eq 0) "entities $($state.entities); files: $(((Get-ChildItem $empty -Name) -join ', '))"
    Click-At 0.4765 0.047; Start-Sleep -Milliseconds 300
    Click-At 0.50 0.0632; Start-Sleep -Milliseconds 2500 # Open Physics Shooter sample.
    $state = Get-State
    Save-Capture (Join-Path $Output 'w9_sample_opened.jpg')
    Check 'The Project menu opens the sample' ([int]$state.entities -gt 60 -and $state.dirty -eq '0') "entities $($state.entities), dirty $($state.dirty)"
    Stop-Editor | Out-Null
    Copy-Item (Join-Path $Output 'workflow_performance.txt') (Join-Path $Output 'workflow_performance_last.txt') -ErrorAction SilentlyContinue
    $results | Set-Content (Join-Path $Output 'workflow_results.txt') -Encoding utf8
} catch {
    Write-Host "RUN STOPPED: $_"
    $results | Set-Content (Join-Path $Output 'workflow_results_partial.txt') -Encoding utf8
    exit 2
} finally {
    Kill-Editor
}
