# Milestone 5, slice 5: the shooter with enemies and effects

Release build from an empty folder (`build-m5-windows-msvc-release`), commit `03fde0b`, 2026-11-05.

## Real input

`Tests/RealInput/shooter_acceptance.ps1` played the Physics Shooter with the real mouse and keyboard, on a copy of the sample.
Its check lines are in `real_input_results.txt`: 26 of 26, on the first attempt. Each check reads the editor's `--game-log`
state or a file on disk. Steps 17 to 22 are new in this milestone: glass shards and slivers, muzzle flashes, enemies walking
with footstep dust, a shot enemy flinching and then falling as a ragdoll, every enemy fallen when the game is won, and nothing
left behind after Stop.

| Image | What it shows |
|---|---|
| `m5_s5_02_arena_playing.jpg` | The game started: the arena through the player's camera |
| `m5_s5_19_enemies_walking.jpg` | Enemies walking their patrols, at 0 of 12 targets |
| `m5_s5_17_glass_breaking.jpg` | The glass barrier shot: its shards fall and slivers fly, the enemy behind it unharmed |
| `m5_s5_20_enemy_fallen.jpg` | "+200 Enemy 2 destroyed": the enemy lies on the floor as a ragdoll |
| `m5_s5_12_game_won.jpg` | YOU WIN: 12 of 12 targets, score 2,000, 6 seconds left |
| `m5_s5_13_game_lost.jpg` | After Restart, the clock ran out with 12 targets left |
| `m5_s5_15_stopped.jpg` | After Stop: the editor scene as it was, its saved file identical |

`performance.txt` is the editor's own log of the run, with the script at the controls: 59.9 fps on average on a 59 Hz
display. It caught one frame of 316 ms, 29.8 s after the editor started, of which 131 ms was the UI and frame snapshot; see
the report's performance section. `memory.txt` is the editor's peak working set: 386 MB.

## The day before's build

The same script passed 26 of 26 at `621c2a7` too, on that commit's fresh Release build. That run had stopped three times
first, each time because the PC was in use: the cursor moved, then the editor lost focus twice. The guards stopped each run,
so no input reached another application. Attempts 2 and 3 had already failed some steps before they stopped (the player did
not move far enough, or shots missed). While the game holds the mouse, the cursor does not move, so the guard cannot see
someone else's mouse until the editor loses focus. Attempt 4 passed every step with the same build and script, so those
failures are put down to the desktop being in use.
