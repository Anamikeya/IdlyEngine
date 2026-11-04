<#
.SYNOPSIS
  Milestone 5 slice 3 with real mouse and keyboard input: animators in the editor.

.DESCRIPTION
  Places Fox (from the dependency tree) by double-clicking it in the Asset Browser, which gives it an animator that
  plays while editing. In the Animation panel: crossfades to another clip, pauses, scrubs the timeline, layers a clip on
  top, adds a clip event (saved in the model's .meta, undone and redone), and makes a clip the default. Attaches a cube to
  the head joint and checks it follows the head while the scene stays unchanged. Shows the bones, saves, plays and
  stops. Runs in a new project folder with its own settings folder. Each check reads the editor's --game-log, which
  reports true state, or a file on disk. Keep hands off the mouse. A stopped run exits with code 2 and proves nothing.
#>
param([string]$Editor = 'build-ux-windows-msvc-release\bin\engine_editor.exe', [string]$Output = 'real-input-run', [string]$Work = "$env:TEMP\engine-m5-animation",
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
$meta = Join-Path $project 'Assets\Fox.glb.meta'
# Started with --project and no --scene, the editor saves Untitled.scene in its working folder (docs/EDITOR_GUIDE.md).
$scene = Join-Path $Work 'Untitled.scene'
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
# The Animation panel floats and starts closed; Window > Animation opens it.
function Show-Animation {
    if (Test-Widget 'Panel.Animation') { return }
    Run-Command 'Animation'
    if (-not (Test-Widget 'Panel.Animation')) { throw 'Window > Animation did not open the panel' }
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
function Event-Lines { if (Test-Path $meta) { return @(Get-Content $meta | Where-Object { $_ -like 'event = *' }) } return @() }

Wait-ForIdleDesktop
Remove-Item $log -ErrorAction SilentlyContinue
Set-UiMap $map
Start-Editor -Executable $Editor -Arguments @('--project', "`"$project`"", '--game-log', "`"$log`"", '--config-dir', "`"$config`"", '--ui-map', "`"$map`"") -WorkingDirectory $Work -Log (Join-Path $Output 'm5_animation') | Out-Null
Start-Sleep -Seconds 3
$code = 0
try {
    $state = Get-State
    Check 'The editor starts with Fox in the project' ($state.state -eq 'editing' -and [int]$state.assets -eq 1) "assets $($state.assets)"
    # The right-hand dock holds four tabs; each must be in sight, not scrolled away (slice 1 hid the Inspector's).
    $dock = Get-Widget 'Panel.Lighting'
    $inSight = @('Inspector', 'Lighting', 'Material Editor', 'Effect Editor') | Where-Object { $t = Get-Widget "Tab.$_"; $t.X0 -ge $dock.X0 - 1 -and $t.X1 -le $dock.X1 + 1 }
    Check 'Every tab of the right-hand dock is in sight' (@($inSight).Count -eq 4) "in sight: $($inSight -join ', ')"

    # ---- Placing it gives it an animator that plays while editing ----
    $before = Get-State
    Double-Click-Widget 'Asset.Fox.glb'
    $state = Get-Settled
    Check 'Double-clicking Fox places it with an animator on its first clip' ([int]$state.entities -eq [int]$before.entities + 1 -and [int]$state.animators -eq 1 -and $state.anim_clip -eq 'Survey') "entities $($state.entities), animators $($state.animators), clip $($state.anim_clip)"
    Over-View; Press-Key 'F'; Start-Sleep -Milliseconds 900
    $placed = Get-State
    $first = Join-Path $Output 'm5_s3_01_preview_a.jpg'; Save-Capture $first
    Start-Sleep -Milliseconds 700
    $second = Join-Path $Output 'm5_s3_02_preview_b.jpg'; Save-Capture $second
    $later = Get-State
    $seen = Count-Changed $first $second
    Check 'The preview plays while editing and can be seen' ($later.anim_time -ne $placed.anim_time -and $seen -gt 25) "time $($placed.anim_time) to $($later.anim_time), $seen sampled pixels changed"
    Check 'The preview changes nothing in the scene' ((Steps $later) -eq (Steps $placed) -and $later.revision -eq $placed.revision) "history $($placed.history) to $($later.history), revision $($placed.revision) to $($later.revision)"
    Check 'The GPU draws it posed' ([int]$later.gpu_posed -eq 1) "posed $($later.gpu_posed)"

    # ---- The Animation panel: crossfade, pause, scrub, layer ----
    Show-Animation
    Click-Widget 'Animation.Play.1'
    $fading = Wait-For { (Get-State).anim_fading -eq '1' } 2
    $state = Get-Settled
    Check 'Play on Walk crossfades to it, without an undo step' ($state.anim_clip -eq 'Walk' -and (Steps $state) -eq (Steps $later)) "clip $($state.anim_clip), fading seen $fading, history $($state.history)"
    Click-Widget 'Animation.PlayPause'
    $paused = Get-Settled
    Start-Sleep -Milliseconds 600
    $still = Get-State
    Check 'Pause holds it' ($paused.anim_paused -eq '1' -and $still.anim_time -eq $paused.anim_time) "paused $($paused.anim_paused), time $($paused.anim_time) then $($still.anim_time)"
    Drag-Between (Get-WidgetPoint 'Animation.Timeline' 0.15 0.5) (Get-WidgetPoint 'Animation.Timeline' 0.8 0.5)
    $scrubbed = Get-Settled
    Check 'Dragging the timeline scrubs the clip' ([double]$scrubbed.anim_time -gt 0.45 -and $scrubbed.anim_paused -eq '1' -and (Steps $scrubbed) -eq (Steps $later)) "time $($scrubbed.anim_time) of Walk's 0.71 s, history $($scrubbed.history)"
    Save-Capture (Join-Path $Output 'm5_s3_03_scrubbed.jpg')

    # ---- A clip event, saved in the model's .meta ----
    $before = Get-State
    Click-Widget 'Animation.EventName'; Start-Sleep -Milliseconds 150
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'
    Type-Text 'footstep'; Start-Sleep -Milliseconds 150
    Click-Widget 'Animation.AddEvent'
    $state = Get-Settled
    $lines = @(Event-Lines)
    Check 'Add at the timeline time saves an event in Fox.glb.meta, one undo step' ([int]$state.clip_events -eq 1 -and $lines.Count -eq 1 -and $lines[0] -like '* footstep "Walk"' -and (Steps $state) -eq (Steps $before) + 1) "events $($state.clip_events), meta '$($lines -join '|')', history $($before.history) to $($state.history)"
    Save-Capture (Join-Path $Output 'm5_s3_04_event.jpg')
    Over-View
    Chord 'Ctrl' 'Z'
    $state = Get-Settled
    Check 'Ctrl+Z removes it from the file' ([int]$state.clip_events -eq 0 -and @(Event-Lines).Count -eq 0) "events $($state.clip_events)"
    Chord 'Ctrl' 'Y'
    $state = Get-Settled
    Check 'Ctrl+Y puts it back' ([int]$state.clip_events -eq 1 -and @(Event-Lines).Count -eq 1) "events $($state.clip_events)"
    $heard = [int]$state.anim_events_seen
    Show-Animation
    Click-Widget 'Animation.PlayPause'
    $fired = Wait-For { [int](Get-State).anim_events_seen -gt $heard } 5
    Check 'Playing on, the event fires each time playback crosses it' $fired "events heard $heard to $((Get-State).anim_events_seen)"
    Click-Widget 'Animation.Layer.0'
    $layered = Wait-For { (Get-State).anim_layered -eq '1' } 2
    $ended = Wait-For { (Get-State).anim_layered -eq '0' } 30
    Check 'Layer plays Survey once on top, then ends' ($layered -and $ended) "seen $layered, ended $ended"

    # ---- Making Walk the default changes the scene, once ----
    $before = Get-State
    Click-Widget 'Animation.Default.1'
    $state = Get-Settled
    Check 'Default makes Walk the authored clip: one undo step' ((Steps $state) -eq (Steps $before) + 1) "history $($before.history) to $($state.history)"

    # ---- A cube on the head joint ----
    Run-Command 'Child Cube'
    $state = Get-Settled
    $cube = Get-State
    Click-Widget 'Tab.Inspector'; Start-Sleep -Milliseconds 250
    Save-Capture (Join-Path $Output 'm5_s3_05a_inspector.jpg')
    Click-Widget 'Details.AddComponent'; Start-Sleep -Milliseconds 250
    Click-Widget 'Details.Add.AttachToBone'; Start-Sleep -Milliseconds 500
    Click-Widget 'Details.Attach To Bone.bone'; Start-Sleep -Milliseconds 300
    Click-Widget 'Details.Typed'; Start-Sleep -Milliseconds 150
    Type-Text 'b_Head_05'; Press-Key 'Enter'
    $attached = Get-Settled
    Start-Sleep -Milliseconds 500
    $moving = Get-State
    Check 'A child cube attached to b_Head_05 is drawn on the head and follows it' ($attached.primary -ne $cube.primary -and $moving.primary -ne $attached.primary -and (Steps $moving) -eq (Steps $attached)) "drawn at $($cube.primary), then $($attached.primary), then $($moving.primary); history $($attached.history) to $($moving.history)"
    Save-Capture (Join-Path $Output 'm5_s3_05_attached.jpg')

    # ---- Bones, with the preview held still so only the bones change ----
    Over-View
    Run-Command 'Play Animations'
    $held = Get-Settled
    Start-Sleep -Milliseconds 300
    $without = Join-Path $Output 'm5_s3_06_bones_off.jpg'
    Save-Capture $without
    Run-Command 'Show Bones'
    $state = Get-Settled
    $with = Join-Path $Output 'm5_s3_07_bones_on.jpg'
    Save-Capture $with
    $seen = Count-Changed $without $with
    # The cube on the head is still selected: it holds still when the head does.
    Check 'View > Play Animations off holds the preview' ($held.animations_on -eq '0' -and $state.primary -eq $held.primary) "on $($held.animations_on), cube at $($held.primary) then $($state.primary)"
    Check 'View > Show Bones draws the skeleton' ($state.bones_on -eq '1' -and $seen -gt 10) "bones $($state.bones_on), $seen sampled pixels changed"
    Run-Command 'Play Animations'

    # ---- Saved, then played ----
    Over-View
    Chord 'Ctrl' 'S'; Start-Sleep -Milliseconds 600
    $text = if (Test-Path $scene) { Get-Content $scene } else { @() }
    $animatorLine = $text | Where-Object { $_ -like 'ANIMATOR *' }
    $attachLine = $text | Where-Object { $_ -like 'ATTACH *' }
    Check 'Ctrl+S saves the animator and the attachment, and no pose' (@($animatorLine).Count -eq 1 -and $animatorLine -like '*"Walk"*' -and $attachLine -like '*"b_Head_05"*') "ANIMATOR '$animatorLine', ATTACH '$attachLine'"
    Chord 'Alt' 'P'
    $running = Wait-For { $s = Get-State; $s.state -eq 'playing' -and [int]$s.animators -eq 1 } 10
    $a = Get-State; Start-Sleep -Milliseconds 500; $b = Get-State
    Check 'Play runs the animator, and the cube rides on the head' ($running -and $a.primary -ne $b.primary) "state $($b.state), animators $($b.animators), cube at $($a.primary) then $($b.primary)"
    Save-Capture (Join-Path $Output 'm5_s3_08_play.jpg')
    Press-Key 'Esc'; Start-Sleep -Milliseconds 300; Press-Key 'Esc'
    $stopped = Wait-For { (Get-State).state -eq 'editing' } 5
    Check 'Esc twice stops' $stopped "state $((Get-State).state)"

    $closed = Stop-Editor
    Check 'The editor closes cleanly' ($closed -eq 0) "exit code $closed"
} catch {
    Write-Host "STOPPED: $_"
    [void]$results.Add("STOPPED  $_")
    $code = 2
} finally {
    Kill-Editor
    $results | Set-Content -Encoding utf8 (Join-Path $Output 'm5_animation_results.txt')
}
if ($code -eq 0 -and ($results | Where-Object { $_ -like 'FAIL*' })) { $code = 1 }
exit $code
