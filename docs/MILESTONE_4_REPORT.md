# Milestone 4 report

Branch `integration/milestone-4`, worktree `D:\AI\GameEngine-Integration`. Plan: `docs/MILESTONE_4_PLAN.md`.
Every result below was produced at commit `af39dec` (code, tests, sample); the commit holding this report adds only documents and evidence.
**Not merged into `main`.** It awaits your approval.

## Verdict: PASS

Every criterion in section 15 of the brief is met (table below), and no P0 or P1 defect is open. The game was played from
start to win, and to a loss, with real mouse and keyboard input on a fresh Release build. Three things limit that verdict and are
worth knowing:

- **No person has played it.** The real input came from a script that reads true positions from a log and aims exactly.
  That proves the controls and rules work, not that the game feels good: mouse sensitivity, movement and difficulty still need a human opinion.
- **Hosted CI has never run** (no remote), and only Windows x64 with MSVC was built.
- **Some Inspector, Lighting and Material Editor controls were not clicked** with real input; they are listed under "What was not verified".

To play: `build-m4-windows-msvc-release\bin\engine_editor.exe --sample`, press **Play**, click the view.

## Acceptance criteria

| Criterion (brief section 15) | Result | Evidence |
|---|---|---|
| Filament remains the sole 3D renderer | Met | `Engine/Renderer` is the only renderer; no other was added |
| Milestone 3 functionality intact | Met | All 146 M3 CTest entries remain and pass; see "Existing tests changed" |
| PBR with the required texture channels | Met | `--render-test`, and WaterBottle (CC0) with all five maps: `evidence/m4/WaterBottle.png` |
| Shadows and IBL visibly work | Met | `--render-test`; `evidence/m4/lighting_test.png`, `shadows_on.png`, `spot_shadow.png` |
| Material assets can be edited and saved | Met | Real input: `workflow_results.txt`, read back from the file on disk |
| The sample loads from the engine | Met | Project menu and `--sample`; real input step 1 |
| The player can move and aim | Met | Real input steps 3–4 |
| The player can shoot targets | Met | Real input steps 7, 9, 12 |
| Box3D raycasts detect hits | Met | `b3World_CastRay` through `ScenePhysics::raycast`; `physics_tests`, `gameplay_tests` |
| Dynamic objects respond to impacts | Met | Real input: a crate moved 5.16 m; the domino chain destroyed its target |
| Collisions and sensor events exercised | Met | Real input: chain reaction (hit events), bonus zone (sensor, score 100 → 150) |
| Scoring, ammunition, restart, game over | Met | Real input steps 10–14 |
| Playable without modifying source | Met | Everything shooter-specific is data in `Samples/PhysicsShooter` |
| Properly licensed assets | Met | All CC0: `Samples/PhysicsShooter/ASSET_LICENSES.md`, `asset_manifest.json` |
| Both MSVC presets build | Met | Below, from fresh directories |
| Existing tests pass; new tests pass | Met | 207/207 on both presets |
| GPU smoke tests pass | Met | `--smoke` and `--render-test` on both presets |
| Real-input verification documented | Met | Below |
| No P0 or P1 defects in scope | Met | None known |

## Build and test results

Windows 10 Pro 19045, AMD Ryzen 7 5800X (8 cores, 16 threads), 128 GB RAM, NVIDIA RTX 3080 (driver 32.0.15.7628, OpenGL), MSVC 19.44,
display at 150% scale. Fresh directories (removed first; I had created them this session).

```powershell
$env:GAMEENGINE_RESOURCES_DIR = 'D:\AI\GameEngine\resources'
./cmake/scripts/dev-shell.ps1
cmake --preset windows-msvc-release -B build-m4-windows-msvc-release
cmake --build build-m4-windows-msvc-release
ctest --test-dir build-m4-windows-msvc-release --output-on-failure -j 8
# The same with windows-msvc-ci and build-m4-windows-msvc-ci.
```

| Preset | Configure | Build | CTest | `--smoke` | `--render-test` |
|---|---|---|---|---|---|
| `windows-msvc-release` | exit 0 | exit 0, 897 steps, 50 s | 207/207, exit 0 | exit 0 | 39/39, exit 0 |
| `windows-msvc-ci` (`/WX`, strict lock) | exit 0, dependencies `(locked)` | exit 0, 897 steps, 53 s | 207/207, exit 0 | exit 0 | 39/39, exit 0 |

An earlier fresh CI build in the same session took 234 s, so the 53 s probably benefits from the disk cache. Warnings: about 380 per preset,
none in `Engine/` or `Tests/`. With `-DENGINE_EDITOR_GPU_TESTS=ON`, both GPU tests also ran through CTest (2 tests, 6.6 s, no failures).

| Suite | M3 | M4 |
|---|---|---|
| `physics_tests` | 31 | 41 |
| `physics_scene_tests` | 26 | 32 |
| `gameplay_tests` (new) | | 33 |
| `asset_tests` | 35 | 47 |
| `engine.core.acceptance` | 23 scenarios | 24 |
| `engine.editor.acceptance` | 12 groups | 17 |
| CTest entries in all | 146 | 207 |

### Existing tests changed

None was removed. These were updated because the behavior they checked changed on purpose:

| Test | Change | Why |
|---|---|---|
| `engine.core.acceptance`: exact record text | New fields in `MESH`, `LIGHT`, `BODY`, `COLLIDER`, and header 6 | Format v6. Versions 1–5 still load, with tests |
| `engine.core.acceptance`: unsupported version | Rejects 7 instead of 6 | 6 is now current |
| `asset_tests`: `glb_reports_ignored_features` | Checks alpha mode instead of a "transparency ignored" warning | Transparency now works |
| `asset_tests`: real models | The helmet has all five maps, correct color spaces and no warnings, instead of one image and a warning | The maps are now used |
| `--smoke`: hue check | A channel must exceed the others by 2×, not 3× | Pixels are now read after tone mapping, which compresses ratios; 2 is about 5 in linear light |
| `--smoke`: light probes | Run with the environment off and a linear tone curve | So the hue is still the surface's and the light's alone |

## Real-input gameplay

Method: `Tests/RealInput/shooter_acceptance.ps1` on the fresh Release editor, on a temporary copy of the sample. It sends `SendInput` events and reads
true state from `--game-log`. Results: `evidence/m4/real_input_results.txt`.

| # | Step | Observed | Result |
|---|---|---|---|
| 1 | The shooter scene loads | 73 entities, clean | Pass |
| 2 | The arena renders | `02_ready_overlay.jpg`, `02_arena_playing.jpg`; a click starts and takes the mouse | Pass |
| 3 | WASD moves the player | W −2.50 m Z, S +2.58 m Z, A −2.50 m X, D +2.50 m X in 0.5 s each | Pass |
| 4 | Mouse look works | Captures differ; walking follows the new direction | Pass |
| 5 | The player collides with walls | Sprinting into the south wall stops at z = 15.650: face at 16 minus radius 0.35 | Pass |
| 6 | Jumping works | Eye height 1.58 → 2.95 → 1.58 | Pass |
| 7 | Shooting registers hits | Ammo 15 → 14; impact mark in `07_shot_impact.jpg` | Pass |
| 8 | Dynamic crates move when shot | Stack Crate 3 moved 5.16 m. One shot at Domino 1 toppled the row and destroyed the Chain Target | Pass |
| 9 | Targets receive damage | Targets 9 → 8 | Pass |
| 10 | Score and ammo update | Score 0 → 100, ammo 6 | Pass |
| 11 | Reload works | Ammo 4, reserve 150 → 15 and 139. Also: a crate knocked into the bonus zone scored through a sensor, 100 → 150 | Pass |
| 12 | The game can be won | All 9 destroyed, score 1400, 21 s left: `12_game_won.jpg` | Pass |
| 13 | The game can be lost | Waited out the clock: `13_game_lost.jpg` | Pass |
| 14 | Restart resets the scene | R: targets 9, score 0, ammo 15. Esc: paused, clock frozen at 88.97; a click resumed | Pass |
| 15 | Stop restores the editor scene | Dirty 0; saving again gives a byte-identical file | Pass |
| 16 | The editor remains usable | + Cube 73 → 74, undo → 73, exit code 0 | Pass |

### Authoring workflow

`Tests/RealInput/editor_workflow.ps1`, same build, 19 of 19 checks: `evidence/m4/workflow_results.txt`.

New material; assign (one undo step); metallic, roughness and base color typed and read back from the `.material` file; alpha mode; Ctrl+Z and Ctrl+Y
changing the file; duplicate; background import of a JPEG and two GLBs; a material on one part of a 9-part model, saved as `MATSLOT`; Indoor and Outdoor presets; Add sun;
save, close, reopen; a missing material (magenta placeholder and a Console message, `w7_missing_material.jpg`); a new empty project; opening the sample from the Project menu.

### An input incident during testing

Partway through one workflow run someone brought a browser to the front. The first version of the driver had no focus check, so it kept sending input,
which may have reached the browser: about seven clicks, two Ctrl+A, two typed file paths each followed by Enter, and a Ctrl+S. I could not tell what those did.
The driver now waits for an idle desktop, checks focus before every event and stops if the editor loses focus or the cursor moves on its own.
The results above come from runs with those guards. M3's "cursor interference" notes were probably this same situation.

## GPU verification

`engine_editor --render-test`, 39 checks by readback: `evidence/m4/render_test_results.txt`. It covers base color, sphere winding, map color space, alpha mask and blend, single and
double sides, normal map direction, metallic-roughness, occlusion, emissive and its independence from exposure, a missing map, directional shadows at each quality and off, spot and point shadows,
IBL, intensity without an IBL rebuild, EV100, a new sky rebuilding the IBL, skybox and its orientation, editor lights following exposure, spot and point falloff, GPU resource lifecycle, a PBR model and the lighting test scene.
Inspected by eye: `WaterBottle.png`, `lighting_test.png`, and DamagedHelmet during development.

## Changes by subsystem

| Subsystem | Change |
|---|---|
| Scene, commands | Format v6: `MESH` material and UV scale, `MATSLOT`, spot lights and shadows, `ENVIRONMENT`, body rotation lock, collision filters, `BEHAVIOR`. Commands `SetEnvironment`, `SetBehaviors`. Earlier versions still load |
| Assets | Material assets, `.hdr`, five maps per color space, tangents, `prepare_import` and `commit_import` |
| Renderer | `pbr.mat` per blend mode, part materials, shadows, IBL through `IBLPrefilterContext`, EV100, tone mapping, AO, bloom, MSAA, FXAA, material preview |
| Physics | Raycast, impulse at a point, filters, rotation lock, hit events |
| Gameplay (new) | `InputMap`, `Game`: player, weapon, health, targets, movers, impact damage, bonus zones, pickups, rules, plain `HudState` |
| Editor | Material Editor and Lighting panels, per-part materials, behaviors in Inspector and Add Component, background import, resource release, cursor capture, HUD, Project menu, `--sample`, `--scene`, `--game-log`, `--perf-log`, `--render-test` |
| Sample | `Samples/PhysicsShooter`: arena, 9 targets of 5 kinds, generator, licenses, lighting test scene |
| Docs | `architecture/007-materials-lighting-gameplay.md`, `006-scene-format.md`, `testing/README.md` |

Architecture decisions, including the player controller choice and why, are in `architecture/007-materials-lighting-gameplay.md`.

## Downloaded assets

All CC0 1.0, from the publishers, 16.6 MB, committed directly (no remote exists to serve Git LFS). Details, URLs and SHA-256 are in `Samples/PhysicsShooter/ASSET_LICENSES.md` and `asset_manifest.json`.

| Asset | Author | Source |
|---|---|---|
| Concrete Floor Worn 001 | Dimitrios Savva, Rico Cilliers | Poly Haven |
| Painted Plaster Wall | Amal Kumar | Poly Haven |
| Metal Plate, Brown Planks 03 | Rob Tuytel | Poly Haven |
| Kloofendal 48d Partly Cloudy (Pure Sky) | Greg Zaal, Jarod Guest | Poly Haven |
| Water Bottle | Microsoft | Khronos glTF Sample Assets |

Kenney's Blaster Kit was not used: its page exposes no direct download link. The gun is built from primitives.

## Performance

Measured by `--perf-log` over the whole real-input acceptance run (about 2.9 minutes, 2160×1350 window, viewport about 1250×850), on the hardware above. Numbers come from the run, not estimates.

| Measure | Value |
|---|---|
| Frame time, drawn frames | Mean 16.68 ms, median 16.50 ms, 99th percentile 22.42 ms (about 60 Hz, following the display) |
| Physics step | Mean 0.28 ms, maximum 0.80 ms, 65 bodies |
| Gameplay per tick | Mean 0.29 ms |
| CPU time submitting a frame | Mean 0.36 ms |
| Filament GPU timer | Mean 14.46 ms. Its closeness to the display interval suggests it may include waiting, so it is not a measure of headroom |
| Primitives submitted | 68, before culling. Actual GPU draw calls, with shadow passes, are not available from this API |
| GPU resources | 68 objects, 68 material instances, 12 textures, 1 model, 1 light, 1 IBL build |
| Peak working set | 363 MB. GPU memory was not measured |
| Startup to first frame | 829 ms with the sample scene; 336 ms with an empty project |
| Import, submit to landing | `wood_base.jpg` 19 ms, `WaterBottle.glb` (9 MB) 173 ms, `TransmissionRoughnessTest.glb` 35 ms |

Per-frame work found and reduced: material parameters are written only when changed, a frame copy was removed, and the loop no longer spins when Filament skips a frame.

## Defects found and fixed

| Defect | Found by | Fix |
|---|---|---|
| Normal maps inverted | Readback | `packTangentFrame` and the shader disagree on the bitangent's sign; `tangent_frame` compensates |
| Normal maps nearly flat on scaled boxes | Readback | Filament's tangent frame is not normalized; `pbr.mat` divides by the axis lengths |
| Built-in sphere inside-out | Looking at the sample | Exposed by back-face culling; winding corrected, readback check added |
| Cube sides showed images upside down | Code reading | UVs corrected |
| DPCF shadows draw nothing on this driver | Readback | PCF at every quality |
| Editor lights blew out at indoor exposure | Looking at a capture | They follow the scene's exposure, readback check added |
| Project dialog's field had no focus, so typed text was lost | Real input | Focus on open; Enter accepts |
| Game stuck after toolbar Pause before Start; no way to recapture after toolbar Resume | Code review | Both handled |
| Stale material undo entries after a project switch | Code review | Cleared on open |
| Chain target destroyed at the start, overlapping a step | `gameplay_tests` | Sample layout corrected |
| Main loop spun at about 890 iterations a second | Performance log | Waits when a frame is skipped |

## What was not verified

- **A person playing the game.** See the verdict.
- **Controls not clicked with real input**, whose behavior is covered by automated tests at the session or renderer level: dragging a slider in the Material Editor (values were typed, so the live draft path ran only in `materials_workflow`), the texture slot combos, the Lighting panel's Source, Image, Tone mapping and Quality combos, Add key light, Add Component's Behavior menu, spot light fields in the Inspector, and Open project. Time and ammo pickups ran only in `gameplay_tests`, not with real input.
- **Dropping files on the window** with the new background import path.
- **Hosted CI, Linux, macOS, `windows-msvc-debug`.** The Linux path compiles `Engine/Gameplay` and runs `gameplay_tests` by design, but has not been built.
- **Other DPI scales and window sizes.** Real-input positions assume the default layout at this window's opening size.
- **The cost of an IBL rebuild**, which runs on the render thread when the sky changes. It was not timed.

## Known limitations

| Area | Limitation |
|---|---|
| Rendering | Objects move at the 60 Hz tick, with no render interpolation. Filament allows one directional shadow caster. Secondary UV sets and texture transforms are ignored, with a warning. An IBL rebuild stalls a frame |
| Materials | Saving is immediate on release, with no separate Save. A material's undo history is per session and cleared on a project switch. A color change through the picker popup can make several undo steps. No material thumbnails in the browser |
| Assets | Importing runs in the background, but references in a loaded scene still load synchronously on first use. Materials referencing a deleted texture keep the reference and draw without it |
| Gameplay | No audio (miniaudio not integrated). No stair stepping. Impact marks are small spheres, not decals. Behavior values are plain numbers, and a misspelled key silently uses its default |
| Editor | While the game runs, the main loop can iterate several times per drawn frame, rebuilding the UI each time. The Inspector shows runtime values only while the mouse is free. The executable finds its materials and sample by build-tree paths, so it is not relocatable |
| Sample | The sample's scene is saved over if you press Save with it open. It is a project like any other; `tools/generate_project.py` restores it |

## Deferred

- **Skeletal animation:** deferred to M5, as the brief allows. Findings and plan are in `architecture/007-materials-lighting-gameplay.md`.
- **Standalone runtime:** the game runs without the editor in `gameplay_tests`, and `HudState` is ready for another HUD, but no standalone player exists.
- **Audio, decals, render interpolation.**
- **Carried from M3:** two ImGui builds, the second CMake entry point, hosted CI.
