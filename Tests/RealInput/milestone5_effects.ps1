<#
.SYNOPSIS
  Milestone 5 slice 1 with real mouse and keyboard input: particle effects in the editor.

.DESCRIPTION
  Makes an effect in the Asset Browser, opens it in the Effect Editor, places an emitter, drags and types values (live
  while dragging, saved on release), undoes and redoes, turns the preview off and on, restarts, saves the scene and plays
  it. Runs in a new project folder with its own settings folder. Each check reads the editor's --game-log, which reports
  true state, or a file on disk. Keep hands off the mouse. A stopped run exits with code 2 and proves nothing either way.
#>
param([string]$Editor = 'build-ux-windows-msvc-release\bin\engine_editor.exe', [string]$Output = 'real-input-run', [string]$Work = "$env:TEMP\engine-m5-effects")
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
# Started with --project and no --scene, the editor saves Untitled.scene in its working folder (docs/EDITOR_GUIDE.md).
$scene = Join-Path $Work 'Untitled.scene'
$effectFile = Join-Path $project 'Assets\effects\sparks.effect'
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
function Rate-On-Disk {
    if (-not (Test-Path $effectFile)) { return $null }
    $line = Get-Content $effectFile | Where-Object { $_ -like 'rate *' } | Select-Object -First 1
    return [double]($line -split ' ')[1]
}
# Runs a command by name through the palette, so no test depends on a key that may be rebound.
function Run-Command { param([string]$Text)
    Chord @('Ctrl', 'Shift') 'P'; Start-Sleep -Milliseconds 300
    Type-Text $Text; Start-Sleep -Milliseconds 300
    Press-Key 'Enter'; Start-Sleep -Milliseconds 400
}

# Pixels of the 3D viewport that differ between two captures by more than a little: what a person would see change.
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
Start-Editor -Executable $Editor -Arguments @('--project', "`"$project`"", '--game-log', "`"$log`"", '--config-dir', "`"$config`"", '--ui-map', "`"$map`"") -WorkingDirectory $Work -Log (Join-Path $Output 'm5_effects') | Out-Null
Start-Sleep -Seconds 3
$code = 0
try {
    $state = Get-State
    Check 'The editor starts with an empty project' ($state.state -eq 'editing' -and [int]$state.assets -eq 0) "assets $($state.assets), entities $($state.entities)"

    # ---- A new effect, from the Asset Browser ----
    Click-Widget 'Asset.NewName'; Start-Sleep -Milliseconds 200
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'
    Type-Text 'sparks'; Start-Sleep -Milliseconds 150
    Click-Widget 'Asset.NewEffect'
    $state = Get-Settled
    Check 'New effect makes an effect file and selects it' ((Test-Path $effectFile) -and [int]$state.assets -eq 1 -and $state.effect_rate -like '40*') "assets $($state.assets), rate $($state.effect_rate), file $(Test-Path $effectFile)"
    Double-Click-Widget 'Asset.effects/sparks.effect'
    $state = Get-Settled
    Check 'Double-clicking it opens the Effect Editor' ((Test-Widget 'Effect.Rate (per s)') -and $state.effect_panel -eq '1') "panel $($state.effect_panel)"
    Save-Capture (Join-Path $Output 'm5_01_effect_editor.jpg')

    # ---- Placing it plays it at once, while editing ----
    $before = Get-State
    Click-Widget 'Effect.Place'
    $playing = Wait-For { [int](Get-State).particles -gt 5 } 10
    $state = Get-State
    Check 'Place in scene adds an emitter that plays while editing' ($playing -and [int]$state.entities -eq [int]$before.entities + 1 -and [int]$state.particle_emitters -eq 1) "entities $($before.entities) to $($state.entities), particles $($state.particles)"
    Check 'Its particles reach the GPU' ([int]$state.gpu_particles -gt 0) "gpu $($state.gpu_particles)"
    Check 'The preview changes nothing in the scene' ((Steps $state) -eq (Steps $before) + 1) "history $($before.history) to $($state.history)"
    Save-Capture (Join-Path $Output 'm5_02_playing.jpg')

    # ---- Dragging a value shows it live, and saves on release as one step ----
    $before = Get-State
    $from = Get-WidgetPoint 'Effect.Rate (per s)' 0.5 0.5
    $size = Get-WindowSize
    Move-To $from[0] $from[1]; [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 150
    Mouse-Press 'Left' $true; Start-Sleep -Milliseconds 120
    try {
        Move-Relative 80 0 400
        Start-Sleep -Milliseconds 300
        $during = Get-State
        $diskDuring = Rate-On-Disk
    } finally { Mouse-Press 'Left' $false; Start-Sleep -Milliseconds 500 }
    $after = Get-Settled
    Check 'While dragging, the rate changes live and the file does not' ([double]$during.effect_rate -gt 40 -and $diskDuring -eq 40) "live $($during.effect_rate), file $diskDuring"
    Check 'Releasing saves it as one undo step' ((Rate-On-Disk) -eq [double]$after.effect_rate -and (Steps $after) -eq (Steps $before) + 1) "file $(Rate-On-Disk), history $($before.history) to $($after.history)"
    $raised = [double]$after.effect_rate

    # ---- Undo and redo the edit ----
    Over-View
    Chord 'Ctrl' 'Z'
    $state = Get-Settled
    Check 'Ctrl+Z puts the rate back, in the file too' ([double]$state.effect_rate -eq 40 -and (Rate-On-Disk) -eq 40) "rate $($state.effect_rate), file $(Rate-On-Disk)"
    Chord 'Ctrl' 'Y'
    $state = Get-Settled
    Check 'Ctrl+Y brings it back' ([double]$state.effect_rate -eq $raised -and (Rate-On-Disk) -eq $raised) "rate $($state.effect_rate), wanted $raised"

    # ---- Typing a value: a burst ----
    Click-Widget 'Tab.Effect Editor'; Start-Sleep -Milliseconds 200
    $burst = Get-WidgetPoint 'Effect.Burst' 0.5 0.5
    Move-To $burst[0] $burst[1]; [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 100
    Key-Down 'Ctrl'; Start-Sleep -Milliseconds 40; Mouse-Press 'Left' $true; Start-Sleep -Milliseconds 60; Mouse-Press 'Left' $false; Key-Up 'Ctrl'
    Start-Sleep -Milliseconds 200
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'
    Type-Text '40'; Press-Key 'Enter'
    $state = Get-Settled
    Check 'Ctrl+click and typing sets the burst, saved' ($state.effect_burst -eq '40' -and ((Get-Content $effectFile) -contains 'burst 40')) "burst $($state.effect_burst)"
    Click-Widget 'Effect.Restart'
    Start-Sleep -Milliseconds 300
    $state = Get-State
    Check 'Restart starts again, with the burst' ([int]$state.particles -ge 40) "particles $($state.particles)"
    Save-Capture (Join-Path $Output 'm5_03_burst.jpg')

    # ---- The preview can be turned off, and back on ----
    Over-View
    $with = Join-Path $Output 'm5_05_effects_on.jpg'
    Save-Capture $with
    Run-Command 'Play Effects'
    $off = Wait-For { [int](Get-State).particles -eq 0 } 5
    Start-Sleep -Milliseconds 300
    $without = Join-Path $Output 'm5_06_effects_off.jpg'
    Save-Capture $without
    $seen = Count-Changed $with $without
    Check 'The particles can be seen in the viewport' ($seen -gt 25) "$seen sampled pixels differ with the preview on and off"
    $state = Get-State
    Check 'View > Play Effects turns the preview off' ($off -and $state.effects_on -eq '0') "on $($state.effects_on), particles $($state.particles)"
    Run-Command 'Play Effects'
    $on = Wait-For { [int](Get-State).particles -gt 0 } 5
    Check 'And back on' ($on -and (Get-State).effects_on -eq '1') "particles $((Get-State).particles)"

    # ---- The emitter is saved with the scene ----
    Over-View
    Chord 'Ctrl' 'S'; Start-Sleep -Milliseconds 600
    $saved = (Test-Path $scene) -and ((Get-Content $scene | Where-Object { $_ -like 'EMITTER *' }).Count -eq 1)
    Check 'Ctrl+S saves the emitter in the scene' $saved "scene $(Test-Path $scene)"

    # ---- Play mode has its own particles; Stop returns to the editor's ----
    Over-View
    Chord 'Alt' 'P'
    $running = Wait-For { (Get-State).state -eq 'playing' -and [int](Get-State).particles -gt 0 } 10
    Check 'Play runs the emitter' $running "state $((Get-State).state), particles $((Get-State).particles)"
    Save-Capture (Join-Path $Output 'm5_04_play.jpg')
    Press-Key 'Esc'; Start-Sleep -Milliseconds 300; Press-Key 'Esc'
    $stopped = Wait-For { (Get-State).state -eq 'editing' } 5
    Check 'Esc twice stops, and the preview plays on' ($stopped -and [int](Get-State).particles -gt 0) "state $((Get-State).state)"

    $closed = Stop-Editor
    Check 'The editor closes cleanly' ($closed -eq 0) "exit code $closed"
} catch {
    Write-Host "STOPPED: $_"
    [void]$results.Add("STOPPED  $_")
    $code = 2
} finally {
    Kill-Editor
    $results | Set-Content -Encoding utf8 (Join-Path $Output 'm5_effects_results.txt')
}
if ($code -eq 0 -and ($results | Where-Object { $_ -like 'FAIL*' })) { $code = 1 }
exit $code
