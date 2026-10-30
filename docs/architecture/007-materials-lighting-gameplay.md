# Materials, lighting and gameplay (Milestone 4)

How the systems added in Milestone 4 fit together, and why they are built this way. The file format is in
`006-scene-format.md`; results are in `../MILESTONE_4_REPORT.md`.

```text
Scene (generic data)          MESH + material refs, LIGHT, ENVIRONMENT, BODY, COLLIDER, BEHAVIOR
   |                                    |                       |
   v                                    v                       v
EditorSession.frame()          ScenePhysics (Box3D)      gameplay::Game (Flecs components)
   | resolves refs               raycast, impulses,        input, movement, weapons, damage, rules
   v                             events                           |
ResourceRegistry + FrameSnapshot        ^                         v
   |                                    |_________________  HudState (plain data)
   v                                                              |
DesktopRenderer (Filament)                                 GameHud (ImGui, editor only)
```

Filament types stay in `Engine/Renderer/src/desktop`. Box3D types stay in `Engine/Physics/src/physics_world.cpp`.
`Engine/Gameplay` links neither, nor ImGui, nor the editor: `gameplay_tests` runs the whole game with no window.

## Materials

**One Filament material per blend mode.** `Engine/Renderer/materials/pbr.mat` is compiled three times with
`matc -TBLENDING=... -TTRANSPARENCY=...`, because blending is fixed when a Filament material is built. Everything else
is set per `MaterialInstance`: factors, five samplers, culling, double-sidedness and the mask threshold. Every sampler
is always bound, with a fallback that means "no change" (white, or a flat normal), so a missing map cannot fail a draw.
The renderer compares parameters and writes only what changed, so a still scene sets no parameters per frame.

**gltfio's `AssetLoader` stays unused** (M3 plan, D4): it would own an entity tree outside Flecs. The importer reads the
same files with cgltf and produces plain CPU data.

**Which material applies to a part:** the part's slot in `MESH`/`MATSLOT`, then the mesh's material, then the model's
own material, then the mesh's roughness and metallic values. The mesh's color always tints, and its texture always
replaces the base color map.

**Color space comes from the slot.** Base color and emissive are read as sRGB, metallic-roughness, normal and occlusion as
linear. A standalone image gets one GPU texture per color space it is used in. A GLB's images are decoded per space too.

**Tangents.** `MeshVertex::tangent` holds xyz and a handedness, from the glTF `TANGENT` when present, otherwise derived from
UVs on the CPU. Conventions, each checked by readback in `--render-test`:

- UV `v = 0` is the image's first row; materials use `flipUV: false`.
- A normal map's +Y leans towards the image's top. `packTangentFrame` makes `q.w` positive when the matrix's second
  column is along `cross(t, n)`, while the shader's bitangent is `cross(n, t) * sign(q.w)`, so `tangent_frame` passes
  `cross(t, n) * handedness`.
- Filament does not normalize its tangent frame, and a non-uniform scale makes the axes very different lengths: on a
  32 m floor box the map all but vanished. `pbr.mat` divides by the axis lengths.

**Editing.** A `.material` file is the source of truth. A drag previews (`preview_material`: a draft, no file, no history);
release saves (`save_material`: atomic file write, one undo step). Scene and material edits share Undo and Redo. Each
material edit records how deep the scene's undo stack was, which puts the two histories in order without changing
`CommandBus`.

## Lighting

**Environment** is a component, because it belongs to a scene and must save, undo and copy into Play like anything else.
The first by entity ID applies; a scene without one gets the defaults (procedural sky, EV100 15).

**Image-based light.** A procedural sky or `.hdr` becomes an equirectangular texture, then `IBLPrefilterContext` builds the
cubemap, reflections and irradiance on the GPU. Only source changes rebuild; intensity and exposure do not.
Rotation shifts the image's columns before conversion, since a Filament skybox cannot rotate.

**Exposure** is EV100, mapped onto aperture, shutter and ISO, because Filament clamps each. Indoor and outdoor presets differ
mainly by exposure, so a 3000 lm bulb reads correctly indoors at EV100 6. The Create menu sizes a point light for the
scene's exposure rather than one fixed number.

**Shadows** use PCF at every quality level: DPCF drew no shadow at all in `--render-test` on this driver. `stable` cascades
keep edges from swimming as a first-person camera turns. Filament allows one directional light to cast shadows: the
brightest.

**Default lights.** A scene with no lights uses two editor lights. A scene with any light uses only its own.

## Resource lifetime

| Stage | Owner | When it goes |
|---|---|---|
| Decoding | A worker (`prepare_import`, `std::async`) | Handed to the owner thread by `poll_imports` |
| CPU data | `AssetDatabase` cache and `ResourceRegistry` | `release_unused_assets`, every 240 frames, for assets no scene references. A later reference re-imports from disk, with the same handle |
| GPU objects | `DesktopRenderer`, on the render thread | The first sync after no object draws them |

Material descriptions stay in the registry: they are small and hold no pixels. The worker touches only its own file and
data; the catalogue, cache and GPU stay on the owner thread. References in a loaded scene still load synchronously on
first use.

## Physics additions

| Addition | Notes |
|---|---|
| `PhysicsWorld::cast_ray`, `ScenePhysics::raycast` | `b3World_CastRay` with a filter callback: closest hit, sensors skipped, one body ignorable, category and mask from the scene |
| `apply_linear_impulse_at`, `apply_impulse` | `b3Body_ApplyLinearImpulse`. Dynamic bodies only; static and kinematic refuse. Keeps no scene state and no undo history |
| `lock_rotation` | `b3MotionLocks` on all three rotation axes |
| Collision filters | `ColliderComponent::category` and `mask`, saved in the scene |
| Hit events | On for colliders with `events`, above Box3D's default 1 m/s threshold |

Nothing touches the world during a callback: events are polled after the step, the ray callback only records, and deletion
goes through the runtime bus and takes effect at the next sync.

## Gameplay

**Behaviors, not shooter types in the scene.** `Components::behaviors` holds `{type, [(key, number)]}`. The scene validates the
shape and gives it no meaning. `Game::create` parses them into typed Flecs components on the runtime scene. A scene with no
`game_mode` or `player` runs no game and pays nothing. The price is that values are numbers, and a misspelled key falls back
to its default; unknown behavior types are reported. `behavior_templates()` lists what is understood.

**Tick order**, at a fixed 60 Hz:

1. `pre_physics`: timers, movement (sets velocity), firing (raycast and impulse), movers, one command batch.
2. `ScenePhysics::fixed_update`: sync, step, publish poses.
3. `post_physics`: collision and sensor events, damage, score, win and loss.

Commands are batched because `CommandBus::execute` copies the scene per transaction. Runtime commands use the runtime bus,
which keeps no history.

**Player controller: a dynamic capsule with locked rotation.** Each tick sets its velocity, approaching the wanted speed at a
limited acceleration. Friction is 0, so it slides along walls.

| Option | Verdict |
|---|---|
| Dynamic capsule, locked rotation | **Chosen.** One collision path: walls and floors stop it through the normal solver, and it pushes crates |
| Kinematic with `b3World_CastMover` and `b3SolvePlanes` | Handles steps and slopes better, but needs its own collision logic and manual pushes |
| Unlocked dynamic body | Tips over |

Costs: no automatic stair stepping (the sample's platform has a step low enough to jump), and it can push hard against
light crates. Ground is checked with five short rays. Falling below `spawn_floor` returns the player to the start.

**Input.** `InputMap` maps device codes to actions, and a press shorter than a tick still reaches the next one. The editor's
`GameInput` reads GLFW and feeds it; gameplay never sees a device.

**Mouse look** applies at once and writes the camera once per rendered frame, so aim does not wait for a tick. Movement and
shots use the aim as of their tick, so a recorded run is reproducible (`the_same_input_gives_the_same_game`).

**HUD.** `Game::hud()` returns plain `HudState`; `Engine/Editor/src/GameHud.cpp` draws it. A standalone runtime replaces only that.

**Restart** discards the runtime scene, bridge and game and builds new ones from the editor scene, staying in Play.
It cannot leave stale state behind, since nothing survives.

## Skeletal animation: investigated, deferred to Milestone 5

| Question | Finding |
|---|---|
| What Filament supports | Bones through `RenderableManager::Builder::skinning` and `setBones`, with `BONE_INDICES` and `BONE_WEIGHTS` vertex attributes; morph targets; clip sampling in gltfio's `Animator` (`applyAnimation`, `applyCrossFade`, `updateBoneMatrices`) |
| What our importer keeps | Bind pose only. It bakes node transforms into one mesh, drops joints, weights, skins, clips and the node tree, and warns |
| The catch | `Animator` works on gltfio's `FilamentAsset`, whose entity tree is the one M3 rejected. Using it would put a second scene graph beside Flecs |
| What would be needed | CPU: `MeshVertex` or a parallel stream with 4 joints and weights; `SkeletonData` (joint hierarchy, inverse bind matrices); `AnimationClip` (sampled channels) in `gltf_importer.cpp`. Runtime: an `AnimationComponent` and a system evaluating clips on the fixed tick or per frame. Renderer: `Surface` carries a bone palette handle, and `Builder::skinning` with `setBones` per frame. Materials: `pbr.mat` needs no change, since skinning happens in Filament's vertex stage |
| Can the import path extend safely | Yes. It is additive, and static meshes keep today's layout if skinning data is a separate stream. Nodes must stop being baked for skinned primitives, since joints need them |

Deferred because it is not needed for the playable slice, and the M4 brief puts the shooter first. Suggested M5 order: importer
data and tests with `Fox.glb`, then bind pose through `setBones`, then one clip, then blending.
