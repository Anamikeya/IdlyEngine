# Milestone 5, slice 6: performance and fresh builds

Release and CI builds from empty folders (`build-m5-windows-msvc-release`, `build-m5-windows-msvc-ci`), commit `cabee69`,
2026-11-05. The machine is in `machine.txt`: a Ryzen 7 5800X, 128 GB, an RTX 3080 driving a 3840x2160 display at 59 Hz,
Windows 10 Pro.

## Frame rate

`Tests/Performance/milestone5_performance.ps1` started the Release editor with `--perf-run 60` on a copy of each scene: it
plays the scene for 60 s with nobody at the controls, then writes its log. The script waits for an idle desktop and records
whether it stayed idle. Both runs here did (`desktop_idle_throughout 1`).

| Scene | Mean | Slowest 1% | Frame mean, p99, max | CPU per frame | Particles | Skinned | Log |
|---|---|---|---|---|---|---|---|
| `physics_shooter` | 60.0 fps | 29.5 fps | 16.67, 33.95, 35.9 ms | 1.18 ms | 114 peak | 3 | `perf_physics_shooter.txt` |
| `stress_test` | 60.0 fps | 44.3 fps | 16.66, 22.58, 36.8 ms | 3.20 ms | 4,957 mean, 5,154 peak | 10 | `perf_stress_test.txt` |

- **What the stress scene is:** the sample's arena with ten enemies walking in view of the player and ten fountains that
  keep about 5,000 particles alive.
- **The display caps both at 60 fps.** The editor presents with vsync on a 59 Hz display, so frames are about 16.7 ms
  whatever the load. The numbers show that the target is met, not how far beyond it the engine could go.
- **CPU per frame** is the session's update, plus the UI and frame snapshot, plus the render call
  (`update_ms_mean + ui_and_snapshot_ms_mean + render_submit_ms_mean`). In the stress scene, a fixed tick takes 0.41 ms of
  physics, 0.51 ms of gameplay and 0.05 ms of animation; the particles' update takes 0.03 ms a frame.
- **`gpu_frame_ms_mean`** (14.2 ms in the stress scene) is Filament's own measure. It includes waiting for the display, so
  it is not the GPU's load.
- Nothing is selected in these runs, so the selection outline's two views are never drawn.

## How much the slowest 1% varies

A p99 of 33 ms is a frame that missed one refresh of the display. It moved from run to run on the same build, so one run's
p99 is not a property of the code. These are the day's runs of the sample that sent no input and kept their logs:

| Build | Runs | Frame p99 | CPU per frame |
|---|---|---|---|
| `621c2a7`, before 11:53 | 3 (60, 60 and 20 s) | 19.1, 19.4, 19.1 ms | 1.1 ms |
| `621c2a7`, from 11:53 | 2 (20 s; one with a footstep emitter playing while editing) | 34.2, 34.4 ms | 1.2 ms |
| `c4268c3`, `03fde0b` and `cabee69` | 6 (60 s; two with the PC in use) | 33.1 to 34.1 ms | 1.1 to 1.2 ms |

The change came between two runs of the same old build, and the CPU's share of a frame did not change with it, so it is
something on the machine, not the engine; what it was is not known. The stress scene's p99 stayed at 20.9 to 22.6 ms
throughout. An earlier run of the stress scene, while the PC was in use, ended with 162 physics bodies against 75 in an
untouched run, so the PC's use had reached the game. Its numbers are not used.

## One slow frame

In four of the sample's ten 60 s runs that day, and in all three of the shooter's complete real-input runs, one frame took
220 to 316 ms, about 25 to 31 s after the editor started where the time was recorded. It never happened in the stress
scene's five runs, nor in runs that ended before then. It is not physics, animation, particles or gameplay: each is timed,
and none ever took more than 3.2 ms. The perf log now records when frames over 100 ms end, and where the slowest frame's
time went. Two were caught that way, both in the shooter's real-input runs (`../m5-shooter`):

| Commit | Slowest frame | At | UI and frame snapshot | Update | Render call | Not in those three |
|---|---|---|---|---|---|---|
| `03fde0b` | 316 ms | 29.8 s | 131 ms | 18 ms | 20 ms | 147 ms |
| `cabee69` | 278 ms | 29.9 s | 108 ms | 18 ms | 17 ms | 135 ms |

The cause is not found. It is a frame a player would notice, once, early in a session.

## Fresh builds

Both presets were configured and built in empty folders (1,042 steps each) and passed 555 of 555 CTest entries. Then, on
each:

| Check | Release | CI | Log |
|---|---|---|---|
| `--smoke` | Passed, 100 frames | Passed, 93 frames | `gpu_release_smoke.txt`, `gpu_ci_smoke.txt` |
| `--render-test` | 75 of 75 | 75 of 75 | `gpu_release_render.txt`, `gpu_ci_render.txt` |
