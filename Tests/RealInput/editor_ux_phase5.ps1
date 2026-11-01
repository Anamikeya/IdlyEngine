<#
.SYNOPSIS
  Drives the editor's feedback with real mouse and keyboard input: toasts, background imports with Cancel, the Message Log
  and its links, tooltips, the cheat sheet, and typing during a gizmo drag.

.DESCRIPTION
  Runs against a temporary copy of Physics Shooter with its own settings folder. Each check reads the editor's --game-log,
  which reports true state, or a file on disk. Keep hands off the mouse. A stopped run exits with code 2 and proves
  nothing either way.
#>
param([string]$Editor = 'build-ux-windows-msvc-release\bin\engine_editor.exe', [string]$Output = 'real-input-run', [string]$Work = "$env:TEMP\engine-ux-phase5")
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

# A model big enough that importing it takes a while, written quickly.
Add-Type -TypeDefinition @'
public static class BigModel {
    public static void Write(string path, int side) {
        using (var w = new System.IO.StreamWriter(path)) {
            for (int z = 0; z < side; z++) for (int x = 0; x < side; x++) w.Write("v " + x + " 0 " + z + "\n");
            for (int z = 0; z + 1 < side; z++) for (int x = 0; x + 1 < side; x++) {
                int a = z * side + x + 1, b = a + 1, c = a + side, d = c + 1;
                w.Write("f " + a + " " + c + " " + b + "\nf " + b + " " + c + " " + d + "\n");
            }
        }
    }
}
'@
$bigFile = Join-Path $Work 'big_import.obj'
[BigModel]::Write($bigFile, 900) # Under the importer's 64 MB limit.

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
function Steps { param($State) return [int](($State.history -split '/')[0]) }
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
    Start-Editor -Executable $Editor -Arguments @('--project', "`"$project`"", '--scene', "`"$scene`"", '--game-log', "`"$log`"", '--config-dir', "`"$config`"", '--ui-map', "`"$map`"") -WorkingDirectory $Work -Log (Join-Path $Output 'phase5') | Out-Null
    Start-Sleep -Seconds 3
}
function Over-View { Move-ToWidget 'Panel.3D Viewport' 0.5 0.6 }
function Wait-For { param([scriptblock]$Done, [int]$Seconds = 30)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ($clock.Elapsed.TotalSeconds -lt $Seconds) { if (& $Done) { return $true }; Start-Sleep -Milliseconds 100 }
    return $false
}
function Toasts-Gone { return Wait-For { [int](Get-State).toasts -eq 0 } 15 }
function Import-Path { param([string]$Path)
    Click-Widget 'Asset.ImportPath'; Start-Sleep -Milliseconds 200
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'
    Type-Text $Path; Start-Sleep -Milliseconds 100
    Press-Key 'Enter'
}
# Holds the gizmo with the left button, moved a little, runs $While, then lets go.
function With-Gizmo { param([scriptblock]$While, [double]$Fx = 0, [int]$Dx = 14, [int]$Dy = 9, [int]$Offset = 0)
    $there = Get-WidgetPoint 'Gizmo.Center'
    $size = Get-WindowSize
    Move-To ($there[0] + $Offset / $size[0]) $there[1]; [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 200
    Mouse-Press 'Left' $true; Start-Sleep -Milliseconds 100
    try {
        Move-Relative $Dx $Dy 200
        Start-Sleep -Milliseconds 150
        & $While
    } finally { Mouse-Press 'Left' $false; Start-Sleep -Milliseconds 300 }
}
function Type-Keys { param([string]$Text) Type-Text $Text; Start-Sleep -Milliseconds 200 }

Wait-ForIdleDesktop
Start-Run
try {
    $state = Get-State
    Check 'The scene loads' ([int]$state.entities -gt 60 -and $state.state -eq 'editing') "$($state.entities) entities"
    [void](Toasts-Gone)

    # ---- Saving says so in the corner, and it goes by itself ----
    Over-View
    $before = Get-State
    Chord 'Ctrl' 'S'
    $state = Get-State
    Save-Capture (Join-Path $Output 'p5_01_saved.jpg')
    Check 'Saving shows a toast and logs it' ([int]$state.toasts -eq 1 -and [int]$state.log_success -eq [int]$before.log_success + 1 -and (Test-Widget 'Toast.0')) "toasts $($state.toasts), done $($before.log_success) to $($state.log_success)"
    Check 'It goes by itself' (Toasts-Gone) "toasts $((Get-State).toasts)"

    # ---- The mouse holds a toast, and x closes it ----
    Chord 'Ctrl' 'S'
    Move-ToWidget 'Toast.0'; [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Seconds 5
    $state = Get-State
    Check 'A toast waits while the mouse is on it' ([int]$state.toasts -eq 1) "toasts $($state.toasts) after 5 s"
    Click-Widget 'Toast.0.Close'; Start-Sleep -Milliseconds 300
    $state = Get-State
    Check 'x closes it' ([int]$state.toasts -eq 0) "toasts $($state.toasts)"

    # ---- An error stays longer, and the same one does not pile up ----
    $before = Get-State
    Import-Path 'C:\no\such\file.obj'; Start-Sleep -Milliseconds 800
    $state = Get-State
    Save-Capture (Join-Path $Output 'p5_02_error.jpg')
    Check 'A failed import is an error toast' ([int]$state.toast_errors -eq 1 -and [int]$state.log_errors -eq [int]$before.log_errors + 1) "toast errors $($state.toast_errors), log errors $($before.log_errors) to $($state.log_errors)"
    Import-Path 'C:\no\such\file.obj'; Start-Sleep -Milliseconds 800
    $state = Get-State
    Check 'The same error again does not stack' ([int]$state.toast_errors -eq 1 -and [int]$state.log_errors -eq [int]$before.log_errors + 2) "toast errors $($state.toast_errors), log errors $($state.log_errors)"
    Click-Widget 'Toast.0.Close'; Start-Sleep -Milliseconds 300

    # ---- A long import does not stop the editor, shows progress, and links to what it made ----
    $before = Get-State
    Import-Path $bigFile
    $shown = Wait-For { Test-Widget 'Task.0.Cancel' } 10
    $first = Get-State
    Save-Capture (Join-Path $Output 'p5_03_importing.jpg')
    Start-Sleep -Milliseconds 400
    $second = Get-State
    Check 'A long import shows a task with Cancel' ($shown -and [int]$first.import_tasks -eq 1) "tasks $($first.import_tasks), imports $($first.imports)"
    Check 'The editor keeps drawing during it' ([int]$second.imports -ge 1 -and [int]$second.frames_drawn -gt [int]$first.frames_drawn + 5) "frames $($first.frames_drawn) to $($second.frames_drawn), imports $($second.imports)"
    Check 'The import finishes' (Wait-For { [int](Get-State).imports -eq 0 } 120) "imports $((Get-State).imports)"
    Start-Sleep -Milliseconds 300
    $state = Get-State
    Save-Capture (Join-Path $Output 'p5_04_imported.jpg')
    Check 'It reports success and the asset is there' ([int]$state.assets -eq [int]$before.assets + 1 -and [int]$state.import_tasks -eq 0 -and [int]$state.log_success -gt [int]$before.log_success) "assets $($before.assets) to $($state.assets), tasks $($state.import_tasks)"
    $imported = $state.selected_asset
    Chord 'Ctrl' 'P'; Type-Text 'wood_base'; Start-Sleep -Milliseconds 200; Press-Key 'Enter'; Start-Sleep -Milliseconds 400
    $elsewhere = (Get-State).selected_asset
    $linked = Test-Widget 'Toast.0.Link'
    if ($linked) { Click-Widget 'Toast.0.Link'; Start-Sleep -Milliseconds 400 }
    $state = Get-State
    Check 'The toast''s link shows the asset' ($linked -and $elsewhere -ne $imported -and $state.selected_asset -eq $imported) "asset $elsewhere to $($state.selected_asset), wanted $imported"
    [void](Toasts-Gone)

    # ---- Cancel: the file is decoded anyway, but nothing comes of it ----
    $before = Get-State
    Copy-Item $bigFile (Join-Path $Work 'cancelled_import.obj')
    Import-Path (Join-Path $Work 'cancelled_import.obj')
    $shown = Wait-For { Test-Widget 'Task.0.Cancel' } 10
    if ($shown) { Click-Widget 'Task.0.Cancel' }
    Start-Sleep -Milliseconds 300
    $state = Get-State
    Check 'Cancel removes the task at once' ($shown -and [int]$state.import_tasks -eq 0) "tasks $($state.import_tasks), still decoding $($state.imports)"
    Check 'The decode ends' (Wait-For { [int](Get-State).imports -eq 0 } 120) "imports $((Get-State).imports)"
    $state = Get-Settled
    $copied = @(Get-ChildItem -Recurse $project -Filter 'cancelled_import*').Count
    Check 'Nothing reached the project' ([int]$state.assets -eq [int]$before.assets -and $copied -eq 0) "assets $($before.assets) to $($state.assets), files $copied"
    [void](Toasts-Gone)

    # ---- Message Log: filters, by kind ----
    Click-Widget 'Tab.Message Log'; Start-Sleep -Milliseconds 300
    Save-Capture (Join-Path $Output 'p5_05_message_log.jpg')
    $state = Get-State
    $all = [int]$state.log_shown; $errors = [int]$state.log_errors
    Check 'The Message Log shows its entries, with filters' ((Test-Widget 'Log.Filter.Error') -and $all -gt 0 -and $errors -eq 2) "shown $all, errors $errors"
    Click-Widget 'Log.Filter.Error'; Start-Sleep -Milliseconds 250
    $state = Get-State
    Check 'A filter hides that kind' ([int]$state.log_shown -eq $all - $errors) "shown $all to $($state.log_shown)"
    Click-Widget 'Log.Filter.Info'; Start-Sleep -Milliseconds 250
    $state = Get-State
    Check 'And another' ([int]$state.log_shown -eq $all - $errors - [int]$state.log_info) "shown $($state.log_shown), info $($state.log_info)"
    Save-Capture (Join-Path $Output 'p5_06_filtered.jpg')
    Click-Widget 'Log.Filter.Info'; Start-Sleep -Milliseconds 250

    # ---- Console lines are the Console's ----
    $before = Get-State
    Over-View
    Press-Key 'Grave'; Start-Sleep -Milliseconds 300
    Type-Text 'nonsense'; Start-Sleep -Milliseconds 80; Press-Key 'Enter'; Start-Sleep -Milliseconds 300
    Press-Key 'Grave'; Start-Sleep -Milliseconds 200
    $state = Get-State
    Check 'A typing mistake in the Console is no toast and no log entry' ([int]$state.toasts -eq 0 -and $state.log_errors -eq $before.log_errors -and [int]$state.console_lines -gt [int]$before.console_lines) "toasts $($state.toasts), errors $($state.log_errors), lines $($before.console_lines) to $($state.console_lines)"

    # ---- A link selects the entity ----
    Click-Widget 'Create.Cube'; Start-Sleep -Milliseconds 300
    $created = Get-State
    $index = [int]$created.console_lines - 1
    Click-Widget 'Tab.Message Log'; Start-Sleep -Milliseconds 300
    Over-View; Press-Key 'Esc'; Start-Sleep -Milliseconds 200
    $none = Get-State
    $linked = Test-Widget "Log.Link.$index"
    if ($linked) { Click-Widget "Log.Link.$index"; Start-Sleep -Milliseconds 400 }
    $state = Get-State
    Check 'A link in the log selects its entity' ($linked -and [int]$none.selected -eq 0 -and [int]$state.selected -eq 1 -and $state.primary -eq $created.primary) "selected $($none.selected) to $($state.selected), at $($state.primary)"
    Over-View; Chord 'Ctrl' 'Z'; Start-Sleep -Milliseconds 200
    $before = Get-State
    Click-Widget "Log.Link.$index"; Start-Sleep -Milliseconds 400
    $state = Get-State
    Check 'A link to what is gone says so' ([int]$state.selected -eq 0 -and [int]$state.log_warnings -eq [int]$before.log_warnings + 1) "selected $($state.selected), warnings $($before.log_warnings) to $($state.log_warnings)"
    Click-Widget 'Log.Filter.Warning'; Start-Sleep -Milliseconds 250 # Errors and warnings stay off, for the restart.

    # ---- Tooltips say the shortcut ----
    function Tooltip-Of { param([string]$Control)
        Over-View; Start-Sleep -Milliseconds 100
        Move-ToWidget $Control; [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 900
        return (Get-State).tooltip
    }
    Click-Widget 'Panel.Scene Hierarchy' 0.5 0.3; Start-Sleep -Milliseconds 300
    $tip = Tooltip-Of 'Viewport.FocusSelected'
    Save-Capture (Join-Path $Output 'p5_07_tooltip.jpg')
    Check 'A command''s tooltip names its shortcut' ($tip -eq 'Focus_Selected_(F)') "tooltip $tip"
    $tip = Tooltip-Of 'Edit.Duplicate'
    Check 'So does another' ($tip -eq 'Duplicate_(Ctrl+D)') "tooltip $tip"
    $tip = Tooltip-Of 'Toolbar.Create'
    Check 'A button with no command still says what it does' ($tip -eq 'Choose_what_to_add_to_the_scene') "tooltip $tip"

    # ---- The cheat sheet, while a key is held ----
    Over-View; Start-Sleep -Milliseconds 200
    Key-Down 'F1'; Start-Sleep -Milliseconds 350
    $state = Get-State
    $there = Test-Widget 'CheatSheet'
    Save-Capture (Join-Path $Output 'p5_08_cheat_sheet.jpg')
    Key-Up 'F1'; Start-Sleep -Milliseconds 300
    $after = Get-State
    Check 'Holding F1 shows the cheat sheet, and letting go hides it' ($state.cheat_sheet -eq '1' -and $there -and $after.cheat_sheet -eq '0') "held $($state.cheat_sheet), after $($after.cheat_sheet)"
    Key-Down 'Shift'; Start-Sleep -Milliseconds 40; Key-Down 'Slash'; Start-Sleep -Milliseconds 350
    $state = Get-State
    Key-Up 'Slash'; Key-Up 'Shift'; Start-Sleep -Milliseconds 300
    Check 'So does holding ?' ($state.cheat_sheet -eq '1' -and (Get-State).cheat_sheet -eq '0') "held $($state.cheat_sheet)"
    Move-ToWidget 'Panel.Scene Hierarchy' 0.5 0.3; [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 200
    Click-Widget 'Panel.Scene Hierarchy' 0.5 0.3; Start-Sleep -Milliseconds 300
    Key-Down 'F1'; Start-Sleep -Milliseconds 350
    Save-Capture (Join-Path $Output 'p5_09_cheat_sheet_outliner.jpg')
    Key-Up 'F1'; Start-Sleep -Milliseconds 200
    Click-Widget 'Outliner.Search'; Start-Sleep -Milliseconds 200
    Key-Down 'Shift'; Start-Sleep -Milliseconds 40; Key-Down 'Slash'; Start-Sleep -Milliseconds 300
    $state = Get-State
    Key-Up 'Slash'; Key-Up 'Shift'; Start-Sleep -Milliseconds 150
    Key-Down 'Ctrl'; Press-Key 'A'; Key-Up 'Ctrl'; Press-Key 'Delete'; Press-Key 'Esc'; Start-Sleep -Milliseconds 200
    Check 'In a text field ? is just a character' ($state.cheat_sheet -eq '0') "held $($state.cheat_sheet)"
    Over-View; Chord 'Alt' 'P'; Start-Sleep -Milliseconds 800
    Click-Widget 'Panel.3D Viewport' 0.5 0.8; Start-Sleep -Milliseconds 400
    Key-Down 'F1'; Start-Sleep -Milliseconds 300
    $state = Get-State
    Key-Up 'F1'
    Check 'While the game has the keyboard there is none' ($state.cheat_sheet -eq '0' -and $state.captured -eq '1') "held $($state.cheat_sheet), captured $($state.captured)"
    Press-Key 'Esc'; Start-Sleep -Milliseconds 300; Over-View; Press-Key 'Esc'; Start-Sleep -Milliseconds 400

    # ---- Typing during a gizmo drag. A cube of its own, at a known place ----
    Click-Widget 'Create.Cube'; Start-Sleep -Milliseconds 300
    Over-View
    Chord 'Alt' 'J'; Start-Sleep -Milliseconds 500 # From above, X to the right and the handles square on.
    Press-Key 'F'; Start-Sleep -Milliseconds 500
    Press-Key 'W'; Start-Sleep -Milliseconds 200
    $home_at = To-Vector (Get-State).primary
    $start = Get-State
    With-Gizmo {
        Type-Keys 'x3'
        $script:typing = Get-State
        Save-Capture (Join-Path $Output 'p5_10_typing.jpg')
        Press-Key 'Enter'; Start-Sleep -Milliseconds 250
        $script:confirmed = Get-State
        Move-Relative 60 40 200 # It is finished: the mouse no longer counts.
        Start-Sleep -Milliseconds 150
        $script:moved_after = Get-State
    }
    $state = Get-State
    $at = To-Vector $state.primary
    Check 'Typing shows what was typed during the drag' ($script:typing.gizmo_typed -eq 'x3') "typed $($script:typing.gizmo_typed)"
    Check 'x3 and Enter move exactly 3 along X' ($at[0] -eq $home_at[0] + 3 -and $at[1] -eq $home_at[1] -and $at[2] -eq $home_at[2]) "from $($start.primary) to $($state.primary)"
    Check 'Enter ends the drag, whatever the mouse does next' ($script:confirmed.primary -eq $state.primary -and $script:moved_after.primary -eq $state.primary) "confirmed $($script:confirmed.primary), after moving $($script:moved_after.primary)"
    Check 'It is one undo step' ((Steps $state) -eq (Steps $start) + 1) "history $($start.history) to $($state.history)"
    Over-View; Chord 'Ctrl' 'Z'
    $state = Get-Settled
    Check 'And one Ctrl+Z undoes it' ($state.primary -eq $start.primary) "primary $($state.primary)"

    With-Gizmo { Type-Keys 'z-1.5'; Press-Key 'Enter'; Start-Sleep -Milliseconds 150 }
    $at = To-Vector (Get-State).primary
    Check 'A negative value along another axis' ($at[2] -eq $home_at[2] - 1.5 -and $at[0] -eq $home_at[0]) "at $($at -join ',')"
    Over-View; Chord 'Ctrl' 'Z'

    $before = Get-Settled
    With-Gizmo {
        Type-Keys 'x9'
        $script:typing = Get-State
        Press-Key 'Esc'; Start-Sleep -Milliseconds 200
        $script:cancelled = Get-State
    }
    $state = Get-Settled
    $during_x = (To-Vector $script:typing.primary)[0]
    Check 'Esc cancels, after moving live' ($during_x -eq $home_at[0] + 9 -and $state.primary -eq $before.primary -and (Steps $state) -eq (Steps $before)) "while typing x $during_x, then $($state.primary), history $($before.history) to $($state.history)"

    With-Gizmo {
        Type-Keys 'x5'
        $script:typing = Get-State
        foreach ($i in 1..3) { Press-Key 'Backspace' }
        Start-Sleep -Milliseconds 200
        $script:deleted = Get-State
        Move-Relative 30 0 150
        Start-Sleep -Milliseconds 150
        $script:mouse_again = Get-State
        Press-Key 'Esc'; Start-Sleep -Milliseconds 150
    }
    $back = (To-Vector $script:mouse_again.primary)[0]
    Check 'Deleting what was typed gives the drag back to the mouse' ($script:deleted.gizmo_typed -eq '-' -and $back -ne $home_at[0] + 5) "typed $($script:deleted.gizmo_typed), x $back"
    Over-View; $state = Get-Settled
    Check 'That was cancelled too' ($state.primary -eq $before.primary) "primary $($state.primary)"

    With-Gizmo { Type-Keys 'y2' }
    $at = To-Vector (Get-Settled).primary
    Check 'Letting go keeps what was typed' ($at[1] -eq $home_at[1] + 2) "at $($at -join ',')"
    Over-View; Chord 'Ctrl' 'Z'

    # The X arrow, with no letter: the arrow says which axis.
    With-Gizmo -Offset 50 -Dx 12 -Dy 0 -While { Type-Keys '4'; Save-Capture (Join-Path $Output 'p5_11_arrow.jpg'); Press-Key 'Enter'; Start-Sleep -Milliseconds 150 }
    $at = To-Vector (Get-State).primary
    Check 'On an arrow, a number alone goes along it' ($at[0] -eq $home_at[0] + 4 -and $at[2] -eq $home_at[2]) "at $($at -join ',')"
    Over-View; Chord 'Ctrl' 'Z'

    # In the middle a number alone needs a letter, and Enter waits for one.
    $before = Get-Settled
    With-Gizmo {
        Type-Keys '7'
        Press-Key 'Enter'; Start-Sleep -Milliseconds 200
        $script:waiting = Get-State
        Save-Capture (Join-Path $Output 'p5_12_needs_axis.jpg')
        Press-Key 'Esc'; Start-Sleep -Milliseconds 200
    }
    $state = Get-Settled
    Check 'In the middle, a number alone waits for an axis' ($script:waiting.gizmo_typed -eq '7' -and $script:waiting.primary -eq $before.primary -and $state.primary -eq $before.primary) "typed $($script:waiting.gizmo_typed), at $($script:waiting.primary)"

    # ---- Rotation and scale ----
    Over-View; Press-Key 'E'; Start-Sleep -Milliseconds 200
    $before = Get-Settled
    # A rotate gizmo has nothing at its center: this is the ring seen from above.
    With-Gizmo -Offset 72 -Dx 0 -Dy 14 -While { Type-Keys 'y90'; Save-Capture (Join-Path $Output 'p5_12_rotating.jpg'); Press-Key 'Enter'; Start-Sleep -Milliseconds 150 }
    $state = Get-Settled
    $rot = ($state.primary_rot -split ',') | ForEach-Object { [double]$_ }
    Check 'y90 turns exactly a quarter around Y, in place' ([Math]::Abs([Math]::Abs($rot[1]) - [Math]::Sqrt(0.5)) -lt 1e-5 -and [Math]::Abs([Math]::Abs($rot[3]) - [Math]::Sqrt(0.5)) -lt 1e-5 -and $state.primary -eq $before.primary) "rotation $($state.primary_rot), at $($state.primary)"
    Over-View; Chord 'Ctrl' 'Z'
    Press-Key 'R'; Start-Sleep -Milliseconds 200
    $before = Get-Settled
    With-Gizmo { Type-Keys '2'; Press-Key 'Enter'; Start-Sleep -Milliseconds 150 }
    $state = Get-Settled
    Check 'On the center handle, a number scales all axes' ($state.primary_scale -eq '2.000000,2.000000,2.000000') "scale $($state.primary_scale)"
    Over-View; Chord 'Ctrl' 'Z'
    With-Gizmo { Type-Keys 'x3'; Press-Key 'Enter'; Start-Sleep -Milliseconds 150 }
    $state = Get-Settled
    Check 'x3 scales X alone' ($state.primary_scale -eq '3.000000,1.000000,1.000000') "scale $($state.primary_scale)"
    Over-View; Chord 'Ctrl' 'Z'
    $before = Get-Settled
    With-Gizmo {
        Type-Keys 'x0'
        Press-Key 'Enter'; Start-Sleep -Milliseconds 200
        $script:zero = Get-State
        Save-Capture (Join-Path $Output 'p5_13_zero_scale.jpg')
        Press-Key 'Esc'; Start-Sleep -Milliseconds 200
    }
    $state = Get-Settled
    Check 'A scale of 0 is refused' ($script:zero.gizmo_typed -eq 'x0' -and $state.primary_scale -eq $before.primary_scale) "typed $($script:zero.gizmo_typed), scale $($state.primary_scale)"
    Over-View; Press-Key 'W'; Start-Sleep -Milliseconds 200

    # ---- Digits mean nothing else during a drag ----
    $bookmarks = (Get-Settled).camera
    With-Gizmo { Type-Keys '1'; Start-Sleep -Milliseconds 300; $script:typing = Get-State; Press-Key 'Esc'; Start-Sleep -Milliseconds 150 }
    Check 'A digit during a drag is no bookmark' ($script:typing.camera -eq $bookmarks) "camera $($script:typing.camera), was $bookmarks"

    # ---- Filters and a closed panel survive a restart ----
    Chord 'Ctrl' 'Z'
    $exit = Stop-Editor -TimeoutMilliseconds 2500
    if ($null -eq $exit) { Kill-Editor }
    Start-Run
    Click-Widget 'Tab.Message Log'; Start-Sleep -Milliseconds 300
    $state = Get-State
    Check 'Log filters survive a restart' ([int]$state.log_shown -eq [int]$state.log_info + [int]$state.log_success) "shown $($state.log_shown), info $($state.log_info), done $($state.log_success)"
    Chord 'Ctrl','Shift' 'P'; Type-Text 'window message log'; Start-Sleep -Milliseconds 200; Press-Key 'Enter'; Start-Sleep -Milliseconds 300
    $state = Get-State
    Check 'The panel can be closed' ($state.log_panel -eq '0' -and -not (Test-Widget 'Panel.Message Log')) "panel $($state.log_panel)"
    $results | Set-Content (Join-Path $Output 'phase5_results.txt') -Encoding utf8
    Over-View
    $exit = Stop-Editor
    Check 'The editor closes cleanly' ($exit -eq 0) "exit code $exit"
    $results | Set-Content (Join-Path $Output 'phase5_results.txt') -Encoding utf8
} catch {
    Write-Host "RUN STOPPED: $_"
    Save-Capture (Join-Path $Output 'p5_stopped.jpg')
    $results | Set-Content (Join-Path $Output 'phase5_results_partial.txt') -Encoding utf8
    Kill-Editor
    exit 2
} finally {
    Kill-Editor
}
