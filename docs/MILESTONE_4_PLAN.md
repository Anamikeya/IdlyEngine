# Milestone 4 plan: production rendering, PBR materials, lighting and a playable physics shooter

Branch `integration/milestone-4`, worktree `D:\AI\GameEngine-Integration`, created from `main` at `dc425e5`.
**Status: all slices done.** Results, evidence and departures: `docs/MILESTONE_4_REPORT.md`. Do not merge into `main` without
the user's approval.

Departures made during the work: every shadow quality uses PCF, because DPCF drew nothing on this driver. The gun is built from primitives,
because Kenney's page has no direct download link. Glass in the sample takes damage so it does not stop every shot. A real-input driver with focus and idle guards
was added after input reached another application.

## 1. Audit

### Git

| Check | Result |
|---|---|
| `main` | `dc425e5` *Merge Milestone 3: functional 3D editor*. `git diff 34c950b main` is empty, so M3 is fully in `main` |
| This worktree at start | `agent/claude-milestone-3` at `34c950b`, one merge commit behind `main`; untracked `audit.txt`, `build-log.txt` (user files, left alone) |
| `agent/claude-milestone-4` | Already exists at `dc425e5` with no work on it, checked out in `D:\AI\GameEngine-Claude`, so it cannot be checked out here. Left alone; M4 uses `integration/milestone-4` |
| Remotes | None. Hosted CI stays unavailable |

### What exists

| Area | State after M3 | Reused as |
|---|---|---|
| Scene | Flecs-mirrored `EntityRecord` with mesh, camera, light, body, colliders; `CommandBus` with undo; text format v5 | Extended with material references, spot lights, environment, behaviors. Format v6 |
| Renderer | Filament behind `DesktopRenderer`; one `lit.mat` (base color, roughness, metallic, one texture); **no shadows, no IBL, post-processing off, fixed exposure, always double-sided** | `.mat` compiled by `matc` in the same CMake loop; `ResourceRegistry`; frame snapshot |
| Importer | cgltf GLB: geometry, base color only. No tangents, other maps warned about | Extended in place. gltfio `AssetLoader` stays rejected (D4 in the M3 plan): it owns an entity tree outside Flecs |
| Assets | `AssetDatabase` catalogue, `.meta`, `import_file`, `reload`, `unload_unused`. Everything stays in RAM because `ResourceRegistry` keeps strong references for the session | Add material and environment asset types, background import, a release path |
| Physics | `PhysicsWorld` and `ScenePhysics` over Box3D f36d061: bodies, colliders, moved/contact/hit/sensor events, kinematic targets. **No raycast, no impulse at a point, no filters from the scene, no motion locks** | Extended. Box3D has `b3World_CastRay`, `b3World_CastRayClosest`, `b3Body_ApplyLinearImpulse`, `b3MotionLocks`, `b3QueryFilter` |
| Play mode | Isolated runtime scene and bus, fixed 60 Hz tick, Pause, Step, Stop | The gameplay runtime plugs into the tick |
| Editor | Hierarchy, Inspector, gizmos, Asset Browser, Console | New Material Editor and Lighting panels, sample project, game view |
| Gameplay | Nothing. No input abstraction, no HUD | New module `Engine/Gameplay` |

Other libraries: miniaudio and Tracy are pinned but not integrated, so M4 does not use them. Filament ships
`libs/iblprefilter` (GPU prefilter), used for IBL. Filament's `third_party/environments` are not redistributed.

### Defects found by reading the code, fixed in M4

- Built-in cube sides show images upside down: `v` grows with the bitangent while the first image row is at `v = 0`.
- `lit.mat` flips `v` because `flipUV` defaults to true, which flips it first; the two cancel. New materials use `flipUV: false`.
- `CommandBus::execute` copies the whole scene per transaction, so runtime systems must batch per tick.

## 2. Architecture

```text
Engine/Scene, Commands        Generic data only: no shooter types
Engine/Assets                 CPU: MeshData, TextureData, MaterialData, EnvironmentData
Engine/Renderer               Frame snapshot (plain data); Filament stays under src/desktop
Engine/Physics                PhysicsWorld, ScenePhysics; Box3D types stay in physics_world.cpp
Engine/Gameplay  (new)        Input actions, player controller, weapons, damage, game rules. No ImGui, Filament, Box3D or editor
Engine/Editor                 Hosts the game: passes input, draws the HUD from plain data
Samples/PhysicsShooter (new)  Project folder: scene, materials, textures, licenses
```

**A1. Material assets.** A `.material` text file is an asset type with a stable ID, like any other file in `Assets`.
`MeshComponent` gains `material` (whole mesh) and `material_slots` (per submesh overrides). Colors are sRGB in the
file, as in scenes. A GLB's own materials are the default for its parts.

**A2. One Filament material per blend mode.** `pbr.mat` is compiled three times with `matc -T` (opaque, masked,
transparent). `MaterialInstance` holds the rest: factors, five samplers, culling, double-sidedness, mask threshold.
Fallback textures keep every sampler bound. Parameters are written only when they change.

**A3. Color space by slot.** Base color and emissive are sRGB; metallic-roughness, normal and occlusion are linear.
The renderer picks the format from the slot, so one image used in two roles gets two GPU textures.

**A4. Tangents on the CPU.** `MeshVertex` gains `tangent` (xyz, handedness): the glTF `TANGENT` when present, otherwise
derived from UVs in the importer. This keeps `Engine/Assets` free of Filament and unit-testable. The bitangent points
at image-up, the glTF convention.

**A5. Environment as a component.** `EnvironmentComponent` on any entity: procedural sky or an `.hdr` asset, intensity,
rotation, skybox, EV100 exposure, tone mapper, shadow quality, AO, bloom. The renderer builds IBL with
`IBLPrefilterContext`. Changing the source rebuilds it; changing intensity does not. A scene without one uses a
default outdoor environment.

**A6. Behaviors, not shooter components in the scene.** `Components::behaviors` is a list of
`{type, [(key, number)]}`, saved as `BEHAVIOR` records and edited by a new command. `Engine/Gameplay` parses them into
typed Flecs components (`Health`, `Weapon`, `Target`, ...) on the runtime scene. A project with no behaviors runs no
gameplay and depends on nothing in the shooter.

**A7. Tick order.** `pre_physics` (input, movement, raycasts, impulses, one command batch) → `ScenePhysics::fixed_update`
(sync, step, publish) → `post_physics` (events, damage, score, rules). Box3D events are polled after the step, so nothing
mutates the world during a callback. Deletions go through the runtime bus and take effect at the next sync.

**A8. Player controller: a dynamic capsule with locked rotation**, driven by setting velocity each tick. Compared with
a kinematic body plus `b3World_CastMover`, it pushes crates and is stopped by walls through the normal solver, with no
second collision path. Ground checks use raycasts. Cost: no stair stepping and a harder crate push, neither of which the
arena needs.

**A9. Hitscan through the physics bridge.** `ScenePhysics::raycast` returns entity, point, normal and fraction, and
`apply_impulse` takes a world point. Filters use collider category and mask bits from the scene plus an ignored entity.

**A10. Game state is independent of the editor.** `Game` exposes a plain `HudState`. The editor draws it with ImGui; a
standalone runtime can draw it another way. Headless tests run the whole game without a window.

**A11. Resource lifetime.** The registry holds CPU data only while a live scene references it. Every couple of seconds
the session releases the rest and calls `unload_unused`; a later reference re-imports from disk. GPU resources are
created and destroyed on the render thread as today, already freed when no object draws them.

**A12. Background import.** `prepare_import` decodes on a worker with no database access; `commit_import` copies the
file and fills the cache on the owner thread.

## 3. Slices

| # | Slice | Done when |
|---|---|---|
| 1 | PBR: `MaterialData`, tangents, importer maps, `pbr.mat`, renderer parts | A freely licensed GLB shows all its maps; readback checks color space, alpha modes, normal direction |
| 2 | Lighting: spot lights, shadows, environment, exposure, tone mapping, Lighting panel, presets, test scene | Readback shows shadow and IBL effects |
| 3 | Material editor: create, duplicate, assign, slots, edit, preview, save, undo, missing diagnostics, background import, lifetime | Tests and real input |
| 4 | Physics: raycast, impulse at point, filters, motion locks, hit events | `physics_tests` and `physics_scene_tests` cases |
| 5 | Gameplay module: input, player, weapon, damage, targets, movers, game mode | Headless tests |
| 6 | Sample: scene generator, assets and licenses, HUD, cursor capture, sample opening, New Project | Plays with real input |
| 7 | Performance, full builds, report | Report written from runs made in this milestone |

Animation: investigated and documented in the report. It is implemented only if everything above is stable; otherwise
it is deferred to M5 with a plan.

## 4. Tests

| Area | Where |
|---|---|
| Material parsing, persistence, tangents, GLB maps, alpha, missing textures | `asset_tests` |
| Format v6, new commands, validation | `engine.core.acceptance` |
| Raycast, filters, impulses, mass response, static immobility, sensors, hits, reset, destruction, determinism | `physics_tests`, `physics_scene_tests` |
| Player, jump, fire rate, ammo, reload, hits, damage, score, win, loss, restart, Play/Stop isolation, the sample end to end | `gameplay_tests`, `engine.editor.acceptance` |
| Materials, shadows, IBL, GPU resource lifecycle | `engine_editor --smoke`, `--render-test` (readback) |
| Real input | The brief's 16 steps on the Release editor; evidence in the report |

## 5. Risks

- **Scale.** If time runs out, rendering plus a playable sample come first; anything cut is reported as incomplete.
- **Post-processing changes every image**, so the M3 smoke values change. Their assertions stay; the light-only probe
  runs with the environment off.
- **Hit events on `events` colliders** add entries that existing tests may count; those tests are read before changing.
- **Real input** needs an unlocked desktop and moves the real mouse, and M3 runs saw outside cursor interference.
- **Downloads** can fail. The sample falls back to procedural materials and does not need the network to run.

## 6. Acceptance

Section 15 of the brief, verbatim, is the checklist in the report. Anything not met makes the verdict CONDITIONAL PASS or FAIL.
