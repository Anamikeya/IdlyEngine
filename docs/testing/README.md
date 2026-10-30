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

## Current suites (346 CTest entries)

| Executable | Labels | Cases | Covers |
|---|---|---|---|
| `physics_tests` | physics, unit | 41 | World lifecycle and limits, config validation, move semantics, Box3D memory release, fixed step and catch-up, free-fall accuracy, bitwise determinism, body/collider lifecycle, stale/foreign handles, input validation, mass, rest/sleep, impulses, kinematic targets, filters, moved/contact/hit/sensor events. Milestone 4: raycast hit and miss, ray filters, ignored body and sensors, collision filters, impulse direction and mass response, off-center spin, wake on impulse, static and kinematic refusal, rotation lock |
| `physics_scene_tests` | physics, integration | 32 | Real Box3D world against the real `Scene`/`CommandBus`, authored through scene components: entity-to-body mapping, pose publish via a history-free bus, editor revision/undo isolation, feedback-loop guard, edit-mode follow, kinematic targets, parented transforms, unsupported hierarchies, scale rules, delete/undo restore, entities losing their components, resource release, contact and sensor events, collider rebuild, pause/reset, debug geometry, play/stop restore, >256-body publish split, owner-thread enforcement. Milestone 4: raycast entity mapping and filters, impulses, removed entities, hit events, reset |
| `gameplay_tests` | gameplay, integration | 33 | Input actions, walking, sprint, looking, floors and walls, jump, falling out, fire rate, ammunition, reload, hits, damage, score, impulses and mass, static walls, dynamic and moving targets, impact damage, bonus zones, pickups, win, loss, the clock and pause, determinism. The real sample scene: loads, settles, can be won, lost and restarted, the domino chain, the bonus zone, breaking glass |
| `asset_tests` | assets, unit | 47 | Paths and stable ids, OBJ, TGA, PPM, `.meta`, database catalogue, cache/reload/revision/unload, missing and failed files, scan issues. GLB: geometry, transforms, mirroring, materials, embedded images, warnings, invalid and unsupported files, real models from the dependency tree. PNG and JPEG decoding and rejection. `import_file` naming, ids, and leaving nothing behind on failure. Milestone 4: all five maps and their color spaces, alpha and sides, broken secondary maps, tangents, material text and files, `.hdr`, background import |
| `systems_smoke_tests` | integration, smoke | 1 | OBJ asset sizes a collider, physics drives a scene entity, `extract_frame` sees the published pose |
| `editor_dependency_smoke` | editor, smoke | 2 | `gameengine::imgui` renders frames through ImGui's null backend (no window, no GPU); `gameengine::glfw` links and reports its version without `glfwInit` |
| `editor_ux_tests` | editor, unit | 61 | The editor's foundation without a window: chords, command registry, context routing, input blocks, conflicts, keymap files, the default command table and its generated document, bus and session transactions, history order and jumps, gizmo drags, selection, property tables |
| `editor_viewport_tests` | editor, unit | 47 | The viewport without a window: the camera state machine and every navigation mode, field of view, eased moves, framing, rays, snapping to the floor, vertex snapping, Alt+drag, hiding, view state files, orthographic views, grid and show flags, settings files |
| `editor_panels_tests` | editor, unit | 31 | What the panels stand on, without a window: folders and their undo, lock and hide, copy and paste, grouping, moving assets and fixing references, property and component text, thumbnail decoding, panel shortcuts |
| `networking_tests` | networking, unit | 41 | Serialization, loopback transport, connection manager |
| `online_services_tests` | online, unit | 7 | NullOnlineService |
| `engine_core_tests` | none | 1 entry, 24 scenarios | `engine.core.acceptance`: scene, commands, undo, components, serialization of all format versions, malformed input |
| `engine_renderer_tests` | none | 1 entry, 9 scenarios | `engine.renderer.snapshot`: frame extraction, cameras, lights, handle validation, owner thread |
| `engine_editor_tests` | none | 1 entry, 17 groups | `engine.editor.acceptance`: session behavior without a window, including the 13-step acceptance workflow, Play mode, physics, assets, materials, lighting, background import, resource release and the shooter in Play mode |

The GPU smoke (`engine_editor --smoke`) opens a real window and checks by readback: surface color, scene lights
replacing editor lights, Play and Stop isolation, resize, and the orientation of an imported texture.

`engine_editor --render-test <folder>` opens a real window and compares pixels: base color, sphere winding, map color
space, alpha mask and blend, single and double sides, normal map direction, metallic-roughness, occlusion, emissive, a
missing map, shadows at each quality and off, IBL, intensity, exposure, skybox, sky orientation, spot and point lights, GPU
resource lifecycle, a PBR model and the sample's lighting test scene. Captures go to the folder for inspection.

Fixtures: `Tests/Assets/editor/` (valid editor test content), `Tests/Assets/invalid/` (negative cases) and
`Tests/Assets/images/` (a JPEG), read in place through `GAMEENGINE_TEST_ASSETS_DIR`. Binary textures are regenerated
with `python -I Tests/Assets/tools/generate_textures.py`. Cases that modify files copy fixtures to
`<build>/Tests/asset_tests_tmp/<case>`. GLB and PNG data are built in memory by the tests.

Results for the current milestone are in `docs/MILESTONE_4_REPORT.md`.

Hosted CI has never run, because the repository has no remote.

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
