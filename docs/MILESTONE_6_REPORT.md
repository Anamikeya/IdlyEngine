# Milestone 6 report: scripting

Branch `milestone-6`, worktree `D:\AI\GameEngine-Integration`. Plan: [`MILESTONE_6_PLAN.md`](MILESTONE_6_PLAN.md). Design:
[`architecture/010-scripting.md`](architecture/010-scripting.md). Reference for script authors: [`SCRIPTING.md`](SCRIPTING.md).
Every result below was produced at commit `7a9aace` (code, tests, sample), except the earlier slices' own real-input runs,
which were made on their slices' commits and are listed with them. The commit after it adds only documents and
evidence. **Merged into `main` on 2026-11-06 with your approval**, and pushed to `origin` with the `milestone-6` branch.

## Verdict

**Done: every acceptance criterion is met, the one on performance on average.** Game behavior can be written in Lua.
Scripts are made, edited, attached and given values in the editor. They run in Play and Simulate inside the fixed tick, and
change the running game when saved, keeping each instance's state. The shooter's `Mover`, `Pickup` and `BonusZone` scripts
play exactly as the C++ behaviors do, and its vault is scripted.
- **Builds and tests:** both presets build from empty folders and pass all 616 tests, 61 more than `main`.
- **Real input:** at the final commit, the shooter with its scripted vault was won and lost, the Script Editor run passed,
  and so did Milestone 5's shooter run.
- **Performance:** 502 scripts, 500 of them moving a cube every tick, cost 1.78 to 1.84 ms of CPU a frame on average.

Measuring found three costs that grew with the size of the scene rather than with what scripts did. All three are fixed:
- the command bus, which copied the whole scene for every change;
- physics, which copied it every tick;
- a finalizer sol2 gives every `Vec3`, which made Lua's collector pause.

The fix to the bus made the C++ game faster too.

Open:
- **The slowest 1% of frames** spend 2.9 to 4.1 ms in scripts, over the 2 ms.
- **Three parts of the planned API** were not built: enabling an entity, joint positions and the look direction.
- **The editor's own frame grows with the scene,** partly because the Outliner and the viewport's markers copy it every
  frame.
- **The slow frame of Milestone 5** is still unexplained, and now comes in every run.
- **The Colliders display** decision is still yours, from Milestone 5.

## What it added

| Slice | What | Evidence |
|---|---|---|
| 0 | Lua 5.4.9 (compiled as C++, with a fixed string hash seed), sol2 3.5.0 and goossens' ImGuiColorTextEdit in the build, pinned, as you approved | `scripting_tests` (7 Lua and sol2 cases), `editor_dependency_smoke` |
| 1 | `Engine/Scripting`: one Lua state a session, the sandbox, instruction and memory budgets, script classes and their properties, `require` through the virtual file system, errors with their script, line and traceback | `scripting_tests` |
| 2 | Script assets, the Script component, scene format 8, Details fields for every property type with defaults and reset, New Script, copy, paste and rename keeping links | `editor_script_tests`, `engine.core.acceptance` |
| 3 | The engine API (`ScriptHost`, `ScriptWorld`): entities, physics, input, animation, effects and the game, called at two fixed points of the tick; the Interact action (E) | `script_world_tests`, `editor_script_tests` |
| 4 | The Script Editor, hot reload in Play keeping each instance's state, Message Log and toast links to a script's line, Open in External Editor | [`evidence/m6-scripts`](evidence/m6-scripts) |
| 5 | The shooter's `Mover`, `Pickup` and `BonusZone` as scripts, played side by side with the C++ behaviors and equal to the bit; the scripted vault, its gate and button | [`evidence/m6-shooter`](evidence/m6-shooter) |
| 6 | What 500 scripts cost, measured; three costs that grew with the scene removed; fresh builds; this report | [`evidence/m6-performance`](evidence/m6-performance) |

The progress notes for each slice, with what testing found, are in section 8 of the plan.

## Acceptance criteria

| Criterion (plan section 7) | Result | Evidence |
|---|---|---|
| Both MSVC presets build from fresh folders | Met | [Build and test results](#build-and-test-results) |
| Every existing test passes, and none is removed or weakened | Met: 616 of 616 | [Existing tests changed](#existing-tests-changed) |
| A script is created in the editor, attached to an entity, and its properties edited in Details and undone. It runs in Play and Simulate, and the scene is the same after Stop | Met | Real input in slice 4 (made, typed, attached, a speed of 3 set in Details, played, the scene back after Stop); `editor_script_tests` (each property type set and undone, Simulate, Stop) |
| Saving a script during Play changes the running game within a second, keeping each instance's state. A syntax error keeps the old code running and shows the error at its line | Met | Real input in slice 4: the target moved by the new code in the next second from where it was; a broken save marked at line 2 while the last good code ran on |
| A runtime error stops only that instance and shows the file, line and traceback; clicking it opens the Script Editor at that line. An endless loop is stopped and the editor stays responsive | Met, partly by tests alone | `editor_script_tests` and `scripting_tests` (an error stops only its instance, with line, function, entity and traceback; a loop stopped in about 40 ms, even through `pcall`); real input in slice 4 followed an error's link to its line, for an error in a saved script. A runtime error's link and an endless loop in Play were not driven with real input |
| Scripts cannot read or write files outside their `require`s, start processes or use the network | Met | `scripting_tests`: no `io`, `os`, `debug`, `package`, `dofile`, `loadfile` or bytecode, and `require` finds only the project's scripts |
| The scripted `mover`, `pickup` and `bonus_zone` play the same as the C++ ones, tick for tick | Met, to the bit | `gameplay_tests` (`scripted_behavior_tests.cpp`), with checks that the comparison can fail |
| The shooter, with its scripted gate, is won and lost with real input | Met | [Real input](#real-input) |
| 500 scripted entities ticking at 60 Hz cost under 2 ms of CPU a frame in Release, or the report says what they cost | Met on average: 1.78 to 1.84 ms a frame. The slowest 1% of frames take 2.9 to 4.1 ms | [Performance](#performance) |
| The report lists what was verified, how, and what was not | Met | This document |

## Build and test results

Both MSVC presets were configured and built in empty folders at `7a9aace` on 2026-11-06, then tested. The machine is a
Ryzen 7 5800X with 128 GB and an RTX 3080, on Windows 10 Pro, with MSVC 19.44 and Ninja.

| Preset | Folder | Configure | Build | CTest | `--smoke` | `--render-test` |
|---|---|---|---|---|---|---|
| `windows-msvc-release` | `build-m6-windows-msvc-release` | 8 s | 97 s, 1,095 steps | 616 of 616 | Passed, 102 frames | 75 of 75 |
| `windows-msvc-ci` | `build-m6-windows-msvc-ci` | 8 s | 99 s, 1,095 steps | 616 of 616 | Passed, 103 frames | 75 of 75 |

- **Warnings:** the CI preset treats warnings in engine code as errors, and it built clean. Its log has 1,256 warnings,
  as slice 0's fresh build had: 875 are the compiler saying `/std:c++20` overrides `/std:c++latest`, 379 are in Filament,
  and 2 are Filament's code instantiated in MSVC's headers. None is in the engine, the tests or the new libraries.
- **CTest:** 616 entries, each test case its own process: 61 more than `main`'s 555, none removed.
- **Line endings:** every file this milestone changed is stored with LF, and the five that `main` stores with a byte
  order mark keep it.
- Slice 0's fresh builds (563 of 563) and the fresh builds at `f06fbd7` (615 of 615) passed alike.

## Real input

At `7a9aace`, on the fresh Release build, with the real mouse and keyboard through `Tests/RealInput/InputDriver.psm1` and
its focus and idle guards. Results: [`evidence/m6-performance`](evidence/m6-performance) (`real_input_*.txt`).

| Run | What it does | Result |
|---|---|---|
| `milestone6_shooter.ps1` (slice 5) | The scripted vault: the shut gate stops the player at the doorway, shooting the west wing's two targets opens it, E at the button inside pays 300 points and 20 seconds once; the game is won, restarted with the gate shut, and lost; no script fails; Stop gives back the scene byte for byte | 14 of 14, the first attempt |
| `milestone6_scripts.ps1` (slice 4) | A script made from the template, typed and saved in the Script Editor, given to a target in Details with a speed of its own, played, saved again while playing (moving the target sideways from where it was), saved broken (marked at line 2 while the last good code runs on), the error's link followed to its line, mended; Stop, and the scene saved with the script | 15 of 15, the second attempt |
| `shooter_acceptance.ps1` (Milestone 5) | The shooter's 26 steps: moving, walls, jumping, shooting, glass, crates, the bonus zone, enemies flinching and falling as ragdolls, the chain, winning, Restart, Pause, losing, and Stop | 26 of 26, the first attempt |

- **The Script Editor run's first attempt** passed 14 of 15: it expected one script in Play, the one it writes, but since
  slice 5 the sample's vault runs two more. The engine was right; the check now expects three, and the run passed 15 of
  15 (`real_input_milestone6_scripts_first_attempt.txt` keeps the first).
- The shooter's numbers are slice 5's: the shut gate held the player at x -16.000, the score went from 300 to 600 at the
  button, and the game was won with 2,300 points and 31 s left.
- The slices' own runs, on their commits: slice 4's 15 of 15 ([`evidence/m6-scripts`](evidence/m6-scripts)) and slice 5's
  14 of 14 ([`evidence/m6-shooter`](evidence/m6-shooter)), with screenshots.

## Performance

The fresh Release build played each scene for 60 s with `--perf-run`, twice in turn, with nobody at the controls
(`Tests/Performance/milestone6_performance.ps1`). Evidence: [`evidence/m6-performance`](evidence/m6-performance).

| Scene | Scripts | Script time a tick | Script time a drawn frame, mean and p99 | CPU a drawn frame |
|---|---|---|---|---|
| The sample, `physics_shooter` | 2 | 0.05 and 0.06 ms | 0.06 ms, 0.17 and 0.22 ms | 7.0 and 7.1 ms |
| **The scripting stress scene, `script_stress`** | **502** | **1.84 ms (desktop in use) and 1.78 ms (idle)** | **1.84 and 1.79 ms, 2.9 and 4.1 ms** | 12.7 and 12.8 ms |
| `script_static`: the same cubes with no scripts | 2 | 0.06 ms | 0.06 ms, 0.24 and 0.18 ms | 11.2 and 11.2 ms |

- **The target is met on average:** 500 scripted entities, each moved by the sample's `Mover.lua`, cost 1.78 to 1.84 ms of
  CPU a frame at 60 Hz. **The slowest 1% of frames are over it,** at 2.9 and 4.1 ms, and the slowest tick took 4.3 ms.
- **The whole frame:** the moving cubes cost 1.5 ms more CPU a frame than the same cubes standing still (12.7 against
  11.2 ms), their drawing included.
- **Without a window** (`script_benchmark`), 500 Movers take 1.40 ms a tick. Calling them is 0.15 ms of it, their
  arithmetic about 0.5, and their moves, through the command bus, about 0.75.
- **How it got there.** The first measurement, before any fix, gave 7.6 ms a tick, with ticks of up to 138 ms, and 56 fps.
  The three costs that grew with the scene (the command bus, physics' scan and the finalizers, under
  [Defects](#defects-found-and-fixed)) brought it to 2.15 ms at `f06fbd7`; Vec3's operators as plain C functions, and
  the mover working out its path once, to 1.78. sol2's safety checks stay on: off, they would have saved another fifth.
- **The rest of the game got faster too:** the sample's C++ gameplay went from 0.42 to 0.04 ms a tick and its physics from
  0.40 to 0.08, as both send their moves through the bus every tick.
- **Vsync caps every scene** at 59.8 fps on the 59 Hz display, as in Milestone 5.
- **The slow frame of Milestone 5** came once in every one of the six runs, 27.2 to 27.5 s into Play: 72 to 130 ms, nearly
  all in the UI and frame snapshot. It is in the scenes without moving scripts too, so scripts do not cause it. The ticks
  it held back run together after it, which gives the 31 ms maximum of script time a frame.

## The libraries

As you approved on 2026-11-06 (decision 1 and 2), fetched by `fetch_dependencies.py` into `D:\AI\GameEngine\resources` at
the commits locked in `cmake/DependencyLock.cmake`; nothing else was added.

| Library | Commit | License | Notes |
|---|---|---|---|
| Lua 5.4.9 | `312b9efa`, tag `v5.4.9` | MIT | The tag's sources match lua.org's `lua-5.4.9.tar.gz`, whose SHA-256 lua.org lists, file for file (`docs/build/dependencies.md`) |
| sol2 3.5.0 | `9190880c` | MIT | Header-only, all safeties on. Compiled under C++20, MSVC 19.44 and `/WX` with no warnings; the fallbacks in the plan were not needed |
| ImGuiColorTextEdit, goossens' fork | `12a5a01d` | MIT | `TextEditor.cpp` only, on the editor's ImGui |

## Defects found and fixed

| Defect | Found by | Fix |
|---|---|---|
| The `GAMEENGINE_WITH_GNS` option lacked a space, so every configure printed a CMake syntax warning | Slice 0's configure log | The space |
| A script that caught the budget's error with `pcall` in a loop ran forever | `scripting_tests` | `pcall`, `xpcall`, `coroutine.resume` and `close`, and `load` pass that error on while the call is over budget |
| After a rename, Details read the script under its old name, because a move keeps the file's time | `editor_script_tests` | The cache compares the path too |
| A copy spawned during `tick` was ticked in the same pass, before its `start`, and a detach in that pass could have invalidated the iteration | `script_world_tests` | Calls go through a copy of the instance list; instances whose entities are gone are forgotten between calls |
| The text widget skipped a closing bracket it had added only when typed at once, so typing `f(x)` gave `f(x))` | Using it | Bracket completion off |
| The editor's shortcuts (W, E, R, Delete, Ctrl+Z) fired while typing code, as the widget does not set ImGui's `WantTextInput` | Slice 4's real input | The text area's focus counts as typing; Ctrl+S there saves the script |
| The Script Editor's **...** button was clipped at the panel's edge while "(unsaved)" showed | Slice 4's captures | The tools wrap |
| A bonus zone script would have failed on a target destroyed as it fell in, as the event named an entity the game had just removed | `scripted_behavior_tests` | Collision and sensor events are not passed on when either entity was removed in the same tick, as the game's own sensors ignore them |
| **The command bus copied and validated the whole scene for every transaction**, and physics, gameplay and scripts send one or more every tick: 500 extra cubes that never moved cost 2.5 ms a tick, one tick's 500 scripted moves 4.7 ms | Slice 6's measurement | A batch that keeps every entity and parent runs in place, at the cost of what it changes; a test compares it with the whole-scene path |
| Physics copied the whole scene every tick to find bodies | Slice 6's measurement | It mirrors components only when the scene's structure revision moves, which transforms alone never do |
| **Every `Vec3` a script made had a finalizer**, which sol2 gives every type: Lua walks each such object every cycle and frees it a cycle late, and ticks of 500 movers paused for 36 to 51 ms | `script_benchmark` | `Vec3`, `Color`, `Quat` and `Entity` own nothing and have none now; `Asset` keeps its own |
| Nothing checked that Simulate runs scripts, which the acceptance names | Reading the acceptance against the tests | A test |

A fault in a test, also fixed: the Script Editor's real-input run expected one script in Play, and since slice 5 the
sample's vault runs two more; it failed at the final commit and expects three now.

## Existing tests changed

None was removed. These were changed because what they check changed on purpose:

| Test | Change | Why |
|---|---|---|
| `engine.core.acceptance`: the command registry | 19 commands instead of 18 | `component.script.set` |
| `engine.core.acceptance`: the documented example and the written version | Header 8 instead of 7 | Format 8. Versions 1 to 7 still load |
| `engine.core.acceptance`: an unsupported version | Rejects 9 instead of 8 | 8 is now current |
| `editor_ux_tests`: the shortcuts a panel may take over from a global one | Adds `File.Save` and `Script.Save` on Ctrl+S | Ctrl+S in the Script Editor saves the script, not the scene |

## Deviations from the plan

- **Three parts of the engine API (D5) were not built:** enabling and disabling an entity, an animated model's joint
  positions, and the look direction. The engine has no disabled state for an entity to bind; joints live in the session's
  animation system, which the script host does not reach; and the view's direction is the camera's, which scripts can read
  as `engine.game.player()` and its children's `world_rotation`.
- **The Script Editor docks beside the Asset Browser**, not with the Material and Effect Editors (D9): that dock already held
  four tabs and is narrow, and code needs width.
- **The budget is 10 million instructions,** about 40 ms of a plain loop on this machine, rather than "about 50 ms".
- **The real-input scripts** are `milestone6_scripts.ps1` (slice 4) and `milestone6_shooter.ps1` (slice 5), not one
  `milestone6.ps1`.
- **Slice 6 changed the engine's core,** which the plan did not foresee: the command bus, the scene and physics. The
  measurement found that their cost per tick grew with the scene's size, which no scripting work could have hidden.

## Not verified

- **Hosted CI.** `.github/workflows/ci.yml` fetches Lua, sol2 and the text editor now (the Linux job Lua and sol2 only), but
  no hosted run has been checked from this machine, and `milestone-6` is not pushed.
- **Linux.** The `linux-gcc-debug` preset was not built, so Lua compiled as C++ and sol2 under GCC are untested.
- **Other machines.** Every number is from one PC: a Ryzen 7 5800X with an RTX 3080 at 59 Hz.
- **Some of the editor by hand.** With real input: an error in a running script followed by its link (the link was
  followed for an error in a saved script), an endless loop in Play, and Open in External Editor starting VS Code. Tests
  cover each without a window, the last by its command line only.
- **The standalone player** runs no scripts yet: it is Milestone 7. `ScriptWorld` depends on nothing in the editor.
- **Scripts other than the shooter's at scale.** Only `Mover` was measured with 500 instances. Scripts that find, spawn,
  cast rays or make many values each tick cost more; `SCRIPTING.md` lists what each costs.
- **People writing scripts.** Every script so far was written for the tests and the sample; nobody else has used the
  Script Editor or the API.
- **Above 60 fps.** Vsync on the 59 Hz display caps every run, as in Milestone 5.

## Known limitations

- **Windows only.** The Linux preset is still not verified.
- **No node graphs, no step debugger.** Scripts are text; errors give the line and a traceback (D11).
- **One Lua state, on the main thread,** runs every script of a session, inside the fixed tick.
- **Three parts of the planned API are missing:** enabling and disabling an entity, joint positions, and the look
  direction (see the deviations).
- **What crosses into the engine costs most.** Every call from a script into the engine goes through sol2 with its safety
  checks on, 0.15 to 0.5 µs each; thousands of scripted entities doing vector arithmetic every tick would take several
  milliseconds.
- **Batches that add, remove, copy or reparent entities** still copy and validate the whole scene. Gameplay sends one
  for each impact effect it spawns and each target it removes, so their cost grows with the scene.
- **The editor's own frame grows with the scene.** With the 500 cubes, the UI and frame snapshot take 2.1 ms a loop
  against 0.7 ms for the sample: more to draw, and the Outliner and the viewport's markers copy the whole scene every
  frame. How much is which was not measured.
- **A scripted pickup's time** comes too late on the very tick the clock runs out, as a script hears events after the
  game's rules.
- **The slow frame of Milestone 5** is still there: one frame of 72 to 130 ms, 27 s into Play, in every run of slice 6,
  nearly all of it in the UI and frame snapshot. Its cause is not known.
- **The Colliders display** still draws every collider's wireframe through models by default; that decision is yours,
  from Milestone 5.
- **No multiplayer.** GameNetworkingSockets is still deferred (it needs protobuf).
