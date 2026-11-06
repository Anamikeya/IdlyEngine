# Testing

## Running

```powershell
./cmake/scripts/dev-shell.ps1 ctest --preset windows-msvc-debug          # everything
./cmake/scripts/dev-shell.ps1 ctest --preset windows-msvc-debug -L physics
./cmake/scripts/dev-shell.ps1 ctest --preset windows-msvc-debug -L smoke          # headless smoke tests
./cmake/scripts/dev-shell.ps1 ctest --preset windows-msvc-debug -R connection_manager
build/windows-msvc-debug/bin/physics_tests.exe --list
build/windows-msvc-debug/bin/physics_tests.exe --run free_fall_matches_gravity
build/windows-msvc-release/bin/engine_editor.exe --smoke                  # GPU smoke; opens a window for a few seconds
```

`engine_editor --smoke` and `--render-test <folder>` are not CTest entries by default, because they need an interactive
desktop. Configure with `-DENGINE_EDITOR_GPU_TESTS=ON` to register them as `engine.editor.gpu` and `engine.editor.render`.

Real mouse and keyboard runs are scripts, on a Release build, with no one using the machine:

```powershell
powershell -ExecutionPolicy Bypass -File Tests/RealInput/shooter_acceptance.ps1 -Editor <engine_editor.exe> -Output <folder>
powershell -ExecutionPolicy Bypass -File Tests/RealInput/editor_workflow.ps1 -Editor <engine_editor.exe> -Output <folder>
powershell -ExecutionPolicy Bypass -File Tests/RealInput/editor_ux_phase1.ps1 -Editor <engine_editor.exe> -Output <folder>
powershell -ExecutionPolicy Bypass -File Tests/RealInput/editor_ux_phase2.ps1 -Editor <engine_editor.exe> -Output <folder>
powershell -ExecutionPolicy Bypass -File Tests/RealInput/editor_ux_phase3.ps1 -Editor <engine_editor.exe> -Output <folder>
powershell -ExecutionPolicy Bypass -File Tests/RealInput/editor_ux_phase4.ps1 -Editor <engine_editor.exe> -Output <folder>
powershell -ExecutionPolicy Bypass -File Tests/RealInput/editor_ux_phase5.ps1 -Editor <engine_editor.exe> -Output <folder>
powershell -ExecutionPolicy Bypass -File Tests/RealInput/milestone5_effects.ps1 -Editor <engine_editor.exe> -Output <folder>
powershell -ExecutionPolicy Bypass -File Tests/RealInput/milestone5_animation.ps1 -Editor <engine_editor.exe> -Output <folder>
powershell -ExecutionPolicy Bypass -File Tests/RealInput/milestone5_ragdoll.ps1 -Editor <engine_editor.exe> -Output <folder>
powershell -ExecutionPolicy Bypass -File Tests/RealInput/milestone6_scripts.ps1 -Editor <engine_editor.exe> -Output <folder>
powershell -ExecutionPolicy Bypass -File Tests/RealInput/milestone6_shooter.ps1 -Editor <engine_editor.exe> -Output <folder>
```

They use a settings folder of their own (`--config-dir`) and find controls by name (`--ui-map`).

They read the editor's `--game-log` and the files on disk, and save captures. `SendInput` goes to whatever has focus, so
`Tests/RealInput/InputDriver.psm1` waits for an idle desktop and stops the run if the editor loses focus or the cursor moves
without it. A stopped run exits with code 2 and proves nothing either way; run it again.

## Framework

`Tests/Framework` is a small self-registering framework with no third-party dependency, so it
builds wherever the engine builds, including future mobile targets.

```cpp
#include "engine/test/test.h"

ENGINE_TEST(my_case) {
	ENGINE_REQUIRE(setup_ok);         // aborts this case on failure
	ENGINE_CHECK(x == 1);             // records the failure and continues
	ENGINE_CHECK_EQ(a, b);            // prints both values
	ENGINE_CHECK_NEAR(v, 9.81, 1e-3);
}
```

Register an executable with `gameengine_add_test_executable(<target> LABELS ... SOURCES ...
LIBRARIES ...)` (`cmake/GameEngineTesting.cmake`). After each build the executable is asked for
`--list`, and every case becomes its own CTest test named `<target>.<case>`. Each case therefore
runs in a fresh process, which isolates process-global state such as Box3D's world table.
Case names must be unique within an executable.

## Current suites (616 CTest entries)

| Executable | Labels | Cases | Covers |
|---|---|---|---|
| `physics_tests` | physics, unit | 58 | World lifecycle and limits, config validation, move semantics, Box3D memory release, fixed step and catch-up, free-fall accuracy, bitwise determinism, body/collider lifecycle, stale/foreign handles, input validation, mass, rest/sleep, impulses, kinematic targets, filters, moved/contact/hit/sensor events. Milestone 4: raycast hit and miss, ray filters, ignored body and sensors, collision filters, impulse direction and mass response, off-center spin, wake on impulse, static and kinematic refusal, rotation lock. Milestone 5 (`joint_tests.cpp`): ball joints holding their cone and twist limits, a hinge, joint friction, filter joints, bad joints refused, joints going with their bodies, disabled bodies, angular impulses, swing and twist as Box3D measures them; the ragdoll builder on a generated person: which bones get bodies and how they are sized, hands and feet riding on the bones above, joint frames at rest, per-bone settings, problems, the part limit, the handoff velocity, and limits widened to fit the starting pose |
| `physics_scene_tests` | physics, integration | 46 | Real Box3D world against the real `Scene`/`CommandBus`, authored through scene components: entity-to-body mapping, pose publish via a history-free bus, editor revision/undo isolation, feedback-loop guard, edit-mode follow, kinematic targets, parented transforms, unsupported hierarchies, scale rules, delete/undo restore, entities losing their components, resource release, contact and sensor events, collider rebuild, pause/reset, debug geometry, play/stop restore, >256-body publish split, owner-thread enforcement. Milestone 4: raycast entity mapping and filters, impulses, removed entities, hit events, reset. Milestone 5 (`ragdoll_scene_tests.cpp`): a ragdoll settling from six poses with its energy never rising, its joints holding and closing up and ending asleep on the floor; the same fall bit for bit; bodies that mirror no entity and never write the scene; the start velocity; ray hits naming the owner and the bone; shots on the nearest part; the owner's body sitting out; ragdolls going with their entity and a reset; contacts naming the owner; bad ragdolls refused; debris capped at 64 oldest first, gone two seconds asleep or past its lifetime, mirroring no entity and left out by rays; bad debris refused |
| `gameplay_tests` | gameplay, integration | 51 | Input actions, walking, sprint, looking, floors and walls, jump, falling out, fire rate, ammunition, reload, hits, damage, score, impulses and mass, static walls, dynamic and moving targets, impact damage, bonus zones, pickups, win, loss, the clock and pause, determinism. The real sample scene: loads, settles, can be won, lost and restarted, the domino chain, the bonus zone, breaking glass. Milestone 5: an impact template with an emitter asks for particles instead of a mark. Milestone 5: enemies patrol facing the way they walk, stand at each end and ask for idle and walk clips; a shot one flinches and stands; a killed one with a Ragdoll asks for one with the shot's push and stays, without one it goes; footsteps raise dust where the foot lands; shots flash at the muzzle; glass breaks into shards and slivers the same way each time; hard hits raise dust once a cooldown; the sample's enemies patrol, flinch and fall, and the sample is won with them. Milestone 6 (`scripted_behavior_tests.cpp`): the sample's Mover, Pickup and BonusZone scripts against the C++ behaviors, side by side, every entity's transform and the HUD compared exactly after every tick, alone and in the whole sample over 1,200 ticks of shooting and walking; the vault's gate opens only once its room's targets have fallen, holds the player until then and slides into the wall; its button gives its prize once, from within reach; a button presses its target's script |
| `asset_tests` | assets, unit | 78 | Paths and stable ids, OBJ, TGA, PPM, `.meta`, database catalogue (scripts catalogued and copied as text), cache/reload/revision/unload, missing and failed files, scan issues. GLB: geometry, transforms, mirroring, materials, embedded images, warnings, invalid and unsupported files, real models from the dependency tree. PNG and JPEG decoding and rejection. `import_file` naming, ids, and leaving nothing behind on failure. Milestone 4: all five maps and their color spaces, alpha and sides, broken secondary maps, tangents, material text and files, `.hdr`, background import. Milestone 5: `.effect` text round trip, the documented example, rejection of bad files, create, save and reload, preparing and importing. Skins and clips (`skin_tests.cpp`, GLBs built in memory): skeleton math, bind-space vertices and weights, the nodes above the joints and model scale, the 4 strongest influences, rigid parts under a joint, linear, step and cubic clips, refusing what cannot be drawn, split joint matrices, and Fox from the dependency tree. Clip events in `.meta` files: sorted, quoted clip names, bad events refused; saved by the database without re-importing the model, kept by a rescan and a move. The sample's enemy, Quaternius' mannequin: its skeleton and 46 clips. Slice 7 (`virtual_path_tests.cpp`): package names in any case and their file, a bare name two assets share as a conflict that the extension resolves, malformed and unmounted names, redirector chains followed and dangling ones named, loops refused by name, the 32-redirector limit, assets winning over redirectors, mount points, resolving from four threads while the index is replaced, the redirector file format, moves leaving redirectors and taking a name back, and scans cataloguing and removing them |
| `systems_smoke_tests` | integration, smoke | 1 | OBJ asset sizes a collider, physics drives a scene entity, `extract_frame` sees the published pose |
| `editor_dependency_smoke` | editor, smoke | 3 | `gameengine::imgui` renders frames through ImGui's null backend (no window, no GPU); `gameengine::imgui_text_edit` draws a Lua script with a line marker and keeps its text; `gameengine::glfw` links and reports its version without `glfwInit` |
| `editor_ux_tests` | editor, unit | 61 | The editor's foundation without a window: chords, command registry, context routing, input blocks, conflicts, keymap files, the default command table and its generated document, bus and session transactions, history order and jumps, gizmo drags, selection, property tables |
| `editor_viewport_tests` | editor, unit | 49 | The viewport without a window: the camera state machine and every navigation mode, field of view, eased moves, framing, rays, snapping to the floor, vertex snapping, Alt+drag, hiding, view state files, orthographic views, grid and show flags, settings files. Milestone 5: a pose is drawn but never edits the scene, and picking and vertex snapping follow it |
| `editor_panels_tests` | editor, unit | 31 | What the panels stand on, without a window: folders and their undo, lock and hide, copy and paste, grouping, moving assets and fixing references, property and component text, thumbnail decoding, panel shortcuts |
| `editor_workflow_tests` | editor, unit | 30 | Reaching things by typing and running without leaving the editor, without a window: fuzzy scoring and ranking, the real commands found by typing, console variables, actions, suggestions and history, simulate and eject, which keys work while the game holds the keyboard or a text field has it, asset type names |
| `editor_feedback_tests` | editor, unit | 33 | What the editor says and how it answers, without a window: the message log and its filters, links, toasts, tasks, cancelled imports, the cheat sheet, tooltips, typing during a gizmo drag and the exact change it makes |
| `editor_effects_tests` | editor, effects, unit | 11 | Particle effects in the editor, without a window: making and placing an effect, the edit-mode preview and its switch, hidden emitters, live drafts that keep living particles, saving as one undo step, assigning, renaming, a missing effect, Play mode's own particles, restart, an effect's image and its lifetime, the Details table, and how frames clean particle batches |
| `editor_animation_tests` | editor, animation, unit | 7 | Animators in the editor, without a window (GLBs built in memory): placing an animated model gives it an animator that plays while editing and adds nothing to the scene or its history, the preview switches, scrubbing, posed bounds, choosing a clip with undo; Play ticks with the fixed step, crossfades, pause and step, Stop; an entity attached to a joint is drawn, picked, moved by `set_world` and a drag on it, and stays on its parent for an unknown joint; clip events in the `.meta` with undo, firing as the clip plays, read by a new session; clips from another model by joint name and the problems reported; bone lines; the skeleton and clips of a model nothing uses are freed |
| `editor_ragdoll_tests` | editor, physics, unit | 8 | Ragdolls and debris in the editor, without a window, with Fox from the dependency tree: Fox's plan sized as it stands, bone settings and the problems reported, falling in Play from its walk where the pose was and never reaching the scene, moving on as the pose was moving, a shot pushing the part hit, standing up and Restart, going with its entity, the Show Ragdolls outlines, debris drawn like its template, never picked, gone once settled, capped and taken by Stop |
| `editor_script_tests` | editor, scripting, unit | 13 | Scripts in the editor without a window: saving in Play reloading the running script with its state, a broken save keeping the last good code, a change by another editor seen within half a second, and saving while editing changing what Details reads; checking typed text without saving it, and the external editor's command; Play running them with the entity's own values, printing with script and line, `on_stop`, and Stop putting the scene back, and Simulate running them too; an error in Play stopping only the instance that made it, with its line, function and entity, and a script that cannot load reported when Play starts; New Script's template, by package name, read and running; assigning a script in one undo step and only a script; values of all seven types kept only where they differ from the defaults, in name order, refused when of the wrong type, undone, and turned into what the script sees; a value the script no longer declares kept until removed; a broken script's line and reading it again once mended; a script that never ends at its top level stopped; values saved in format 8; copies pointing their links at the copies, keeping links to entities in the scene and clearing those to none; renaming a script updating the component, an asset value and the script's name |
| `editor_shooter_tests` | editor, gameplay, unit | 7 | The Physics Shooter sample in Play without a window, the session carrying out what the game asks: enemies walk, flinch on top of their walk and fall as ragdolls that stay down, and stand in their idle clip at the ends of their patrols; footsteps raise dust and shots flash at the muzzle; glass breaks into debris drawn like its shard, gone once settled; Stop leaves the editor scene as it was. The stress scene holds what its performance numbers claim: ten enemies walking and 5,000 particles alive, with animation and particle time counted apart from gameplay. Milestone 6: the vault's scripts run in Play through the project's asset database, its gate opens and its button pays, with no script error, and Stop leaves the scene as it was. Slice 6: the scripting performance scene holds what its numbers claim, 500 cubes each moved by Mover.lua beside the vault's two scripts, at their exact heights after a second, with script time counted apart |
| `editor_asset_path_tests` | editor, assets, unit | 8 | Slice 7 in the session, without a window: the selection mirrored as Flecs tags and the outline marks read from them, which never reach the scene, its history or its file; a selected group outlining its members; Play tagging the runtime copy only; package names in any case; a rename to a package name leaving a redirector that old names follow; a reference whose ID is gone found through the redirector; Fix Up Redirectors rewriting a closed scene file, the open scene as one undo step, and a material, then removing the redirector; loops and dangling redirectors kept, and none of it in Play |
| `animation_tests` | animation, unit | 19 | ozz-animation as the engine builds it (5 cases): a skeleton and clip built in code, sampled, blended, archived, an invalid clip refused. `Engine/Animation` (14): a rig keeps the model's joint order whatever ozz's, broken skeletons refused, clips at known times, looping, speed and playing backwards, crossfades, layered clips as differences, events crossed across loops and backwards, the joint world query, clips from another model by joint name, two-bone IK reaching, out of reach and at weight 0, restarts only on authored changes, bit-for-bit determinism, and every Fox clip. Poses compare at ozz's key precision, about 0.5 mm per metre |
| `scripting_tests` | scripting, unit | 27 | `Engine/Scripting` (20 cases): hot reload keeping each instance and its state, adding a new property's default, and a broken version keeping the last good one; a changed module running again the scripts that require it; a script class and its properties in the order its text names them, instances with defaults, overrides and a wrongly typed override, entity order, `print` with its script and line, a failing call stopping only its instance with the line and a traceback, load errors (syntax, no table, bad names and values, a callback that is not a function), the sandbox (no io, os, debug, package, files or bytecode), separate globals per script, `require` once, missing and in a cycle, runaway loops stopped even through pcall, xpcall, coroutines and load, the default budget timed, the memory limit, seeded random numbers, Vec3 and Color as values, values made by the thousand having no finalizer and gone after one collection (Asset keeping its own), Vec3's operators computing and refusing as they did through sol2, inspecting a script, and finding scripts by package name. Lua 5.4.9 and sol2 as the engine builds them (7): the pinned release, only the libraries opened, a script calling C++ types and functions and C++ calling the script, errors naming the script and line, an engine exception becoming a Lua error that `pcall` catches, a Lua error unwinding C++ frames (Lua is C++), and the same table order in two states (the fixed hash seed) |
| `script_world_tests` | scripting, gameplay, integration | 9 | Scripts in a running world: the real scene, command bus, Box3D and game, ticked as a play session ticks them. `start` once before the first `tick`, `late_tick` after physics; moving entities and reading the change at once, world positions under a scaled parent; spawned copies starting on the next tick and destroyed ones stopping; rays, impulses and velocities; triggers and hits reaching both entities; damage and death from the game's rules, score, time and winning; input by action name; a destroyed entity and a missing game as errors at their line; and a run with physics, random pushes and table order repeating tick for tick |
| `effects_tests` | effects, unit | 15 | The particle system: rate and bursts per cycle, stopping without a loop, motion and death, gravity and drag, the cone and speed range, a rotated emitter, local space, floor bounces, size and color curves in linear light, the particle cap, one-shots and their limit, waiting emitters, following the emitter list and restarting on an edited effect, seeded determinism, bad input |
| `networking_tests` | networking, unit | 41 | Serialization, loopback transport, connection manager |
| `online_services_tests` | online, unit | 7 | NullOnlineService |
| `engine_core_tests` | none | 1 entry, 29 scenarios | `engine.core.acceptance`: scene, commands, undo, components, serialization of all format versions (version 7's EMITTER, ANIMATOR, ATTACH, RAGDOLL and RAGBONE; version 8's SCRIPT with all seven value types, its command, links following a duplicated subtree, and refused values), malformed input; Milestone 6: batches of names, transforms and components committed in place giving what the whole-scene path gives, to the bit, with the same errors, receipts and undo, with and without history, and the structure revision moving only with more than transforms |
| `engine_renderer_tests` | none | 1 entry, 9 scenarios | `engine.renderer.snapshot`: frame extraction, cameras, lights, handle validation, owner thread |
| `engine_editor_tests` | none | 1 entry, 18 groups | `engine.editor.acceptance`: session behavior without a window, including the 13-step acceptance workflow, Play mode, physics, assets, materials, lighting, background import, resource release, the project a scene file belongs to, and the shooter in Play mode |

The GPU smoke (`engine_editor --smoke`) opens a real window and checks by readback: surface color, scene lights
replacing editor lights, Play and Stop isolation, resize, and the orientation of an imported texture.

`engine_editor --render-test <folder>` opens a real window and compares pixels: base color, sphere winding, map color
space, alpha mask and blend, single and double sides, normal map direction, metallic-roughness, occlusion, emissive, a
missing map, shadows at each quality and off, IBL, intensity, exposure, skybox, sky orientation, spot and point lights, GPU
resource lifecycle, particles (additive light, strength, the soft dot's edge, alpha covering and blending over a lit wall, the
preview switch and freeing a batch's GPU buffers), skinning (a bar at rest against its unskinned twin, a pose moving the
predicted pixels, culling by the pose's box, a pose of the wrong size, freeing, and Fox at rest and in a frame of its Run clip),
animation (placing an animated bar gives it an animator, which poses it from its clip; a cube attached to the tip joint is
drawn on it and follows it; the scene revision does not move; Fox walking through its animator), ragdolls and debris (Fox
falling in Play as a ragdoll and drawn where its bodies lie, posed; debris drawn like its template, and gone on Stop), the
selection outline (a ring all round a selected cube, in orange, with no pixel changed inside its silhouette or away from it, and
the same ring over a wall that hides the cube), a PBR model and the sample's lighting test scene. Captures go to the folder for
inspection.

`Tests/RealInput/milestone5_animation.ps1` drives the Animation panel with real mouse and keyboard input (see
`docs/evidence/m5-animation/`), and `Tests/RealInput/milestone5_ragdoll.ps1` the Ragdoll in Details, Simulate, Ragdoll Selected
and Drop Debris (see `docs/evidence/m5-ragdoll/`). `Tests/RealInput/shooter_acceptance.ps1` plays the shooter: its steps
17 to 22 check Milestone 5's enemies and effects, and winning takes the enemies too. `Tests/RealInput/milestone5_asset_paths.ps1`
(slice 7) selects with a click in the viewport and checks the outline, renames a material to a package name, types the old
name in a Details field and takes it through the redirector, then fixes up redirectors and checks the scene file on disk (see
`docs/evidence/m5-asset-paths/`). `Tests/RealInput/milestone6_scripts.ps1` (Milestone 6, slice 4) makes a script, types it in
the Script Editor, saves it with Ctrl+S, gives it to a target through Details, plays it, saves it again while playing and
broken, follows the error's link to its line, and saves the scene with it (see `docs/evidence/m6-scripts/`).
`Tests/RealInput/milestone6_shooter.ps1` (slice 5) plays the shooter with its scripted vault: the shut gate stops the
player, shooting the west wing's two targets opens it, E at the button inside pays once, then the game is won, restarted
with the gate shut again, and lost; Stop gives back the scene (see `docs/evidence/m6-shooter/`).

`Tests/Performance/milestone5_performance.ps1` measures the Release editor playing the sample and its stress scene
(`stress_test.scene`: ten enemies in view and ten fountains of 500 particles). Each run is `engine_editor --scene <file>
--perf-run <seconds> --perf-log <file>`, which plays the scene with nobody at the controls, then quits and writes the log. It
sends no input, but waits for an idle desktop and records whether it stayed idle, because someone using the machine
changes the numbers. The log holds frame times (mean, p50, p99, maximum, and when frames over 100 ms happened), the CPU's
share of a frame, physics, gameplay, animation and particle times, and what was drawn.

`Tests/Performance/milestone6_performance.ps1` (Milestone 6, slice 6) measures what scripts cost in the Release editor
the same way: the sample, its scripting performance scene (`script_stress.scene`: 500 cubes over the arena, each moved by
`Mover.lua`, 502 scripts with the vault's two) and that scene with the movers' scripts taken out, each played twice. The
log adds the scripts' time per tick and per drawn frame, apart from gameplay, and each drawn frame's CPU time.
`script_benchmark` (built with the tests; a tool that prints times, not a CTest entry) times the same without a window: a
scene snapshot, one tick's moves through the command bus, and 500 instances of five scripts that tell apart the call, the
arithmetic, reading a position, moving and the whole Mover.

Fixtures: `Tests/Assets/editor/` (valid editor test content), `Tests/Assets/invalid/` (negative cases) and
`Tests/Assets/images/` (a JPEG), read in place through `GAMEENGINE_TEST_ASSETS_DIR`. Binary textures are regenerated
with `python -I Tests/Assets/tools/generate_textures.py`. Cases that modify files copy fixtures to
`<build>/Tests/asset_tests_tmp/<case>`. GLB and PNG data are built in memory by the tests.

Results for the current milestone are in `docs/MILESTONE_6_REPORT.md`.

`main` is on the `origin` remote, which has `.github/workflows/ci.yml`, but no hosted CI result has been checked from
this machine.

Not covered:
- Real mouse and keyboard runs are not part of CTest, since they need an idle desktop. Results and what they did not reach
  are in the milestone reports.
- GameNetworkingSockets over real UDP (blocked on protobuf; the backend only compiles).
- Cross-compiler determinism. Determinism tests compare two runs within one binary.
- Symlink-escape rejection in `AssetDatabase` is implemented (canonical-path check) but untested:
  creating symlinks on Windows needs elevated rights.
- The build without cgltf and stb_image that the Linux CI job uses has only been compiled with MSVC.

## Conventions

- Tests use only public headers of the module under test.
- No wall clock in assertions. Time is passed explicitly (`advance(ns)`, `poll(now)`).
- No real sockets in unit tests. Use `LoopbackTransport`, which is deterministic and seedable.
- Network integration tests that use real UDP will carry the `integration` label and bind
  `127.0.0.1` on ephemeral ports.
