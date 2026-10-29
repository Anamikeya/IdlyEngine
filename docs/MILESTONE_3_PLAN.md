# Milestone 3 plan: functional 3D editor

Branch `agent/claude-milestone-3` (from `main` at `37db654`), worktree `D:\AI\GameEngine-Integration`.
Status: **approved 2026-10-29. S1 and S2 complete; S3 next.** Progress and evidence: `docs/MILESTONE_3_REPORT.md`.

M3 turns the M2 prototype editor into a usable scene editor without replacing Filament, Flecs, ImGui docking,
GLFW or the typed `CommandBus`. M3 work is not merged into `main` without explicit approval.

## 1. Phase 1 audit: what works today

Source: real mouse/keyboard input against the Release editor on 2026-10-29, read from desktop captures, plus a
code read of `Engine/Editor`, `Engine/Renderer`, `Engine/Scene`, `Engine/Commands`, `Engine/Physics`,
`Engine/Assets`. Evidence: `docs/MILESTONE_2_INTEGRATION_AUDIT.md` section 11.

| Area | Control | State |
|---|---|---|
| Rendering | Filament OpenGL viewport in an ImGui dock, grid, lit cubes | Works. Every entity is drawn as the built-in cube with one blue material; two fixed editor lights |
| Viewport | Click picking, orbit / pan / zoom / focus | Works. Picking is a ray against unit cubes only |
| Gizmos | Move / rotate / scale, Local / World combo | Works; one undo step per drag |
| Hierarchy | Select, double-click focus | Works. Flat list in ID order with indentation, not a tree; no drag-reparent, no context menu |
| Toolbar | + Cube, Delete, Focus, Undo, Redo, Save, Load, scene path | Works. No New Scene, no Duplicate, no Play/Pause/Stop, no unsaved-changes guard |
| Inspector | Name, stable ID, local TRS, parent combo, world position | Works. "Render binding" is static text. No mesh, material, camera, light or physics components exist in the scene model |
| Asset Browser | "Built-in: Cube", list of `.scene` files beside the scene path | Lists scene files only. No import, no assets; `engine::assets` (OBJ/TGA/PPM) is built and tested but not linked into the editor |
| Console | Log of command results | Works; does not auto-scroll |
| Persistence | `ENGINE_SCENE 1` text format, atomic save, undoable load | Works for id, name, parent, TRS only |
| Undo / redo | Ctrl+Z / Ctrl+Y and buttons | Works for every command above |
| Play mode | none | Missing. `engine::physics_scene` (Box3D) is built and tested but not connected |

Decorative or misleading controls: none found. The "Render binding" text is accurate for M2 and goes away when
the component exists.

## 2. Gaps against the M3 brief

| Brief item | Gap |
|---|---|
| Picking, selection sync, gizmos, Local/World | Done in M2. Picking must use real mesh bounds once meshes vary |
| Create / delete | Done. **Duplication missing**. Hierarchy needs a tree view with drag-to-reparent |
| Inspector components | Scene model has no components beyond name/parent/transform: **needs a component model** (mesh renderer, camera, light, rigid body, colliders) in `Scene`, commands, serialization and undo |
| Asset workflow | No GLB, no PNG/JPG, no import UI, no renderer resource registry (C-5a), mesh conventions unconfirmed (C-5b) |
| Persistence | **New Scene** missing; format has no components or asset references; no dirty tracking |
| Play mode | Missing entirely; physics bridge not connected (C-1a, C-2a, C-3) |

## 3. Design decisions (recommended; approval needed)

**D1. Components live in the scene record.** Extend `EntityRecord` with optional components:
`MeshRenderer {mesh: AssetRef, material: MaterialDesc}`, `Camera {fov, near, far, primary}`,
`Light {kind, color, intensity, range}`, `RigidBody`, `std::vector<Collider>`. `Scene::commit` mirrors them into
Flecs components. New typed commands (`SetMeshRenderer`, `SetCamera`, `SetLight`, `SetRigidBody`,
`SetColliders`, `RemoveComponent`) go through `CommandBus`, so validation, revisions and undo/redo (deltas are
before/after `EntityRecord`s) work unchanged. Rejected alternative: a physics sidecar file (C-2a option b), which
would split one entity across two undo histories.

**D2. Physics authoring moves into the scene (closes C-2a).** `ScenePhysics` becomes runtime-only: `sync()` reads
`RigidBody`/`Collider` from scene records. Its own authoring maps and the "detached" bookkeeping are removed,
because undo now restores the whole record. `scene_physics_tests` are migrated to author through commands. Same
coverage, no weakened assertions.

**D3. Play mode runs on an isolated runtime scene.** Play copies the editor `SceneSnapshot` into a second
`Scene` with its own history-free `CommandBus` (actor `"runtime"`, `history_limit = 0`, closing C-1a for this
use) and its own `ScenePhysics`. The viewport renders the runtime scene while playing. Pause stops stepping; Step
advances one fixed tick; Stop destroys the runtime scene and the editor scene is shown again, untouched. Editor
authoring commands are disabled during Play. Fixed step 1/60 s via `ScenePhysics::fixed_update` (C-3). The
alternative, snapshot and restore on the same scene, would interleave runtime writes with editor history.

**D4. GLB via cgltf into the existing CPU asset pipeline.** Parse `.glb`/`.gltf` with `cgltf` (vendored in
`resources/filament/third_party/cgltf`, the parser Filament's own gltfio uses). Produce engine `MeshData` per
primitive (positions, normals, UV0, indices) and decode embedded or external PNG/JPEG images with the `stb_image`
copy Filament ships. The renderer registry (D5) uploads them. Filament's `gltfio::AssetLoader` was considered and
rejected: it creates its own Filament entity tree outside Flecs, which breaks "editing modifies the real Flecs
scene", and it needs the ubershader archive build. Unsupported glTF features (skins, animation, morph targets,
Draco, KTX2/Basis, non-triangle primitives) are reported as import errors or warnings, never silently dropped.

**D5. Renderer resource registry (closes C-5a).** `DesktopRenderer` gains `register_mesh(AssetId, revision,
shared_ptr<const MeshData>)` and `register_material(...)` returning generation-checked `MeshHandle` /
`MaterialHandle`. The registry uploads `VertexBuffer`/`IndexBuffer`, computes tangent frames with Filament's
`geometry::SurfaceOrientation`, re-uploads when the revision changes, and keeps the CPU data alive until the GPU no
longer needs it. One new lit material `standard.mat` takes a base-color factor, an optional base-color texture,
roughness and metallic. The built-in cube stays handle `{1,1}`. Lights in the scene are drawn through the existing
`LightBinding` path; the fixed editor lights become an "editor lighting" toggle used when a scene has none.

**D6. Mesh conventions (closes C-5b).** Right-handed, +Y up, meters, counter-clockwise front faces, 32-bit
indices, UV origin top-left of the image rows. glTF already matches; OBJ is already converted. Confirmed with a
checker-textured quad and a GPU readback test in which the orientation is asserted, not assumed.

**D7. Scene format version 2.** The text format stays line-based and human-readable: header
`ENGINE_SCENE 2 <next_id> <count>`, then `ENTITY` lines as in v1, followed by optional component lines keyed by
entity (`MESH`, `MATERIAL`, `CAMERA`, `LIGHT`, `BODY`, `COLLIDER`). Asset references are stored as the 16-hex
`AssetId` plus the virtual path for diagnostics. A missing asset loads as a visible "missing asset" placeholder
with a Console error, not as a failed load. v1 files still load. The reader keeps the 64 MiB and token limits.
Documented in `docs/architecture/006-scene-format.md`.

**D8. Project folder.** `engine_editor --project <dir>` (default: the working directory). Assets live under
`<dir>/Assets` and are managed by `engine::assets::AssetDatabase`. Import copies the source file into `Assets/`
and writes a `.meta` with a new `AssetId`. Import is triggered from an Asset Browser path field plus Import button,
or by dropping files onto the window (`glfwSetDropCallback`).

## 4. Implementation order (vertical slices, each built, tested and run before the next)

| Slice | Content | Done when |
|---|---|---|
| **S1: acceptance slice** | New Scene (undoable `ReplaceScene` with an empty snapshot), dirty tracking with a confirm dialog on New/Load/close, scene format v2 header with v1 compatibility, Play/Pause/Stop on an isolated runtime scene (D3, no physics yet), window title `GameEngine Editor` | The 13-step acceptance workflow passes with real input, plus an automated CPU test of the same workflow |
| **S2: component model** | D1 for `MeshRenderer` (built-in cube / sphere / plane, material color), `Camera`, `Light`; Inspector sections with Add/Remove Component; format v2 component lines; undo; renderer draws scene lights and colors; Play renders from the primary scene camera | Component edits round-trip through save/load and undo; GPU readback shows color and light changes |
| **S3: hierarchy and duplication** | Tree view with drag-to-reparent and a context menu; `DuplicateEntity` (subtree, components, one transaction = one undo step); Delete key, Ctrl+D; picking against mesh bounds | Editor tests cover duplicate/undo and reparent cycles; real-input check |
| **S4: physics** | D2 (scene-owned physics authoring, closes C-2a), Inspector for rigid body and colliders, play-mode stepping, collider debug overlay drawn as ImGui viewport lines (C-4a), runtime issue list in the Console | A dropped dynamic box lands on a static floor in Play; Stop restores the editor scene exactly; physics tests migrated and passing |
| **S5: assets** | D5 registry, `standard.mat`, D6 test, D4 GLB importer plus PNG/JPEG, D8 import UI and drag-drop, Asset Browser listing (type, status, errors), assigning a mesh/texture to an entity, asset references in v2 files | A `.glb` imports, appears in the browser, is assigned to an entity, survives save/close/reopen/load; unsupported files are reported |
| **S6: report and polish** | `docs/MILESTONE_3_REPORT.md`; Console auto-scroll; DPI font scaling; known-limitations list | Report written from runs made in this milestone |

S1 comes first because the acceptance test only needs New Scene and Play/Stop on top of what already works.

## 5. Acceptance criteria

Milestone 3 is complete only when all of these hold:

1. **Brief acceptance workflow** passes on the Release editor with real input (the input driver from the M2
   follow-up run), each step checked on a desktop capture: open the editor → New Scene → add a cube → select it in
   the viewport → move, rotate and scale it with gizmos → edit its transform in the Inspector → save → close and
   reopen the editor → load → the transform matches → Play → Stop → the editor scene is unchanged (compared by
   `SceneSnapshot` equality and on screen).
2. The same workflow runs as an automated CPU test in `engine.editor.acceptance` (no window).
3. `windows-msvc-release` and `windows-msvc-ci` configure, build and pass CTest from fresh directories; no test
   removed or weakened; engine targets warning-free under `/WX`.
4. Every Phase 2–6 feature above has a test where practical (commands, serialization round trips, importer
   positive and negative cases, registry generation checks, play-mode isolation, physics landing), plus a
   real-input check recorded in the report.
5. `docs/MILESTONE_3_REPORT.md` lists what was verified, how, and what was not.

## 6. M2 carryovers

| Item | Disposition in M3 |
|---|---|
| C-1a runtime pose path | Closed by D3 (runtime scene with its own history-free bus) |
| C-2a physics persistence/undo | Closed by D1/D2 |
| C-4a debug-draw route | ImGui viewport overlay (S4) |
| C-5a renderer resource registry | D5 (S5) |
| C-5b mesh conventions | D6, with a readback test (S5) |
| Two build entry points (`cmake -S Engine/Editor`) | Keep as a thin shim in M3; the presets are the documented path. Removal proposed for M4 |
| Two ImGui builds (`gameengine::imgui`, `editor_imgui`) | Unify on `editor_imgui` if S5 touches the build anyway; otherwise carry |
| P3: ImGui renderable rebuilt per frame, `flushAndWait` on resize, `-0.000` Euler display, Console auto-scroll, DPI font scale, small-window dock layout | Console, DPI and `-0.000` fixed in S6; the rest carried unless profiling shows a problem |
| Hosted CI never run (no remote) | Carried; needs a GitHub remote from the user |
| Build dir must share a drive with resources | Carried (Filament limitation) |
| GNS (protobuf), miniaudio, Tracy | Out of M3 scope |

## 7. Risks

- **Scale.** S1–S5 is large. If time runs short, S1–S3 deliver the acceptance workflow and a real component
  model; S4/S5 would then be reported as incomplete, never stubbed.
- **D2 touches a tested subsystem** (868 lines plus 755 lines of tests). Migration keeps every existing assertion.
- **glTF variety.** Only static, triangle-list glTF 2.0 with core PBR base color is in scope; everything else is
  reported as unsupported.
- **Real-input checks** depend on an unlocked interactive desktop and move the real mouse while they run.
