<#
.SYNOPSIS
  Milestone 5 slice 7 with real mouse and keyboard input: the selection outline, package names and redirectors.

.DESCRIPTION
  Opens a copy of the Physics Shooter sample. A click in the viewport selects a target, which is then outlined around
  its silhouette; Esc clears it. In the Asset Browser, F2 renames a material to a package name in a new folder, which
  leaves a redirector at the old path. In Details, a material field takes the old name typed in full, followed through
  the redirector. Fix Up Redirectors then rewrites the scene the editor does not have open, on disk, and removes the
  redirector. Ctrl+S saves. Each check reads the editor's --game-log, which reports true state, or a file on disk.
  Keep hands off the mouse. A stopped run exits with code 2 and proves nothing.
#>
param([string]$Editor = 'build-ux-windows-msvc-release\bin\engine_editor.exe', [string]$Output = 'real-input-run', [string]$Work = "$env:TEMP\engine-m5-asset-paths")
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
$assets = Join-Path $project 'Assets'
$scene = Join-Path $project 'physics_shooter.scene'
$closed = Join-Path $project 'stress_test.scene'
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
            if ($stream.Length -gt 14000) { [void]$stream.Seek(-14000, 'End') }
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
function Get-Settled { Start-Sleep -Milliseconds 400; return Get-State }
function Steps { param($State) return [int](($State.history -split '/')[0]) }
function Chord { param([string[]]$Modifiers, [string]$Key)
    foreach ($m in $Modifiers) { Key-Down $m }
    Start-Sleep -Milliseconds 40
    Press-Key $Key
    foreach ($m in $Modifiers) { Key-Up $m }
    Start-Sleep -Milliseconds 250
}
function Wait-For { param([scriptblock]$Done, [int]$Seconds = 10)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalSeconds -lt $Seconds) { if (& $Done) { return $true }; Start-Sleep -Milliseconds 100 }
    return $false
}
function Over-View { Move-ToWidget 'Panel.3D Viewport' 0.5 0.6 }
function Clear-Field { Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'; Press-Key 'Delete' }
# Details is long: scroll it until the item is wholly in sight.
function Reveal { param([string]$Name)
    for ($i = 0; $i -lt 12; $i++) {
        if (Test-Widget $Name) {
            $w = Get-Widget $Name; $panel = Get-Widget 'Panel.Inspector'
            if ($w.Y0 -ge $panel.Y0 + 20 -and $w.Y1 -le $panel.Y1 - 4 -and $w.X0 -ge $panel.X0 -and $w.X1 -le $panel.X1 - 4) { return }
        }
        Move-ToWidget 'Panel.Inspector' 0.5 0.7
        Scroll-Wheel -2; Start-Sleep -Milliseconds 250
    }
    throw "$Name did not come into sight in Details"
}
# Sampled viewport pixels that differ between two captures.
function Count-Changed { param([string]$Before, [string]$After)
    Add-Type -AssemblyName System.Drawing
    $w = Get-Widget 'Panel.3D Viewport'
    $a = New-Object System.Drawing.Bitmap $Before
    $b = New-Object System.Drawing.Bitmap $After
    $changed = 0
    try {
        for ($y = [int]$w.Y0 + 40; $y -lt [int]$w.Y1 - 10; $y += 2) {
            for ($x = [int]$w.X0 + 10; $x -lt [int]$w.X1 - 10; $x += 2) {
                $p = $a.GetPixel($x, $y); $q = $b.GetPixel($x, $y)
                if ([Math]::Abs($p.R - $q.R) + [Math]::Abs($p.G - $q.G) + [Math]::Abs($p.B - $q.B) -gt 60) { $changed++ }
            }
        }
    } finally { $a.Dispose(); $b.Dispose() }
    return $changed
}

Wait-ForIdleDesktop
Remove-Item $log -ErrorAction SilentlyContinue
Set-UiMap $map
Start-Editor -Executable $Editor -Arguments @('--project', "`"$project`"", '--scene', "`"$scene`"", '--game-log', "`"$log`"", '--config-dir', "`"$config`"", '--ui-map', "`"$map`"") -WorkingDirectory $Work -Log (Join-Path $Output 'm5_asset_paths') | Out-Null
Start-Sleep -Seconds 3
$code = 0
try {
    $state = Get-State
    Check 'The sample opens clean, with no redirectors and nothing tagged' ($state.state -eq 'editing' -and [int]$state.entities -eq 80 -and $state.redirectors -eq '0' -and $state.tags -eq '0' -and $state.dirty -eq '0') "entities $($state.entities), redirectors $($state.redirectors), tags $($state.tags)"

    # ---- A click in the viewport selects, and the outline wraps the silhouette ----
    Click-Widget 'Row.Static Target 1'; Start-Sleep -Milliseconds 300
    Over-View; Press-Key 'F'; Start-Sleep -Milliseconds 1200
    Press-Key 'Esc'
    $state = Get-Settled
    Check 'Esc clears the selection, and its tags' ([int]$state.selected -eq 0 -and $state.tags -eq '0') "selected $($state.selected), tags $($state.tags)"
    $plain = Join-Path $Output 'm5_s7_01_not_selected.jpg'; Save-Capture $plain
    Click-Widget 'Panel.3D Viewport' 0.5 0.52
    $state = Get-Settled
    Check 'A click on the target in the viewport selects it, tagged in Flecs' ($state.primary_name -eq 'Static_Target_1' -and [int]$state.selected -eq 1 -and $state.tags -eq '1') "primary $($state.primary_name), selected $($state.selected), tags $($state.tags)"
    Over-View; Start-Sleep -Milliseconds 300
    $outlined = Join-Path $Output 'm5_s7_02_outlined.jpg'; Save-Capture $outlined
    $changed = Count-Changed $plain $outlined
    Check 'The selected target is outlined (with its gizmo) and nothing else changes much' ($changed -gt 40 -and $changed -lt 4000) "$changed sampled pixels changed"
    Key-Down 'Ctrl'; Click-Widget 'Row.Static Target 2'; Key-Up 'Ctrl'
    $state = Get-Settled
    Check 'Ctrl+click in the hierarchy adds a second, which becomes the primary one' ([int]$state.selected -eq 2 -and $state.tags -eq '2' -and $state.primary_name -eq 'Static_Target_2') "selected $($state.selected), tags $($state.tags), primary $($state.primary_name)"

    # ---- F2 renames a material to a package name; a redirector is left ----
    $before = Get-State
    Click-Widget 'Asset.Search'; Start-Sleep -Milliseconds 150
    Type-Text 'target_static'; Start-Sleep -Milliseconds 400
    Click-Widget 'Asset.materials/target_static.material'; Start-Sleep -Milliseconds 300
    Press-Key 'F2'; Start-Sleep -Milliseconds 500
    Clear-Field
    Type-Text '/Game/materials/targets/target_static'; Press-Key 'Enter'
    $renamed = Wait-For { (Get-State).redirectors -eq '1' } 5
    $state = Get-Settled
    Save-Capture (Join-Path $Output 'm5_s7_03_renamed.jpg')
    $moved = Test-Path (Join-Path $assets 'materials\targets\target_static.material')
    $left = Test-Path (Join-Path $assets 'materials\target_static.material.redirect')
    Check 'Renaming to /Game/materials/targets/target_static moves the file and leaves a redirector' ($renamed -and $moved -and $left -and -not (Test-Path (Join-Path $assets 'materials\target_static.material')) -and (Steps $state) -eq (Steps $before) + 1) "moved $moved, redirector $left, redirectors $($state.redirectors), history $($before.history) to $($state.history)"
    Click-Widget 'Asset.Search'; Start-Sleep -Milliseconds 150
    Clear-Field; Press-Key 'Enter'; Start-Sleep -Milliseconds 300

    # ---- Details takes the old name, typed, through the redirector ----
    Click-Widget 'Row.Moving Target 1'; Start-Sleep -Milliseconds 300
    Click-Widget 'Tab.Inspector'; Start-Sleep -Milliseconds 250
    $before = Get-State
    Reveal 'Details.Mesh Renderer.material'
    Click-Widget 'Details.Mesh Renderer.material'; Start-Sleep -Milliseconds 400
    Type-Text '/game/materials/target_static'; Start-Sleep -Milliseconds 500
    $offered = Test-Widget 'Field.Resolved'
    Save-Capture (Join-Path $Output 'm5_s7_04_typed_old_name.jpg')
    Press-Key 'Enter'
    $state = Get-Settled
    Check 'A material field resolves the old name through the redirector, and Enter takes it' ($offered -and $state.primary_name -eq 'Moving_Target_1' -and $state.primary_material -eq 'materials/targets/target_static.material' -and (Steps $state) -eq (Steps $before) + 1) "offered $offered, material $($state.primary_material), history $($before.history) to $($state.history)"

    # ---- Fix Up Redirectors rewrites the closed scene and removes the redirector ----
    $stale = (Get-Content $closed -Raw) -match '"materials/target_static\.material"'
    Click-Widget 'Asset.FixUpRedirectors'
    $fixed = Wait-For { (Get-State).redirectors -eq '0' } 5
    $state = Get-Settled
    Save-Capture (Join-Path $Output 'm5_s7_05_fixed_up.jpg')
    $text = Get-Content $closed -Raw
    Check 'Fix Up Redirectors rewrites the scene on disk and removes the redirector' ($stale -and $fixed -and $text -match '"materials/targets/target_static\.material"' -and $text -notmatch '"materials/target_static\.material"' -and -not (Test-Path (Join-Path $assets 'materials\target_static.material.redirect')) -and -not (Test-Widget 'Asset.FixUpRedirectors')) "stale before $stale, redirectors $($state.redirectors)"

    # ---- Saved ----
    Over-View
    Chord 'Ctrl' 'S'; Start-Sleep -Milliseconds 600
    $saved = Get-Content $scene -Raw
    $state = Get-Settled
    Check 'Ctrl+S saves the open scene with the new path' ($state.dirty -eq '0' -and $saved -match '"materials/targets/target_static\.material"' -and $saved -notmatch '"materials/target_static\.material"') "dirty $($state.dirty)"

    $exit = Stop-Editor
    Check 'The editor closes cleanly' ($exit -eq 0) "exit code $exit"
} catch {
    Write-Host "STOPPED: $_"
    [void]$results.Add("STOPPED  $_")
    $code = 2
} finally {
    Kill-Editor
    $results | Set-Content -Encoding utf8 (Join-Path $Output 'm5_asset_paths_results.txt')
}
if ($code -eq 0 -and ($results | Where-Object { $_ -like 'FAIL*' })) { $code = 1 }
exit $code
