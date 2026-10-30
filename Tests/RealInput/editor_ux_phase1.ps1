<#
.SYNOPSIS
  Drives the editor UX foundation with real mouse and keyboard input: menus from commands, context-aware shortcuts,
  rebinding, undo transactions and history, multi-selection.

.DESCRIPTION
  Works in a temporary project with its own settings folder. Each check reads the editor's --game-log, which reports
  true state, or a file on disk. Keep hands off the mouse. A stopped run exits with code 2 and proves nothing either way.
#>
param([string]$Editor = 'build-ux-windows-msvc-release\bin\engine_editor.exe', [string]$Output = 'real-input-run', [string]$Work = "$env:TEMP\engine-ux-phase1")
$ErrorActionPreference = 'Stop'
$WarningPreference = 'SilentlyContinue'
Import-Module (Join-Path $PSScriptRoot 'InputDriver.psm1') -Force

$Editor = (Resolve-Path $Editor).Path
New-Item -ItemType Directory -Force $Output | Out-Null
$Output = (Resolve-Path $Output).Path
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
$project = Join-Path $Work 'project'
New-Item -ItemType Directory -Force $project | Out-Null
$log = Join-Path $Work 'state.log'
$map = Join-Path $Work 'ui-map.txt'
$config = Join-Path $Work 'config'
$keymap = Join-Path $config 'keymap.ini'
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
            if ($stream.Length -gt 4000) { [void]$stream.Seek(-4000, 'End') }
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
# Give the state log (20 lines a second) time to catch up with what the last input did.
function Get-Settled { Start-Sleep -Milliseconds 250; return Get-State }
function Send-Ctrl { param([string]$Key) Key-Down 'Ctrl'; Press-Key $Key; Key-Up 'Ctrl'; Start-Sleep -Milliseconds 200 }
function Modifier-Click { param([string]$Modifier, [string]$Name) $p = Get-WidgetPoint $Name; Move-To $p[0] $p[1]; Key-Down $Modifier; Start-Sleep -Milliseconds 60; Click-At $p[0] $p[1]; Key-Up $Modifier; Start-Sleep -Milliseconds 120 }
function Start-Run { param([string[]]$Extra = @())
    Remove-Item $log -ErrorAction SilentlyContinue
    Set-UiMap $map
    Start-Editor -Executable $Editor -Arguments (@('--project', "`"$project`"", '--game-log', "`"$log`"", '--config-dir', "`"$config`"", '--ui-map', "`"$map`"") + $Extra) -WorkingDirectory $Work -Log (Join-Path $Output 'phase1') | Out-Null
    Start-Sleep -Seconds 2
}

Wait-ForIdleDesktop
Start-Run
try {
    $state = Get-State
    Save-Capture (Join-Path $Output 'p1_01_opened.jpg')
    Check 'The editor opens with its starter cube, one entity selected' ([int]$state.entities -eq 1 -and [int]$state.selected -eq 1) "entities $($state.entities), selected $($state.selected)"
    Check 'A fresh settings folder gets a layout file on exit, not imgui.ini in the working folder' (-not (Test-Path (Join-Path $Work 'imgui.ini'))) 'no imgui.ini'

    # ---- Menus are built from commands, and show shortcuts and enabled state ----
    Click-Widget 'Menu.Edit'; Start-Sleep -Milliseconds 300
    Save-Capture (Join-Path $Output 'p1_02_edit_menu.jpg')
    Click-Widget 'Item.Edit.Duplicate'; Start-Sleep -Milliseconds 300
    $state = Get-Settled
    Check 'A menu item runs its command' ([int]$state.entities -eq 2) "entities $($state.entities)"
    Send-Ctrl 'Z'
    $state = Get-Settled
    Check 'Ctrl+Z undoes it' ([int]$state.entities -eq 1) "entities $($state.entities), history $($state.history)"
    Key-Down 'Ctrl'; Key-Down 'Shift'; Press-Key 'Z'; Key-Up 'Shift'; Key-Up 'Ctrl'
    $state = Get-Settled
    Check 'Ctrl+Shift+Z redoes it' ([int]$state.entities -eq 2) "entities $($state.entities), history $($state.history)"
    Send-Ctrl 'Z'

    # ---- Esc closes an open menu and does nothing else ----
    Click-Widget 'Row.Cube'; Start-Sleep -Milliseconds 200
    Click-Widget 'Menu.Edit'; Start-Sleep -Milliseconds 300
    $before = [int](Get-Settled).selected
    Press-Key 'Esc'
    $state = Get-Settled
    Check 'Esc closes a menu without deselecting' ($before -eq 1 -and [int]$state.selected -eq 1) "selected $before to $($state.selected)"

    # ---- Multi-selection ----
    Click-Widget 'Create.Cube'; Start-Sleep -Milliseconds 300
    Click-Widget 'Create.Cube'; Start-Sleep -Milliseconds 300
    $count = [int](Get-Settled).entities
    Move-ToWidget 'Hierarchy.Empty'
    Send-Ctrl 'A'
    $state = Get-Settled
    Check 'Ctrl+A selects everything' ([int]$state.selected -eq $count) "selected $($state.selected) of $count"
    Save-Capture (Join-Path $Output 'p1_03_selected_all.jpg')
    Press-Key 'Esc'
    $state = Get-Settled
    Check 'Esc deselects' ([int]$state.selected -eq 0) "selected $($state.selected)"
    Click-Widget 'Row.Cube'; Start-Sleep -Milliseconds 200
    $state = Get-Settled
    Check 'Clicking a row selects one' ([int]$state.selected -eq 1) "selected $($state.selected)"
    Modifier-Click 'Ctrl' 'RowAt.0'
    $state = Get-Settled
    Check 'Ctrl+click adds a row' ([int]$state.selected -eq 2) "selected $($state.selected)"
    Modifier-Click 'Ctrl' 'RowAt.0'
    $state = Get-Settled
    Check 'Ctrl+click again takes it away' ([int]$state.selected -eq 1) "selected $($state.selected)"
    Click-Widget 'RowAt.0'; Start-Sleep -Milliseconds 200
    Modifier-Click 'Shift' 'RowAt.2'
    $state = Get-Settled
    Check 'Shift+click selects the range' ([int]$state.selected -eq 3) "selected $($state.selected)"
    Click-Widget 'RowAt.1'; Start-Sleep -Milliseconds 300
    $state = Get-Settled
    Check 'A plain click on a selected row leaves only that row' ([int]$state.selected -eq 1) "selected $($state.selected)"
    # Dragging one of two selected rows onto a third moves both, in one undo step.
    Modifier-Click 'Ctrl' 'RowAt.2'
    $before = Get-Settled
    $p = Get-WidgetPoint 'RowAt.2'; $q = Get-WidgetPoint 'RowAt.0'
    Move-To $p[0] $p[1]; [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 120
    Mouse-Button $true; Start-Sleep -Milliseconds 80
    Update-Window
    $dy = [int](($q[1] - $p[1]) * (Get-WindowSize)[1])
    Move-Relative 0 ([int]($dy / 2)) 150
    Move-Relative 0 ($dy - [int]($dy / 2)) 150
    Start-Sleep -Milliseconds 150
    Save-Capture (Join-Path $Output 'p1_03b_dragging_rows.jpg')
    Mouse-Button $false; Start-Sleep -Milliseconds 400
    $state = Get-Settled
    Save-Capture (Join-Path $Output 'p1_03c_rows_dropped.jpg')
    Check 'Dragging a multi-selection reparents it in one step' ([int]($state.history -split '/')[0] -eq [int]($before.history -split '/')[0] + 1 -and [int]$state.selected -eq 2) "history $($before.history) to $($state.history), selected $($state.selected)"
    Send-Ctrl 'Z'
    Move-ToWidget 'Hierarchy.Empty'
    Send-Ctrl 'A'
    $before = Get-Settled
    Send-Ctrl 'D'
    $state = Get-Settled
    $steps = [int]($before.history -split '/')[0]
    Check 'Ctrl+D copies a multi-selection in one undo step' ([int]$state.entities -eq 2 * $count -and [int]($state.history -split '/')[0] -eq $steps + 1) "entities $count to $($state.entities), history $($before.history) to $($state.history)"
    Send-Ctrl 'Z'
    $state = Get-Settled
    Check 'One Ctrl+Z removes all the copies' ([int]$state.entities -eq $count) "entities $($state.entities)"

    # ---- No shortcut fires while a text field has the keyboard ----
    Click-Widget 'Row.Cube'; Start-Sleep -Milliseconds 200
    $count = [int](Get-Settled).entities
    Click-Widget 'Toolbar.ScenePath'; Start-Sleep -Milliseconds 200
    Press-Key 'Delete'
    Send-Ctrl 'D'
    $state = Get-Settled
    Check 'Delete and Ctrl+D do nothing while typing' ([int]$state.entities -eq $count) "entities $count to $($state.entities)"
    Press-Key 'Enter'; Start-Sleep -Milliseconds 150
    Click-Widget 'Hierarchy.Empty'; Start-Sleep -Milliseconds 150

    Click-Widget 'Row.Cube'; Start-Sleep -Milliseconds 200

    # ---- A gizmo drag is live, and one undo step ----
    $before = Get-Settled
    $history = [int]($before.history -split '/')[0]
    $start = $before.primary
    $p = Get-WidgetPoint 'Gizmo.Center'
    Move-To $p[0] $p[1]; [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 200
    Mouse-Button $true; Start-Sleep -Milliseconds 80
    Move-Relative 90 -30 500
    Start-Sleep -Milliseconds 200
    $during = Get-State
    Save-Capture (Join-Path $Output 'p1_04_gizmo_dragging.jpg')
    Move-Relative 60 -10 300
    Mouse-Button $false; Start-Sleep -Milliseconds 300
    $after = Get-Settled
    Check 'The object moves during the drag' ($during.primary -ne $start -and $after.primary -ne $during.primary) "start $start, during $($during.primary), end $($after.primary)"
    Check 'The drag is one undo step, added at the end' ([int]($during.history -split '/')[0] -eq $history -and [int]($after.history -split '/')[0] -eq $history + 1) "history $history, during $($during.history), after $($after.history)"
    Send-Ctrl 'Z'
    $state = Get-Settled
    Check 'One Ctrl+Z puts it back' ($state.primary -eq $start) "primary $($state.primary), started $start"
    Send-Ctrl 'Y'
    $state = Get-Settled
    Check 'Ctrl+Y moves it again' ($state.primary -eq $after.primary) "primary $($state.primary)"

    # ---- F belongs to the viewport. The cube was just moved, so focusing on it visibly moves the view ----
    Click-Widget 'Row.Cube'; Start-Sleep -Milliseconds 200 # Takes the focus from the viewport, which the drag gave it.
    $camera = (Get-Settled).camera
    Move-ToWidget 'Hierarchy.Empty'
    Press-Key 'F'
    $state = Get-Settled
    Check 'F does nothing with the mouse and focus on the Hierarchy' ($state.camera -eq $camera) "camera $($state.camera)"
    Move-ToWidget 'Panel.3D Viewport' 0.5 0.9
    Press-Key 'F'
    $state = Get-Settled
    Check 'F focuses with the mouse over the viewport' ($state.camera -ne $camera) "camera $camera to $($state.camera)"

    # ---- Undo History ----
    Click-Widget 'Menu.Window'; Start-Sleep -Milliseconds 200
    Click-Widget 'Item.Window.UndoHistory'; Start-Sleep -Milliseconds 500
    Save-Capture (Join-Path $Output 'p1_05_history.jpg')
    $before = Get-Settled
    Click-Widget 'History.1'; Start-Sleep -Milliseconds 400
    $state = Get-Settled
    Check 'Clicking a history entry jumps there' ([int]($state.history -split '/')[0] -eq 1 -and ($state.history -split '/')[1] -eq ($before.history -split '/')[1]) "history $($before.history) to $($state.history)"
    Click-Widget 'History.0'; Start-Sleep -Milliseconds 300
    Click-Widget "History.$(($before.history -split '/')[1])"; Start-Sleep -Milliseconds 400
    $state = Get-Settled
    Check 'And back to the end' ($state.history -eq $before.history -and $state.entities -eq $before.entities) "history $($state.history), entities $($state.entities)"
    Click-Widget 'Menu.Window'; Start-Sleep -Milliseconds 200
    Click-Widget 'Item.Window.UndoHistory'; Start-Sleep -Milliseconds 300

    # ---- Dirty mark ----
    $state = Get-Settled
    Check 'Edits mark the scene dirty' ($state.dirty -eq '1') "dirty $($state.dirty)"
    Save-Capture (Join-Path $Output 'p1_06_dirty_tab.jpg')

    # ---- Rebinding: add J to Create Cube, see it work, and see it survive a restart ----
    Click-Widget 'Row.Cube'; Start-Sleep -Milliseconds 200 # Jumping to the start of the history emptied the selection.
    Click-Widget 'Menu.Window'; Start-Sleep -Milliseconds 200
    Click-Widget 'Item.Window.KeyboardShortcuts'; Start-Sleep -Milliseconds 500
    Save-Capture (Join-Path $Output 'p1_07_shortcuts.jpg')
    Click-Widget 'Shortcuts.Search'; Start-Sleep -Milliseconds 150
    Type-Text 'Cube'; Start-Sleep -Milliseconds 400
    Save-Capture (Join-Path $Output 'p1_08_shortcuts_search.jpg')
    Press-Key 'Enter'; Start-Sleep -Milliseconds 100
    # Esc while waiting cancels, and does nothing else.
    Click-Widget 'Keys.Create.Cube.add'; Start-Sleep -Milliseconds 300
    $selected = [int](Get-Settled).selected
    Press-Key 'Esc'
    $state = Get-Settled
    Check 'Esc cancels waiting for keys without deselecting' ($selected -ge 1 -and [int]$state.selected -eq $selected -and -not (Test-Path $keymap)) "selected $selected to $($state.selected)"
    Click-Widget 'Keys.Create.Cube.add'; Start-Sleep -Milliseconds 300
    Save-Capture (Join-Path $Output 'p1_09_waiting_for_keys.jpg')
    $count = [int](Get-Settled).entities
    Press-Key 'J'; Start-Sleep -Milliseconds 300
    $state = Get-Settled
    Check 'The key press that binds does not also run the command' ([int]$state.entities -eq $count) "entities $count to $($state.entities)"
    Check 'A new binding is saved to the keymap file' ((Test-Path $keymap) -and ((Get-Content $keymap -Raw) -match 'Create\.Cube = J')) (Get-Content $keymap -Raw -ErrorAction SilentlyContinue)
    Click-Widget 'Hierarchy.Empty'; Start-Sleep -Milliseconds 200
    $count = [int](Get-Settled).entities
    Press-Key 'J'
    $state = Get-Settled
    Check 'The new key runs the command' ([int]$state.entities -eq $count + 1) "entities $count to $($state.entities)"
    Send-Ctrl 'Z'
    # Save nothing; the dirty scene is discarded through the prompt below.
    Stop-Editor -TimeoutMilliseconds 1500 | Out-Null
    Start-Sleep -Milliseconds 500
    Save-Capture (Join-Path $Output 'p1_10_unsaved_prompt.jpg')
    Kill-Editor
    Start-Run
    $count = [int](Get-State).entities
    Move-ToWidget 'Hierarchy.Empty'
    Press-Key 'J'
    $state = Get-Settled
    Check 'The binding survives a restart' ([int]$state.entities -eq $count + 1) "entities $count to $($state.entities)"

    # ---- A conflict asks, and Replace moves the key ----
    Click-Widget 'Menu.Window'; Start-Sleep -Milliseconds 200
    Click-Widget 'Item.Window.KeyboardShortcuts'; Start-Sleep -Milliseconds 500
    Click-Widget 'Shortcuts.Search'; Start-Sleep -Milliseconds 150
    Type-Text 'Select All'; Start-Sleep -Milliseconds 400
    Press-Key 'Enter'; Start-Sleep -Milliseconds 100
    Click-Widget 'Keys.Edit.SelectAll.add'; Start-Sleep -Milliseconds 300
    Press-Key 'J'; Start-Sleep -Milliseconds 400
    Save-Capture (Join-Path $Output 'p1_11_conflict.jpg')
    Check 'A taken key shows what uses it, and is not bound yet' (-not ((Get-Content $keymap -Raw) -match 'Edit\.SelectAll')) 'waiting for Replace'
    Click-Widget 'Shortcuts.Replace'; Start-Sleep -Milliseconds 400
    $text = Get-Content $keymap -Raw
    Check 'Replace moves the key' ($text -match 'Edit\.SelectAll = Ctrl\+A, J' -and $text -notmatch 'Create\.Cube') ($text -replace "`r?`n", ' | ')
    Click-Widget 'Hierarchy.Empty'; Start-Sleep -Milliseconds 200
    $count = [int](Get-Settled).entities
    Press-Key 'J'
    $state = Get-Settled
    Check 'The key now does the new command only' ([int]$state.entities -eq $count -and [int]$state.selected -eq $count) "entities $($state.entities), selected $($state.selected)"
    # ---- Reset ----
    Click-Widget 'Shortcuts.ResetAll'; Start-Sleep -Milliseconds 300
    Press-Key 'Esc'; Start-Sleep -Milliseconds 200
    Click-Widget 'Hierarchy.Empty'; Start-Sleep -Milliseconds 200
    Press-Key 'J'
    $state = Get-Settled
    Check 'Reset All removes it' ([int]$state.entities -eq $count -and [int]$state.selected -eq 0 -and -not ((Get-Content $keymap -Raw) -match '=')) "entities $($state.entities), selected $($state.selected)"
    # ---- Right-click removes a key; changing one replaces it; Reset puts one command back ----
    Click-Widget 'Keys.Edit.SelectAll.add'; Start-Sleep -Milliseconds 300
    Press-Key 'J'; Start-Sleep -Milliseconds 300
    Click-Widget 'Keys.Edit.SelectAll.1' -Right; Start-Sleep -Milliseconds 300
    $text = Get-Content $keymap -Raw
    Check 'Right-click removes a key' ($text -notmatch 'Edit\.SelectAll') ($text -replace "`r?`n", ' | ')
    Click-Widget 'Keys.Edit.SelectAll.0'; Start-Sleep -Milliseconds 300
    Press-Key 'K'; Start-Sleep -Milliseconds 300
    $text = Get-Content $keymap -Raw
    Check 'Clicking a key and pressing another replaces it' ($text -match 'Edit\.SelectAll = K') ($text -replace "`r?`n", ' | ')
    Click-Widget 'Keys.Edit.SelectAll.reset'; Start-Sleep -Milliseconds 300
    $text = Get-Content $keymap -Raw
    Check 'Reset puts one command back' ($text -notmatch 'Edit\.SelectAll') ($text -replace "`r?`n", ' | ')
    Click-Widget 'Menu.Window'; Start-Sleep -Milliseconds 200
    Click-Widget 'Item.Window.KeyboardShortcuts'; Start-Sleep -Milliseconds 300

    # ---- The layout file lands in the settings folder ----
    $exit = Stop-Editor -TimeoutMilliseconds 1500
    if ($null -eq $exit) { Kill-Editor }
    Check 'Settings live in the settings folder' ((Test-Path (Join-Path $config 'layout.ini')) -or (Test-Path $keymap)) ((Get-ChildItem $config -Name) -join ', ')
    $results | Set-Content (Join-Path $Output 'phase1_results.txt') -Encoding utf8
} catch {
    Write-Host "RUN STOPPED: $_"
    $results | Set-Content (Join-Path $Output 'phase1_results_partial.txt') -Encoding utf8
    exit 2
} finally {
    Kill-Editor
}
