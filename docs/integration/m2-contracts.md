# Milestone 2 integration contracts (Claude systems ↔ Codex editor/renderer)

> **Status update (Milestone 3, slice S5).** Resolved in `agent/claude-milestone-3`:
> **C-1a**: each play session has its own scene and history-free bus, so two buses on one scene are no longer
> needed in the editor. **C-2a**: physics authoring moved into the scene (`RigidBodyComponent`,
> `ColliderComponent`, commands `SetRigidBody`/`SetColliders`, scene format v4); the bridge's authoring setters and
> "detached" bookkeeping are gone. **C-4a**: colliders are drawn as an ImGui viewport overlay from `debug_geometry()`.
> **C-5a**: `renderer::ResourceRegistry` maps `(AssetId, revision)` to generation-checked handles and
> `DesktopRenderer` uploads from it on first use. **C-5b**: the conventions below hold; an imported image's first row
> shows at the far end of a plane seen from above, asserted by a GPU readback in the smoke run. The lit material
> flips v for Filament's sampling. Sections below are updated where the API changed.

Status: **proposed by Claude, awaiting Codex confirmation.** Written 2026-10-28 on
`agent/claude-systems-m2`, branched from `main` at `d876e42` (the M1 integration).
Everything below is implemented against committed `main` headers only. Nothing here
modifies a Codex-owned file.

Ownership decisions made by the user for this milestone:
- Claude owns `Engine/Assets/` for the **CPU side**: identifiers, path normalization,
  metadata, validation, decoding, cache. Codex owns GPU resource creation and
  `MeshHandle`/`MaterialHandle` allocation.
- Formats: Wavefront OBJ meshes plus uncompressed/RLE TGA and binary PPM textures, parsed by
  engine code. No new third-party dependency.

Contract IDs (C-n) are referenced from code comments and the integration report.

---

## C-1 Transform ownership and physics write-back

Facts from Codex's committed code:
- `Scene::commit` is private. The only mutation path for `Transform`/`Parent` is `CommandBus`.
- `CommandBus::execute` records undo history, unless the bus was constructed with
  `history_limit == 0`.
- Undo/redo refuses (`Conflict`) when a managed entity changed outside that bus's history.

Rules adopted by `engine::physics::ScenePhysics`:

| Body type | Authority while simulating | Authority while not simulating |
|---|---|---|
| Static | Scene. An external change teleports the body. | Scene |
| Kinematic | Scene. A change becomes a `move_kinematic` target, not a teleport. | Scene (teleport) |
| Dynamic | **Physics.** Its pose is published to the scene after each tick. | Scene (teleport) |

Feedback-loop guard: after publishing, `ScenePhysics` reads back the scene's
`WorldTransform` for every entity it wrote and stores it. On the next tick, a scene world
matrix equal to the stored one counts as "ours" and is not teleported back into physics.
Only a matrix that differs (an editor drag, a moving parent, a scene replace) counts as an
external edit.

Publish path (interim, needs Codex confirmation):
- The host passes a `CommandBus&` to `ScenePhysics::fixed_update`. Recommended: a
  **dedicated runtime bus** constructed with `history_limit = 0`, actor `"physics"`.
  It writes plain `SetTransform` commands, so validation and world-matrix recompute stay
  Codex's code.
- Consequences, all covered by tests:
  - Physics ticks never enter the editor's undo stack.
  - The editor bus's revision does not change during play.
  - Undoing an editor change to an entity that physics has since moved fails safely with
    `Conflict`.
  - Restoring a pre-play `SceneSnapshot` through the runtime bus (`ReplaceScene`) makes the
    editor's undo history valid again.
- Cost: each transaction snapshots the scene (O(n log n), see 001-core-milestone.md). Up to
  256 moved bodies fit in one transaction per tick; more are split.

**Ask for Codex (C-1a):** confirm that two `CommandBus` instances on one `Scene` is a
supported pattern, or provide a pose-stream entry point. Example: `Scene::apply_runtime_poses`
or `CommandBus::execute_runtime`, skipping history and taking a batch without a full-scene
snapshot. `ScenePhysics::pending_transforms()` already produces the batch, so swapping the
sink is a one-line host change.

Hierarchy rules:
- Physics poses are world space with no scale. Before publishing, local position and rotation
  are computed with the parent's world matrix. The local scale is never changed by physics.
- A dynamic body whose ancestors include another dynamic body is rejected (`issues()`).
  Within one batch, its local transform would be computed against a stale parent.
- A dynamic body under a non-uniformly scaled parent is rejected. Its world pose could not be
  expressed as a local TRS without shear.
- World scale is baked into collider sizes when the body is (re)built. A scale change rebuilds
  the body's colliders. Non-uniform scale is accepted only for boxes with an identity local
  rotation. Other shapes need uniform scale, or they are rejected with an issue.

## C-2 Physics authoring data and entity lifetime

- `RigidBodyComponent` and `ColliderComponent` are part of each `EntityRecord`, so commands,
  undo/redo, play-mode copies and scene files carry them like the transform. `ScenePhysics::sync`
  converts them to `RigidBody` and `Collider` (`to_rigid_body`, `to_collider`) and builds bodies.
  An entity with colliders but no body is a static collider.
- Entity deleted, components removed, or entity absent after `ReplaceScene`: the body is destroyed
  on the next sync. Undo restores the record, and the next sync rebuilds the body.
- The dynamic-hierarchy rule is checked on every sync, because a reparent that keeps the world
  transform changes the verdict without changing the entity's world matrix.
- Body `user_key` is the `EntityId`. Collision events report `EntityId`s, never Box3D handles.

## C-3 Simulation stepping

- `ScenePhysics::fixed_update(delta, scene, bus, ctx)` runs exactly one fixed tick. Call it from
  `ApplicationClient::on_fixed_update`. `delta` must equal `WorldConfig::fixed_timestep`,
  otherwise `InvalidArgument` is returned. Set the core `LoopConfig::fixed_step` to the same
  value.
- Order inside a tick: scene → physics sync, then `PhysicsWorld::step()` (only when simulation
  is enabled), then event translation, then pose publish.
- While simulation is disabled (editor edit mode), `fixed_update` still syncs. That keeps
  bodies, debug geometry and queries current with editor edits.
- Determinism: identical command/tick sequences in one binary produce bitwise-identical scene
  snapshots (tested). Not claimed across compilers or platforms.
- The runtime bus must be destroyed before `Application` destroys the scene, e.g. in
  `on_stop`.

## C-4 Editor-facing physics API (for Codex's ImGui panels)

All calls happen on the scene owner thread. None of them takes ImGui types.

| Need | API |
|---|---|
| Play / pause | `set_simulation_enabled(bool)`, `simulation_enabled()` |
| Restart from authored state | `reset_simulation(scene)` |
| Add/replace/remove a body | `commands::SetRigidBody{id, body}`; an empty optional removes it |
| Edit colliders | `commands::SetColliders{id, list}` replaces the whole list; `colliders(id)` reads what was last synced |
| Inspector read-out | `body_state(id)`: pose, velocities, mass, awake, type |
| Diagnostics panel | `diagnostics()`: counts, tick counters, teleports/publishes last tick, `issues` |
| Collision log | `events()`: valid until the next `fixed_update`/`step` |
| Collider overlay | `debug_geometry(options)`: world-space line list with a category per line |

`DebugGeometry` is CPU data: `{a, b, entity, category}` per line. The renderer owns drawing,
for example a Filament line-list renderable or an ImGui viewport overlay, and picks colors
per `DebugCategory`. `default_color(category)` is provided only as a convenience.
**Ask for Codex (C-4a):** which of those two the viewport will use. Either works with
this data.

## C-5 Asset identity and the renderer resource boundary

- `AssetId` is a 64-bit value. It comes from the asset's `.meta` sidecar (`id = <16 hex>`),
  which survives renames. If no sidecar exists, it is FNV-1a-64 of the normalized virtual
  path: stable while the path is unchanged. Zero is invalid. The scan rejects ID
  collisions and case-only path collisions.
- Virtual paths are relative to the asset root and use `/`. They are case-preserving and
  normalized (`.` removed, `..` resolved). Absolute paths, drive letters, UNC paths,
  escapes above the root, Windows reserved names and control or reserved characters are
  rejected. Opening a file additionally checks that the canonical path stays inside the
  canonical root, which blocks symlink escapes.
- `AssetDatabase` is a **trusted-host service**. Agents and commands never receive filesystem
  authority through it, which is consistent with Codex's "no filesystem commands" rule.
- CPU outputs are `std::shared_ptr<const MeshData>` and `std::shared_ptr<const TextureData>`.
  Each load result carries an `AssetId` and a `revision`, and the revision increments on
  reload.
- **Renderer boundary (C-5a, closed):** `renderer::ResourceRegistry` maps
  `(AssetId, revision)` to `MeshHandle`/`TextureHandle` and bumps the handle generation when
  the revision changes; `DesktopRenderer` uploads from the CPU data when a frame first draws a
  handle. The registry holds the `shared_ptr` for the session.
  `AssetDatabase::unload_unused()` frees only entries nobody else references. Assets create no
  GPU objects. A mesh may carry `SubMesh` parts with a metallic-roughness `MeshMaterial` and
  embedded images; there is no other material model. A texture is RGBA8 rows with a top-left
  origin, plus an `srgb` flag from metadata.
- Mesh layout: an interleaved `MeshVertex {position[3], normal[3], uv[2]}` array, 32-bit
  indices, triangle lists, counter-clockwise front faces, right-handed coordinates, meters,
  and an axis-aligned bounding box. OBJ `vt` is converted to a top-left UV origin
  (`v' = 1 - v`) to match the texture rows, as glTF already is (C-5b, closed).

## C-6 Build integration for the editor

- New root option `GAMEENGINE_BUILD_EDITOR` (default ON). It defines
  `gameengine::glfw` (GLFW, static) and `gameengine::imgui` (ImGui docking core plus the GLFW
  platform backend; no renderer backend). Both come from the read-only resources checkout
  at their locked commits. The root adds `Engine/Editor` when
  `Engine/Editor/CMakeLists.txt` exists.
- The ImGui renderer backend is Codex's choice and is not in `gameengine::imgui`. Filament has
  its own ImGui helper, and Filament's build or CRT setup is not attempted in this milestone.
- The root `CMakeLists.txt` is shared. Every change to it is listed in the integration report.
- A headless smoke test (`editor_dependency_smoke`) builds an ImGui frame without a window and
  links GLFW without opening a display. It verifies the targets compile, link and run, and
  says nothing about GUI behavior.

## Open questions for Codex (summary)

C-1a runtime pose path · C-2a physics authoring persistence/undo · C-4a debug draw route ·
C-5a renderer resource registry keyed by `(AssetId, revision)` · C-5b mesh/UV conventions.
