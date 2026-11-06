# Milestone 6, slice 5: the shooter's scripted vault, won and lost

Release build of the `milestone-6` working tree after slice 4 (`79f81ec`) with slice 5's changes, 2026-11-06, in
`build-ux-windows-msvc-release`. Both presets pass 612 of 612 CTest entries with the same files.

## Real input

`Tests/RealInput/milestone6_shooter.ps1` played a copy of the Physics Shooter sample with the real mouse and keyboard.
Its check lines are in `real_input_results.txt`: 14 of 14, on the first run. Each check reads the editor's
`--game-log` state, which now holds where the scripted entities are, the HUD's message and the script errors since Play
began, or the scene file on disk.

| Image | What it shows |
|---|---|
| `m6_s5_01_gate_shut.jpg` | The vault's gate, shut, in the west wall by the start |
| `m6_s5_02_gate_opening.jpg` | The pillar's target and Enemy 1 down (targets 2 of 12): "The vault is open", the gate sliding into the wall |
| `m6_s5_03_gate_open.jpg` | Open, 3 m along, where it stays |
| `m6_s5_04_in_the_vault.jpg` | Inside, within reach of the green button: "Press E for the vault's prize" |
| `m6_s5_05_prize.jpg` | E pressed: the score from 300 to 600, the clock up 20 seconds, "+300 Vault, and 20 seconds" |
| `m6_s5_06_won.jpg` | Every target destroyed: won, final score 2300 with 31 seconds left |
| `m6_s5_07_lost.jpg` | Restarted with the gate shut again, then the clock run out: lost |

What the checks found, in numbers: the shut gate stopped the player at x -16.000, which is its face at -16.35 plus the
capsule's radius; once both targets of its room had fallen it slid from z 12 to 15.000 and stayed; inside, at x -21.40,
the button's hint showed; E gave 300 points and moved the clock from 73.55 to 93.03 seconds (20, less the time between
the two readings), and the button sank from x -22.900 to -22.980; a second E gave nothing. The game was won with 2300
points; after Restart there were 12 targets, a score of 0 and the gate at z 12 again; then it was lost when the clock
ran out. No script failed in the whole run, and after Stop, saving the scene wrote the same bytes as before Play.

## Without a window

`Tests/Gameplay/scripted_behavior_tests.cpp` plays the sample's `Mover`, `Pickup` and `BonusZone` scripts against the
C++ behaviors side by side, alone and in the whole sample over 1,200 ticks of shooting and walking, and compares every
entity's transform and the HUD exactly after every tick: they agree to the bit. As a check that the comparison can fail,
a mover speed changed by a part in ten million failed it on the first tick, and a bonus message with one space fewer
failed it on the tick the crate landed; both were put back.
