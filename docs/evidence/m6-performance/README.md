# Milestone 6, slice 6: what 500 scripts cost

The fresh Release build at `7a9aace` (`build-m6-windows-msvc-release`), 2026-11-06. The machine is in `machine.txt`: a
Ryzen 7 5800X, 128 GB, an RTX 3080 driving a 3840x2160 display at 59 Hz, Windows 10 Pro.

## In the editor

`Tests/Performance/milestone6_performance.ps1` started the Release editor with `--perf-run 60` on a copy of each scene,
twice in turn: it plays the scene for 60 s with nobody at the controls, then writes its log (`perf_<scene>_<round>.txt`).
It sends no input, waits for an idle desktop first and records whether the desktop stayed idle. Two of the six runs were
not idle throughout: someone used the PC during them, and they are marked.

| Scene | Scripts | Script time a tick, mean and worst | Script time a drawn frame, mean and p99 | CPU a drawn frame | Idle |
|---|---|---|---|---|---|
| `physics_shooter`, the sample | 2 | 0.054 and 0.31 ms; 0.064 and 0.51 ms | 0.06 and 0.17 ms; 0.06 and 0.22 ms | 6.99; 7.07 ms | yes; no |
| `script_stress` | 502 | **1.84 and 4.30 ms; 1.78 and 3.78 ms** | **1.84 and 2.91 ms; 1.79 and 4.15 ms** | 12.69; 12.77 ms | no; yes |
| `script_static` | 2 | 0.063 and 0.53 ms; 0.059 and 0.70 ms | 0.06 and 0.24 ms; 0.06 and 0.18 ms | 11.21; 11.20 ms | yes; yes |

- **The scenes:** `script_stress` is the sample with 500 cubes over the arena, 25 to a row, each moved up and down 2 m by
  `Mover.lua` at its own speed: 502 scripts with the vault's two. `script_static` is the same scene with the movers'
  Script components taken out, so the same cubes stand still.
- **Script time** is both of a tick's calls into the scripts, applying what they changed and carrying out what they asked
  (`script_tick_ms_*`), and per drawn frame the sum over the ticks since the frame before (`script_frame_ms_*`). Gameplay
  is timed apart from it.
- **What the movers cost the whole frame:** 12.7 against 11.2 ms of CPU for the cubes standing still, 1.5 ms.
- **CPU a drawn frame** is every loop since the frame before: the session's update, the UI and frame snapshot, and the
  render call. Vsync on the 59 Hz display caps every scene at 59.8 fps.
- **One slow frame in every run,** 27.2 to 27.5 s into Play: 72 to 130 ms, nearly all in the UI and frame snapshot. It is
  in `script_static` too, and in the sample, so scripts do not cause it; it is Milestone 5's slow frame, still not
  explained. The ticks it held back run together after it, which is the 31 ms maximum of script time a frame.
- **The editor's own frame grows with the scene:** the UI and frame snapshot take 2.05 to 2.08 ms a loop with the 500
  cubes against 0.72 for the sample.

## Before and after

| | Scripts a tick, `script_stress` | Gameplay and physics a tick, the sample | Log |
|---|---|---|---|
| Before slice 6's fixes (working tree on `2a218dc` with the new timings, 20 s runs) | 7.58 ms, at worst 138 ms | 0.42 and 0.40 ms | `summary_before_fixes.txt` |
| `f06fbd7`: changes committed in place, physics mirroring only on structure, no finalizers | 2.16 and 2.15 ms (desktop in use) | 0.045 and 0.08 ms | `summary_f06fbd7.txt` |
| `7a9aace`: Vec3's operators as C functions, the mover's path once | 1.84 and 1.78 ms | 0.04 and 0.08 ms | `performance_summary.txt` |

## Without a window

`script_benchmark.txt`: `script_benchmark` on the same build, 500 cubes, 600 ticks, no game, times per tick.

| Script | Before physics | What it shows |
|---|---|---|
| `empty` | 0.15 ms | Calling 500 instances |
| `math` | 0.63 ms | The Mover's arithmetic, kept in `self` |
| `read` | 0.36 ms | Reading `self.entity.position` |
| `write` | 0.93 ms | Moving the entity, and the bus taking the moves |
| `mover` | 1.36 ms (and 0.04 after physics) | The sample's `Mover.lua` |

One tick's 500 moves through the command bus with no script take 0.46 ms (4.7 ms before the in-place path), and a scene
snapshot of 500 entities 0.13 ms.

## Fresh builds

Both presets were configured and built in empty folders at `7a9aace` (`build-m6-windows-msvc-release` and `-ci`, 1,095
steps each) and passed 616 of 616 CTest entries. Then, on each:

| Check | Release | CI | Log |
|---|---|---|---|
| `--smoke` | Passed, 102 frames | Passed, 103 frames | `gpu_release_smoke.txt`, `gpu_ci_smoke.txt` |
| `--render-test` | 75 of 75 | 75 of 75 | `gpu_release_render.txt`, `gpu_ci_render.txt` |

## Real input again

The real-input runs that play the shooter and drive the Script Editor, run again on the fresh Release build, because slice
6 changed the command bus and physics under them:

| Run | Result | Log |
|---|---|---|
| `Tests/RealInput/milestone6_shooter.ps1` | 14 of 14 | `real_input_milestone6_shooter.txt` |
| `Tests/RealInput/milestone6_scripts.ps1` | 15 of 15 on the second attempt | `real_input_milestone6_scripts.txt` |
| `Tests/RealInput/shooter_acceptance.ps1` (Milestone 5) | 26 of 26 | `real_input_shooter_acceptance.txt` |

The Script Editor run's first attempt (`real_input_milestone6_scripts_first_attempt.txt`) failed one check of 15: it
expected one script in Play, and since slice 5 the sample's vault runs two more. The check expects three now.