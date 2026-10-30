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

`engine_editor --smoke` is not a CTest entry by default, because it needs an interactive desktop. Configure with
`-DENGINE_EDITOR_GPU_TESTS=ON` to register it as `engine.editor.gpu`.

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

## Current suites (146 CTest entries)

| Executable | Labels | Cases | Covers |
|---|---|---|---|
| `physics_tests` | physics, unit | 31 | World lifecycle and limits, config validation, move semantics, Box3D memory release, fixed step and catch-up, free-fall accuracy, bitwise determinism, body/collider lifecycle, stale/foreign handles, input validation, mass, rest/sleep, impulses, kinematic targets, filters, moved/contact/hit/sensor events |
| `physics_scene_tests` | physics, integration | 26 | Real Box3D world against the real `Scene`/`CommandBus`, authored through scene components: entity-to-body mapping, pose publish via a history-free bus, editor revision/undo isolation, feedback-loop guard, edit-mode follow, kinematic targets, parented transforms, unsupported hierarchies, scale rules, delete/undo restore, entities losing their components, resource release, contact and sensor events, collider rebuild, pause/reset, debug geometry, play/stop restore, >256-body publish split, owner-thread enforcement |
| `asset_tests` | assets, unit | 35 | Paths and stable ids, OBJ, TGA, PPM, `.meta`, database catalogue, cache/reload/revision/unload, missing and failed files, scan issues. GLB: geometry, transforms, mirroring, materials, embedded images, warnings, invalid and unsupported files, real models from the dependency tree. PNG and JPEG decoding and rejection. `import_file` naming, ids, and leaving nothing behind on failure |
| `systems_smoke_tests` | integration, smoke | 1 | OBJ asset sizes a collider, physics drives a scene entity, `extract_frame` sees the published pose |
| `editor_dependency_smoke` | editor, smoke | 2 | `gameengine::imgui` renders frames through ImGui's null backend (no window, no GPU); `gameengine::glfw` links and reports its version without `glfwInit` |
| `networking_tests` | networking, unit | 41 | Serialization, loopback transport, connection manager |
| `online_services_tests` | online, unit | 7 | NullOnlineService |
| `engine_core_tests` | none | 1 entry, 23 scenarios | `engine.core.acceptance`: scene, commands, undo, components, serialization of all format versions, malformed input |
| `engine_renderer_tests` | none | 1 entry, 9 scenarios | `engine.renderer.snapshot`: frame extraction, cameras, lights, handle validation, owner thread |
| `engine_editor_tests` | none | 1 entry, 12 groups | `engine.editor.acceptance`: session behavior without a window, including the 13-step acceptance workflow, Play mode, physics and the asset workflow |

The GPU smoke (`engine_editor --smoke`) opens a real window and checks by readback: surface color, scene lights
replacing editor lights, Play and Stop isolation, resize, and the orientation of an imported texture.

Fixtures: `Tests/Assets/editor/` (valid editor test content), `Tests/Assets/invalid/` (negative cases) and
`Tests/Assets/images/` (a JPEG), read in place through `GAMEENGINE_TEST_ASSETS_DIR`. Binary textures are regenerated
with `python -I Tests/Assets/tools/generate_textures.py`. Cases that modify files copy fixtures to
`<build>/Tests/asset_tests_tmp/<case>`. GLB and PNG data are built in memory by the tests.

Verified on 2026-10-30, Windows 10 x64, MSVC 19.44, Ninja, branch `agent/claude-milestone-3`, from fresh binary
directories (`cmake --preset <p> -B build-m3-<p>`):

| Configuration | CTest | Core | Editor | Assets | GPU smoke |
|---|---|---|---|---|---|
| `windows-msvc-release` | 146/146 | 23/23 | 12 groups | 35/35 | pass |
| `windows-msvc-ci` (RelWithDebInfo, `/WX`, strict dependency lock) | 146/146 | 23/23 | 12 groups | 35/35 | pass |
| `windows-msvc-debug`, MinGW GCC, Linux, macOS, mobile | **not run** in M3 | | | | |

Hosted CI has never run, because the repository has no remote.

Not covered:
- Real mouse and keyboard input is not automated: those checks use a local driver and are recorded per slice in
  `docs/MILESTONE_3_REPORT.md`, along with what they did not reach.
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
