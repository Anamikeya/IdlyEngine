<#
.SYNOPSIS
  Milestone 6 slice 4 with real mouse and keyboard input: writing a script, running it, and changing it while it runs.

.DESCRIPTION
  Opens a copy of the Physics Shooter sample. New script makes Scripts/Lift.lua; a double-click opens it in the Script
  Editor, where its text is replaced by typing, with the editor's own shortcuts kept out of the text, and Ctrl+S saves
  the script, not the scene. Add Component > Script gives it to a target, and Details gives that target its own speed.
  In Play the target rises at that speed. Saved again while playing, the script moves it sideways from where it was,
  without a restart. Saved broken, the old code runs on while the error is marked at its line, and its link opens the
  script there. Mended, then stopped, the scene is as before Play, and saving it writes the script and its value.
  Each check reads the editor's --game-log, which reports true state, or a file on disk. Keep hands off the mouse. A
  stopped run exits with code 2 and proves nothing.
#>
param([string]$Editor = 'build-ux-windows-msvc-release\bin\engine_editor.exe', [string]$Output = 'real-input-run', [string]$Work = "$env:TEMP\engine-m6-scripts")
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
$lift = Join-Path $assets 'Scripts\Lift.lua'
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
    throw 'No state in the log'
}
function Get-Settled { Start-Sleep -Milliseconds 400; return Get-State }
function Steps { param($State) return [int](($State.history -split '/')[0]) }
function Position { param($State) return @($State.primary_pos -split ',' | ForEach-Object { [double]$_ }) }
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
function Reveal { param([string]$Name)
    for ($i = 0; $i -lt 14; $i++) {
        if (Test-Widget $Name) {
            $w = Get-Widget $Name; $panel = Get-Widget 'Panel.Inspector'
            if ($w.Y0 -ge $panel.Y0 + 20 -and $w.Y1 -le $panel.Y1 - 4 -and $w.X0 -ge $panel.X0 -and $w.X1 -le $panel.X1 - 4) { return }
        }
        Move-ToWidget 'Panel.Inspector' 0.5 0.7
        Scroll-Wheel -2; Start-Sleep -Milliseconds 250
    }
    throw "$Name did not come into sight in Details"
}
# Replaces the Script Editor's text with these lines, typed one by one with Enter between them.
function Type-Script { param([string[]]$Lines)
    Click-Widget 'Script.Text' 0.6 0.5; Start-Sleep -Milliseconds 200
    Clear-Field; Start-Sleep -Milliseconds 150
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        Type-Text $Lines[$i]
        if ($i -lt $Lines.Count - 1) { Press-Key 'Enter' }
    }
    Start-Sleep -Milliseconds 300
}
$rising = @('local Lift = { properties = { speed = 1.0 } }', 'function Lift:tick(dt)', 'self.entity.position = self.entity.position + Vec3(0, self.speed * dt, 0)', 'end', 'return Lift')
$sideways = @('local Lift = { properties = { speed = 1.0 } }', 'function Lift:tick(dt)', 'self.entity.position = self.entity.position + Vec3(self.speed * dt, 0, 0)', 'end', 'return Lift')
$broken = @('local Lift = { properties = { speed = 1.0 }', 'function Lift:tick(dt)', 'end', 'return Lift')

Wait-ForIdleDesktop
Remove-Item $log -ErrorAction SilentlyContinue
Set-UiMap $map
Start-Editor -Executable $Editor -Arguments @('--project', "`"$project`"", '--scene', "`"$scene`"", '--game-log', "`"$log`"", '--config-dir', "`"$config`"", '--ui-map', "`"$map`"") -WorkingDirectory $Work -Log (Join-Path $Output 'm6_scripts') | Out-Null
Start-Sleep -Seconds 3
$code = 0
try {
    $state = Get-State
    Check 'The sample opens with no script running or open' ($state.state -eq 'editing' -and $state.script_instances -eq '0' -and $state.script_open -eq '-' -and $state.dirty -eq '0') "state $($state.state), instances $($state.script_instances), open $($state.script_open)"

    # ---- New script, opened in the Script Editor ----
    Click-Widget 'Asset.NewName'; Start-Sleep -Milliseconds 150
    Clear-Field; Type-Text 'Lift'
    Click-Widget 'Asset.NewScript'
    $made = Wait-For { Test-Path $lift } 5
    Check 'New script makes Scripts/Lift.lua from the template' ($made -and (Get-Content $lift -Raw) -match 'function Script:tick') "made $made"
    Double-Click-Widget 'Asset.Scripts/Lift.lua'
    $opened = Wait-For { $s = Get-State; $s.script_open -eq '/Game/Scripts/Lift' -and $s.script_panel -eq '1' } 5
    Start-Sleep -Milliseconds 500
    Save-Capture (Join-Path $Output 'm6_s4_01_template.jpg')
    $state = Get-State
    Check 'A double-click opens it in the Script Editor' ($opened -and [int]$state.script_lines -gt 10) "open $($state.script_open), lines $($state.script_lines)"

    # ---- Typed: the text takes the keys, the editor's shortcuts do not ----
    $before = Get-State
    Type-Script $rising
    Press-Key 'R'; Start-Sleep -Milliseconds 200   # Scale mode, were it the editor's: here an "r" typed, then taken back.
    Press-Key 'Backspace'
    $checked = Wait-For { $s = Get-State; $s.script_unsaved -eq '1' -and $s.script_problem_line -eq '0' } 4
    $state = Get-State
    Save-Capture (Join-Path $Output 'm6_s4_02_typed.jpg')
    Check 'Typing replaces the text; R types an r rather than switching to Scale, and the text checks clean' ($checked -and [int]$state.script_lines -eq 5 -and $state.operation -eq $before.operation -and $state.script_focused -eq '1') "lines $($state.script_lines), operation $($before.operation) to $($state.operation), problem line $($state.script_problem_line)"
    $sceneTime = (Get-Item $scene).LastWriteTimeUtc
    Chord 'Ctrl' 'S'
    $saved = Wait-For { (Get-State).script_unsaved -eq '0' } 4
    $text = Get-Content $lift -Raw
    Check 'Ctrl+S in the Script Editor saves the script, not the scene' ($saved -and $text -match 'Vec3\(0, self\.speed \* dt, 0\)' -and $text -notmatch 'self\.speed \* dtr' -and (Get-Item $scene).LastWriteTimeUtc -eq $sceneTime) "saved $saved"

    # ---- Given to a target, with a speed of its own ----
    Click-Widget 'Row.Static Target 1'; Start-Sleep -Milliseconds 300
    Click-Widget 'Tab.Inspector'; Start-Sleep -Milliseconds 300
    $before = Get-State
    Reveal 'Details.AddComponent'
    Click-Widget 'Details.AddComponent'; Start-Sleep -Milliseconds 400
    Click-Widget 'Details.Add.Script'; Start-Sleep -Milliseconds 400
    $state = Get-Settled
    Reveal 'Details.Script.speed'
    $field = Test-Widget 'Details.Script.speed'
    Check 'Add Component > Script runs the script chosen in the Asset Browser; Details shows its speed' ($state.primary_script -eq '/Game/Scripts/Lift' -and $field -and (Steps $state) -eq (Steps $before) + 1) "script $($state.primary_script), field $field, history $($before.history) to $($state.history)"
    Click-Widget 'Details.Script.speed'; Start-Sleep -Milliseconds 300
    Clear-Field; Type-Text '3'; Press-Key 'Enter'
    $state = Get-Settled
    Save-Capture (Join-Path $Output 'm6_s4_03_details.jpg')
    $arrow = Test-Widget 'Arrow.Script.speed'
    Check 'The target gets a speed of 3 of its own, with an arrow back to the default' ($arrow -and (Steps $state) -eq (Steps $before) + 2) "arrow $arrow, history $($state.history)"
    $rest = Position $state

    # ---- In Play the script runs ----
    $clicks = 0
    do { Click-Widget 'Play.Play'; $clicks++; Start-Sleep -Seconds 1; $state = Get-State } while ($state.state -ne 'playing' -and $clicks -lt 3)
    $a = Position (Get-State); Start-Sleep -Seconds 2; $state = Get-State; $b = Position $state
    Save-Capture (Join-Path $Output 'm6_s4_04_playing.jpg')
    $rise = $b[1] - $a[1]
    Check 'In Play the target rises at its own speed, 3 m/s' ($state.script_instances -eq '1' -and $rise -gt 4.5 -and $rise -lt 7.5 -and [Math]::Abs($b[0] - $a[0]) -lt 1e-6) ('rose {0:n2} m in 2 s' -f $rise)

    # ---- Saved while playing: the running script changes, keeping where it was ----
    $logged = [int]$state.log_info
    Type-Script $sideways
    Chord 'Ctrl' 'S'
    Start-Sleep -Milliseconds 600
    $c = Position (Get-State); Start-Sleep -Seconds 1; $state = Get-State; $d = Position $state
    Save-Capture (Join-Path $Output 'm6_s4_05_reloaded.jpg')
    $across = $d[0] - $c[0]
    Check 'Saved during Play, the script moves the target sideways from where it rose to, with no restart' ($across -gt 2 -and $across -lt 4.5 -and [Math]::Abs($d[1] - $c[1]) -lt 1e-6 -and $c[1] -gt $b[1] - 1e-6 -and [int]$state.log_info -gt $logged -and $state.state -eq 'playing') ('{0:n2} m across in 1 s, height {1:n2}' -f $across, $d[1])

    # ---- Saved broken: the error is marked, and the old code runs on ----
    $errors = [int]$state.log_errors
    Type-Script $broken
    $marked = Wait-For { [int](Get-State).script_problem_line -gt 0 } 4
    Save-Capture (Join-Path $Output 'm6_s4_06_marked.jpg')
    Chord 'Ctrl' 'S'
    Start-Sleep -Milliseconds 600
    $e = Position (Get-State); Start-Sleep -Seconds 1; $state = Get-State; $f = Position $state
    Check 'Saved broken, it is marked and reported, and the last good code runs on' ($marked -and [int]$state.log_errors -gt $errors -and [int]$state.toast_errors -ge 1 -and [int]$state.script_problems -ge 1 -and $f[0] - $e[0] -gt 2) ("problem line {0}, {1:n2} m across in 1 s" -f $state.script_problem_line, ($f[0] - $e[0]))
    $line = [int]$state.script_problem_line
    Click-Widget 'Script.Text' 0.5 0.1; Press-Key 'Up'; Start-Sleep -Milliseconds 200
    Click-Widget 'Toast.0.Link'
    $jumped = Wait-For { [int](Get-State).script_cursor -eq $line } 4
    $state = Get-State
    Check "The error's link opens the script at its line" ($jumped -and $line -gt 0) "line $line, cursor $($state.script_cursor)"

    # ---- Mended, then stopped ----
    Type-Script $sideways
    Chord 'Ctrl' 'S'
    $mended = Wait-For { $s = Get-State; $s.script_unsaved -eq '0' -and $s.script_problems -eq '0' -and $s.script_problem_line -eq '0' } 5
    Check 'Mended and saved, the markers go' $mended "problems $((Get-State).script_problems)"
    Click-Widget 'Play.Stop'
    $stopped = Wait-For { (Get-State).state -eq 'editing' } 5
    $state = Get-Settled
    $back = Position $state
    Check 'Stop puts the target back where it was before Play' ($stopped -and $state.script_instances -eq '0' -and [Math]::Abs($back[0] - $rest[0]) -lt 1e-6 -and [Math]::Abs($back[1] - $rest[1]) -lt 1e-6) ("at {0:n3}, {1:n3}" -f $back[0], $back[1])

    # ---- The scene keeps the script and its value ----
    Over-View
    Chord 'Ctrl' 'S'
    $written = Wait-For { (Get-State).dirty -eq '0' } 4
    $saved = Get-Content $scene -Raw
    Check 'Ctrl+S over the view saves the scene, with the script and its speed' ($written -and $saved -match 'SCRIPT \d+ [0-9a-f]{16} "Scripts/Lift\.lua" 1 speed n 3') "written $written"

    $exit = Stop-Editor
    Check 'The editor closes cleanly' ($exit -eq 0) "exit code $exit"
} catch {
    Write-Host "STOPPED: $_"
    [void]$results.Add("STOPPED  $_")
    $code = 2
} finally {
    Kill-Editor
    $results | Set-Content -Encoding utf8 (Join-Path $Output 'm6_scripts_results.txt')
}
if ($code -eq 0 -and ($results | Where-Object { $_ -like 'FAIL*' })) { $code = 1 }
exit $code
