# Milestone 2 integration report: Claude systems

Branch `agent/claude-systems-m2`, from `main` at `d876e42` (the M1 integration, verified as the tip
of `main` before branching). Date 2026-10-28. Not merged anywhere.
Codex's worktree had no M2 commits at audit time (clean, at `d876e42`), so everything here is built
against committed `main` APIs only. Contracts: [m2-contracts.md](m2-contracts.md).
Panel examples: [m2-editor-integration.md](m2-editor-integration.md).

## 1. Files changed

New, Claude-owned:

| Path | Purpose |
|---|---|
| `Engine/Physics/include/engine/physics/scene_physics.h`, `src/scene_physics.cpp` | `ScenePhysics`: ECS/scene ↔ Box3D bridge plus editor API |
| `Engine/Assets/` (`CMakeLists.txt`, `asset_types.h`, `importers.h`, `asset_database.h`, `src/*.cpp`) | CPU asset pipeline (ownership assigned to Claude by the user, CPU side only) |
| `Tests/Physics/scene_physics_tests.cpp` | 24 physics↔scene integration cases |
| `Tests/Assets/asset_tests.cpp`, `Tests/Assets/editor/**`, `Tests/Assets/invalid/**`, `Tests/Assets/tools/generate_textures.py`, `Tests/Assets/.gitattributes` | Asset tests, editor test content, fixture generator |
| `Tests/Integration/systems_smoke_tests.cpp` | Cross-module headless smoke test |
| `Tests/Editor/editor_dependency_smoke.cpp` | Headless ImGui/GLFW smoke test |
| `cmake/GameEngineEditorDependencies.cmake` | `gameengine::glfw`, `gameengine::imgui` |
| `docs/integration/m2-contracts.md`, `m2-editor-integration.md`, `m2-integration-report.md` | Contracts, examples, this report |

Modified, Claude-owned: `Engine/Physics/CMakeLists.txt` (new `engine::physics_scene` target),
`Engine/Physics/include/engine/physics/physics_world.h` and `src/physics_world.cpp` (public
`is_valid(const ColliderDesc&)`, no behaviour change), `Tests/CMakeLists.txt`,
`.github/workflows/ci.yml`, `docs/testing/README.md`, `docs/build/building.md`,
`docs/build/dependencies.md`.

Shared files, listed for coordination:
- **Root `CMakeLists.txt`**: new option `GAMEENGINE_BUILD_EDITOR` (default ON); includes
  `GameEngineEditorDependencies` when it is on; adds `Engine/Assets` when `engine::core` exists;
  adds `Engine/Editor` when its `CMakeLists.txt` exists; the status line reports `editor=`.
- **`.gitignore`**: un-ignores `Tests/Assets/**/*.obj`. The global `*.obj` rule would otherwise have
  silently dropped the OBJ fixtures.

**No Codex-owned file was modified** (`Engine/Core`, `ECS`, `Scene`, `Commands`, `Renderer`,
`docs/architecture`). The shared resources directory was not modified.

## 2. Interfaces exposed to Codex

- `engine::physics_scene` → `engine/physics/scene_physics.h`
  - `RigidBody`, `Collider` (= `ColliderDesc`) authoring values keyed by `EntityId`
  - `ScenePhysics::create`, `fixed_update(delta, scene, bus, ctx)`, `sync`, `step`, `publish`,
    `pending_transforms`, `mark_published`
  - `set_simulation_enabled`, `reset_simulation`
  - `set_rigid_body`, `remove_rigid_body`, `add_collider`, `set_collider`, `remove_collider`,
    `colliders`, `authored_entities`, `purge_detached`
  - `body_state`, `events` (`CollisionEvent` with EntityIds), `diagnostics`, `debug_geometry`,
    `default_color`
- `engine::assets` → `engine/assets/asset_database.h`, `importers.h`, `asset_types.h`
  - `AssetId`, `normalize_asset_path`, `asset_id_from_path`, `AssetMetadata`, `ImportSettings`,
    `ImportLimits`
  - `MeshData`/`MeshVertex`/`Bounds`, `TextureData` (RGBA8)
  - `import_obj`, `import_tga`, `import_ppm`, `parse_meta`, `write_meta` (pure functions over bytes)
  - `AssetDatabase`: `open`, `scan`, `issues`, `assets`, `find`, `load_mesh`, `load_texture`,
    `validate_all`, `state`, `find_missing`, `reload`, `unload_unused`
- Build: `gameengine::glfw`, `gameengine::imgui` (core plus GLFW platform backend, no renderer
  backend), and an automatic `add_subdirectory(Engine/Editor)`.

## 3. Required shared changes / decisions for Codex

| ID | Ask | Blocking? |
|---|---|---|
| C-1a | Confirm that two `CommandBus` instances on one `Scene` (editor with history, runtime with `history_limit 0`) is supported. Otherwise provide a pose-stream entry point. | No. It works and is tested today; it needs sign-off |
| C-2a | Persistence and undo for physics authoring: either scene format + `physics.*` commands, or a Claude sidecar keyed by `EntityId` | Yes, before saving physics in levels |
| C-4a | Debug-line route: Filament line renderable or ImGui overlay | No |
| C-5a | Renderer resource registry keyed by `(AssetId, revision)` that holds the `shared_ptr` CPU data | Yes, before drawing imported meshes |
| C-5b | Confirm mesh conventions: CCW front faces, top-left UV origin, interleaved `MeshVertex` | Yes, for the Filament adapter |

## 4. Dependencies and versions

No new third-party dependency. Newly **built** from the existing locked checkouts:

| Dependency | Commit | Version | How |
|---|---|---|---|
| GLFW | `92dcf4ce74f2e2554a98fea09be7c705c17daa5a` | 3.5.1-1 (3.6 dev) | `add_subdirectory`, static, `/MD`, `EXCLUDE_FROM_ALL` |
| Dear ImGui | `d88f238d1d363ef387382b2e29ad12cf96f826ac` | 1.93.1 WIP docking | Engine-defined static library |

Unchanged: Box3D `f36d061b` (v0.1.0-41), Flecs `3988dee7` (Codex), GLM/spdlog (Codex headers).
Configure verified both new checkouts against `cmake/DependencyLock.cmake` ("locked"). Filament is
**not** built in M2. `stb_image`/`cgltf` exist only under `filament/third_party` and are not used.

Toolchain: MSVC 19.44.35228 (14.44.35207), Windows SDK 10.0.26100.0, CMake from
`C:/Program Files/CMake`, Ninja from Strawberry Perl.

## 5. Build commands

Normal path, PowerShell:

```powershell
$env:GAMEENGINE_RESOURCES_DIR = 'D:/AI/GameEngine/resources'
./cmake/scripts/dev-shell.ps1 cmake --preset windows-msvc-debug      # or -release / -ci
./cmake/scripts/dev-shell.ps1 cmake --build --preset windows-msvc-debug
```

What was actually run for this report: Git Bash with the manual MSVC environment from
`docs/build/building.md` ("When vcvars64.bat stalls"):

```bash
cmake --preset windows-msvc-<debug|release|ci> -B build/m2-<debug|release|ci>
cmake --build build/m2-<...> --parallel 6
```

## 6. Test commands

```bash
ctest --test-dir build/m2-debug --output-on-failure            # all 131
ctest --test-dir build/m2-debug -L integration                 # physics↔scene + cross-module smoke
ctest --test-dir build/m2-debug -L "assets|smoke|editor"
build/m2-debug/bin/physics_scene_tests.exe --list
```

## 7. Passing and failing tests

| Configuration | Total | Passed | Failed |
|---|---|---|---|
| Debug (`windows-msvc-debug`) | 131 | 131 | 0 |
| Release (`windows-msvc-release`) | 131 | 131 | 0 |
| CI (`windows-msvc-ci`: RelWithDebInfo, `/WX`, strict lock) | 131 | 131 | 0 |

The 131 entries are the M1 baseline (81, which was re-run first and passed 81/81) plus:
- 24 `physics_scene_tests`
- 23 `asset_tests`
- 1 `systems_smoke_tests`
- 2 `editor_dependency_smoke`

**Not run:** GUI tests (none exist; no window, GPU or editor executable), MinGW GCC, Linux, macOS,
mobile, sanitizers, GitHub-hosted CI. The `.github/workflows/ci.yml` changes are authored but have
not executed on GitHub. Mocks: none. Every physics test uses real Box3D plus Codex's real
`Scene`/`CommandBus`. The asset tests use real files.

## 8. Known limitations

Physics:
- Pose publishing goes through `CommandBus`: each transaction snapshots the scene (O(n log n)). This
  is fine for editor-scale scenes; high body counts need C-1a.
- Physics authoring is neither saved in `SceneSnapshot` nor undoable (C-2a). Delete followed by undo
  *is* handled: detached authoring data is kept.
- Some setups are rejected with an issue rather than simulated:
  - dynamic bodies under another dynamic body;
  - dynamic bodies under a non-uniformly scaled or sheared parent;
  - spheres, capsules and rotated boxes under non-uniform world scale.
- Single shape types only (sphere, box, capsule). No mesh/hull colliders, joints, raycast API or
  per-collider debug colour.
- Kinematic bodies end within about 1e-3 m of their scene target, because velocity is cleared after
  the target tick. The scene stays authoritative.
- Determinism is verified within one binary only.
- A rejected body is re-evaluated when its own authoring data or scene world matrix changes. If only an
  ancestor's authoring changes (e.g. the parent stops being dynamic), the child is not rebuilt until
  `reset_simulation` or its next transform change.

Assets:
- OBJ only: no materials (`usemtl`/`mtllib` ignored), no lines/points/curves. Polygons are
  fan-triangulated on the assumption that they are convex. Normals are generated area-weighted for
  the whole mesh when any corner lacks them.
- Textures: TGA (types 2, 3, 10; 8/24/32 bpp; no colour maps, no right-to-left) and binary PPM
  (maxval 255). No PNG/JPEG, mipmaps or compression.
- No file watching: the host calls `find_missing`/`reload`. No asynchronous loading, not thread
  safe. Content changes are not detected by `scan` (call `reload`).
- Case-only path collisions are only detectable on case-sensitive filesystems. The symlink-escape
  check is untested.

Build/CI:
- `gameengine::imgui` has no renderer backend by design. Filament is not built and its CRT
  alignment is not attempted.
- The editor targets are disabled in the Linux CI job; GLFW on Linux is unverified.
- `vcvars64.bat` stalled on the reference machine during this milestone (cause unknown). Some
  orphaned `ninja`/`cmake`/`link` processes from an interrupted build in `build/windows-msvc-debug`
  were still running at the end of the session and were left alone.
