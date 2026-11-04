# Milestone 5 plan: characters and effects

Branch `milestone-5`, worktree `D:\AI\GameEngine-Integration`, created from `main` at `14c6642`.
**Status: approved 2026-11-03. Slices 0 to 4 done; slice 5 (the shooter) done but for its real-input run; slice 6 next.** Do not merge into `main` without the user's approval.

Decisions (2026-11-03): the plan is approved as written. ozz-animation 0.17.0 goes into `resources` at its pinned commit.
The enemy is Quaternius' Universal Animation Library (CC0), downloaded in glTF. Edit-mode preview is on by default.

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

## 8. Progress

### Slice 0: ozz in the build (done 2026-11-03)

- ozz-animation fetched into `resources` at `744eb9d9` by `fetch_dependencies.py`, and pinned in
  `cmake/DependencyLock.cmake`. CI fetches it on both jobs.
- `GAMEENGINE_WITH_ANIMATION` (on) adds ozz with the DLL runtime and its tools, samples and tests off, as
  `gameengine::ozz`. Configure stops with a message on CMake older than 3.30.
- **The compile risk is closed:** ozz builds under C++20 with MSVC 19.44 with no warnings, keeping its own `/W4` and
  warnings-as-errors.
- `animation_tests`: 5 cases. One first failed by 0.45 mm. That is ozz's 16-bit rotation keys, not a bug, so poses
  are compared at 1 mm per metre, and the precision is recorded in `docs/build/dependencies.md`.
- 414 of 414 CTest entries pass in Release and in the CI configuration (warnings as errors, strict lock), both in
  the existing editor-UX build folders. Fresh-folder builds of both presets are left to slice 6, as they rebuild
  Filament.

### Slice 1: particles (done 2026-11-03)

- **Scene format 7:** an `EMITTER` record for `ParticleEmitterComponent` (effect, auto play, loop, rate scale) and its
  `component.emitter.set` command. Versions 1 to 6 still load; a version 6 file with an `EMITTER` is refused.
- **`.effect` assets:** text like materials, documented in `docs/architecture/006-scene-format.md` with an example a test
  compares against the writer. Created, saved, reloaded, validated and imported by the asset database.
- **`Engine/Effects`:** CPU simulation with a seeded splitmix64 stream per emitter: bursts and rate per cycle, cone and
  sphere spawning, gravity, drag, spin, floor bounces, size and color curves (interpolated in linear light), world or
  local space, a particle cap, one-shots (at most 256) and triggered emitters. It never touches the scene, physics or
  gameplay.
- **Renderer:** `particle.mat` (unlit, compiled additive and transparent). Each batch is one renderable over a vertex
  buffer of quads rebuilt each frame, camera-facing or stretched along the velocity, alpha batches sorted back to front,
  capacity doubling. A generated soft dot is the default image. Frames clean batches (sorted, deduplicated, non-finite
  particles dropped, 200,000 at most).
- **Session and gameplay:** an edit-mode particle system (View > Play Effects, on by default) and one per Play session,
  which pauses with the run and is discarded on Stop. Gameplay's `impact_effect` template fires its emitter's effect
  along the hit normal through `effect_events()`, adding nothing to the scene. Effect drafts update living particles
  without restarting them; saving restarts them and is one undo step.
- **Editor:** the Effect Editor panel, the Particle Emitter in Details and Add Component, New effect, the Effect chip,
  placing and assigning effects, Create > Particle Emitter, View > Play Effects and Restart Effects, `r.effects` in the
  Console.
- **Tests:** 446 of 446 CTest entries (new: `effects_tests` 15, `editor_effects_tests` 11, `asset_tests` +5,
  `gameplay_tests` +1, a format 7 scenario). `--render-test` 48 of 48 with 9 particle checks, and `--smoke` pass.
- **Deviation from the plan:** particles are not drawn with Filament instancing, which gives each copy only a transform;
  quads built on the CPU carry each particle's color and fade (D10 already said so).
- **Found by testing:** a check sampling the middle row of the view read the editor grid, which lies edge-on there; checks
  now sample below it. A test bound a reference to a member of a temporary record through `ENGINE_CHECK_EQ`; tests now
  copy the value first.
- **Not done here:** the shooter's own effects (sparks, dust, glass) are slice 5. Real input runs used the CI-style build
  (`build-ux-windows-msvc-ci`), because an editor started on 2026-11-02 still holds the Release build's executable.

### Slice 2: skinned import and drawing (done 2026-11-04)

- **Skeletons (D3):** a GLB gets a skeleton when a mesh in its scene is skinned or a node in it is animated. The skeleton
  holds those joints and every node above them, parents first, with unique names and rest translation, rotation and
  scale (a node's matrix is split; shear is lost). At most 1,024 joints.
- **Bones:** a palette of at most 256 bindings, each a joint and an inverse bind matrix. Skinned primitives keep their
  bind-space vertices and ignore their node's transform, as glTF says. Every other part of a skinned or animated model is
  baked as it rests and follows the nearest joint above it rigidly, or the model itself. All `JOINTS_n`/`WEIGHTS_n` sets
  are read, repeated joints merged and the 4 strongest kept and renormalized, with a warning; bones exist only for kept
  influences. A vertex with no weight stays where it was bound. More than 256 bones, a joint index outside its skin, or a
  non-finite weight refuses the model. `mesh_scale` scales joint, key and inverse bind translations with the vertices.
- **Clips (D4):** each glTF animation becomes a clip of per-joint translation, rotation and scale keys at linear
  interpolation. STEP keys are held until 1 ms (or a quarter of the gap) before the next key; CUBICSPLINE is sampled at
  30 Hz or finer, keeping the keys. Rotations are normalized. A channel without keys holds the rest value. Unnamed clips are
  "Animation N", repeated names are numbered, and a clip with times that do not increase is skipped with a warning.
  Morph weight channels and skins no mesh uses are ignored with a warning.
- **`Engine/Assets/skeleton.h`:** compose, invert, rest and model poses, skinning palette, skinned positions, and a box for
  any pose from each bone's own box (every vertex is a weighted mean of its bones' transforms of it). Bounds of a mesh are
  its rest pose.
- **Renderer:** frames carry an optional `Pose` per object: model-space joint matrices behind a shared pointer, so a frame
  copies a pointer (D1). Extraction refuses a non-finite or oversized pose. Skinned meshes get a second vertex buffer of
  bone indices and weights; renderables are built with the rest palette and take `setBones` and a new box only when the
  pose changes. A pose that does not fit the skeleton draws the rest pose. `RendererStats` counts skinned and posed
  objects. Thumbnails are drawn skinned at rest.
- **Editor:** `EditorSession::set_pose`/`pose` hold poses by entity, outside the scene, its history and its file; new
  scenes, loads and projects clear them. Picking and vertex snapping use the pose's box and skinned vertices. The Asset
  Browser tooltip lists a model's joints and animations.
- **Tests:** 457 of 457 CTest entries (new: `asset_tests` +9 in `skin_tests.cpp` on GLBs built in memory, and Fox from the
  dependency tree; `editor_viewport_tests` +2). `--render-test` 59 of 59 with 11 skinning checks: a skinned bar at rest
  matches its unskinned twin pixel for pixel, posing moves exactly the predicted pixels, a pose outside the rest box is not
  culled, a pose of the wrong size draws the rest pose, deleting frees the renderable, and Fox draws at rest and in a frame of
  its Run clip.
- **Found by testing:** the render test showed imported models drawing from an unfilled index buffer after the vertex
  buffer change; the line that fills it was restored. The old "PBR model is visible" check could not have caught it (the
  empty background passed it), so it now compares with the same view without the model. Vertex snapping skipped posed
  models whose rest box was away from the cursor; it now uses the pose's box, as picking does.
- **Not done here:** clips are imported but nothing plays them yet; that is slice 3's Animator. Real input was not needed:
  the only UI change is the tooltip text.

### Slice 3: animation runtime (done 2026-11-04)

- **Scene format 7, extended (D12):** an `ANIMATOR` record (clip source model, clip, speed, loop, start time, edit preview) and
  an `ATTACH` record (a joint of the parent's model), with `component.animator.set` and `component.attach.set`. Format 7 was
  not released, so it grew rather than becoming 8; version 6 files with either record are refused.
- **Clip events in the `.meta` (D5):** `event = <seconds> <name> "<clip>"` lines, checked and sorted.
  `AssetDatabase::set_clip_events` rewrites the sidecar without re-importing the model or bumping its revision, and a move
  keeps them.
- **`Engine/Animation`** on ozz, which stays behind its header:
  - `Rig` builds an ozz skeleton from the model's joints and maps ozz's depth-first order to the model's.
  - `ClipSet` binds a model's clips to a rig, or another model's clips by joint name. A joint without keys holds its rest
    value: ozz would make it identity. Rotation keys more than 0.2 rad apart get slerped keys between them, because ozz's
    nlerp lagged glTF's slerp by 0.9 degrees a quarter of the way through a quarter turn.
  - `AnimationSystem` runs animators by entity:
    - Clips at a speed (negative plays backwards), looping or held at the end.
    - Crossfades, and one layered clip played once as its difference from its first frame (the flinch).
    - Events crossed per update, across loops and backwards.
    - Scrubbing and pausing, and two-bone IK chains with world targets.
    - `pose`, `joint_model` and `joint_world` queries (D2), and bit-for-bit repeatable runs.
- **Session:**
  - **Edit mode:** an edit-mode system plays animators whose preview is on (View > Play Animations).
  - **Play:** a per-run system advances with the 60 Hz fixed tick, so gameplay in slice 5 stays in step. Stop throws it away.
  - **Poses:** frames use the animator's pose unless the host set one. Placing a model that has clips adds an Animator on its
    first clip.
  - **Attachments:** attached entities and their subtrees are drawn on the joint through a world override in `MeshBinding`
    and `LightBinding`. Emitters follow it too. Gizmos, outlines, focus, floor snapping, copying, `set_world` and drags
    work in that drawn space. Picking follows from the frame.
  - **Clip events:** editing them is one undo step that rewrites the `.meta`.
  - **Bones:** bone lines for the overlay.
- **Editor:**
  - **Details:** Animator and Attach To Bone sections, through a new text property type that offers the model's clips or
    the parent's joints and accepts a typed name. Both are in Add Component.
  - **Animation panel:**
    - Play/Pause, Restart, and a timeline to scrub, with event marks.
    - Per clip: Play (crossfade), Layer, and Default (one undo step).
    - Events: add at the playhead, drag a time, Go, Delete.
    - The events that fired, and Show bones.
  - **View menu:** Play Animations, Restart Animations and Show Bones. The Console has `r.animations` and `view.bones`.
- **Tests:** 480 of 480 CTest entries in Release and in the CI configuration.
  - New: `animation_tests` +14, `editor_animation_tests` 7, `asset_tests` +2, and a format 7 animation scenario in
    `engine.core.acceptance`.
  - `--render-test`: 66 of 66, with 7 animation checks:
    - The animator poses the bar from its clip.
    - A cube attached to the tip is drawn on it and follows it.
    - The scene revision does not move.
    - Fox walks through its animator.
  - `--smoke` passes.
  - Real input: `Tests/RealInput/milestone5_animation.ps1`, 22 of 22. Evidence is in `docs/evidence/m5-animation/`.
- **Deviations from the plan:**
  - Clip events are found by the engine's own crossing code, not ozz's track triggering job: it is exact for the events of
    a `.meta`, both ways and across loops.
  - Foot IK is the runtime's two-bone IK (`set_ik`, tested). Nothing probes the ground for it yet; that comes with the
    enemies in slice 5.
  - The Animation panel floats and starts closed. In the right-hand dock, a fifth tab pushed the Inspector's tab out of
    sight.
- **Found by testing:**
  - Checks bound a reference to a member of a temporary (`status()->clip`, `history_names().back()`) through
    `ENGINE_CHECK_EQ` and read garbage; they copy first now, as in slice 1.
  - The first real-input runs found tabs out of reach. Docked with the others, the Animation panel's tab was scrolled out
    of the right-hand dock. With only the four older tabs, the Inspector's still was: it had been since slice 1 added the
    Effect Editor tab, and the older scripts that click it would have failed. Tabs now shrink to 40 px before the bar
    scrolls (ImGui's default is 80), and the run checks that every tab of that dock is in sight. A click on the hidden tab
    had closed the whole dock.
  - Placing Fox now adds an animator, so the slice 2 render check that poses Fox by hand removes it first.
  - Found in review: both caches of rigs and clips held a model, so neither saw itself as its last holder and they were
    never freed. They hold it weakly now, and are pruned after the asset database lets go. A test checks it.
  - The Bone list of a 25-joint model scrolled its name field out of view. The field is now at the top, has the
    keyboard and narrows the list.

### Slice 4: ragdolls and debris (done 2026-11-04)

- **Joints in `PhysicsWorld`:** ball joints with cone and twist limits, hinges and filter joints, each with friction (a
  motor held still with a torque cap) and an optional spring; joint state for checking limits; bodies taken out of the
  simulation and put back; angular impulses. Box3D stays behind the header.
- **The ragdoll builder** (`engine/physics/ragdoll.h`, plain data, D7):
  - **Bones:** skinned bones at least 4% of the skeleton's size long get a capsule. A bone runs to its unpaired child that
    carries most of the skeleton (the spine from the hips, the neck from the chest), or between paired children.
  - **Limb ends:** the end of a limb shorter than the bone above it (a hand, a foot) rides on that bone's body as a second
    capsule, as Box3D's `human.c` does. At most 64 parts; past that the shortest bones go.
  - **Sizes:** from the skin, using the box of the vertices each joint moves most, kept within 8% to 35% of the bone's
    length or 8% of the skeleton.
  - **Joints:** joint frames run along each bone and are at rest when the skeleton is. Swing and twist come from the
    settings (40 and 20 degrees, within `human.c`'s range). Joint friction is a fraction of each part's own weight.
    Joined parts weigh within 4:1 of each other.
  - **Settings per bone** by name: body as generated, a body of its own, or none; radius, density, swing and twist.
  - **Placing:** parts start at the pose, moving as the pose moved over the last tick. A joint the pose has turned past its
    limits gets them widened to fit, so starting never fights the joints.
- **Bodies the run owns, in `ScenePhysics`:**
  - **Ragdolls:** one per entity, at most 64. Their parts never collide with each other. The entity's own body sits out
    while it lies, even when rebuilt. Contacts and ray hits name the entity, and hits also name the ragdoll and the bone;
    sensors do not see them. An impulse through the entity lands on its nearest part, so the shooter's shots push ragdolls.
  - **Debris (D11):** at most 64, the oldest going to make room. A piece goes once asleep for 2 s or past its lifetime,
    counted in simulated ticks. It has a category bit of its own, and its contacts name no entity.
  - Both go with `reset_simulation`, and a ragdoll goes with its entity. They never write the scene.
- **Animation:** `place_joints` makes a pose from the joints a ragdoll's bodies place, and keeps every other joint where it
  was relative to its parent. `previous_pose` gives the handoff velocity.
- **Scene format 7, extended (D12):** `RAGDOLL` (trigger, density, swing, twist, joint friction) and `RAGBONE` records, and
  `component.ragdoll.set`. Version 6 files with either are refused.
- **Session:**
  - `ragdoll_plan` sizes the skeleton at the entity's uniform scale; each joint's flesh is cached per model like rigs.
  - `start_ragdoll` works in Play only. It uses the pose and the one before it, adds the entity's own body's speed, and takes
    an optional impulse at a point; `stop_ragdoll` undoes it.
  - Each tick poses the ragdolls from their bodies. Frames, joint queries (D2), bones and bounds use the shown pose: the
    host's, else the ragdoll's, else the animator's.
  - `ragdoll_lines` outlines the ragdolls.
  - `spawn_debris` draws a piece like a template entity at that entity's scale. Pieces use frame keys the run owns
    (`renderer::runtime_object_base`), and are never picked or snapped to.
- **Editor:**
  - **Details:** a Ragdoll section with Falls, density, swing, twist and joint friction, and the body count and mass (hover
    for each body). Show ragdolls, Add bone settings, a section per bone, and Ragdoll now / Stand up while running.
  - **Menus:** Add Component > Ragdoll, View > Show Ragdolls, Play > Ragdoll Selected and Play > Drop Debris. The Console has
    `view.ragdolls`.
- **Tests:** 519 of 519 CTest entries in Release and in the CI configuration.
  - New: `physics_tests` +17, `physics_scene_tests` +14, `editor_ragdoll_tests` 8, and a format 7 ragdoll scenario in
    `engine.core.acceptance`.
  - **Settling:** a generated person falls from six poses: standing, slumped, sitting in the air, twisted past its limits,
    face down, and thrown backwards spinning.
    - Its energy never rises above the start's, and no part moves faster than 7.5 m/s.
    - Joints stretch by up to 6 cm as a limb hits the floor and close to under 3 mm.
    - Every fall ends asleep on the floor within 10 s, and the same fall repeats bit for bit.
  - **Fox:** plans 17 bodies, 46 kg at 1:100. It falls in Play from its walk and starts exactly where the walk left it.
  - **`--render-test`:** 71 of 71. Fox falls, and the skeleton's top drops from 0.71 m to 0.18 m with 41,277 pixels changed.
    Debris is drawn like its template and goes on Stop. `--smoke` passes.
  - **Real input:** `Tests/RealInput/milestone5_ragdoll.ps1`, 18 of 18. Evidence is in `docs/evidence/m5-ragdoll/`.
- **Deviations from the plan:**
  - Only ball joints are generated. `human.c` uses hinges for knees and elbows, but a skeleton does not say which joints are
    knees; hinges are in `PhysicsWorld` for later.
  - Hands and feet ride on forearms and calves unless asked for. On joints of their own, these light ends whipped about
    (77 rad/s) and stretched their joints by 8 cm at impact. Damping and more sub-steps did not help.
  - Limits are centred on the rest pose, then widened per joint to fit the pose a ragdoll starts from.
  - Nothing kills an entity into a ragdoll yet: On death is stored, and slice 5's enemies use it. Ragdolls fall in Play
    through Details or Play > Ragdoll Selected.
  - Debris collides as a box around its template's look. The shooter's glass and crate pieces come in slice 5.
- **Found by testing:**
  - Speed and spin limits flagged honest impacts, such as a hand slapping the floor. The settle test now checks energy instead:
    an explosion is energy from nowhere.
  - Fox's first plan weighed 71 kg, with fat parts, because each joint's box held every vertex any weight reached. Sizing
    from each vertex's strongest joint gave 46 kg.
  - Taking a hand's body away made its forearm look like the end of a limb, so it rode on the upper arm, and a test crashed on
    the missing body. What ends a limb is now the builder's own choice.
  - In Play, animators only get their inputs when a frame is drawn. The editor tests now tick the way the editor runs, with
    a frame between ticks.
  - Real input found Ragdoll now clipped off the right of the docked Inspector, the third button on its row. Details buttons
    now go beside the last one only when they fit. The script checks that an item is wholly inside its panel: clipped items
    keep their place in the UI map. Earlier runs also picked combo rows without opening the combo, a fault in the script.
  - `small` is a Windows macro.

### Slice 5: the shooter (done 2026-11-04)

- **The enemy character (decision 2):** Quaternius' Universal Animation Library, Standard version (CC0), from OpenGameArt.
  Its Godot GLB is `Samples/PhysicsShooter/Assets/models/Mannequin.glb`, unchanged: a 1.83 m mannequin, 53 joints and 46
  clips, among them `Walk_Loop`, `Idle_Loop`, `Hit_Chest` and `Death01`. The license, source and both checksums are in
  `ASSET_LICENSES.md` and `asset_manifest.json`. Its `.meta` adds two footstep events to `Walk_Loop`, at the frames where
  each foot plants, worked out by sampling the clip.
- **Gameplay** (`Engine/Gameplay`), still with no animation or editor dependency:
  - **`enemy`:** patrols between two points, faces the way it walks, stands at each end, and turns at a rate. A shot
    staggers it for a moment.
  - **Requests to the host:** the game asks for animations by role (walk is the Animator's clip, idle and flinch are found
    by name, the flinch layered on top). It asks for ragdolls with the killing shot's push and point.
  - **Death:** an entity whose Ragdoll falls on death stays in the scene as a fallen target, and takes no more damage.
    `standing_targets()` lists the targets still up.
  - **`hear()`:** the host passes clip events with the entity's lowest joint, and footsteps fire the `footstep_effect`
    template there.
  - **`muzzle_effect`:** a template under the gun fires at its world position along the aim, every shot.
  - **`dust_effect`:** hit events above a speed fire dust at the contact, once a cooldown for each body.
  - **`breakable` and `shard`:** destroyed, an entity breaks into debris like the shard template, spread through its box,
    thrown along the shot from a seeded stream, and fires the template's slivers.
  - **`GameCounters`:** running totals for tests and logs.
- **Session:** each tick it hears the run's clip events (not for ragdolls), plays what the game cued, and starts the
  ragdolls it asked for. Problems go to `game_issues()`.
- **The game log:** lists only standing targets, at their first collider's middle (an enemy's chest), and the game's counters.
- **The sample**, regenerated, now in format 7:
  - Effects in `Assets/effects/`: sparks, muzzle flash, crate dust, footstep dust and glass slivers. Their templates sit
    below the floor.
  - The impact template fires sparks instead of copying a mesh. A Muzzle sits at the barrel's tip, and the glass barrier
    breaks into 14 shards.
  - Three enemies patrol open lanes with 75 health, worth 200 each. There are 12 targets now.
- **Tests:** 533 of 533 CTest entries in Release and in the CI configuration; `--render-test` 71 of 71 and `--smoke` pass.
  - New: `gameplay_tests` +9, `asset_tests` +1, and `editor_shooter_tests` 4, which play the real sample with the session.
  - The sample's own tests now count 12 targets. Winning goes by standing targets, and the glass test stops firing once it
    breaks, because an enemy walks behind it.
  - **Real input: not run yet.** `Tests/RealInput/shooter_acceptance.ps1` gains steps 17 to 22 and kills the enemies to
    win. Both attempts stopped before step 1 because the machine was locked; nothing was played. The run is still owed.
- **Deviations from the plan:**
  - Foot IK does not probe the ground: it is first in the cut order, the arena's floor is flat, and the runtime's two-bone IK
    from slice 3 is there.
  - Enemies do not attack. The plan asks for patrolling, flinching and dying; losing is still the clock.
  - Clips are found by convention (Walk is the Animator's clip; Idle and Flinch the first with "idle" and "hit" in the name),
    because behaviors hold numbers only.
- **Found by testing and review:**
  - The sample's glass test kept firing after the glass broke and killed the enemy walking behind it.
  - Idle went to the first clip with "idle" in its name, the mannequin's `Crouch_Idle_Loop`. A name that starts with the
    word now wins; a test checks that an enemy stands in `Idle_Loop`.
  - Enemies sent a transform every tick, even standing; only a change is sent now.
  - A real-input run that could not take focus at the start left its editor open, because every script starts the editor
    before its own `finally`. `Start-Editor` now closes it. A locked desktop passed the idle wait and failed as "someone is
    using the desktop"; the wait now also waits out the lock screen.
  - Running test executables from the repository's folder left their scratch folders there; they were removed.
