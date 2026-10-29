# Milestone 3 report

Branch `agent/claude-milestone-3`, worktree `D:\AI\GameEngine-Integration`. Plan: `docs/MILESTONE_3_PLAN.md`
(approved 2026-10-29).

**Status: in progress. Slices S1 and S2 are complete and verified; S3–S6 are not started.** Milestone 3 is not
complete: duplication and the hierarchy tree, physics in Play mode, and imported assets are still missing.

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
