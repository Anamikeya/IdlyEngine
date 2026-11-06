# Milestone 6, slice 4: the Script Editor and hot reload

Release build of the `milestone-6` working tree after slice 3 (`6e6f596`) with slice 4's changes, 2026-11-06, in
`build-ux-windows-msvc-release`. Both presets pass 603 of 603 CTest entries with the same files.

## Real input

`Tests/RealInput/milestone6_scripts.ps1` drove the editor with the real mouse and keyboard, on a copy of the Physics
Shooter sample. Its check lines are in `real_input_results.txt`: 15 of 15. Each check reads the editor's `--game-log`
state or a file on disk.

It passed 15 of 15 on its first run too. That run's captures showed the Script Editor's **...** button clipped at the
panel's right edge while "(unsaved)" was on the row; the tools now wrap, and this second run, with that change, is the
one kept here.

| Image | What it shows |
|---|---|
| `m6_s4_01_template.jpg` | New script's template, opened by a double-click in the Script Editor, a tab beside the Asset Browser |
| `m6_s4_02_typed.jpg` | The script typed in, unsaved: Lua colors, line numbers, its property and functions listed above it |
| `m6_s4_03_details.jpg` | Static Target 1 with the Script component: the script's field, its speed of 3 with the arrow back to the default, Edit Script |
| `m6_s4_04_playing.jpg` | Play: the target has risen out of the arena at 3 m/s |
| `m6_s4_05_reloaded.jpg` | Saved while playing: the target now moves sideways, and the Message Log says the script was reloaded |
| `m6_s4_06_marked.jpg` | Saved broken: Lua's message in red, line 2 marked, while the target keeps moving on the last good code |

What the checks found, in numbers: the target rose 6.05 m in 2 s at a speed of 3; after the save during Play it moved
3.00 m sideways in the next second, at the height it had reached; after the broken save it went on moving 3.00 m a
second; the error's link put the cursor on line 2, the line Lua named; and after Stop it was back at x -8, y 3, where it
started. The saved scene holds `SCRIPT ... "Scripts/Lift.lua" 1 speed n 3`.
