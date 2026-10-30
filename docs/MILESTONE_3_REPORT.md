# Milestone 3 report

Branch `agent/claude-milestone-3`, worktree `D:\AI\GameEngine-Integration`. Plan: `docs/MILESTONE_3_PLAN.md`
(approved 2026-10-29).

**Status: complete on `agent/claude-milestone-3`, not merged into `main`.** The brief's 13-step acceptance workflow
passes on a fresh Release build with real mouse and keyboard input. Three Inspector controls were not clicked through
(listed under "What was not verified"). Hosted CI has never run.

This top part is the final result, written on 2026-10-30. The slice sections below are records as of each slice; where
one says something is still open, this part supersedes it.

## Final result

### Deliverables

| Deliverable | Where |
|---|---|
| Editor executable | `build-m3-windows-msvc-release\bin\engine_editor.exe`, run as `engine_editor [--project <folder>]`. It finds its compiled materials by an absolute path into the build tree, so it runs from there and is not yet relocatable |
| Tests | 146 CTest entries, including 23 core scenarios, 12 editor groups and 35 asset cases, plus the GPU smoke (`engine_editor --smoke`). See `docs/testing/README.md` |
| Verification results | Below, and per slice |
| Documented scene format | `docs/architecture/006-scene-format.md`, version 5; versions 1–4 still load |
| Known limitations and defects | Below |

### Acceptance criteria (plan section 5)

| # | Criterion | Result |
|---|---|---|
| 1 | Brief's workflow on the Release editor with real input | **Met**: table below |
| 2 | Same workflow as an automated CPU test | **Met**: `acceptance_workflow` in `engine.editor.acceptance` |
| 3 | Both presets configure, build and pass from fresh directories; nothing removed or weakened; engine targets warning-free under `/WX` | **Met**: below. Physics tests that used a removed API were migrated, as listed under S4 |
| 4 | Every Phase 2–6 feature has a test where practical, plus a real-input check | **Met, with the exceptions listed under "What was not verified"** |
| 5 | This report lists what was verified, how, and what was not | **Met** |

### Fresh-directory builds

Run from new directories `build-m3-windows-msvc-release` and `build-m3-windows-msvc-ci` on Windows 10, MSVC 19.44, with
the code in this commit.

| Preset | Configure | Build | CTest | Core | Editor | Assets | GPU smoke |
|---|---|---|---|---|---|---|---|
| `windows-msvc-release` | exit 0 | exit 0, 883 steps, about 4 min | 146/146 | 23/23 | 12 groups | 35/35 | exit 0 |
| `windows-msvc-ci` (`/WX`, strict lock) | exit 0, dependencies `(locked)` | exit 0, 883 steps, about 4 min | 146/146 | 23/23 | 12 groups | 35/35 | exit 0 |

Compiler warnings: none in engine code. The logs hold warnings from `resources/filament`, plus 2 C4267 in the MSVC header
`xutility` while instantiating Filament's `FixedCapacityVector`. GPU smoke readback on both: surface color (73, 1, 1),
scene light (0, 0, 151), imported texture top (165, 3, 19) and bottom (20, 3, 164).

### Acceptance workflow with real input

Fresh Release executable, new empty folder, real `SendInput` events, a desktop capture after the actions, and the file
read from disk. The driver reported no cursor interference during this run.

| # | Step | Evidence | Result |
|---|---|---|---|
| 1 | Open the editor | Starter scene, clean title | Pass |
| 2 | Create a new scene | **New**: no prompt, 0 entities, "New scene" | Pass |
| 3 | Add a cube | **+ Cube**: 1 entity, stable ID 2, "(unsaved)" | Pass |
| 4 | Select it in the viewport | Empty click clears the Inspector; clicking the cube shows ID 2 | Pass |
| 5 | Move, rotate, scale | X arrow: X 0 → 1.766. Y ring: rotation Y 66.161°. Y scale handle: 1.828. Three "Gizmo transform committed" | Pass |
| 6 | Edit transform in the Inspector | Position Z typed as −1.25; world position 1.766, 0.500, −1.250 | Pass |
| 7 | Save | Ctrl+S, "Scene saved"; `ENGINE_SCENE 5 3 1` / `ENTITY 2 0 "Cube" 1.7659938… 0.5 -1.25 …` / `MESH 2 cube … - -` | Pass |
| 8 | Close and reopen | Close exits with no prompt; relaunched | Pass |
| 9 | Load | **Load**, "Scene loaded (undoable)" | Pass |
| 10 | Transform retained | Inspector: (1.766, 0.500, −1.250), (0, 66.161, 0), (1, 1.828, 1); same on screen | Pass |
| 11 | Enter Play mode | Green border and banner, `PLAYING tick 150`, Undo, Redo, Save and Load disabled | Pass |
| 12 | Stop Play mode | "Stopped; runtime copy discarded", revision still 2 | Pass |
| 13 | Editor scene unchanged | Same Inspector values and view; saving again gives a byte-identical file (SHA-256) | Pass |

An earlier run the same day is not counted. The driver reported the cursor jumping during the scale drag, leaving Y
scale at 0.001. It was repeated from step 1 in a new folder, and the table above is that repeat.

### Brief's phases

| Phase | Delivered | Slice |
|---|---|---|
| 1. Audit | `docs/MILESTONE_3_PLAN.md` section 1 | Plan |
| 2. Scene interaction | Picking, selection sync, gizmos, Local and World, create, delete, duplicate, parent and child tree. All edits go through typed commands into the Flecs-mirrored scene | M2, S1–S3 |
| 3. Inspector | Name, ID, transform; mesh, model, texture and color; camera; light; rigid body; colliders | S2, S4, S5 |
| 4. Asset workflow | GLB, PNG, JPEG (with OBJ, TGA, PPM), Asset Browser, assignment, errors and warnings, references saved in scenes. Filament's gltfio `AssetLoader` is not used, but its parsers are: see D4 in the plan | S5 |
| 5. Persistence | New, Save, Load, components and references in a versioned format, undo and redo | S1, S2, S4, S5 |
| 6. Play mode | Isolated runtime scene, Play, Pause, Step, Stop, physics stepping, editor scene restored | S1, S4 |
| 7. Verification | This report | All |

### S6 changes

| Change | Detail |
|---|---|
| DPI scaling | The window opens at the monitor's scale, kept within its work area, and centered. Fonts are rasterized for that scale and UI sizes follow it. Checked at 150% only: 2160×1350 window, sharp text |
| Moving between monitors | Sizes and text follow the window's content scale each frame. Not exercised on a monitor with another scale |
| `-0.000` | Euler angles round to 0 within 0.0005°, so rotation fields no longer read `-0.000` |
| Fitted collider | `EditorSession::model_collider` replaces the code in `Main.cpp`, so it is now covered by `asset_workflow` |
| Console auto-scroll | Done in S5 |
| Testing guide | `docs/testing/README.md` refreshed |

### Real-input checks closed in S6

Same run conditions, on the fresh Release executable, with DamagedHelmet.glb, Fox.glb and two_tone.jpg.

| Check | Observed | Result |
|---|---|---|
| Inspector Model combo | Lists `(built-in mesh)` and both models; DamagedHelmet replaces the cube; the Mesh row disappears; roughness and metallic become a note | Pass |
| Inspector Texture combo, overriding a GLB's image | Lists `(from the model)` and two_tone.jpg; the helmet turns red and blue | Pass |
| Reload | "Asset re-imported from disk"; the helmet keeps drawing | Pass |
| Fitted collider for a model | Outline encloses the helmet; center (−0.002, 0, −0.187), half extents (0.945, 0.901, 1.000) | Pass |
| Place Fox | Added as "Fox", textured and upright, with skinning and animation warnings at import; Focus frames it | Pass |
| Collider shape: capsule | Fields become radius and half height; the outline changes | Pass |
| Collider local rotation | X typed as 45; the outline tilts | Pass |
| Close | 10 of 10 start and close cycles exited within 0.3 s, 4 of them after editing and saving | Pass |

One check in this batch looked like a failure and was not: an editor was still running after a close, but it was the
instance from the discounted run, which that script never closed.

### What was not verified

- **Inspector controls never clicked through:** Sensor and Report events (the attempt was spoiled by cursor movement,
  and neither box was set), the Kinematic body type, and a primary camera combined with physics. Sensor, events and
  kinematic bodies are covered by `physics_scene_tests` at the bridge level.
- **Hosted CI, and every platform other than Windows x64 with MSVC.** No remote exists. Linux code paths are unbuilt
  here; the build without cgltf and stb_image was only compiled with MSVC.
- **`windows-msvc-debug`** was not built in this milestone.
- **DPI scales other than 150%**, and moving the window to a monitor with another scale.
- **GameNetworkingSockets, miniaudio, Tracy:** out of scope; GNS stays deferred.
- **Symlink-escape rejection in the asset database:** implemented, untested, since symlinks need elevated rights here.

### Known limitations and remaining defects

No known defect blocks the acceptance workflow. These remain:

| Area | Limitation |
|---|---|
| Distribution | The executable depends on its build tree for materials. The build directory must share a drive with `resources` (Filament limitation) |
| Rendering | No shadows and no image-based lighting, so surfaces facing away from every light are black, and a fully metallic surface with no map renders nearly black. Exposure is fixed (sunny 16), so indoor-scale point lights need about 10⁶ lm. No transparency. Everything draws double-sided. A plane at y = 0 fights with the grid |
| Materials | No material assets: a mesh renderer has a color, roughness, metallic and one texture. GLB import uses the base color image only and ignores other maps with a warning. A material with a metallic-roughness map becomes a non-metal with roughness 0.65 |
| Models | Skinned models import in the bind pose, with no animation. A file's own scale is kept, so Fox is far larger than the 40 m grid. One asset per file, with no entity hierarchy from glTF nodes. `.gltf`, external images, Draco, WebP and KTX2 are rejected. Picking uses bounding boxes, not triangles |
| Assets | Importing blocks the UI (the helmet takes about a second). No thumbnails. Imported data stays in RAM until exit. Scene files are not tied to a project: loading one with another project open gives placeholders and problems |
| Physics | Collider outlines have no depth test, and an edge with an end behind the camera is skipped. A runtime edit teleports a dynamic body. Play timing follows wall-clock frames, so identical inputs can land a few millimeters apart |
| Hierarchy | Drag-and-drop reparents but does not reorder. No multi-selection. Duplicates sit exactly on the original |
| Inspector | Values pass through `float`, so 0.2 is stored as 0.20000000298023224. A color change through the picker popup makes several undo steps. Cameras have no frustum gizmo |
| Gizmos | `float` matrices can leave `1.0000001` on untouched scale components. A scale drag past the origin stops near zero (Y 0.001 in the discounted run), which collapses the object until undone |
| Layout | Very small windows are not handled specially. A scaled UI fills a small window faster |
| Build | Two ImGui builds (`gameengine::imgui`, `editor_imgui`) and two entry points (`cmake -S Engine/Editor`) remain. The ImGui renderable is rebuilt each frame, and resizing calls `flushAndWait` |
| Documents | `docs/integration/m2-integration-report.md` is a Milestone 2 record and still describes the old API |

## S5: assets (2026-10-30)

### Delivered

| Feature | Implementation |
|---|---|
| GLB import | `import_glb` in `Engine/Assets/src/gltf_importer.cpp`, using the cgltf copy Filament ships. Reads self-contained binary glTF 2.0: all triangle primitives of the default scene, node transforms baked in, normals generated when absent, one `SubMesh` per primitive with base color factor and image, roughness and metallic factors |
| PNG and JPEG | `import_image` with Filament's stb_image, also used for images embedded in a GLB. Dimensions are checked before decoding |
| Unsupported input | Errors: Draco and other unknown required extensions (WebP included), external buffers or images (a file's content can never make the importer open a path), `.gltf`, corrupt files, limits. Warnings in the Console: skins, animation, morph targets, other maps, transparency, non-triangle primitives |
| Importing files | `AssetDatabase::import_file` copies into the asset folder under a free name (`model_1.glb`), writes a `.meta` with a random ID, rescans and loads once. On failure nothing stays behind. A file already in the folder is not copied |
| Scene format v5 | `MESH` gains model and texture references: 16 hex digit ID plus path, or `-`. IDs resolve first, paths second, so a recreated `.meta` still works. Versions 1–4 still load |
| Resource registry (closes C-5a) | Header-only `renderer::ResourceRegistry` maps `(AssetId, revision)` to `MeshHandle` and the new `TextureHandle`, with a new generation per revision. Frames carry handles only |
| GPU upload | `DesktopRenderer` uploads a mesh or texture when a frame first draws it, with mipmaps and sRGB per metadata, one material instance per part, and frees what no object uses. A handle gone stale is skipped for that frame. An image above the GPU limit is shown white and reported |
| Material | `lit.mat` gains `baseColorMap` and UV0, and built-in meshes gain UVs. Color = tint × part color × image. A GLB's own roughness and metallic win over the entity's |
| Conventions (closes C-5b) | The GPU smoke run imports a two-color image and asserts by readback that its first row is at the far end of a plane seen from above. Its first run failed, showing the image inverted; `lit.mat` now flips v, as gltfio does. Confirmed in a desktop capture too |
| Session | `open_project` (`<project>/Assets`), `import_asset`, `rescan_assets`, `reload_asset`, `assign_asset` (one undo step, adds a mesh renderer if needed), `place_asset` (named after the file, resting on y = 0). Unresolved references draw as a magenta cube, are listed in `asset_problems()` and stay in the scene unchanged |
| Asset Browser | Path field with Import, drop files on the window, Rescan, a list with type and state, Assign to selection, Add to scene (or double-click), Reload. Catalogue issues and import results go to the Console |
| Inspector | Model and Texture combos on the Mesh Renderer. Add Component's fitted collider uses a model's bounds |
| Picking and focus | Models pick by their bounding box. Focus frames a model's bounds instead of its origin |
| Console | Scrolls to the newest message (moved up from S6, since an import error could be out of view) |
| Command line | `engine_editor [--project <folder>]`; the default project is the working directory |

Departures from the plan, made during S5:

- **D4:** one asset per file with `SubMesh` parts instead of one per primitive, and no entity hierarchy from glTF
  nodes. `.gltf` and external images are rejected instead of being read: following paths named in a file's content is
  the risk, and the message says to export a self-contained `.glb`.
- **D5:** the CPU registry is separate from `DesktopRenderer`, so the session and its tests use it without a GPU.
  `lit.mat` was extended rather than adding `standard.mat`, and there is no `MaterialHandle` per asset. Tangent frames
  come directly from normals instead of `geometry::SurfaceOrientation`, because no material uses tangent-space maps.
- **D7:** references arrived in format v5, after the component and physics versions.

### Tests

| Suite | New or changed coverage |
|---|---|
| `asset_tests`, 23 → 35 cases | GLB geometry, transform, material, mirroring node, embedded images (data URI and binary chunk), ignored-feature warnings, 12 rejected inputs, real files from the dependency tree when present (Box, DamagedHelmet, Fox, DamagedHelmetWebp, invalid_accessor_type); PNG, JPEG fixture, corrupt and oversized images; `import_file` naming, stable IDs, errors leaving nothing behind |
| `engine.core.acceptance`, 22 → 23 | New `asset_references`: exact text, round trip, undo, 4 invalid references refused by commands, 11 malformed records refused by the reader, v4 records |
| `engine.renderer.snapshot` | New `invalid surface texture` |
| `engine.editor.acceptance`, 11 → 12 groups | New `asset_workflow`: import, unsupported and broken files, place, assign, undo, picking and focus by bounds, save and load in a new session, Play, lookup by path, missing assets as placeholders with references kept, rescan, reload generations, GLB |
| GPU smoke | Texture orientation by readback (top 166,3,17; bottom 18,3,166) |

Results: `windows-msvc-release` and `windows-msvc-ci` build with no warnings outside `resources/filament` and pass
146/146 CTest entries, in two consecutive runs. GPU smoke exits 0 on both. The build without cgltf and stb_image that
Linux CI uses was only checked by compiling `gltf_importer.cpp` that way with MSVC; hosted CI is still unavailable.

### Real-input verification

Real mouse and keyboard through the same driver, on the Release preset editor, in a fresh project folder, with real
files from the dependency tree. The driver reported no cursor interference.

| Check | Observed | Result |
|---|---|---|
| Import GLB | DamagedHelmet.glb (3.7 MB) by path and Enter: listed as `[model] (loaded)`; Console warns about ignored metallic-roughness, normal, occlusion and emissive maps | Pass |
| Add to scene | Upright, textured helmet resting on the floor; named `DamagedHelmet`; Y 0.901; Inspector says roughness and metallic come from the model | Pass |
| Unsupported and invalid | DamagedHelmetWebp.glb: "requires unsupported extension EXT_texture_webp". AnimatedTriangle.gltf: "unsupported file type '.gltf' (re-export…". CMakeLists.txt: "unsupported file type '.txt'; supported: …". `C:\no\such\file.png`: "file not found". None left a file in `Assets` | Pass |
| Warnings | Fox.glb imports with skinning and animation warnings; invalid_accessor_type.glb with a morph target warning | Pass |
| Textures | two_tone.jpg, material_chart.jpg and NDC.png import; Assign to selection puts the chart on the cube, the combo shows it, the color turns white | Pass |
| Picking and focus | Clicking the helmet selects it; Focus centers its bounds | Pass |
| Save, close, reopen, load | File has `ENGINE_SCENE 5` and both references; after a restart Load shows the same scene and marks it clean | Pass |
| Play and Stop | Models render in Play; Stop returns to the same scene | Pass |
| Missing asset | With the file moved away, Load gives a magenta cube, "Model 'DamagedHelmet.glb' cannot be used: it is not in the asset folder" and the sidecar issue. File back, **Rescan**: the helmet returns | Pass |
| OS file drop | A real OLE drag from a helper window to the viewport: imported as `two_tone_1.jpg` beside the existing `two_tone.jpg`, selected in the list | Pass |
| Orientation on screen | The dropped image on a plane seen from above: red (first row) at the top of the desktop capture, blue below | Pass |
| Undo and close | Ctrl+Z removes the texture; closing gives the unsaved-changes prompt, Discard exits; a clean close exits with no prompt | Pass |

Not exercised with real input in S5: the **Reload** button, the Inspector's Model and Texture combos, a texture
overriding a GLB's image, placing Fox, and Add Component's fitted collider for a model. All of them were checked in S6.

### Limitations

- Only the base color image is used. Normal, metallic-roughness, occlusion and emissive maps are ignored with a
  warning, and a material with a metallic-roughness map falls back to a non-metal, roughness 0.65.
- There is no image-based lighting, so a fully metallic surface with no map renders nearly black.
- Skinned models import in their bind pose, without animation. A file's own scale is kept (Fox is large).
- No transparency, no shadows, no thumbnails. Models are drawn double-sided, like everything using `lit.mat`.
- Importing runs on the UI thread; the helmet takes about a second.
- A plane at y = 0 fights with the grid lines.
- `ResourceRegistry` keeps CPU data for the session, so nothing imported is freed from RAM until exit.
- Scene files are not tied to a project: loading one with another project open gives placeholders and problems.

## S4: physics (2026-10-30)

### Delivered

| Feature | Implementation |
|---|---|
| Physics in the scene (closes C-2a) | `RigidBodyComponent` (kind, initial velocity and spin, damping, gravity scale, can sleep, continuous) and a list of up to 64 `ColliderComponent`s (sphere, box or capsule, with local center and rotation, density, friction, restitution, sensor, events) are part of each entity record, validated and mirrored into Flecs. Commands `SetRigidBody` and `SetColliders`; registry: 12 commands. Colliders without a body form a static collider |
| Scene format v4 | `BODY` and `COLLIDER` records, documented in `docs/architecture/006-scene-format.md`. Earlier versions still load; v3 rejects the new records |
| Bridge reads the scene | `ScenePhysics::sync` converts the components and builds bodies. Its setters, `purge_detached` and the "detached" bookkeeping are removed, because undo now restores the whole record |
| Play-mode simulation (C-1a, C-3) | Each play session has its own bridge stepping once per fixed tick (including Step) and publishing dynamic poses into the runtime copy through its history-free bus. A physics failure pauses Play and is reported |
| Edit-mode bridge | A second bridge mirrors the editor scene without stepping, so collider outlines and issues are available while editing |
| Inspector | Rigid Body and Collider sections, a warning for a body with no colliders, a live speed/mass/sleep readout in Play, and the entity's physics issues. Add Component offers Rigid Body and a collider fitted to the mesh |
| Create menu | Falling Cube, Falling Sphere, Floor (a plane with a thin box collider whose top face is the plane) |
| Viewport and Console (C-4a) | **Colliders** toggle draws `debug_geometry()` as an ImGui overlay, colored by category (static, kinematic, awake, asleep, sensor). Issues appear in the Console once, when they arise |
| Inspector drafts | A field being edited keeps its value until released, since moving bodies change the revision every tick |

### Tests

| Suite | New or changed coverage |
|---|---|
| `physics_scene_tests` | All 24 cases migrated to author through scene commands; assertions kept, except where the API they tested no longer exists: `purge_detached` became `entities_that_lose_physics_components_are_dropped` (also covers component removal, undo and `ReplaceScene`), `authoring_api_validates_input` became `physics_components_are_validated_by_the_scene`, and the `detached_entities` checks became `authored_entities` checks. New: `scene_components_convert_to_physics_descriptions`, `hierarchy_issues_follow_changes_that_keep_the_world_transform` |
| `systems_smoke_tests` | Authors the floor and crate through scene components; same assertions |
| `engine.core.acceptance` | New `physics_components`: storage, Flecs mirroring, undo, documented record text, exact round trip, 10 malformed inputs, several colliders per entity, v3 rejecting `BODY`. Core: 22/22 |
| `engine.editor.acceptance`, 10 → 11 groups | New `physics_play`: edit-mode bridge never steps; a crate dropped on a floor rests at y = 0.5 within 0.02 after 240 ticks; editor scene, revision and history untouched; Stop restores the snapshot exactly; second Play restarts; Pause, Step, teleport by runtime edit; initial velocity; body edit undo; issues from both bridges, including a world-preserving reparent; save/load; duplicates keep physics |

Results: `windows-msvc-release` and `windows-msvc-ci` build with no warnings outside `resources/filament` and
pass 134/134 CTest entries. GPU smoke exits 0.

### Real-input verification

Same driver as S2 and S3, on the Release preset editor. The driver reported no cursor interference in this run.

| Check | Observed | Result |
|---|---|---|
| Create menu and overlay | Floor and Falling Cube created; hierarchy tags "collider" and "dynamic"; outlines green (static) and orange (awake) | Pass |
| Drop and land | Tick 32: y = 1.594, 5.23 m/s, mass 1000 kg, awake (free fall predicts about 5.2 m/s). After landing: y = 0.500, 0 m/s, asleep | Pass |
| Pause and Stop | Pause holds the pose; Stop returns to y = 3 at the same revision | Pass |
| Initial velocity and undo | X velocity 2 m/s: lands 1.44 m away; Ctrl+Z restores 0 | Pass |
| Restitution | 0.8: rebounds to y = 2.06 at tick 83 (about 2.1 predicted) | Pass |
| Collider shape and toggle | Box → Sphere changes the outline; unticking **Colliders** hides all outlines | Pass |
| Nested dynamic body | Dragging one dynamic body under another: Console and Inspector report it, the body's outline disappears; undo restores it. This check found the defect below | Pass after fix |
| Two bodies | Cube and sphere land side by side; sphere mass 523.6 kg (1000 kg/m³ × volume) | Pass |
| Save / New / Load / Play | File has `ENGINE_SCENE 4` with `BODY` and `COLLIDER` records; New empties; Load restores; Play simulates the loaded scene; re-saving after Stop is byte-identical | Pass |
| Add Component | On a plain sphere: warning until a collider exists, Rigid Body greyed out once present, fitted collider is a sphere | Pass |
| Static body | Combo → Static: tag "static", mass 0, stays put while the falling cube lands on it | Pass |
| Close | Unsaved-changes prompt, Discard, "Editor stopped cleanly." | Pass |

Not exercised with real input: Kinematic bodies, capsule colliders, Sensor and Report events, collider local
rotation, primary camera combined with physics. The first four are covered by `physics_scene_tests` at the bridge
level; none of them has been clicked through in the Inspector.

### Defect found and fixed

**A body reparented under a dynamic body was not reported.** The hierarchy check in `sync_entry` ran only when the
entity's world matrix changed, and since S3 a reparent keeps the world matrix. The same early return would have left
a stale issue after moving a body back out, and would have missed a parent becoming dynamic. The check now runs on
every sync. The two new tests failed before the fix and pass after it.

### Limitations

- Collider outlines have no depth testing, and an edge with an end behind the camera is skipped rather than clipped.
- The Console still does not auto-scroll (planned for S6), so a new physics issue can be below the visible area.
- A runtime edit to a dynamic body teleports it; there is no drag-with-force interaction.
- A flat landing can leave a few millimeters of sideways drift (x = -0.003, z = 0.004 in one run). Simulation is
  deterministic for identical inputs, but the Play tick timing follows wall-clock frames.
- `docs/integration/m2-integration-report.md` is a Milestone 2 record and still describes the old API.

## S3: hierarchy and duplication (2026-10-29)

### Delivered

| Feature | Implementation |
|---|---|
| Duplicate command | `commands::DuplicateEntity{id, name}` copies the entity and its whole subtree with components under the same parent, in one transaction (one undo step). Copies get fresh stable IDs in breadth-first order (`Receipt::created`, root first) and are wired to the copied parents. Registry: 10 commands |
| Duplicate in the editor | `EditorSession::duplicate` names the copy "<name> (copy)", trimming to the 256-byte limit without splitting a UTF-8 sequence, and selects it. Toolbar **Duplicate**, Ctrl+D, hierarchy menu. In Play mode it affects only the runtime copy |
| World-preserving reparent | `EditorSession::reparent` sets parent and recomputed local transform in one transaction, so objects do not move on screen; one undo step. It refuses self-parenting, parenting under a descendant (`Cycle`), missing entities, and poses that would need shear under a non-uniformly scaled parent, all without changing the scene. Used by the tree, its menu, and the Inspector's Parent combo (which previously kept the local transform) |
| Hierarchy tree | Real tree in stable-ID order with expand arrows. Drag a row onto another row to parent it there, or onto the empty area below the tree to move it to the root. Right-click menu: Create child (Empty, Cube, Sphere, Point Light), Duplicate, Move to root, Focus, Delete. Picking an entity in the viewport opens its collapsed ancestors. Clicking the empty area clears the selection |
| Parent combo | The entity itself and its descendants are listed but disabled |
| Shortcuts | Delete removes the selected subtree; Ctrl+D duplicates; both are ignored while a text field is active or the unsaved-changes prompt is open |
| Picking against real shapes | Delivered in S2 (box, sphere, plane) |

### Tests

| Suite | New or changed coverage |
|---|---|
| `engine.core.acceptance` | New `duplication` test: whole subtree copied with fresh IDs, breadth-first order, parent wiring, transform and components copied, originals untouched, one undo removes every copy, redo restores the same IDs, empty name keeps the original, missing ID and over-long name rejected, and the copies usable by a later command in the same transaction. Registry size 10. Core: 21/21 |
| `engine.editor.acceptance`, 9 → 10 groups | `hierarchy_editing`: reparent keeps the world matrix (all 16 values) and is one undo step, local transform recomputed, move to root, undo back to the exact original record; cycle, self, missing parent and missing entity refused atomically; shear under a non-uniformly scaled parent refused atomically; subtree duplicate with name, selection, component copies and one-step undo; long names trimmed in the middle of a two-byte UTF-8 tail; duplication in Play mode stays in the runtime copy |

Results: `windows-msvc-release` and `windows-msvc-ci` build with no warnings outside `resources/filament` and
pass 132/132 CTest entries. GPU smoke unchanged and passing.

### Real-input verification

| Check | Observed | Result |
|---|---|---|
| Drag to reparent | Sphere (X = 2) dragged onto Cube: now its child with local (2, 0, 0); world position (2, 0.5, 0) and on-screen position unchanged; "Reparented (world transform kept)" | Pass |
| Ctrl+D | Cube with its child duplicated as "Cube (copy)" with its own Sphere child, selected, one revision | Pass |
| Right-click menu | Lists Create child ▸ (Empty, Cube, Sphere, Point Light), Duplicate, Move to root (disabled on a root), Focus, Delete | Pass |
| Move to root | Child Sphere moved to the root with world position kept (local becomes (2, 0.5, 0)) | Pass |
| Pick reveals tree | With "Cube (copy)" collapsed, clicking its child sphere in the viewport opens the node and highlights the child | Pass |
| Delete key and undo | Delete removes the selected child sphere (4 → 3 entities); Ctrl+Z restores it | Pass |
| Create child, drag to root zone | Child Cube created at local Y 1 (world 1.5); dragged onto the empty area it moves to the root with local Y 1.5 | Pass |
| Parent combo | For "Cube (copy)", itself and its child Sphere are disabled; other entities selectable | Pass |
| Close with unsaved changes | Prompt appears; Discard changes exits with "Editor stopped cleanly." | Pass |

Driver issues found and fixed during the run (not editor defects):

- The first Delete-key press did nothing. The driver sent `VK_DELETE` without the extended-key flag, so Windows
  reported the numpad scan code and GLFW saw keypad-decimal. A real Delete key sets the flag; the driver now does
  too for navigation keys, after which Delete worked.
- Cursor drift again (jumps of up to 530 px) disrupted one drag and one click; both succeeded when retried. While
  idle, the cursor stayed still for 3 s, so the movement was intermittent and external to the driver.

### Limitations

- Drag-and-drop only reparents; it does not reorder siblings (order is by stable ID).
- Duplicates are placed exactly on top of the original, as in most editors; the selection moves to the copy.
- No multi-selection, so Duplicate and Delete act on one subtree at a time.

## S2: component model (2026-10-29)

### Delivered

| Feature | Implementation |
|---|---|
| Components in the scene | `MeshComponent` (built-in cube, sphere or plane; sRGB color, roughness, metallic), `CameraComponent` (vertical FOV, near, far, primary), `LightComponent` (directional or point; sRGB color, intensity in lux or lumens, range). They live in `EntityRecord::components`, so commands, undo/redo, Play-mode copies and files carry them like the transform. `Scene::commit` mirrors them into Flecs components; `Scene::validate` enforces their ranges for every command and every load |
| Commands | `SetMesh`, `SetCamera`, `SetLight` (an empty value removes the component), and `CreateEntity` takes components, so creating a light or camera is one undo step. Registry: 9 commands |
| Scene format v3 | `MESH`, `CAMERA`, `LIGHT` records, documented in `docs/architecture/006-scene-format.md`. v1 and v2 files load with a default cube on every entity, which is how they were drawn before |
| Rendering | `extract_frame` carries a per-object `Surface` (linear color, roughness, metallic, validated to [0, 1]). The desktop renderer has built-in cube, UV-sphere and plane meshes, a lit material `lit.mat` (replaces `cube.mat`) with one material instance per object, and mirrors scene lights into Filament directional and point lights. The editor's two default lights are used only while the scene has no lights |
| Play-mode camera | Play views through the first primary camera component by entity ID. If there is none, or its transform is scaled (not rigid), the editor camera is used and the viewport says why |
| Picking | Ray tests against the real shape in local space (box, sphere, plane) instead of a unit box for everything |
| Editor UI | **Create...** menu (Cube, Sphere, Plane, Empty, Directional Light, Point Light, Camera); Asset Browser buttons for the three built-in meshes; Inspector sections for Mesh Renderer, Camera and Light with **Remove** and **Add Component**; hierarchy tags for lights and cameras; clickable viewport markers for lights and cameras (directional lights also show their direction); a hint when editor lighting is in use |

### Tests

| Suite | New or changed coverage |
|---|---|
| `engine.core.acceptance` | New `components` test: create with components, set/remove each kind, Flecs mirroring (including removal), undo/redo, delete-undo restores components, invalid values and missing entities rejected atomically, exact and deterministic text round trip, 12 malformed component inputs, records in any order. `malformed_serialization`: v1 and v2 files get default cubes; version 4 rejected. Registry size 9 |
| `engine.renderer.snapshot` | Surface carried into the frame; out-of-range and NaN surfaces rejected |
| `engine.editor.acceptance`, 8 → 9 groups | `components_and_rendering`: only mesh components are drawn, built-in handles, sRGB→linear surface and light colors, light extraction; picking misses a sphere at a cube-only corner and hits it as a cube, plane hit and miss; mesh change undo; Play views through the primary camera (lens and pose), a scaled camera falls back with a reason, edit mode never uses scene cameras; component edits in Play stay in the runtime copy; save/New/load round trip with components |
| GPU smoke | Reads back the viewport center: a red surface under editor lighting read **(73, 1, 1)**; the same probe made white and lit only by a blue directional scene light read **(0, 0, 151)**, with exactly one scene light mirrored (editor lights off) |

Results: `windows-msvc-release` and `windows-msvc-ci` build with no warnings outside `resources/filament` and
pass 132/132 CTest entries (`engine_core_tests` 20/20, `engine_editor_tests` 9 groups). The GPU smoke exits 0.

### Real-input verification

Same driver as S1. It now re-asserts the cursor immediately before every button event and reports any cursor
movement it did not make.

| Check | Observed | Result |
|---|---|---|
| Mesh color | Typed R/G/B 1, 0.15, 0.1: three "Mesh renderer changed" commits; the cube renders red | Pass |
| Mesh shape and undo | Mesh combo → Sphere renders a sphere; Ctrl+Z back to the cube; Ctrl+Y sphere again | Pass |
| Create menu | All seven entries listed; Plane (10 × 10) and Point Light created and selected | Pass |
| Scene lights replace editor lights | With a point light present the editor lights switch off and the hint disappears; the light pools on the plane | Pass |
| Light marker | Clicking the point light's marker selects it, also with the plane behind it | Pass |
| Light edits | Intensity typed as 6,000,000 lm and color (1, 0.5, 0.2): three "Light changed" commits, orange and brighter | Pass |
| Directional light and camera | Created; the directional light lights the scene and shows its direction line; the camera is tagged "camera (primary)" | Pass |
| Play through the camera | Banner "Viewing through camera 'Camera'"; the view is from the camera's position; Stop returns to the editor camera | Pass |
| Remove / Add Component | Remove Mesh Renderer hides the cube; Add Component → Mesh Renderer brings it back; two undos restore the original | Pass |
| Save / New / Load | Saved file has `ENGINE_SCENE 3 6 5`, two `MESH`, two `LIGHT`, one `CAMERA` record; New empties the scene; Load restores all five entities | Pass |
| Documented example | The example from `006-scene-format.md`, saved as a file, loads: red sphere, "Sun", "Camera" | Pass |

Issues found by this run:

- **Point-light preset too dim (fixed).** At the first preset, 400,000 lm, the scene looked almost black: the editor
  camera uses daylight exposure (sunny 16, EV100 ≈ 15). The Create-menu preset is now 1,500,000 lm, which
  visibly lights a 2 m radius. Scene exposure control is not implemented; see limitations.
- **Interference, not a defect.** Two marker clicks first failed. Temporary click logging showed the clicks arriving
  up to 15 px away from where the driver had put the cursor, and the drift report showed jumps of up to 140 px
  between positioning and the button press. Something else was moving the mouse while the run was in progress.
  With the cursor re-asserted and the mouse left alone, every marker click selected its light. The S1 step-5 miss
  was most likely the same cause.

### Limitations

- Exposure is fixed (sunny 16) for the editor and Play cameras, so indoor-scale point lights need about 10⁶ lm.
- No shadows (shadowing is disabled in the view) and no image-based lighting, so surfaces facing away from every
  light are black.
- Camera components have no frustum gizmo; the marker shows position only. A camera at the editor eye or behind it
  has no visible marker.
- Inspector values pass through `float` (ImGui), so typed values such as 0.2 are stored as 0.20000000298023224.
- A color change through the picker popup commits on every change (several undo steps); typed values and inline
  drags commit once.

## S1: acceptance slice (2026-10-29)

### Delivered

| Feature | Implementation |
|---|---|
| New Scene | `EditorSession::new_scene()`: an undoable `ReplaceScene` with an empty snapshot; stable IDs are not recycled. Toolbar **New**, Ctrl+N |
| Unsaved-changes tracking | The session keeps the entities as last saved, loaded or started fresh; `dirty()` compares the editor scene with them, recomputed only when the revision changes, so undoing back to the saved state is clean again. The title shows `*`; the toolbar shows "(unsaved)" |
| Unsaved-changes prompt | New, Load and window close ask **Save / Discard changes / Cancel** when the scene is dirty. Closing a minimized window restores it first so the prompt is visible |
| Scene format v2 | `ENGINE_SCENE 2` with a counted `COMPONENTS` section (empty until S2); v1 files still load. Spec: `docs/architecture/006-scene-format.md` |
| Play mode | Play copies the editor scene into an isolated runtime `Scene` with its own history-free `CommandBus`. While playing, the hierarchy, Inspector, gizmos, `+ Cube` and Delete act on the runtime copy; New, Load, Save, Undo and Redo are refused; the viewport has a green border and "PLAY MODE" banner; the title shows `[Playing]`/`[Paused]`. Each play session is a new renderer scene identity |
| Pause / Step / Stop | Pause freezes runtime time; Step advances one fixed tick (1/60 s) while paused; Stop discards the runtime copy. Runtime time advances in whole fixed ticks, at most 5 per frame (a stall drops the backlog) |
| Window title | `GameEngine Editor - <file>[*] [Playing]` |

What Play mode does **not** do yet: no runtime system is connected, so a running scene only changes through
the user's own edits. Physics stepping arrives in S4, behind the same fixed tick.

### Tests

| Suite | New or changed coverage |
|---|---|
| `engine.core.acceptance` (`CoreTests.cpp`) | v2 round trip (existing test now writes v2); rejects versions 0 and 3, a missing or malformed `COMPONENTS` section, a negative count, unknown component kinds, trailing data, and a v1 file with a `COMPONENTS` section; v1 files load and are rewritten as v2 |
| `engine.editor.acceptance` (`EditorTests.cpp`), 5 → 8 groups | `documents`: dirty tracking through edit, undo, New Scene (undoable, no ID reuse), save, load and undoing a load. `play_mode`: runtime copy and identity, runtime edits incl. world-space gizmo edits, editor scene/revision/history untouched, document and history commands refused, tick accounting (fixed ticks, remainder, burst cap, pause, step, resume), Stop clears runtime-only selection, a second Play restarts from the editor scene, editor undo still works afterwards. `acceptance_workflow`: the brief's 13-step workflow without a window (below) |
| GPU smoke (`engine_editor --smoke`) | Now also plays, edits the runtime copy (move + create), checks that the runtime is ticking and rendered (2 objects), stops, and checks that the editor snapshot and revision are unchanged |

Results: `windows-msvc-release` and `windows-msvc-ci` (`/WX` on engine targets) both build and pass 132/132
CTest entries. The CTest count is unchanged because the new coverage lives inside existing executables;
`engine_editor_tests` prints `8 editor acceptance groups passed`, `engine_core_tests` `19/19 tests passed`. The
GPU smoke prints `GPU smoke passed: 91 frames; transform, undo/redo, save/load, play/stop isolation, resize and
capture.` and exits 0. No warnings outside `resources/filament`.

### Acceptance workflow with real input

Method as in the M2 follow-up audit: real `SendInput` mouse and keyboard events to the Release-preset editor
(`build-m2-windows-msvc-release\bin\engine_editor.exe`), a desktop capture after each action, and the saved file
read from disk. The driver binds to the PID it launched.

| # | Step | Evidence | Result |
|---|---|---|---|
| 1 | Open the editor | Title `GameEngine Editor - Untitled.scene` (clean starter scene) | Pass |
| 2 | Create a new scene | Toolbar **New**: no prompt (clean), 0 entities, Console "New scene" | Pass |
| 3 | Add a cube | **+ Cube**: 1 entity, Stable ID 2, title gains `*` | Pass |
| 4 | Select it in the viewport | Click empty space (Inspector clears), click the cube (Inspector shows ID 2) | Pass |
| 5 | Move, rotate, scale | X arrow: X 0 → 1.984. Y ring: rotation Y 81.524°. Y scale handle: scale Y 1.944. Each "Gizmo transform committed" | Pass |
| 6 | Edit transform in the Inspector | Position Z typed as −1.25 → Z −1.250, world position updated | Pass |
| 7 | Save | Ctrl+S; title loses `*`; file `ENGINE_SCENE 2 3 1` / `ENTITY 2 0 "Cube" 1.98… 0.5 -1.25 …` / `COMPONENTS 0` | Pass |
| 8 | Close and reopen | WM_CLOSE exits with no prompt (clean), `Editor stopped cleanly.`; relaunch | Pass |
| 9 | Load | **Load**: no prompt (the starter scene was clean); Console "Scene loaded (undoable)" | Pass |
| 10 | Transform retained | Inspector: position (1.984, 0.500, −1.250), rotation (0, 81.524, 0), scale (1, 1.944, 1) | Pass |
| 11 | Enter Play mode | Green border and banner, `PLAYING tick 67`, New/Undo/Redo/Save/Load disabled, title `[Playing]`. In Play: Y-arrow drag moved the runtime cube; Ctrl+Z refused ("Undo is unavailable in Play mode; press Stop first"); + Cube created runtime entity ID 3. Pause held the counter at 1890 for 0.7 s; Step → 1891 | Pass |
| 12 | Stop Play mode | **Stop**: banner gone, 1 entity, title without `*`, revision still 2 | Pass |
| 13 | Editor scene unchanged | Inspector shows the step-10 values; saving again produced a file byte-identical (SHA-256) to the step-7 save | Pass |

Unsaved-changes prompt, same run: + Cube then WM_CLOSE → the prompt "…before closing?" appears and the
process stays alive; **Cancel** keeps the editor and the changes. **New** → "…before starting a new scene?" →
**Discard changes** gives an empty, clean scene. + Cube, WM_CLOSE → **Discard changes** exits cleanly. The saved
file was left untouched throughout (hash unchanged).

### Observations and limitations

- One drag, the first attempt at step 5, did not engage the gizmo, and the selection was empty afterwards; the
  combo click after it did not open the combo either. Neither repeated in this run: the retried drag, the clicks at
  the same spot and every later combo click behaved correctly. Probably the drag started just off the arrow (the
  driver aims by pixel); not reproduced, so no fix was made.
- Gizmo edits go through `float` matrices, so untouched scale components can come back as `1.0000001`.
- The P3 items from the M2 audit are still open (DPI font scale, Console auto-scroll, small-window dock layout,
  hierarchy in ID order rather than as a tree).
- `docs/testing/README.md` still describes the M1/M2 suite list and counts; it will be refreshed with the S6 report.
- Hosted CI: still unavailable (no Git remote).
