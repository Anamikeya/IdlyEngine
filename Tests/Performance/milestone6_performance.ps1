<#
.SYNOPSIS
  Measures what 500 scripted entities cost in the Release editor (Milestone 6, slice 6).

.DESCRIPTION
  Each run starts the editor with --perf-run: it plays the scene for the given time with nobody at the controls, then
  quits and writes its --perf-log. No input is sent. A run waits for an idle desktop first, because someone using the
  machine changes the numbers, and records whether the desktop stayed idle throughout. Runs use a temporary copy of the
  sample, so the repository's files are never touched.

  Three scenes, each played -Rounds times, in turn:
    physics_shooter   the sample, whose vault has 2 scripts
    script_stress     the sample with 500 cubes above the arena, each moved up and down by Mover.lua: 502 scripts
    script_static     script_stress with the movers' Script components taken out, so the same 500 cubes stand still

  The perf log times scripts apart from the C++ game: script_tick_ms_* per fixed tick, script_frame_ms_* per drawn
  frame. What the movers cost the whole frame, drawing their moves included, is script_stress's cpu_frame_ms against
  script_static's.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File Tests/Performance/milestone6_performance.ps1 -Output perf-run
#>
param([string]$Editor = 'build-ux-windows-msvc-release\bin\engine_editor.exe', [string]$Output = 'perf-run', [int]$Seconds = 60,
      [int]$Rounds = 2, [string]$Work = "$env:TEMP\engine-m6-performance")
$ErrorActionPreference = 'Stop'
$WarningPreference = 'SilentlyContinue'
Import-Module (Join-Path $PSScriptRoot '..\RealInput\InputDriver.psm1') -Force

$Editor = (Resolve-Path $Editor).Path
$repo = Split-Path (Split-Path $PSScriptRoot)
New-Item -ItemType Directory -Force $Output | Out-Null
$Output = (Resolve-Path $Output).Path
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force $Work | Out-Null
$project = Join-Path $Work 'project'
Copy-Item -Recurse (Join-Path $repo 'Samples\PhysicsShooter') $project

# script_static: the stress scene without the movers' scripts. The component count in the header goes down with them.
$lines = [IO.File]::ReadAllLines((Join-Path $project 'script_stress.scene'))
$kept = @($lines | Where-Object { $_ -notmatch '^SCRIPT \d+ \S+ "Scripts/Mover\.lua"' })
$removed = $lines.Count - $kept.Count
if ($removed -ne 500) { throw "script_stress.scene has $removed Mover scripts, not 500" }
for ($i = 0; $i -lt $kept.Count; ++$i) {
    if ($kept[$i] -match '^COMPONENTS (\d+)$') { $kept[$i] = "COMPONENTS $([int]$Matches[1] - $removed)" }
}
[IO.File]::WriteAllText((Join-Path $project 'script_static.scene'), (($kept -join "`n") + "`n"))

# The machine, as Windows reports it.
$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
$gpus = Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) at $($_.CurrentRefreshRate) Hz, $($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)" }
$memory = [Math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
$machine = @("cpu $cpu", "memory_gb $memory") + ($gpus | ForEach-Object { "gpu $_" }) + @("os $((Get-CimInstance Win32_OperatingSystem).Caption)")
$machine | Set-Content -Encoding ascii (Join-Path $Output 'machine.txt')

$summary = New-Object System.Collections.ArrayList
for ($round = 1; $round -le $Rounds; ++$round) {
    foreach ($scene in @('physics_shooter', 'script_stress', 'script_static')) {
        Wait-ForIdleDesktop
        $name = "${scene}_$round"
        $log = Join-Path $Output "perf_$name.txt"
        Remove-Item $log -ErrorAction SilentlyContinue
        $arguments = @('--project', "`"$project`"", '--scene', "`"$(Join-Path $project "$scene.scene")`"", '--perf-run', $Seconds, '--perf-log', "`"$log`"")
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $process = Start-Process -FilePath $Editor -ArgumentList $arguments -WorkingDirectory $Work -PassThru -Wait `
            -RedirectStandardOutput (Join-Path $Output "editor_$name.out.txt") -RedirectStandardError (Join-Path $Output "editor_$name.err.txt")
        # Idle since before the editor started: nobody touched the machine during the run. A lock screen changes what is
        # presented, so a run that ends locked is marked too.
        $idle = [Win32Input]::IdleMilliseconds() -ge $clock.ElapsedMilliseconds
        $locked = Test-DesktopLocked
        if ($process.ExitCode -ne 0 -or -not (Test-Path $log)) { throw "The $name run failed (exit $($process.ExitCode)); see editor_$name.err.txt" }
        Add-Content -Encoding ascii $log "desktop_idle_throughout $(if ($idle) { 1 } else { 0 })"
        Add-Content -Encoding ascii $log "desktop_locked_at_end $(if ($locked) { 1 } else { 0 })"
        if ($locked) { $idle = $false }
        $values = @{}
        foreach ($line in Get-Content $log) { $k, $v = $line -split ' ', 2; $values[$k] = $v }
        $line = ('{0,-17} fps {1,5:N1} | frame p99 {2,5:N2} ms | cpu/frame {3:N2} mean {4:N2} p99 | scripts {5,3}: {6:N3} ms/tick mean {7:N3} max, ' +
                 '{8:N3} ms/frame mean {9:N3} p99 {10:N3} max | gameplay {11:N3} physics {12:N3} ms/tick | phase {13} | idle {14}') -f $name,
            [double]$values.fps_mean, [double]$values.frame_ms_p99, [double]$values.cpu_frame_ms_mean, [double]$values.cpu_frame_ms_p99,
            $values.script_instances, [double]$values.script_tick_ms_mean, [double]$values.script_tick_ms_max, [double]$values.script_frame_ms_mean,
            [double]$values.script_frame_ms_p99, [double]$values.script_frame_ms_max, [double]$values.gameplay_tick_ms_mean,
            [double]$values.physics_step_ms_mean, $values.phase, $(if ($idle) { 'yes' } else { 'NO' })
        [void]$summary.Add($line); Write-Host $line
    }
}
$summary | Set-Content -Encoding ascii (Join-Path $Output 'performance_summary.txt')
Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
