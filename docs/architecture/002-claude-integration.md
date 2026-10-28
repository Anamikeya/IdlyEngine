# Shared integration contract: core milestone

Ownership: Codex owns Core/ECS/Scene/Renderer/Editor/Commands. Claude owns physics,
networking, root build infrastructure and shared test orchestration. This commit
adds only an owned module CMake and core acceptance tests; no root/shared files.

## Build
Public target: engine::core (static library target engine_core).
C++20, MSVC /MD and /MDd by default. ENGINE_DEPENDENCY_ROOT points at the read-only
resources directory. Core privately builds the Flecs amalgamation and uses GLM and
spdlog headers from that directory. No downloads or writes in resources.
Claude can add_subdirectory(Engine/Core) and link engine::core. If supplying
engine_flecs, its headers/definitions must agree with the audited checkout.
ENGINE_CORE_BUILD_TESTS and ENGINE_CORE_BUILD_EXAMPLE control owned acceptance tools.
Do not add a second copy of Flecs to an executable.

## Runtime
Application is the lifetime owner. on_start returns Result<void>; on_fixed_update
gets an integer tick index and fixed delta; on_update gets a clamped frame delta,
interpolation alpha and dropped-time diagnostics. on_stop is noexcept and exactly
once after a start attempt reaches the client callback. Shutdown destroys command
history before Scene/World. Accessors are null outside the running lifetime.
The caller supplies frame time; an OS host can use SteadyClock. No fixed callback
reads the wall clock. Tick numbering starts at zero. A bounded catch-up policy
drops excess full ticks and reports the exact dropped duration. Catch-up count is
limited to 1024; negative and extremely large frame durations are rejected.
Shutdown requested from startup/update callbacks is deferred until the callback
returns. Startup re-entry is rejected.
This is deterministic scheduling for equal inputs, not cross-platform bitwise
physics, network lockstep or deterministic arbitrary user systems.

## Scene / physics
Right-handed coordinates; meters; +Y up. Local transform is translation,
quaternion (x,y,z,w), scale. World = parent-world * local, column-major matrices.
SetParent keeps local transform; world-space-preserving reparent is a later command.
DeleteEntity deletes a subtree; undo restores stable IDs and parent links.
Runtime-only Flecs IDs never cross the network or serialize.
Scene::ecs() is a trusted native integration seam, not an agent capability.
Do not mutate the managed structural components directly: use the command bus at
quiescent fixed-tick boundaries. Physics can use its own runtime components and
publish a batch of engine transforms. Ordinary batched authoring currently uses O(n log n) preflight;
a profiled pose-stream integration will be negotiated before high-volume physics.

## Commands / editor / agents
Registry names: entity.create, entity.delete, entity.rename, transform.set,
hierarchy.reparent, scene.replace. Payload is a C++ variant, never shell text.
Request context: actor name + granted capability mask; expected scene revision
is mandatory for execute, undo and redo. Host authenticates identity and grants
capabilities; a remote client must never supply its own trusted mask.
One transaction = one revision. Validation is complete before Flecs changes.
No filesystem/process commands. Inspection is also permission-checked.
Network layer serializes these typed payloads, enforces message/byte limits and
marshals them onto the world owner thread; no arbitrary callback deserialization.
Transaction count is capped, entity/name/data validity checked, undo history bounded.

## Persistence / networking
SceneSnapshot is a sorted sequence of stable records plus next_id. Version-1 text
serialization is stream-based, locale independent, bounded and strict: 100,000
entities, 256 bytes per name, 64 bytes per numeric token and 64 MiB input total. Parse into
a candidate snapshot, validate it, then submit ReplaceScene through commands.
Replacing a scene retains the same Flecs world and registered systems; matching
managed entities are updated, missing IDs removed, and new IDs created.
Unknown runtime components on surviving entities remain; they are not serialized.
Claude must define runtime-component cleanup semantics for scene replacement before
integrating physics/network state. Never treat the authoring snapshot as a savegame
of arbitrary native components.

## Renderer/editor next step
Claude supplies/coordinates root dependency build, Filament CRT alignment, platform
window ownership, CI jobs and integration tests. Codex then implements engine-owned
render snapshots, Filament adapter, and a minimal ImGui docking editor. Initial tools:
hierarchy, transform inspector, viewport, console, create/delete and undo/redo.
Asset browser/material editor follow working mesh/material import. Animation and
graph/model/texture editors are planned, not empty production interfaces.
