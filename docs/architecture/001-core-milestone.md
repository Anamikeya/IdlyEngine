# Core milestone and ownership contract
Date: 2026-10-27. Branch: agent/codex-core. Baseline: 87ef77a.

## Audit
The baseline contains only .gitignore, no engine, shared interfaces, or root CMake.
.gitignore contains literal PowerShell wrapper lines around its patterns; fixing it is
left to build-infrastructure ownership. No shared dependency is modified.
VS 2022 Community (MSVC 14.44 available), Windows SDK 10.0.26100.0, CMake 3.31.2,
Ninja 1.12.0 and GCC 13.2.0 are installed. Use MSVC for the milestone.
Existing Git global safe.directory warnings are unrelated to this checkout; no global
Git configuration is changed.

| Source | Checked-out revision | Observed version |
|---|---|---|
| Flecs | 3988dee75 | v4.1.6-692; header 4.1.6 |
| Filament | 4e2df5e24 | rc/1.80.0-cut-6 |
| ImGui | d88f238d1 | docking branch; header 1.93.1 WIP |
| GLFW | 92dcf4ce | 3.5.1-1 (3.6 development) |
| GLM | 6f14f479 | header 1.1.0 |
| spdlog | 6d9805df | header 1.17.0 |
| Box3D | f36d061 | v0.1.0-41 |
| ENet | 5a9c537 | v1.3.18-17 |
| GameNetworkingSockets | d534c19 | v1.6.1 |
| miniaudio | 9634bedb | 0.11.25 |
| Tracy | 1f4c5357 | v0.14.1-262 |
| Ninja sources | 3ccb227 | development checkout |
| vcpkg | 5363acb796 | 2026.10.16-184 |
| glTF-Transform | 8e2a94a2 | v4.5.1-6; JavaScript tooling, not a C++ runtime |

Flecs, Filament, ImGui, GLM and spdlog had clean tracked worktrees at audit time.
Versions above are actual source observations, not release guarantees. Pin exact
commits when Claude builds the shared dependency lock and CI.

## Smallest independently buildable milestone
A headless static library, sample application and automated acceptance executable.
Configure Engine/Core directly; no root/shared CMake changes. The owned module CMake
can also be included by Claude via add_subdirectory(Engine/Core).
No window or GPU is required. A native Flecs world stores StableId, Name,
Transform (local), Parent and WorldTransform components. Commands validate complete
batches before modifying the live world; history stores per-entity deltas.
The application accepts explicit integer-nanosecond frame durations, advances fixed
ticks, supplies interpolation and variable-update hooks, and shuts down idempotently.

## Actual source architecture
- Engine/Core: Result/errors, application lifecycle, fixed-step loop, diagnostics;
  owned standalone CMake, headless example and core acceptance tests.
- Engine/ECS: RAII Flecs world wrapper; trusted engine integration only.
- Engine/Scene: stable scene-local IDs, hierarchy, transforms and stream serialization.
- Engine/Commands: typed built-ins, discoverable registry, permission checks,
  revision-checked transactions, bounded undo/redo and inspection.
- Engine/Renderer: next milestone; render-scene extraction and Filament ownership.
- Engine/Editor: after a verified renderer/window; hierarchy, inspector, viewport,
  console, create/delete/transform commands first.
- docs/architecture: decisions and committed integration contracts.

Core modules are static parts of one process. No dynamic plugin host or second renderer.
The first command planner materializes ordered snapshots and validates the full
hierarchy: O(n log n) for ordinary authoring transactions; batches containing several
subtree deletions can cost more. This is a deliberate, documented correctness tradeoff, not a
claim of production-scale editor performance. Batches that create, delete, duplicate, reparent or
replace still take that path. Since Milestone 6 (slice 6), a batch that only renames, moves or sets
components runs in place: it reads, checks and commits only the entities it names and the world
matrices of their subtrees, with the same checks in the same order, errors, receipts and history
as the whole-scene path (`engine.core.acceptance` compares the two to the bit). Moves alone copy
no entity record unless the bus keeps history. Physics, gameplay and scripts send that kind every
fixed tick; until then each such transaction copied and validated the whole scene, so a tick cost
what the scene held (moving all 500 entities of a scene: 4.7 ms, and 0.45 ms in place).
`Scene::structure_revision()` moves only when more than transforms change, and physics mirrors
components only when it does. History retains only changed entities. World matrices are recomputed
on successful commits; unchanged matrix components are not rewritten. Optimize profiled hot paths
before adding generic storage or plugin frameworks.

## Boundaries and safety
Single owner thread for world, scene, commands and lifecycle. Expected revisions
prevent stale editor/agent writes; a revision is published once per transaction,
undo or redo. Failed validation leaves the world, history and revision untouched.
IDs are scene-local, never recycled during the session, and preserved by serialization
and undo. Network identity must pair scene/session identity with EntityId.
Typed capabilities allow scene inspection or editing only. No command can execute a
process, access a path or invoke an unrestricted script. Stream serialization does
not grant filesystem authority; the trusted host owns opening streams.
Memory exhaustion is fatal/not an atomicity guarantee. Native Flecs observers must
not perform external effects during a commit. Cross-thread command transport belongs
at the host boundary, not inside the scene.

## Filament audit and gate
Read filament/BUILDING.md, CMakeLists.txt, filament/include/filament/Engine.h and
Renderer.h. This checkout uses C++20 and requires CMake >=3.22.1. Windows supports
MSVC; BUILDING.md explicitly excludes MSYS2. USE_STATIC_CRT defaults ON; the core
uses the normal MSVC DLL CRT, so Claude must align CRT options before linking.
Engine::Builder / Engine::create create a backend instance; Engine owns renderer,
view, scene and swap-chain resources, which must be destroyed before Engine.
Renderer::beginFrame determines whether a frame should be drawn; skipped frames
must not call endFrame. A native window handle is required for a windowed swap chain.
No prebuilt Filament SDK or linked renderer is assumed or included in this milestone.
Filament's public API is not a general-purpose Vulkan command buffer/compute API.

Next renderer contract: engine-owned asset handles (index + generation), camera
projection, light parameters, mesh/material handles and stable render-object IDs in
an immutable frame snapshot. No Filament pointer enters ECS or public scene files.
A Filament adapter owns maps from engine handles to native resources. Shutdown order:
stop frame production, drain GPU work using verified APIs, destroy adapter resources,
then Engine and window. Future Vulkan/compute/graph/ray-tracing/ReSTIR work replaces
or extends this boundary; it is not represented as implemented today.

## Shared contracts for Claude
See 002-claude-integration.md. Consume committed headers, not uncommitted work.
