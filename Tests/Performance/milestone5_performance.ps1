<#
.SYNOPSIS
  Measures the Release editor playing the Physics Shooter sample and its stress scene (Milestone 5, slice 6).

.DESCRIPTION
  Each run starts the editor with --perf-run: it plays the scene for the given time with nobody at the controls, then
  quits and writes its --perf-log. No input is sent. A run waits for an idle desktop first, because someone using the
  machine changes the numbers, and records whether the desktop stayed idle throughout. Runs use a temporary copy of the
  sample, so the repository's files are never touched.

  The stress scene is the sample's arena with ten enemies walking in view of the player and ten fountains that keep 5,000
  particles alive.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File Tests/Performance/milestone5_performance.ps1 -Output perf-run
#>
param([string]$Editor = 'build-ux-windows-msvc-release\bin\engine_editor.exe', [string]$Output = 'perf-run', [int]$Seconds = 60,
      [string]$Work = "$env:TEMP\engine-m5-performance")
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

# The machine, as Windows reports it.
$cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
$gpus = Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) at $($_.CurrentRefreshRate) Hz, $($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)" }
$memory = [Math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
$machine = @("cpu $cpu", "memory_gb $memory") + ($gpus | ForEach-Object { "gpu $_" }) + @("os $((Get-CimInstance Win32_OperatingSystem).Caption)")
$machine | Set-Content -Encoding ascii (Join-Path $Output 'machine.txt')

$summary = New-Object System.Collections.ArrayList
foreach ($scene in @('physics_shooter', 'stress_test')) {
    Wait-ForIdleDesktop
    $log = Join-Path $Output "perf_$scene.txt"
    Remove-Item $log -ErrorAction SilentlyContinue
    $arguments = @('--project', "`"$project`"", '--scene', "`"$(Join-Path $project "$scene.scene")`"", '--perf-run', $Seconds, '--perf-log', "`"$log`"")
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $Editor -ArgumentList $arguments -WorkingDirectory $Work -PassThru -Wait `
        -RedirectStandardOutput (Join-Path $Output "editor_$scene.out.txt") -RedirectStandardError (Join-Path $Output "editor_$scene.err.txt")
    # Idle since before the editor started: nobody touched the machine during the run.
    $idle = [Win32Input]::IdleMilliseconds() -ge $clock.ElapsedMilliseconds
    if ($process.ExitCode -ne 0 -or -not (Test-Path $log)) { throw "The $scene run failed (exit $($process.ExitCode)); see editor_$scene.err.txt" }
    Add-Content -Encoding ascii $log "desktop_idle_throughout $(if ($idle) { 1 } else { 0 })"
    $values = @{}
    foreach ($line in Get-Content $log) { $k, $v = $line -split ' ', 2; $values[$k] = $v }
    $line = '{0,-16} fps {1,6:N1} mean, {2,6:N1} at p99 | frame {3:N2} ms mean, {4:N2} ms p99 | particles {5} peak | skinned {6} | animators {7} | idle {8}' -f $scene,
        [double]$values.fps_mean, [double]$values.fps_p99, [double]$values.frame_ms_mean, [double]$values.frame_ms_p99, $values.particles_drawn_peak,
        $values.skinned_objects_peak, $values.animators_peak, $(if ($idle) { 'yes' } else { 'NO' })
    [void]$summary.Add($line); Write-Host $line
}
$summary | Set-Content -Encoding ascii (Join-Path $Output 'performance_summary.txt')
Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
