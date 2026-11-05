# Milestone 5 report: characters and effects

Branch `milestone-5`, worktree `D:\AI\GameEngine-Integration`. Plan: [`MILESTONE_5_PLAN.md`](MILESTONE_5_PLAN.md).
Every result below was produced at commit `cabee69` (code, tests, sample), except the earlier slices' own real-input runs,
which were made on their slices' commits. The commit holding this report adds only documents and evidence. **Not merged into
`main`, and not pushed.** It awaits your approval.

## Verdict: done, every acceptance criterion met

The engine now has skeletal animation, ragdolls and particles, all editable in the editor. The Physics Shooter has three
enemies that walk, flinch and fall as ragdolls, with sparks, dust and breaking glass:
- **Builds and tests:** both presets build from empty folders and pass all 555 tests, 146 more than `main`.
- **Real input:** the game was won and lost with the real mouse and keyboard.
- **Performance:** the stress scene, with 10 animated enemies and 5,000 particles, holds 60 fps.

Slice 7, which you asked for before pushing, adds three Unreal-style features:
- **Package names:** assets can be named `/Game/...`.
- **Redirectors:** a move leaves one, and **Fix Up Redirectors** clears them.
- **Selection outline:** the renderer draws it around the outer silhouette only.

All of slice 7 is covered by tests, a render test and a real-input run.

Three things are open:
- **One slow frame in the sample.** About 25 to 31 seconds after the editor starts, one frame often takes 220 to 316 ms.
  Its cause is not found. It doesn't happen in the stress scene.
- **The headroom above 60 fps is unknown.** Vsync on this 59 Hz display caps the frame rate, so the margin is not
  measured.
- **The Colliders display still draws wireframes through models** when it is on, which it is by default. It is not the
  selection highlight, and is left for you to decide ([slice 7](#slice-7-package-names-redirectors-and-the-selection-outline)).

You found one defect while trying the sample: Open Scene kept the editor's current project, so every surface drew magenta.
It is fixed in `c4268c3`, and every result in this report was produced after the fix.

## What it added

| Slice | What | Evidence |
|---|---|---|
| 0 | ozz-animation 0.17 in the build, pinned, compiling under C++20 and MSVC 19.44 with its own warnings as errors | `animation_tests` (5 ozz cases) |
| 1 | Particles: `.effect` assets, emitters in scenes (format 7), a CPU simulation, `particle.mat` batches, the Effect Editor, edit-mode preview, impact sparks from gameplay | [`evidence/m5-effects`](evidence/m5-effects) |
| 2 | Skinned models: skeletons, skin weights and clips from GLB, skinned drawing with `setBones`, posed bounds and picking | [`evidence/m5-skinning`](evidence/m5-skinning) |
| 3 | Animation: Animator and Attach To Bone components, crossfades, a layered flinch, clip events in `.meta` files, two-bone IK, the Animation panel, the bone overlay | [`evidence/m5-animation`](evidence/m5-animation) |
| 4 | Ragdolls and debris: joints in `PhysicsWorld`, a ragdoll builder from the skin, handoff from the pose, bone hits, the Ragdoll component, debris capped at 64 | [`evidence/m5-ragdoll`](evidence/m5-ragdoll) |
| 5 | The shooter: three enemies that patrol, flinch and fall as ragdolls; muzzle flash, sparks, footstep and crate dust, glass shards and slivers | [`evidence/m5-shooter`](evidence/m5-shooter) |
| 6 | Performance: `--perf-run`, a stress scene, finer timings; fresh builds; Open Scene opens the scene's own project; this report | [`evidence/m5-performance`](evidence/m5-performance) |
| 7 | Package names (`/Game/...`) and a thread-safe virtual file system, redirectors and Fix Up Redirectors, the selection as Flecs tags, and a silhouette selection outline | [`evidence/m5-asset-paths`](evidence/m5-asset-paths) |

The progress notes for each slice, with what testing found, are in section 8 of the plan.

## Acceptance criteria

| Criterion (plan section 7) | Result | Evidence |
|---|---|---|
| Both MSVC presets build from fresh folders | Met | [Build and test results](#build-and-test-results) |
| Every existing test passes, and none is removed or weakened | Met | 555 of 555; [changed tests](#existing-tests-changed) |
| A skinned, animated GLB imports, draws in its bind pose and plays its clips in the editor and in Play | Met | `--render-test` skinning and animation checks; real input in slices 3 and 4 |
| Crossfades, a hit flinch, a clip event and a bone attachment work, and can be seen in Play | Met | `animation_tests`, `editor_animation_tests`, `editor_shooter_tests`; real input slice 3 and shooter steps 19 and 20 |
| A character turns into a ragdoll on death, from its pose, moving the way the shot pushed it | Met | `editor_shooter_tests`, `editor_ragdoll_tests`, `--render-test`; shooter step 20 |
| Effects are created, edited, previewed, undone and saved in the editor, and fire from gameplay | Met | `editor_effects_tests`; real input slice 1; shooter steps 17 to 21 |
| The shooter, with enemies and effects, is won and lost with real input | Met, 26 of 26 | [Real-input gameplay](#real-input-gameplay) |
| Release reaches 60 fps with 10 animated enemies and 5,000 particles, or the report says what it reaches | Met: 60.0 fps, capped by vsync | [Performance](#performance) |
| The report lists what was verified, how, and what was not | Met | This document |

## Build and test results

Both MSVC presets were configured and built in empty folders at `cabee69` on 2026-11-05, then tested. The machine is a
Ryzen 7 5800X with 128 GB and an RTX 3080, on Windows 10 Pro, with MSVC 19.44 and Ninja.

| Preset | Folder | Configure | Build | CTest | `--smoke` | `--render-test` |
|---|---|---|---|---|---|---|
| `windows-msvc-release` | `build-m5-windows-msvc-release` | 8 s | 106 s, 1,042 steps | 555 of 555 | Passed, 100 frames | 75 of 75 |
| `windows-msvc-ci` | `build-m5-windows-msvc-ci` | 8 s | 102 s, 1,042 steps | 555 of 555 | Passed, 93 frames | 75 of 75 |

- **Warnings:** the CI preset treats warnings in engine code as errors, and it built clean. The 381 warnings in its log
  are all in third-party code, the same count as in the earlier fresh builds.
- **CTest:** 555 entries, each test case its own process. That is 146 more than `main`'s 409; none was removed.
- **Line endings:** every source file changed from `main` is stored with LF, as `main` stores them. Seventeen files had
  been saved with CRLF during the milestone: 11 that `main` has (restored in `2fdbec3`), the testing README (in the commit
  with this report), and 5 new source files (`03fde0b`).

Fresh builds at `03fde0b` (before slice 7: 534 of 534, 71 of 71) and on 2026-11-04 (before the Open Scene fix) passed
alike, as did `--smoke` and `--render-test` on them.

## Real-input gameplay

`Tests/RealInput/shooter_acceptance.ps1` played the shooter on the fresh Release build at `cabee69`, with the real mouse and
keyboard: **26 of 26, on the first attempt.** It won with all 12 targets down, 2,000 points and 10 s left, every enemy fallen
as a ragdoll. After Restart it lost on time, and Stop gave back the editor scene with its saved file unchanged. Evidence,
with screenshots: [`evidence/m5-shooter`](evidence/m5-shooter).

| Steps | What they check | Result |
|---|---|---|
| 1, 2 | The scene loads clean; Play offers the game; a click starts it | Passed |
| 3 to 7 | WASD, looking, walls, jumping, firing | Passed |
| 8 to 11 | Crates pushed, targets destroyed, score and ammunition, reload, a crate scoring through the bonus zone's sensor, the domino chain | Passed |
| 12, 13, 14 | Win, Restart, Pause, loss on time | Passed |
| 15, 16 | Stop restores the scene; the editor stays usable and exits cleanly | Passed |
| 17 to 22 (new) | Glass shards and slivers, muzzle flashes, enemies walking with footstep dust, a shot enemy flinching and then falling as a ragdoll, every enemy fallen at the win, nothing left behind after Stop | Passed |

The builds at `03fde0b` and `621c2a7` passed 26 of 26 too. At `621c2a7` that came after three attempts the guards
stopped because the PC was in use; no input reached another application. Slice 7's own real-input run is in
[its section](#slice-7-package-names-redirectors-and-the-selection-outline).

The earlier slices' real-input runs were made on their own commits and are not repeated here: effects (slice 1, 19 of 19),
animation (slice 3, 22 of 22) and ragdolls (slice 4, 18 of 18), in their evidence folders.

## Performance

The fresh Release build played each scene for 60 s with `--perf-run`, nobody at the controls and the desktop idle
throughout (`Tests/Performance/milestone5_performance.ps1`). Evidence: [`evidence/m5-performance`](evidence/m5-performance).

| Scene | Mean | Slowest 1% | CPU per frame | What it held |
|---|---|---|---|---|
| The sample, `physics_shooter` | 60.0 fps | 29.5 fps | 1.18 ms | 3 enemies, 114 particles at most |
| **The stress scene**, `stress_test` | **60.0 fps** | 44.3 fps | 3.20 ms | **10 animated enemies, 4,957 particles on average, 5,154 at most** |

- **The target is met:** 60 fps with 10 animated enemies and 5,000 particles.
- **Vsync caps both scenes.** The display runs at 59 Hz, so these numbers can't show how far above 60 fps the engine
  could go. The CPU's 3.2 ms a frame in the stress scene suggests a good margin, but the GPU's share is not measured on its
  own: Filament's frame time includes waiting for the display.
- **In the stress scene,** a fixed tick takes 0.41 ms of physics, 0.51 ms of gameplay and 0.05 ms of animation. The
  particles take 0.03 ms a frame to update. The UI and frame snapshot take 1.9 ms against the sample's 0.7, and the render
  call 1.0 ms against 0.4: the cost of drawing seven more enemies and 5,000 particles.
- **The slowest 1% varies with the machine, not the code.** The sample's p99 frame was 19 ms in the morning and 33 to 34 ms
  from 11:53 on. The change came between two runs of the same old build, with the CPU's share unchanged. A 33 ms frame is one
  missed refresh. The stress scene's p99 stayed at 21 to 23 ms.
- **One slow frame, not explained.** In four of the sample's ten 60 s runs, and in all three complete real-input runs of the
  shooter, one frame took 220 to 316 ms, about 25 to 31 s after the editor started. Physics, animation, particles and
  gameplay are each timed, and none took over 3.2 ms. The two frames caught with the new breakdown spent 131 and 108 ms in the
  UI and frame snapshot, and 147 and 135 ms outside the update, the UI and the render call. It never happened in the stress
  scene. The perf log now records when such frames happen and splits the slowest one, for the next look.
- Nothing is selected in these runs, so slice 7's outline views are not drawn; selected, they add a pass of the selected
  models and one full-screen pass.

## Slice 7: package names, redirectors and the selection outline

Asked for after the first version of this report, before pushing: three Unreal-style editor features. You chose to keep
the file formats, name assets without extensions, outline the whole silhouette even where it is hidden, and prove it in the
engine rather than with a mock program. Design: [`architecture/009-package-names-and-outline.md`](architecture/009-package-names-and-outline.md).

| Part | What it does | How it is verified |
|---|---|---|
| Package names (`VirtualFileSystem`) | `/Game/Textures/Brick` names `Assets/Textures/Brick.png`, in any case, from any thread; a bare name two assets share is a conflict the extension resolves | `asset_tests` (13 new cases, among them four threads resolving while the index is replaced), `editor_asset_path_tests` |
| Redirectors | A rename or move leaves `<old path>.redirect`; old names, and references whose ID is gone, follow them recursively; loops are refused by name | The same, and real input: the old name typed in a Details field resolves through the redirector |
| Fix Up Redirectors | Rewrites scene files on disk, the open scene (one undo step) and materials, then removes the redirectors; keeps those that lead nowhere | `editor_asset_path_tests`; real input: `stress_test.scene` rewritten on disk and the redirector deleted |
| Selection as Flecs tags | `SelectedTag` and `PrimarySelectedTag` mirror the selection on the shown scene, never saved or undone | `editor_asset_path_tests`; real input reads the tag count after a viewport click and a Ctrl+click |
| Silhouette outline | Drawn by the renderer around the outer edge only, over whatever hides the model; the old bounding-box lines are gone | `--render-test` (four checks: about 2,440 pixels change, none inside the silhouette, none away from it, the same behind a wall); real input screenshots |

Real input (`Tests/RealInput/milestone5_asset_paths.ps1`) passed 10 of 10 on the fresh Release build at `cabee69`. Evidence:
[`evidence/m5-asset-paths`](evidence/m5-asset-paths).

**Not changed:** the toolbar's **Colliders** toggle, which is on by default, still draws every collider's wireframe through
the geometry. That is the collision display, not the selection highlight, but it looks like the cage you described. In the
slice 7 screenshots those lines are there before and after selecting. Whether to draw them depth-tested, only for the
selection, or off by default is yours to decide.

## The enemy character

Quaternius' Universal Animation Library, Standard version, CC0 1.0, as you approved on 2026-11-03 (decision 2). Its Godot GLB
is committed unchanged as `Samples/PhysicsShooter/Assets/models/Mannequin.glb`: 6,671,104 bytes, SHA-256
`1b7bf67866360665426bb99e4c71bd619f19b408453c24e30f0c3071601eee5c`. It came from the
[OpenGameArt page](https://opengameart.org/node/174563)'s zip, whose checksum is in `Samples/PhysicsShooter/ASSET_LICENSES.md`
and `asset_manifest.json`. Imported, it is a 1.83 m mannequin with 54 joints and 46 clips. Its `.meta` adds two footstep events
to `Walk_Loop`, where each foot plants. Nothing else was downloaded in this milestone besides ozz-animation, which you also
approved.

## Defects found and fixed

| Defect | Found by | Fix |
|---|---|---|
| Imported models drew from an unfilled index buffer after skinning changed the vertex buffers | `--render-test` | The fill was restored; the old check, which an empty background passed, now compares with and without the model |
| Vertex snapping skipped posed models whose rest box was away from the cursor | Code review | It uses the pose's box, as picking does |
| The Inspector's dock tab had been scrolled out of sight since the Effect Editor tab was added, and a click on the hidden tab closed the whole dock | Real input | Tabs shrink to 40 px before the bar scrolls; the scripts check every tab is in sight |
| The caches of rigs and clips held each model, so neither was ever freed | Code review | They hold it weakly and are pruned; a test checks it |
| The Bone list of a 25-joint model scrolled its name field away | Real input | The field is at the top and narrows the list |
| ozz's nlerp lagged glTF's slerp by 0.9 degrees a quarter of the way through a quarter turn | `animation_tests` | Rotation keys more than 0.2 rad apart get slerped keys between them |
| Fox's first ragdoll weighed 71 kg, with fat parts | `editor_ragdoll_tests` | Flesh is sized from each vertex's strongest joint: 46 kg |
| Hands and feet on joints of their own whipped about at 77 rad/s and stretched their joints by 8 cm | `ragdoll_scene_tests` | Limb ends ride on the bone above, as Box3D's `human.c` does; joined parts stay within 4:1 in mass |
| Taking a hand's body away made the forearm look like a limb end, and a test crashed on the missing body | `physics_tests` | Limb ends are the builder's own choice |
| Ragdoll now clipped off the right of the docked Inspector | Real input | Details buttons go beside the last one only when they fit |
| Enemies at the ends of their patrols would have crouched: idle took the first clip with "idle" in its name, `Crouch_Idle_Loop` | `editor_shooter_tests` | A name that starts with the word wins; a test checks `Idle_Loop` |
| Enemies sent a transform every tick, even standing | Code review | Only a change is sent |
| A real-input run that could not take focus at its start left its editor open | A real-input attempt | `Start-Editor` closes it |
| A locked desktop passed the idle wait and was reported as someone using it | A real-input attempt | The wait also waits out the lock screen |
| Seventeen files were stored with CRLF endings, where `main` uses LF, so the milestone's diff showed every line of twelve of them changed | Reviewing the diff for this report | All LF again (`2fdbec3`, `03fde0b` and this report's commit). The slice 5 "restore CRLF" fix had compared with `HEAD`, not `main` |
| **Open Scene kept the current project.** Started with no arguments, the editor's project is its `bin` folder, so the sample's scene opened with Ctrl+O drew every surface magenta and every material and model was reported missing | You, trying the sample | A scene saved in another project, the nearest folder above it with an `Assets` folder, opens that project too; so does `--scene` without `--project`. A test checks `project_of`. From the `bin` folder, the sample went from 0 textures and 0 models drawn to 12 and 2 (`c4268c3`) |
Faults in the tests themselves, also fixed: checks that bound `ENGINE_CHECK_EQ` to a member of a temporary and read garbage, a
particle check that sampled the editor grid lying edge-on, scripts that picked combo rows without opening the combo, the sample's
glass test that kept firing after the glass broke and killed the enemy walking behind it, and test executables run from the
repository's folder that left scratch folders there.

## Existing tests changed

None was removed. These were changed because what they check changed on purpose:

| Test | Change | Why |
|---|---|---|
| `engine.core.acceptance`: exact record text and the documented example | Header 7 instead of 6 | Format 7. Versions 1 to 6 still load, and a version 6 file with a version 7 record is refused |
| `engine.core.acceptance`: unsupported version | Rejects 8 instead of 7 | 7 is now current |
| `gameplay_tests`: unusable behaviors | Expects 4 issues instead of 3, adding an enemy with no kinematic body | Stricter: the new behavior is checked too |
| `gameplay_tests`: the sample is won | 12 targets instead of 9, won by standing targets, score at least 1,850 instead of 1,250 | The three enemies are targets worth 200, and a fallen one stays in the scene |
| `gameplay_tests`: the sample's glass | Fires until the glass breaks, then stops | An enemy walks behind it, and 40 ticks of fire killed it |
| `asset_tests`: ignored features, and Fox and the other real models | Check that skins and clips are read, and warn of a skin no mesh uses or an animation that moves nothing, instead of warning that skinning and animations are ignored | They are read now |
| `--render-test`: the PBR model is visible | Compares with the same view without the model, instead of a brightness threshold | The old check passed on an empty background, so it missed the index buffer defect above |

## Deviations from the plan

- **Foot IK does not probe the ground.** The runtime has two-bone IK (tested), but nothing aims it at the floor. It was first in the
  cut order, and the arena's floor is flat.
- **Enemies do not attack.** The plan asks them to patrol, flinch and die. Losing is still the clock.
- **Clips are found by convention.** Walk is the Animator's clip; idle and flinch are the first clips whose names start with
  "idle" and "hit" (else contain them), because behaviors hold numbers only.
- **Only ball joints are generated for ragdolls.** A skeleton does not say which joints are knees; hinges are in `PhysicsWorld` for
  later.
- **Particles are quads built on the CPU,** not Filament instancing, which gives each copy only a transform (D10 said so).
- **Clip events use the engine's own crossing code,** not ozz's track triggering job: it is exact both ways and across loops.
- **The Animation panel floats and starts closed,** because a fifth tab in the right-hand dock pushed the Inspector's tab out of
  sight.

## Not verified

- **Hosted CI.** `.github/workflows/ci.yml` fetches ozz now, but no hosted run has been checked from this machine, and
  `milestone-5` is not pushed.
- **Linux.** The `linux-gcc-debug` preset was not built.
- **Above 60 fps.** Vsync on the 59 Hz display caps every run, so the margin above the target is not measured, and the
  GPU's share of a frame is not measured on its own.
- **Other machines.** Every number is from one PC, with one GPU and driver (NVIDIA 576.28, OpenGL).
- **The slow frame's cause,** above.
- **People.** The shooter was played by a script. Its feel and difficulty with enemies have not been tried by players.
- **Other models.** Skinning, animation and ragdolls were tested with Fox, the mannequin and models generated in tests,
  not with a wider set of characters.
- **The earlier slices' real input at this commit.** The effects, animation and ragdoll scripts ran on their own slices'
  commits, not again at `cabee69`. Since then, CTest, the shooter's run and slice 7's run cover the editor. The changes
  affecting those panels are which project a loaded scene opens, and the outline replacing the bounding-box lines in the
  viewport, which none of those scripts reads.
- **Redirectors under version control and in a team.** Fix Up Redirectors rewrites scene files on disk; it was tested on
  one machine with no other copy of the project open.
- **The outline on other GPUs**, and at UI scales other than this PC's; its width is 2 pixels times the UI scale.

## Known limitations

- **Windows only.** The Linux preset is still not verified.
- **No multiplayer.** GameNetworkingSockets is still deferred (it needs protobuf).
- **Scripting comes in Milestone 6**, and the standalone player and packaging in Milestone 7. Game behaviors are C++ with number
  settings.
- **Enemies only patrol.** They do not see, chase or attack the player.
- **Ragdolls cannot get up by blending back to animation.** Stand up puts the entity back on its feet at once.
- **Particles are simulated on the CPU, on the main thread,** and have no lighting, shadows or soft edges against geometry.
- **One additive layer per animator.** There are no blend trees or state machines; gameplay picks clips by role.
- **One slow frame early in the sample,** often, as described under Performance.
- **Undoing a load that opened another project** brings back the old scene in the new project, whose assets it may not
  find. Opening the sample from the menu has always done the same.
- **Package names cover one mount,** `/Game`. The file system takes more (an `/Engine` or plugin mount), but nothing else is
  mounted yet, and a redirector cannot point into another mount.
- **Fix Up Redirectors rewrites old scene files in the current format,** as saving them would, and only finds `.scene`
  files under the project folder, up to 8 folders deep.
- **Entities without a model have no outline,** since they have no silhouette: lights, cameras and empty entities show only
  their gizmo and marker.
- **The Colliders display** still draws every collider's wireframe through the geometry while it is on, which it is by
  default.
