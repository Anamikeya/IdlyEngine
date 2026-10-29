# Milestone 2 integration audit

Branch `integration/milestone-2`, worktree `D:\AI\GameEngine-Integration`. Date 2026-10-29.
Auditor: Claude (single agent). Toolchain: MSVC 19.44.35228 x64 (14.44.35207), CMake 3.31.2, Ninja 1.12.0
(Strawberry, first on `PATH`), Windows 10 Pro 19045, NVIDIA RTX 3080 (driver 576.28, OpenGL 4.5).

**Verdict: CONDITIONAL PASS** (see section 10).

## 1. Git integration status

| Check | Result |
|---|---|
| HEAD | `73435db` *Merge Claude Milestone 2 physics and asset systems* (parents `a0933b5`, `a143cb0`) |
| Codex M2 (`agent/codex-editor-m2` = `064eb68`) | ancestor of HEAD via `a0933b5` (parents `d876e42`, `064eb68`) |
| Claude M2 (`agent/claude-systems-m2` = `a143cb0`) | ancestor of HEAD |
| M1 baseline `d876e42` | ancestor of HEAD |
| Merge state | no `MERGE_HEAD`/rebase/cherry-pick in progress; no conflict markers (`git grep`, `git diff --check`). A stale `.MERGE_MSG.swp` (vim swap, 12:26) sits in the worktree's git dir; harmless |
| Uncommitted work found at start | `Engine/Editor/CMakeLists.txt`, `docs/architecture/005-editor-m2.md`, `docs/build/building.md`: an earlier in-progress fix for exactly this build problem. It was kept, evaluated, corrected (section 2, items 1 and 5) and committed with this audit, not overwritten. Untracked `audit.txt`, `build-log.txt` (user files) are left uncommitted |
| `main` | at `a0933b5`: contains the Codex M2 merge but **not** the Claude M2 merge. Not changed by this audit |
| Other worktrees | inspected read-only; Codex and Claude worktrees clean; main worktree has an untracked `build-msvc/` |

Both Milestone 2 contributions are fully integrated on this branch.

## 2. Confirmed root causes

The errors listed in the brief were environment and build-system problems, not C++ defects. No engine C++ source
needed changing.

| # | Symptom | Root cause | Evidence | Severity | Status |
|---|---|---|---|---|---|
| 1 | Configure of the committed HEAD never finishes; `add_library cannot create target ... already exists` repeated | Codex's `Engine/Editor/CMakeLists.txt` was written as a standalone entry point that does `add_subdirectory(../.. foundation)`. Claude's root `CMakeLists.txt` adds `Engine/Editor` automatically, so each pass re-adds the root: unbounded recursion | `git archive HEAD` configured into a scratch dir: 63,802 log lines in under 5 min, nested `Engine/Editor/foundation/Engine/Editor/foundation/...`. The user's own `build-msvc` attempt reached 498 nested `foundation` dirs | P0 | Fixed (the uncommitted fix guarded the standalone branch; its guard was then corrected, see #5) |
| 2 | `could not load cache` / `ninja: error: loading 'build.ninja'` | The configure step for that directory never completed (#1, or no MSVC env). Ninja has nothing to load | `build-msvc/CMakeCache.txt` exists but no `build.ninja` | n/a | Consequence of #1/#3 |
| 3 | CMake selected Strawberry GCC | Configured outside a VS developer environment and without `-DCMAKE_C(XX)_COMPILER=cl`; Strawberry's `gcc`/`c++` are the first compilers on `PATH` | `build/CMakeCache.txt`: `CMAKE_CXX_COMPILER:FILEPATH=C:/Strawberry/c/bin/c++.exe` | n/a (environment) | Documented in `docs/build/building.md` |
| 4 | "Editor CMake configuration requires Windows/MSVC" | Follows from #3: the editor rejects non-MSVC compilers by design | `Engine/Editor/CMakeLists.txt` | n/a | Non-MSVC now skips the editor with a status message instead of failing |
| 5 | Any **re-configure** of the standalone editor build (`cmake -S Engine/Editor`) drops to a bare, compiler-less project: `Engine/Editor skipped`, `The detected version of Ninja () is less than ... (1.3)` | The uncommitted fix guarded the standalone branch with `if(NOT DEFINED GameEngine_SOURCE_DIR)`, but `project(GameEngine)` *caches* that variable, so on the second run the branch (and `project()`) is skipped | `build-claude-audit-standalone`: first configure exit 0, second exit 1 | P1 | Fixed: guard is `CMAKE_CURRENT_BINARY_DIR STREQUAL CMAKE_BINARY_DIR` (top level only; the nested pass has a different binary dir) |
| 6 | Any **re-configure** of the combined build fails: `file RELATIVE_PATH must be passed a full path to the directory: install` (Box3D `src/CMakeLists.txt:278-281`) | Filament's `third_party/glslang/tnt/CMakeLists.txt:12-14` force-caches `CMAKE_INSTALL_PREFIX=install` (relative) when the prefix was defaulted. The first configure succeeds; every later one (including the one Ninja runs automatically after a CMakeLists edit) fails | `build-claude-audit-msvc/CMakeCache.txt:239` `CMAKE_INSTALL_PREFIX:STRING=install`; `RECONFIGURE_EXIT=1` | P1 | Fixed: `set(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT OFF)` in the editor directory scope, before Filament |
| 7 | Debug link mismatch against Filament (`_ITERATOR_DEBUG_LEVEL`) | Filament's `FILAMENT_SHORTEN_MSVC_COMPILATION` (default ON) adds `/D_ITERATOR_DEBUG_LEVEL=0` to Debug; engine Debug libraries use level 2 | Found by the earlier in-progress fix; this audit did not reproduce the failure with ON. With OFF, the Debug tree links (882/882) | P0 for Debug | Fixed (kept from the uncommitted change) |
| 8 | Standalone `ctest --test-dir out/editor` would find no tests | The uncommitted fix removed `enable_testing()` from the standalone top level; CTest needs `CTestTestfile.cmake` in the top binary dir | `ctest -N` in the standalone dir reports `Total Tests: 10` after the fix | P2 | Fixed |
| 9 | Windows CI job would fail to configure | `.github/workflows/ci.yml` does not fetch `filament`, which `Engine/Editor` (on by default) now requires; 60 min timeout is short for a from-source Filament build | `ci.yml` fetch list | P1 (CI only) | Fixed, **not run on GitHub** |
| 10 | Duplicate GLFW target | The original standalone editor also `add_subdirectory`'d GLFW, which the root already provides as `glfw` | Fixed by the uncommitted change | P0 (part of #1) | Fixed |

## 3. Files changed and reasons

| File | Change | Reason |
|---|---|---|
| `Engine/Editor/CMakeLists.txt` | Standalone branch only at the top level (`CMAKE_CURRENT_BINARY_DIR STREQUAL CMAKE_BINARY_DIR`), with `project()` + `enable_testing()`; combined path no longer re-adds the root or GLFW; non-MSVC skips with a status message; `FILAMENT_SHORTEN_MSVC_COMPILATION OFF`; `CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT OFF` before Filament; `gameengine_verify_dependency(filament)` | #1, #4, #5, #6, #7, #8, #10; Filament now checked against `DependencyLock.cmake` like every other dependency |
| `.github/workflows/ci.yml` | Fetch `filament` in the Windows job; timeout 60 → 120 min | #9 |
| `docs/build/building.md` | MSYSTEM note (kept from the earlier change); new "Editor build (Filament) requirements" section | #2, #3, #6, cross-drive limitation |
| `docs/architecture/005-editor-m2.md` | Executable path `out/editor/bin` (kept from the earlier change) | The root sets `CMAKE_RUNTIME_OUTPUT_DIRECTORY` to `bin` |
| `.gitignore` | `/build-*/` | `build-msvc/`, `build-claude-audit-*` showed up as untracked |
| `docs/MILESTONE_2_INTEGRATION_AUDIT.md` | This report | |

No engine C++ source, test, or shared dependency (`D:\AI\GameEngine\resources`) was modified. No test was removed,
weakened or skipped.

## 4. Configuration result

Environment: `vcvars64.bat` completed within a 90 s watchdog this time (it stalled on 2026-10-28);
`where cl` → `...\14.44.35207\bin\Hostx64\x64\cl.exe`, `Version 19.44.35228 for x64`. `MSYSTEM` was empty.

```cmd
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build-claude-audit-msvc -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DGAMEENGINE_RESOURCES_DIR=D:/AI/GameEngine/resources
```

```
-- The CXX compiler identification is MSVC 19.44.35228.0
-- Dependency box3d: f36d061ba26998c87cb7dc593a4065aaa03fb7eb (locked)
-- Dependency glfw: 92dcf4ce74f2e2554a98fea09be7c705c17daa5a (locked)
-- Dependency imgui: d88f238d1d363ef387382b2e29ad12cf96f826ac (locked)
-- GameEngine 0.1.0: physics=ON gns=OFF editor=ON tests=ON
-- Configuring done (5.2s)
-- Generating done (1.4s)
CONFIGURE_EXIT=0
```

Re-configure checks (all after the fixes):

| Directory | Entry point | 1st | 2nd | 3rd |
|---|---|---|---|---|
| `build-claude-audit-recfg` (fresh) | root, Debug | 0 | 0 | 0 (with `GAMEENGINE_STRICT_DEPENDENCY_LOCK=ON`; `Dependency filament: 4e2df5e... (locked)`) |
| `build-claude-audit-standalone2` (fresh) | `Engine/Editor`, Release | 0 | 0 | 0 |
| `build-claude-audit-msvc` | root, Debug | 0 | 1 before fix #6 → 0 after (one-time `-DCMAKE_INSTALL_PREFIX="C:/Program Files (x86)/GameEngine"` to repair the cache poisoned by its first run) | 0 (Ninja's automatic `Re-running CMake...`) |

## 5. Compilation result

```cmd
cmake --build build-claude-audit-msvc --parallel 1
```

```
==== Thu 10/29/2026 13:20:40.64 build start
[882/882] Linking CXX executable bin\editor_dependency_smoke.exe
BUILD_EXIT=0
==== Thu 10/29/2026 13:44:54.96 build end
```

882/882 serial steps in 24 min 14 s, including Filament (OpenGL only), `matc`, the three editor materials, GLFW,
ImGui, Box3D, Flecs and all engine targets. 0 errors. 383 warnings, all in Filament/third-party code; **none in
`Engine/` or `Tests/`** (the engine targets that use `/W4 /WX` compiled clean). Subsequent incremental builds after the
CMake fixes: `ninja: no work to do` (the fixes do not change any compiled output).

Standalone Release (`cmake -S Engine/Editor`, `--parallel 8`): 883 steps, exit 0; details in section 7.

## 6. Automated test results

```cmd
ctest --test-dir build-claude-audit-msvc --output-on-failure
```

| Run | Total | Passed | Failed |
|---|---|---|---|
| Debug, default configuration | 132 | 132 | 0 |
| Debug, `-DENGINE_EDITOR_GPU_TESTS=ON` (adds the visible GPU smoke run) | 133 | 133 | 0 |
| Same, final CMake state (after Ninja's automatic re-configure) | 133 | 133 | 0 |
| Standalone Release (`cmake -S Engine/Editor`), GPU test off | 132 | 132 | 0 |

Breakdown: `engine.core.acceptance`, `engine.renderer.snapshot`, `engine.editor.acceptance`, `engine.editor.gpu`
(3.4–3.7 s), `physics_tests` 31, `physics_scene_tests` 24, `networking_tests` 41, `online_services_tests` 7,
`asset_tests` 23, `systems_smoke_tests` 1, `editor_dependency_smoke` 2.

```
100% tests passed, 0 tests failed out of 133
assets;unit = 0.20 sec*proc (23 tests)    networking;unit = 0.27 sec*proc (41 tests)
physics;integration = 0.67 sec*proc (24)  physics;unit = 0.40 sec*proc (31 tests)
```

Not covered: GameNetworkingSockets (`gns=OFF`, protobuf unavailable; deferred by the user on 2026-10-27), Linux,
sanitizers, GitHub-hosted CI.

## 7. Editor launch and rendering verification

Run on the local interactive desktop (RTX 3080). The Debug executable was used.

| Check | Method | Result |
|---|---|---|
| Window appears | Normal launch; process alive after 8 s with `MainWindowTitle` = `GameEngine \| Milestone 2 Editor`, 130 MB working set | Verified |
| Filament initializes | stdout: `FEngine resolved backend: OpenGL`, `[NVIDIA GeForce RTX 3080/PCIe/SSE2], [4.5.0 NVIDIA 576.28]` | Verified |
| Visible frame rendered | `engine_editor.exe --smoke` → `GPU smoke passed: 92 frames; transform, undo/redo, save/load, resize and capture.` Exit 0. The 1280×800 readback (`editor-smoke.ppm`) shows the lit blue cube at the edited transform (1.5, 0.75, 0; scale 1.5) on the reference grid, with the ImGuizmo translate gizmo | Verified via GPU readback |
| ImGui + docking | Same capture: Toolbar, Scene Hierarchy, Inspector, 3D Viewport, Asset Browser, Console docked in the default layout; `imgui.ini` written with a `DockSpace` split tree after a normal run | Verified |
| Resizing | Smoke: `glfwSetWindowSize` 1440×900 → 1280×800. Interactive: OS `SetWindowPos` to 800×500, 1700×1000, 320×240, 1440×900 | No crash, process alive |
| Minimize / restore | Smoke: immediate iconify/restore. Interactive: `ShowWindow` minimize for 3 s, then restore | No crash, process alive |
| Shutdown | `WM_CLOSE` (`CloseMainWindow`) → exit 0, `Editor stopped cleanly.`, empty stderr. Smoke exit 0 | Verified |
| Scene persistence | `smoke.scene`: `ENGINE_SCENE 1 2 1` / `ENTITY 1 0 "Cube" 1.5 0.75 0 0 0 0 1 1.5 1.5 1.5` | Verified |

Not verified (no input automation): real mouse-driven orbit/pan/zoom, click picking and gizmo dragging, keyboard
shortcuts, dragging dock splitters, DPI changes, extended minimized periods. Picking, camera and world-edit maths are
covered by `engine.editor.acceptance` on the CPU side. Visual verification used the GPU readback, not a desktop
screenshot.

### Standalone Release entry point

The documented Codex path, run from a fresh directory with the corrected CMake:

```cmd
cmake -S Engine/Editor -B build-claude-audit-standalone2 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DGAMEENGINE_RESOURCES_DIR=D:/AI/GameEngine/resources
cmake --build build-claude-audit-standalone2 --parallel 8      &:: STANDALONE_BUILD_EXIT=0 (883 steps)
ctest --test-dir build-claude-audit-standalone2 --output-on-failure   &:: 100% tests passed, 0 tests failed out of 132
cd build-claude-audit-standalone2\bin && .\engine_editor.exe --smoke
```

```
FEngine resolved backend: OpenGL
GPU smoke passed: 91 frames; transform, undo/redo, save/load, resize and capture.
Editor stopped cleanly.
STANDALONE_SMOKE_EXIT=0
```

Build and CTest took 5 min 7 s together (13:55:33–14:00:40). Use an explicit `.\` prefix in `cmd` when
`NoDefaultCurrentDirectoryInExePath` is set, as it is in the agent shell; with that variable set, a bare
`engine_editor.exe` gives exit 9009.

## 8. Remaining issues by priority

P0: none open.

P1: none open within M2 scope. Fixes #6 and #9 change CI/infrastructure behaviour; #9 is unverified until the workflow
runs on GitHub (Filament fetch size and build time on hosted runners are unknown).

P2:
- **Cross-drive build dirs fail.** Filament joins `IMPORT_EXECUTABLES_DIR` to its own source root, so the build dir
  must be on the same drive as `GAMEENGINE_RESOURCES_DIR` (reproduced: a C: build dir against D: resources fails to
  generate `ImportExecutables-Debug.cmake`). Documented; a real fix needs Filament-side support.
- **Build dirs configured before this fix** keep the poisoned `CMAKE_INSTALL_PREFIX=install` and need one configure
  with an absolute prefix (documented).
- **Two ImGui builds.** `gameengine::imgui` (root, used by `editor_dependency_smoke`) and `editor_imgui` (editor,
  adds ImGuizmo) compile the same sources. They are never linked into one binary, so no ODR clash, but the headless
  smoke test does not exercise the library the editor ships with.
- **GUI interaction not automated** (section 7).
- **The combined `windows-msvc-release`/`windows-msvc-ci` presets were not built** in this audit. Release was
  built, tested and smoke-run through the standalone entry point, which compiles the same root tree.
- **Orphan process.** `cmake -S . -B build-msvc ...` (PID 47348, started 12:40, 1 CPU-second) is still alive from the
  user's earlier attempt, idle after the recursion in #1. It was not killed. `build-msvc/` (498 nested dirs) and the old
  `build/` (Strawberry GCC) can be deleted once that process is gone.
- GNS remains compile-check only (user decision); miniaudio and Tracy are pinned in the lock but not integrated (not in
  M2 scope).

P3:
- `ImGuiRenderer::update` destroys and rebuilds the UI renderable each frame; `resize_viewport` calls `flushAndWait`
  on every size change (a stall during splitter drags). Fine at editor scale.
- Strawberry Perl's `ninja`/`cmake`/`gcc` precede the VS ones on `PATH`; it works (Ninja 1.12.0) but is fragile.
- Inspector shows `-0.000` for some Euler components.

## 9. Architectural concerns

- **Two entry points for one build.** The editor can be configured as the top level (`cmake -S Engine/Editor`) or as a
  root subdirectory. That dual role caused #1 and #5. Recommend making the root the only entry point (presets already
  do it) and keeping the standalone branch only as a thin compatibility shim, or removing it in M3.
- **Editor ↔ Claude systems are not connected yet.** `engine_editor` links `engine::renderer` and the session only;
  `engine::physics_scene` and `engine::assets` are built and tested (including `systems_smoke_tests`) but not used by
  the editor. That matches the M2 scope in `docs/architecture/005-editor-m2.md`. Open contracts in
  `docs/integration/m2-contracts.md` (C-2a physics persistence/undo, C-5a renderer resource registry, C-5b mesh
  conventions) block that wiring.
- **The renderer accepts built-in handles only.** `DesktopRenderer::render` throws for any mesh/material other than
  `{1,1}`. That is correct for M2 but must become the C-5a registry before imported assets can be drawn.
- **Boundaries hold.** Filament types stay inside `Engine/Renderer/src/desktop`; the UI gets an opaque `uint64_t`
  texture token; all scene edits go through the typed `CommandBus` (validated, owner-thread, capability-checked,
  revisioned, undoable). Shutdown order is correct: renderer (Filament engine, swap chain) before the GLFW window and
  ImGui context.
- **Dependency lock.** Filament is now verified like the others. `GAMEENGINE_STRICT_DEPENDENCY_LOCK=ON` configures
  cleanly with all dependencies at their locked commits.

## 10. Final readiness verdict

**CONDITIONAL PASS.** The clean MSVC Debug configure and serial build succeed; all 133 tests pass, including the
visible GPU smoke test; and the editor launches, renders Filament frames inside the ImGui docking layout, survives
OS resize and minimize/restore, and shuts down cleanly. The blocking build defects (#1, #7, #10) and the
re-configure defects (#5, #6) are fixed and verified.

Non-blocking limitations: CI changes are unverified on GitHub, the build dir must share a drive with the resources,
interactive input is not automated, and the `windows-msvc-ci` preset (`/WX`, RelWithDebInfo) was not built in this
audit. Release was built and verified through the standalone entry point: 132/132 tests and a 91-frame GPU smoke run.

Next reproducible actions: run `.github/workflows/ci.yml` on GitHub; build `--preset windows-msvc-release` and
`windows-msvc-ci` from a fresh directory; once PID 47348 is gone, delete `build-msvc/` and `build/`.
