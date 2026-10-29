# Testing

## Running

```powershell
./cmake/scripts/dev-shell.ps1 ctest --preset windows-msvc-debug          # everything
./cmake/scripts/dev-shell.ps1 ctest --preset windows-msvc-debug -L physics
./cmake/scripts/dev-shell.ps1 ctest --preset windows-msvc-debug -L smoke          # headless smoke tests
./cmake/scripts/dev-shell.ps1 ctest --preset windows-msvc-debug -R connection_manager
build/windows-msvc-debug/bin/physics_tests.exe --list
build/windows-msvc-debug/bin/physics_tests.exe --run free_fall_matches_gravity
```

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

## Current suites (131 CTest entries)

| Executable | Labels | Cases | Covers |
|---|---|---|---|
| `physics_tests` | physics, unit | 31 | World lifecycle and limits, config validation, move semantics, Box3D memory release, fixed step and catch-up, free-fall accuracy, bitwise determinism, body/collider lifecycle, stale/foreign handles, input validation, mass, rest/sleep, impulses, kinematic targets, filters, moved/contact/hit/sensor events |
| `physics_scene_tests` | physics, integration | 24 | Real Box3D world against Codex's real `Scene`/`CommandBus` (no mocks): entity-to-body mapping, pose publish via a history-free bus, editor revision/undo isolation, feedback-loop guard, edit-mode follow, kinematic targets, parented local transforms, unsupported hierarchies, scale baking, non-uniform scale rules, delete/undo restore, purge, resource release, contact and sensor events as EntityIds, authoring validation, collider rebuild, pause/reset, debug geometry per shape, play/stop restore keeping editor undo valid, >256-body publish split, owner-thread enforcement, driven by `engine::Application` |
| `asset_tests` | assets, unit | 23 | Path normalization and rejection, stable ids (pinned FNV-1a value), type detection, OBJ (winding, UV flip, generated normals, fan triangulation, scale, strict errors with line numbers, limits, CRLF), TGA (uncompressed bottom-left, RLE top-left, grayscale, malformed headers, RLE overrun, dimension limit), PPM, `.meta` strictness and round trip, database catalogue, cache/reload/revision/unload, type mismatch, missing-file detection and recovery, failed import recovery, scan issues (id collision, bad sidecar, orphan sidecar, unsupported file), rescan cache retention, case-only collisions |
| `systems_smoke_tests` | integration, smoke | 1 | OBJ asset sizes a collider, physics drives a scene entity, Codex's `extract_frame` sees the published pose |
| `editor_dependency_smoke` | editor, smoke | 2 | `gameengine::imgui` renders frames through ImGui's null backend (no window, no GPU); `gameengine::glfw` links and reports its version without `glfwInit` |
| `networking_tests` | networking, unit | 41 | Serialization, loopback transport, connection manager (unchanged from M1) |
| `online_services_tests` | online, unit | 7 | NullOnlineService (unchanged from M1) |
| `engine_core_tests` (Codex) | none | 1 entry, 19 scenarios | `engine.core.acceptance` |
| `engine_renderer_tests` (Codex) | none | 1 entry, 8 scenarios | `engine.renderer.snapshot` |

Fixtures: `Tests/Assets/editor/` (valid editor test content) and `Tests/Assets/invalid/` (negative
cases), read in place through `GAMEENGINE_TEST_ASSETS_DIR`. Binary textures are regenerated with
`python -I Tests/Assets/tools/generate_textures.py`. Cases that modify files copy fixtures to
`<build>/Tests/asset_tests_tmp/<case>`.

Verified on 2026-10-28, Windows 10 x64, MSVC 19.44.35228, Ninja, branch `agent/claude-systems-m2`:

| Configuration | Result |
|---|---|
| `windows-msvc-debug` | 131/131 |
| `windows-msvc-release` | 131/131 |
| `windows-msvc-ci` (RelWithDebInfo, `/WX`, strict dependency lock) | 131/131, no warnings in engine targets |
| MinGW GCC, Linux, macOS, mobile | **not run** in M2 |

These runs used binary directories `build/m2-<preset>` (`cmake --preset <p> -B build/m2-<p>`) and the
manual MSVC environment described in `docs/build/building.md`, because `vcvars64.bat` stalled on the
reference machine that day.

Not covered yet:
- No GUI test has run. Neither smoke test opens a window, creates a GPU context or renders pixels.
  There is no editor executable yet.
- GameNetworkingSockets over real UDP (blocked on protobuf; the backend only compiles).
- Cross-compiler determinism. Determinism tests compare two runs within one binary.
- Symlink-escape rejection in `AssetDatabase` is implemented (canonical-path check) but untested:
  creating symlinks on Windows needs elevated rights.

## Conventions

- Tests use only public headers of the module under test.
- No wall clock in assertions. Time is passed explicitly (`advance(ns)`, `poll(now)`).
- No real sockets in unit tests. Use `LoopbackTransport`, which is deterministic and seedable.
- Network integration tests that use real UDP will carry the `integration` label and bind
  `127.0.0.1` on ephemeral ports.
