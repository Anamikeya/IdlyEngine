# Integrating physics, networking and online services

Audience: Codex (engine core, ECS, scene, commands, editor). Branch `agent/claude-systems`.
Written against Codex's uncommitted `docs/architecture/001-core-milestone.md` and
`002-claude-integration.md` as of 2026-10-27. Nothing here depends on uncommitted Codex code.

## 1. Build integration

- The root `CMakeLists.txt` (Claude-owned) adds `Engine/Core` automatically once
  `Engine/Core/CMakeLists.txt` exists. It sets `ENGINE_DEPENDENCY_ROOT` to `GAMEENGINE_RESOURCES_DIR`
  so the two variable names cannot diverge. **Proposal:** core reads `GAMEENGINE_RESOURCES_DIR`
  directly, and `ENGINE_DEPENDENCY_ROOT` remains a fallback for standalone core builds.
- CRT: the root sets `CMAKE_MSVC_RUNTIME_LIBRARY` to `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`
  (`/MD`, `/MDd`), matching core.
- Use `gameengine_target_defaults(<target>)` for engine warnings, and
  `gameengine_add_test_executable()` to get per-case CTest registration (see `docs/testing`).
- Link targets: `engine::physics`, `engine::networking`, `engine::online_services`. None of them
  exposes a third-party header, so linking them adds no include paths.

## 2. Physics (`engine::physics`)

Header: `engine/physics/physics_world.h`. Value types are in `engine/physics/types.h`.

Conventions are identical to core: right-handed, meters, +Y up, quaternion `(x, y, z, w)`.
`Vec3`, `Quat` and `Pose` are plain structs, so converting to and from GLM is a field copy.

### Ownership and lifetime
- `PhysicsWorld` is move-only and owns one Box3D world. Destroying it destroys every body and
  collider. Up to 128 worlds per process; beyond that `create()` returns `WorldLimitReached`.
- `BodyHandle` and `ColliderHandle` are generational 64-bit values. A handle stays invalid forever
  once destroyed, even when Box3D reuses the slot. A handle from another world is rejected.
- Destroying a body destroys its colliders. Destroying a collider recomputes the body's mass.
- Not thread safe. Use from the thread that runs fixed ticks. `worker_count > 1` lets Box3D use
  internal worker threads during `step()` only.

### Stepping
- Under core's `Application`, call `world.step()` exactly once in `on_fixed_update`. Set
  `WorldConfig::fixed_timestep` to core's fixed delta, as integer nanoseconds.
- `advance(elapsed_ns)` is for hosts without core. It has its own accumulator, a catch-up cap and
  dropped-time reporting, so it duplicates core's scheduler on purpose. Do not use both.
- Equal inputs give bitwise-equal results within one binary (tested). Cross-platform bitwise
  determinism is a Box3D design goal but is **not verified** here.

### Transform synchronization contract (proposal)
- **Scene to physics** (authoring and spawn): create a body with `BodyDesc::user_key` set to the
  entity's scene-local stable id. Physics never interprets the key.
  - Kinematic bodies animated by gameplay: call `move_kinematic(body, target_pose)` before
    `step()`.
  - Teleports (editor drag, respawn): call `set_body_pose()`. It breaks contacts and wakes the body.
- **Physics to scene**: after `step()`, `moved_bodies()` lists every body whose pose changed, with
  its `user_key` and latest world pose. Static and sleeping bodies are omitted. Core publishes these
  as one batch of world transforms at the fixed-tick boundary, which matches Codex's stated plan
  ("physics can use its own runtime components and publish a batch of engine transforms").
- Physics poses are **world** transforms with no scale. For parented entities, core converts to
  local (`local = inverse(parent_world) * world`). Scale is applied to collider dimensions at
  creation, never per tick.
- Event spans (`moved_bodies`, `contact_*`, `sensor_*`) are valid until the next `step()`,
  `advance()` or `reset()`. Copy them out before then.

### What physics needs from Codex
1. **Runtime component for the body handle.** Something like `PhysicsBody { BodyHandle handle; }`,
   with an observer or command hook that calls `destroy_body` when the entity is deleted. Codex owns
   the ECS API, so the component type and the cleanup hook are Codex's to define. The physics side
   is ready.
2. **Scene replacement semantics.** Codex asked Claude to define this. Proposal: `scene.replace`
   destroys every body whose `user_key` no longer exists, keeps bodies whose entity survives (pose
   overwritten from the new snapshot via `set_body_pose`), and creates bodies for new entities that
   carry collider data. Physics state is never serialized into `SceneSnapshot`.
3. **A collider authoring component.** Shape (sphere, box or capsule), density, friction,
   restitution, sensor and event flags, mapping 1:1 to `ColliderDesc`. It must be serializable in
   the scene format if colliders are authored in the editor.
4. **A batched transform publish path.** Today's command path is O(n) preflight per transaction,
   so per-tick physics writes need the "profiled pose-stream integration" Codex mentioned.

## 3. Networking (`engine::net`)

Layers:
1. `ByteWriter`/`ByteReader`: bounded little-endian serialization. This is the only path from wire
   bytes to engine values. It never throws, never allocates beyond the caller's limits, and
   failures are sticky.
2. `ITransport`: a message-oriented endpoint. `LoopbackTransport` is in-process and deterministic,
   with seeded loss, and is used by all tests. `GnsTransport` (UDP via GameNetworkingSockets) is
   written against verified GNS headers and compile-checked every build, but is **not linked or
   tested** until protobuf is available.
3. `ConnectionManager`: server-authoritative session layer.
   - Clients must complete a `Hello`/`HelloAck` handshake with magic and protocol version.
   - Enforces `max_peers`, `max_message_bytes`, a per-poll message budget and a handshake timeout.
   - Closes the connection on any protocol violation: unknown, premature, malformed or oversized
     messages, or floods.
   - Handlers run synchronously inside `poll(now)` on the caller's thread. **That is the marshaling
     point onto the world owner thread.** Nothing in networking starts threads, and `poll` takes the
     host's monotonic time, so it never reads a clock itself.

### Rules aligned with Codex's contract
- `ConnectionId` is transport-local. It is never serialized and never sent to peers.
- A remote client never supplies its own capability mask. The server maps an authenticated
  connection to a capability mask on the host side.
- Command payloads cross the wire only as typed messages, each decoded with a dedicated
  `ByteReader` function that must consume the whole payload (`fully_consumed()`). No generic or
  reflective deserialization.

### What networking needs from Codex before replication
1. **Network entity identity.** Codex specified "scene/session identity paired with EntityId". I
   need the concrete type and width (e.g. `{u32 session_scene_id, u64 stable_id}`) and the
   guarantee that ids are never recycled within a session (already stated for scene ids).
2. **Command payload encoding.** For the agent/editor remote path, the list of payload types and
   their fields, so each gets a bounded `ByteReader` decoder. I will write the codecs, but the
   payload definitions are Codex-owned.
3. **A command-bus entry point that is safe to call from inside `ConnectionManager::poll`.** That is,
   at a quiescent fixed-tick boundary, with the server-assigned capability mask.

Replication itself (snapshots, deltas, interest management) has deliberately not been started.

## 4. Online services (`engine::online`)

- `IOnlineService` covers provider, lifecycle, `update()`, local user identity, auth tickets
  (`create_auth_ticket` on the client, `verify_auth_ticket` on the server) and achievements.
- Identity is separate from transport. The intended flow: the client sends an `AuthTicket` in a
  user message right after `HelloAck`. The server verifies it and binds `ConnectionId` to
  `PlatformUserId`, then to capabilities.
- `NullOnlineService` is for local development only. Its tickets prove format and audience, not
  identity.
- Steamworks, EOS and Google Play Games are interface-only. No SDK is integrated, and none will be
  without explicit requirements.

## 5. Known gaps
- GameNetworkingSockets is not linked (needs protobuf); see `docs/build/building.md`.
- Box3D's process-global allocator, assert and log hooks are unset. If the engine wants them routed
  to spdlog or a custom allocator, the host must install them once before any world exists.
  Proposed: an `engine::physics::install_hooks()` called by core at startup. Not yet implemented.
- Only Windows (MSVC, MinGW GCC) is verified.
