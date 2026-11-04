<#
.SYNOPSIS
  Milestone 5 slice 4 with real mouse and keyboard input: ragdolls and debris in the editor.

.DESCRIPTION
  Opens a small scene: a floor that collides, Fox (from the dependency tree) at 1:100 walking, and a small red cube.
  Gives Fox a Ragdoll through Details > Add Component, shows its capsules, sets it to fall on demand, and adds settings
  for its tail tip that take that bone's body away, with undo and redo. Saves, then simulates: Ragdoll now drops Fox
  from its walk onto the floor, Stand up hands it back to its animator, Play > Ragdoll Selected drops it again, and
  Play > Drop Debris drops pieces like the cube that go once they settle. Esc stops, and the editor scene is as it was.
  Runs in a new project folder with its own settings folder. Each check reads the editor's --game-log, which reports
  true state, or a file on disk. Keep hands off the mouse. A stopped run exits with code 2 and proves nothing.
#>
param([string]$Editor = 'build-ux-windows-msvc-release\bin\engine_editor.exe', [string]$Output = 'real-input-run', [string]$Work = "$env:TEMP\engine-m5-ragdoll",
      [string]$Fox = 'D:\AI\GameEngine\resources\filament\third_party\models\Fox\Fox.glb')
$ErrorActionPreference = 'Stop'
$WarningPreference = 'SilentlyContinue'
Import-Module (Join-Path $PSScriptRoot 'InputDriver.psm1') -Force

$Editor = (Resolve-Path $Editor).Path
New-Item -ItemType Directory -Force $Output | Out-Null
$Output = (Resolve-Path $Output).Path
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force $Work | Out-Null
$project = Join-Path $Work 'project'
New-Item -ItemType Directory -Force (Join-Path $project 'Assets') | Out-Null
Copy-Item $Fox (Join-Path $project 'Assets\Fox.glb')
# The model is named by path; its ID is the importer's, so the reference finds it by path.
$scene = Join-Path $Work 'ragdoll.scene'
@'
ENGINE_SCENE 7 4 3
ENTITY 1 0 "Floor" 0 -0.05 0 0 0 0 1 8 0.1 8
ENTITY 2 0 "Fox" 0 0 0 0 0 0 1 0.01 0.01 0.01
ENTITY 3 0 "Shard" 1.2 0.05 0.6 0 0 0 1 0.1 0.1 0.1
COMPONENTS 5
MESH 1 cube 0.55 0.55 0.55 0.9 0 - - - 1 1
COLLIDER 1 box 0 0 0 0 0 0 1 0.5 0.5 0.5 0.5 0.5 1000 0.75 0 0 0 18446744073709551615 18446744073709551615
MESH 2 cube 1 1 1 0.5 0 00000000000000a1 "Fox.glb" - - 1 1
ANIMATOR 2 - "Walk" 1 1 0 1
MESH 3 cube 0.9 0.15 0.1 0.6 0 - - - 1 1
'@ | Set-Content -Encoding ascii $scene
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
function Wait-For { param([scriptblock]$Done, [int]$Seconds = 20)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalSeconds -lt $Seconds) { if (& $Done) { return $true }; Start-Sleep -Milliseconds 100 }
    return $false
}
function Over-View { Move-ToWidget 'Panel.3D Viewport' 0.5 0.6 }
function Run-Command { param([string]$Text)
    Chord @('Ctrl', 'Shift') 'P'; Start-Sleep -Milliseconds 300
    Type-Text $Text; Start-Sleep -Milliseconds 300
    Press-Key 'Enter'; Start-Sleep -Milliseconds 400
}
# A combo's choice: open it, then click its row.
function Pick { param([string]$Name, [int]$Row)
    Click-Widget $Name; Start-Sleep -Milliseconds 300
    Click-ComboRow $Name $Row; Start-Sleep -Milliseconds 300
}
# Details is long once a model has a mesh, an animator and a ragdoll: scroll it until the item is in sight.
function Reveal { param([string]$Name)
    for ($i = 0; $i -lt 12; $i++) {
        if (Test-Widget $Name) {
            $w = Get-Widget $Name; $panel = Get-Widget 'Panel.Inspector'
            # Clipped items keep their place in the map: the whole of it must be inside the panel.
            if ($w.Y0 -ge $panel.Y0 + 20 -and $w.Y1 -le $panel.Y1 - 4 -and $w.X0 -ge $panel.X0 -and $w.X1 -le $panel.X1 - 4) { return }
        }
        Move-ToWidget 'Panel.Inspector' 0.5 0.7
        Scroll-Wheel -2; Start-Sleep -Milliseconds 250
    }
    throw "$Name did not come into sight in Details"
}
function Count-Changed { param([string]$Before, [string]$After)
    Add-Type -AssemblyName System.Drawing
    $w = Get-Widget 'Panel.3D Viewport'
    $a = New-Object System.Drawing.Bitmap $Before
    $b = New-Object System.Drawing.Bitmap $After
    $changed = 0
    try {
        for ($y = [int]$w.Y0 + 40; $y -lt [int]$w.Y1 - 10; $y += 3) {
            for ($x = [int]$w.X0 + 10; $x -lt [int]$w.X1 - 10; $x += 3) {
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
Start-Editor -Executable $Editor -Arguments @('--project', "`"$project`"", '--scene', "`"$scene`"", '--game-log', "`"$log`"", '--config-dir', "`"$config`"", '--ui-map', "`"$map`"") -WorkingDirectory $Work -Log (Join-Path $Output 'm5_ragdoll') | Out-Null
Start-Sleep -Seconds 3
$code = 0
try {
    $state = Get-State
    Check 'The editor opens the scene: a floor, Fox walking and a cube' ($state.state -eq 'editing' -and [int]$state.entities -eq 3 -and [int]$state.animators -eq 1 -and [int]$state.ragdolls -eq 0) "entities $($state.entities), animators $($state.animators), ragdolls $($state.ragdolls)"
    $dock = Get-Widget 'Panel.Lighting'
    $inSight = @('Inspector', 'Lighting', 'Material Editor', 'Effect Editor') | Where-Object { $t = Get-Widget "Tab.$_"; $t.X0 -ge $dock.X0 - 1 -and $t.X1 -le $dock.X1 + 1 }
    Check 'Every tab of the right-hand dock is in sight' (@($inSight).Count -eq 4) "in sight: $($inSight -join ', ')"

    # ---- A Ragdoll from Details ----
    Click-Widget 'Row.Fox'; Start-Sleep -Milliseconds 300
    Over-View; Press-Key 'F'; Start-Sleep -Milliseconds 900
    Click-Widget 'Tab.Inspector'; Start-Sleep -Milliseconds 250
    $before = Get-State
    Reveal 'Details.AddComponent'
    Click-Widget 'Details.AddComponent'; Start-Sleep -Milliseconds 300
    Click-Widget 'Details.Add.Ragdoll'
    $state = Get-Settled
    Check 'Add Component > Ragdoll plans bodies for Fox: one undo step' ([int]$state.ragdolls -eq 1 -and $state.ragdoll_trigger -eq '0' -and [int]$state.ragdoll_bodies -ge 12 -and (Steps $state) -eq (Steps $before) + 1) "ragdolls $($state.ragdolls), trigger $($state.ragdoll_trigger), bodies $($state.ragdoll_bodies), history $($before.history) to $($state.history)"
    $planned = [int]$state.ragdoll_bodies
    Over-View; Start-Sleep -Milliseconds 300
    $hidden = Join-Path $Output 'm5_s4_01_ragdoll_hidden.jpg'; Save-Capture $hidden
    Reveal 'Details.ShowRagdolls'
    Click-Widget 'Details.ShowRagdolls'
    $state = Get-Settled
    Over-View; Start-Sleep -Milliseconds 300
    $shown = Join-Path $Output 'm5_s4_02_ragdoll_shown.jpg'; Save-Capture $shown
    $seen = Count-Changed $hidden $shown
    Check 'Show ragdolls outlines the planned capsules on Fox, without an undo step' ($state.ragdolls_on -eq '1' -and $seen -gt 20 -and (Steps $state) -eq (Steps $before) + 1) "on $($state.ragdolls_on), $seen sampled pixels changed, history $($state.history)"

    # ---- On demand, and settings for the tail tip ----
    $before = Get-State
    Reveal 'Details.Ragdoll.trigger'
    Pick 'Details.Ragdoll.trigger' 1
    $state = Get-Settled
    Check 'Falls: On demand, one undo step' ($state.ragdoll_trigger -eq '1' -and (Steps $state) -eq (Steps $before) + 1) "trigger $($state.ragdoll_trigger), history $($before.history) to $($state.history)"
    Reveal 'Details.AddRagdollBone'
    Click-Widget 'Details.AddRagdollBone'
    $state = Get-Settled
    Check 'Add bone settings adds one, for a bone with a body' ([int]$state.ragdoll_bones -eq 1 -and [int]$state.ragdoll_bodies -eq $planned) "bones $($state.ragdoll_bones), bodies $($state.ragdoll_bodies)"
    Reveal 'Details.Ragdoll Bone 1.bone'
    Click-Widget 'Details.Ragdoll Bone 1.bone'; Start-Sleep -Milliseconds 300
    Click-Widget 'Details.Typed'; Start-Sleep -Milliseconds 150
    Type-Text 'b_Tail03_014'; Press-Key 'Enter'
    $state = Get-Settled
    Reveal 'Details.Ragdoll Bone 1.body'
    Pick 'Details.Ragdoll Bone 1.body' 2
    $removed = Get-Settled
    Check 'Its bone set to the tail tip and Body to None takes that body away' ([int]$removed.ragdoll_bodies -eq $planned - 1 -and [int]$removed.ragdoll_bones -eq 1) "bodies $planned to $($removed.ragdoll_bodies)"
    Save-Capture (Join-Path $Output 'm5_s4_03_bone_settings.jpg')
    Over-View
    Chord 'Ctrl' 'Z'
    $state = Get-Settled
    Check 'Ctrl+Z gives the tail tip its body back' ([int]$state.ragdoll_bodies -eq $planned -and (Steps $state) -eq (Steps $removed) - 1) "bodies $($state.ragdoll_bodies), history $($state.history)"
    Chord 'Ctrl' 'Y'
    $state = Get-Settled
    Check 'Ctrl+Y takes it away again' ([int]$state.ragdoll_bodies -eq $planned - 1) "bodies $($state.ragdoll_bodies)"

    # ---- Saved: the settings, and no bodies ----
    Chord 'Ctrl' 'S'; Start-Sleep -Milliseconds 600
    $text = Get-Content $scene
    $ragdollLine = $text | Where-Object { $_ -like 'RAGDOLL *' }
    $boneLine = $text | Where-Object { $_ -like 'RAGBONE *' }
    Check 'Ctrl+S saves the Ragdoll and its bone settings' ($ragdollLine -like 'RAGDOLL 2 1 1000 40 20 0.5' -and $boneLine -like 'RAGBONE 2 "b_Tail03_014" 2 *') "'$ragdollLine', '$boneLine'"
    $edited = Get-State

    # ---- Simulate: falling, standing up, falling again ----
    Over-View
    Chord 'Alt' 'S'
    $running = Wait-For { $s = Get-State; $s.state -eq 'playing' -and $s.simulating -eq '1' } 10
    Start-Sleep -Milliseconds 800
    $walking = Get-State
    Check 'Simulate runs the scene with Fox walking, and the view stays the editor''s' ($running -and [int]$walking.ragdolled -eq 0 -and $walking.skeleton_top -ne '-' -and [double]$walking.skeleton_top -gt 0.4) "state $($walking.state), simulating $($walking.simulating), skeleton top $($walking.skeleton_top) m"
    Save-Capture (Join-Path $Output 'm5_s4_04_walking.jpg')
    Reveal 'Details.RagdollNow'
    Click-Widget 'Details.RagdollNow'
    $fell = Wait-For { $s = Get-State; [int]$s.ragdolled -eq 1 -and [double]$s.skeleton_top -lt [double]$walking.skeleton_top - 0.15 } 6
    Start-Sleep -Milliseconds 1500
    $down = Get-State
    Check 'Ragdoll now drops Fox from its walk onto the floor' ($fell -and [int]$down.ragdoll_parts -eq $planned - 1 -and [double]$down.skeleton_top -gt -0.02) "ragdolled $($down.ragdolled), parts $($down.ragdoll_parts), skeleton top $($walking.skeleton_top) to $($down.skeleton_top) m"
    Save-Capture (Join-Path $Output 'm5_s4_05_fallen.jpg')
    Click-Widget 'Details.RagdollNow'
    $stood = Wait-For { $s = Get-State; [int]$s.ragdolled -eq 0 -and [double]$s.skeleton_top -gt 0.4 } 4
    Check 'Stand up hands Fox back to its animator' $stood "ragdolled $((Get-State).ragdolled), skeleton top $((Get-State).skeleton_top) m"
    Run-Command 'Ragdoll Selected'
    $again = Wait-For { $s = Get-State; [int]$s.ragdolled -eq 1 -and [double]$s.skeleton_top -lt [double]$walking.skeleton_top - 0.15 } 6
    Check 'Play > Ragdoll Selected drops it again' $again "ragdolled $((Get-State).ragdolled), skeleton top $((Get-State).skeleton_top) m"

    # ---- Debris like the cube ----
    Click-Widget 'Row.Shard'; Start-Sleep -Milliseconds 300
    Run-Command 'Drop Debris'
    $state = Get-Settled
    Check 'Play > Drop Debris drops eight pieces like the cube' ([int]$state.debris -eq 8 -and [int]$state.debris_spawned -eq 8) "debris $($state.debris), spawned $($state.debris_spawned)"
    Start-Sleep -Milliseconds 300
    Save-Capture (Join-Path $Output 'm5_s4_06_debris.jpg')
    $gone = Wait-For { [int](Get-State).debris -eq 0 } 15
    Check 'They settle and go' $gone "debris $((Get-State).debris)"

    # ---- Stop: the editor scene is as it was ----
    Over-View
    Press-Key 'Esc'
    $stopped = Wait-For { (Get-State).state -eq 'editing' } 5
    $state = Get-Settled
    Check 'Esc stops, and the editor scene, its history and its file are as they were' ($stopped -and [int]$state.ragdolled -eq 0 -and [int]$state.debris -eq 0 -and [int]$state.entities -eq 3 -and $state.history -eq $edited.history -and $state.revision -eq $edited.revision -and $state.dirty -eq '0') "state $($state.state), entities $($state.entities), history $($edited.history) to $($state.history), revision $($edited.revision) to $($state.revision), dirty $($state.dirty)"

    $closed = Stop-Editor
    Check 'The editor closes cleanly' ($closed -eq 0) "exit code $closed"
} catch {
    Write-Host "STOPPED: $_"
    [void]$results.Add("STOPPED  $_")
    $code = 2
} finally {
    Kill-Editor
    $results | Set-Content -Encoding utf8 (Join-Path $Output 'm5_ragdoll_results.txt')
}
if ($code -eq 0 -and ($results | Where-Object { $_ -like 'FAIL*' })) { $code = 1 }
exit $code
