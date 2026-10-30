# Milestone 3 report

Branch `agent/claude-milestone-3`, worktree `D:\AI\GameEngine-Integration`. Plan: `docs/MILESTONE_3_PLAN.md`
(approved 2026-10-29).

**Status: in progress. Slices S1–S4 are complete and verified; S5 and S6 are not started.** Milestone 3 is not
complete: imported assets (GLB, textures) are still missing.

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
