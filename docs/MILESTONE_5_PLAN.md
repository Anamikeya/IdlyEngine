# Milestone 5 plan: characters and effects

Branch `milestone-5`, worktree `D:\AI\GameEngine-Integration`, created from `main` at `14c6642`.
**Status: plan, waiting for approval.** No code has been written. Do not merge into `main` without the user's approval.

Goal: particles, skeletal animation with ozz-animation, and Box3D ragdolls, all editable in the editor. The shooter
gains animated enemies that walk, flinch when shot and fall as ragdolls, plus sparks, dust and glass shards.
Scripting (Lua expected) is Milestone 6. The standalone player and packaging are Milestone 7, so they ship all of this at once.

## 1. Audit

### Git

| Check | Result |
|---|---|
| `main` | `14c6642` *Add README*. The editor UX work is merged at `4e88193`, and `main` is pushed to `origin` (github.com/Anamikeya/IdlyEngine) |
| This worktree | `milestone-5`, new, at `main`. Untracked files from earlier runs (`audit.txt`, `build-log.txt`, `smoke-*`, `imgui.ini`, `docs/UNREAL_UX_REFERENCE.md`) are left alone |
| Hosted CI | Triggered by the first push. Its result has not been seen, as the GitHub connector failed and `gh` is not installed |

### What exists

| Area | State after the editor UX work | Reused as |
|---|---|---|
| Importer | cgltf GLB: geometry, five PBR maps, tangents. Every node is flattened into one mesh with world transforms baked in. **Skins and animations are dropped** with a warning (`gltf_importer.cpp:471`). Morph targets are dropped | Extended in place. cgltf already parses skins, inverse bind matrices and animation channels |
| Asset types | `MeshData` with `MeshVertex` (position, normal, uv, tangent), submeshes, images. No skeleton or clip types | New plain CPU types beside `MeshData`. `Engine/Assets` stays free of ozz |
| Renderer | `DesktopRenderer` over Filament, OpenGL backend. One vertex buffer of position, tangent frame, color and uv. `pbr.mat` compiled three times. No skinning, no particles | A second vertex buffer with bone indices and weights; `RenderableManager::Builder::skinning` and `setBones`. A new `particle.mat` |
| Frame snapshot | Objects, lights, camera, environment | Gains bone palettes per object and particle batches |
| Physics | Bodies, colliders, raycasts, impulses, filters, hit events with point, normal and approach speed. **No joints.** Every body mirrors a scene entity | Joints added to `PhysicsWorld`. Ragdoll bodies are runtime-owned and mirror no entity |
| Gameplay | Tick order `pre_physics`, physics, `post_physics`. Behaviors parsed into Flecs components. `impact_effect` copies one glowing sphere entity per shot | Enemies become a behavior. Effects replace the sphere copies |
| Commands | `CommandBus::execute` copies the whole scene per transaction | Animation, particles and ragdoll poses must never go through the bus every frame (D1) |
| Editor | Details is generated from property tables, there is a Material Editor, the Asset Browser has thumbnails | New components appear in Details. A new Effect Editor and an animation preview |

### Libraries

| Library | State | Use |
|---|---|---|
| ozz-animation 0.17.0 (`744eb9d9`, MIT) | Not in `resources`. Read on 2026-11-03: its runtime and offline libraries need nothing else, and with tools, samples, tests and howtos off, its bundled GLFW, gtest and jsoncpp stay out of the build. **A compile under our settings is unverified:** the probe in the scratchpad was not allowed to run | Sampling, blending, additive layers, events, IK |
| Box3D `f36d061b` | Linked. It has spherical joints with cone and twist limits, revolute and motor joints. Its sample `shared/human.c` builds a 12-part ragdoll (MIT) and is not linked | Joints for ragdolls. `human.c` is the reference for limits, friction and springs |
| Filament | Skinning on the GPU, 256 bones per renderable (`CONFIG_MAX_BONE_COUNT`), 4 influences per vertex. `InstanceBuffer` gives each copy a transform only | Skinning. **Not** instancing for particles: it has no per-copy color or fade, so particles are camera-facing quads built on the CPU, one draw per emitter |

Characters: ozz ships Quaternius' Universal Animation Library (CC0), but only as FBX, which the engine does not
import. The Khronos CesiumMan and RiggedSimple models are CC BY 4.0 and in `.gltf` form. See decision 2.

## 2. Architecture

```text
Engine/Assets            CPU: SkinData, SkeletonData, ClipData from GLB; EffectData from .effect text. No ozz
Engine/Animation (new)   ozz behind it: builds skeletons and clips, Animator runtime, blending, events, IK, ragdoll pose mapping
Engine/Effects   (new)   Particle simulation, CPU, visual only. No Box3D, Filament or editor
Engine/Physics           Joints, runtime-owned bodies, ragdoll builder
Engine/Renderer          Bone palettes and particle batches in the frame; skinning and particle.mat under src/desktop
Engine/Gameplay          Enemy behavior, hit reactions, death to ragdoll, effect triggers
Engine/Editor            Details for new components, Effect Editor, animation preview, bone and ragdoll overlays
```

**D1. Bones are not scene entities.** A skeleton's pose lives in the animation runtime, not in the scene, the
undo history or the scene file. A 60-bone character animating at 60 Hz through `CommandBus` would copy the scene 60
times a second. The scene stores only what an author sets: which model, which clip, speed, ragdoll settings.

**D2. Attach points are queries.** `Animation::joint_world(entity, "hand_r")` gives a bone's world transform this tick.
Gameplay aims the muzzle flash with it and effects emit from it. Rendering an entity on a bone (a gun in a hand) uses
an `AttachToBone` component that extraction resolves, again without writing the scene.

**D3. Skin data beside the mesh.** `MeshData` gains `skin`, empty for unskinned models: 4 joint indices and 4 weights
per vertex, inverse bind matrices, and the skeleton (joint names, parents, rest transforms). Skinned primitives keep
their bind-space vertices, as glTF requires, instead of having the node transform baked in. More than 4 influences
keeps the 4 largest and renormalizes, with a warning. A skin above 256 joints is refused with a clear message, as
Filament cannot draw it.

**D4. Clips come from the same GLB.** Each glTF animation becomes a `ClipData`: per-joint translation, rotation and
scale keys. STEP keys are kept as pairs of keys. CUBICSPLINE is resampled to linear at 30 Hz, as ozz stores linear
keys only. A clip is referenced as model asset plus clip name. Clips from another GLB play on a skeleton whose joint
names match; there is no retargeting.

**D5. Clip events in the `.meta`.** glTF has no events, so `footstep at 0.42 s` lives in the model's `.meta`, edited in
the animation preview. ozz's track triggering finds the events crossed in a tick, including across a loop boundary.

**D6. `AnimatorComponent`:** model asset, default clip, speed, loop, start time, and play in edit mode (preview only).
The runtime adds crossfades (`play(clip, blend_seconds)`), one additive layer for hit reactions, and two-bone foot IK.
States are chosen by gameplay code until Milestone 6 adds scripts; there is no state-machine graph in this milestone.

**D7. Ragdolls are generated, then tuned.** `RagdollComponent`: on death, on demand, or never. Bodies are built from
the skeleton: a capsule per chosen bone, sized from the distance to its child, spherical joints with cone and twist
limits from `human.c`. Bone choice, radius, density and limits can be overridden per bone in Details. A ragdoll's bodies
are runtime-owned and mirror no entity. They start at the last animated pose with the velocity it had (pose difference
over the last tick), and the shot's impulse lands on the bone it hit. Raycasts against ragdoll bodies report the
owning entity and the bone.

**D8. Particles never touch gameplay or Box3D.** `Engine/Effects` simulates on its own, with one seeded random stream
per emitter, so Box3D's determinism, Play/Stop restore and gameplay tests are unchanged. Particles collide with a
floor plane or with up to a few raycast results per emitter per tick, never with a raycast per particle.

**D9. `.effect` assets** are text, like `.material`: emission rate and bursts, lifetime, initial speed and cone,
gravity, drag, size and color over life (a few keys each), texture, additive or alpha blending, floor collision,
maximum count, local or world space. `ParticleEmitterComponent`: effect asset, auto play, loop, and an optional bone.
Gameplay fires one-shot effects at a point and direction (`effects.spawn("sparks", point, normal, strength)`).

**D10. Particle drawing.** Each emitter becomes one Filament renderable over a dynamic vertex buffer of
camera-facing quads, rewritten each frame. `particle.mat` is unlit and compiled twice with `matc -T`, once additive and
once alpha-blended. Alpha emitters sort back to front within the emitter. The first budget is 5,000 live particles at
60 fps in the Release editor.

**D11. Solid debris is physics, capped.** Glass and crate pieces that bounce and pile up are Box3D bodies in their
own collision group, at most 64 alive, removed once asleep for 2 s or past their lifetime. Dust and slivers are
particles.

**D12. Scene format 7.** New records for Animator, AttachToBone, Ragdoll and ParticleEmitter. Versions 1 to 6 still
load.

## 3. Slices

| # | Slice | Done when |
|---|---|---|
| 0 | **ozz in the build:** lock entry, `fetch_dependencies.py`, CI fetch list, `gameengine::ozz` with `/MD`, C++20, its warnings-as-errors off for its own targets only if needed, docs/build/dependencies.md | Both MSVC presets configure from fresh folders and a smoke test samples a clip built in code |
| 1 | **Particles:** `Engine/Effects`, `.effect` assets, emitter component, `particle.mat`, frame batches, edit-mode preview, Effect Editor panel with undo | `effects_tests`, readback of additive and alpha quads, real input in the Effect Editor |
| 2 | **Skinned import and drawing:** skin, skeleton and clips from GLB, skinning attributes, `setBones`, bind pose | `asset_tests` on generated GLBs. `--render-test` matches a bind-pose model with its unskinned twin, and a posed one moves pixels where expected |
| 3 | **Animation runtime:** `Engine/Animation`, Animator, crossfades, additive layer, events, attach query, `AttachToBone`, foot IK, preview and scrub in the editor, bone overlay | `animation_tests`, editor tests, real input |
| 4 | **Ragdolls and debris:** joints in `PhysicsWorld`, runtime-owned bodies, ragdoll builder and overrides, pose handoff, bone hits, debris cap | `physics_tests` and new ragdoll cases; a ragdoll settles from a pose with no explosion, deterministically |
| 5 | **The shooter:** enemy behavior that patrols, flinches and dies into a ragdoll. Footstep dust, muzzle flash from the barrel, sparks along the hit normal, glass shards and slivers, crate dust from hard hits | `gameplay_tests`, and the game won and lost with real input |
| 6 | **Performance, full builds, report** | Report from runs made in this milestone |

Cut order if time runs short: foot IK, then debris bodies (particles only), then the Effect Editor's curve editing
(text values stay).

## 4. Tests

| Area | Where |
|---|---|
| Skins, weights, more than 4 influences, joint limits, bind space, clips, STEP and CUBICSPLINE, bad files | `asset_tests` (GLBs built in memory, as now) |
| `.effect` parsing and saving | `asset_tests` |
| Building skeletons and clips, sampling known values, crossfades, additive, events across loops, attach query, IK, determinism | `animation_tests` (new) |
| Emission, bursts, lifetime, curves, gravity, drag, floor collision, maximum count, seeded determinism | `effects_tests` (new) |
| Joints and limits, runtime-owned bodies, ragdoll settle, handoff continuity, bone hits, debris cap and cleanup | `physics_tests`, `physics_scene_tests` |
| Format 7, new commands, versions 1 to 6 loading | `engine.core.acceptance` |
| Enemy states, flinch, death to ragdoll, win counting enemies, Play/Stop isolation | `gameplay_tests`, `engine.editor.acceptance` |
| Details, Effect Editor and preview, without a window | `editor_*_tests` |
| Skinning and particle pixels, GPU resource lifecycle | `--smoke`, `--render-test` |
| Real input | `Tests/RealInput/milestone5.ps1`, with the focus and idle guards |

## 5. Risks

- **ozz may not compile cleanly under C++20 and MSVC 19.44** with its own `/W4` and warnings-as-errors. Slice 0 finds
  out first. The fallbacks are warnings-as-errors off for its targets, then C++17 for its folder only.
- **The skinning variant of our own `pbr.mat`** has never been drawn. Slice 2 checks it before anything builds on it.
- **Animation must stay off the command bus** (D1). A test fails if animating adds scene revisions.
- **Ragdoll stability.** Long joint chains jitter. Start from `human.c`'s 12 bones and settings, and test a fall from
  several poses.
- **A CC0 character may not be available as GLB** (decision 2). The fallback is a generated character, which works
  but looks like a robot made of boxes.
- **CPU particle cost** in Debug builds. Budgets are measured in Release.
- **Real input** needs a free desktop.

## 6. Decisions for the user

1. **Add ozz-animation 0.17.0 to `D:\AI\GameEngine\resources`.** It is read-only without your approval. Recommended:
   yes, by `fetch_dependencies.py` at the pinned commit.
2. **The enemy character.** Recommended: download Quaternius' Universal Animation Library (CC0) in glTF from
   quaternius.com, check its license and record it in `ASSET_LICENSES.md`. Alternative: a generated box robot with
   generated clips, with no download.
3. **Live preview in edit mode** for animations and particles. Recommended: on, as in Unreal. It is visual only and
   never changes the scene.

## 7. Acceptance

- Both MSVC presets build from fresh folders. Every existing test passes, and none is removed or weakened.
- A skinned, animated GLB imports, draws in its bind pose and plays its clips in the editor and in Play.
- Crossfades, a hit flinch, a clip event and a bone attachment work, and can be seen in Play.
- A character turns into a ragdoll on death, starting from its pose and moving the way the shot pushed it.
- Effects are created, edited, previewed, undone and saved in the editor, and fire from gameplay.
- The shooter, with enemies and effects, is won and lost with real input.
- Release reaches 60 fps with 10 animated enemies and 5,000 particles on this PC, or the report says what it reaches.
- The report lists what was verified, how, and what was not.
