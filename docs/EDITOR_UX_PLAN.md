# Editor UX plan: Unreal-style shortcuts, viewport, panels and workflow

Branch `feature/ui-ux`, worktree `D:\AI\GameEngine-Integration`, created from `main` at `8d12a03` (Milestone 4 merged).
Reference: `docs/UNREAL_UX_REFERENCE.md`. Blueprints are out of scope.

**Status: Phase 2 done, waiting for approval to start Phase 3.** Results and deviations are in section 9.

## 1. Audit

### 1.1 Language, build, run

C++20, CMake 3.25+, Ninja, MSVC 2022. The editor target builds on Windows/MSVC only.

```powershell
cmake --preset windows-msvc-release      # Also: windows-msvc-debug, windows-msvc-ci
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
<build>\bin\engine_editor.exe [--sample] [--project <folder>] [--scene <file>] [--smoke] [--render-test <folder>]
```

Dependencies come from `GAMEENGINE_RESOURCES_DIR` (`D:\AI\GameEngine\resources`). Baseline from Milestone 4: 207 CTest entries, 2 GPU tests, 39 render checks.

### 1.2 UI framework

Dear ImGui 1.93.1 WIP, docking branch, pinned in `cmake/DependencyLock.cmake`. GLFW is the platform layer and `Engine/Renderer/src/desktop/ImGuiRenderer.cpp` draws through Filament.

- Docking is on. Multi-viewports (OS windows) are not: the renderer has one swap chain and no viewport support.
- ImGui draws raw Filament texture pointers, so more image tokens (thumbnails) are simple to expose.
- Editor code lives in `Engine/Editor`. Nearly all UI is one struct, `EditorUI`, in `src/Main.cpp` (1439 lines).

### 1.3 Input

GLFW callbacks feed ImGui. Shortcuts are hard-coded `ImGui::IsKeyPressed` calls in two places:

- `EditorUI::draw()` (`Main.cpp:1145`): Ctrl+Z, Ctrl+Shift+Z, Ctrl+Y, Ctrl+S, Ctrl+N, Ctrl+D, Delete. Blocked while a text field has focus, a gizmo is in use, the game holds the mouse, or the unsaved-changes dialog is open.
- `EditorUI::viewport()` (`Main.cpp:933`): RMB orbit, MMB pan, wheel zoom, F focus, only while the viewport is hovered.

While the game holds the mouse, `GameInput` reads GLFW directly, sets ImGui's `NoMouse` and turns keyboard navigation off. There is no registry, no contexts and no rebinding. Menu items spell out their shortcuts by hand.

### 1.4 Undo and redo

Exists, and is sound. `engine::commands::CommandBus` runs typed commands as atomic, validated batches and stores per-entity before/after deltas (128 steps). Missing: names, transactions spanning several `execute` calls, and history browsing.

- Runtime scenes use a history-free bus, so Play mode cannot pollute undo.
- Material edits sit in a second pair of stacks in `EditorSession`, ordered against the scene by recording `scene_depth`.
- The dirty flag compares the scene with the last saved entities, so undoing back to the saved state clears `*`. The title already shows `*`.

### 1.5 Reflection and inspector

No reflection. The Inspector is hand-written ImGui over plain structs in `Engine/Scene/include/engine/scene/Scene.hpp`, about 40 property rows across `component_sections`, `physics_sections`, `lighting()` and `material_editor()`. It edits a draft of the selected entity and commits on release. Behaviors are already generic `(key, number)` lists.

### 1.6 Selection

A single `EntityId selected_` in `EditorSession` with `selection()`, `select()` and `reconcile()`. Panels poll it, so they are not coupled, but there is no multi-select and no change event. Selection changes are not undoable, which already matches Unreal.

### 1.7 Scene, serialization, assets, Play

| Area | State |
|---|---|
| Scene | Flecs-mirrored `EntityRecord`s with stable IDs. Mutated only through `CommandBus` |
| Scene files | Text format v6, read back to v1 (`docs/architecture/006-scene-format.md`), atomic save |
| Assets | `AssetDatabase`: stable IDs in `.meta` sidecars, `.glb .obj .png .jpg .tga .ppm .hdr .material`, background import with `std::async`, CPU mesh data held while referenced |
| References | `AssetRef` holds ID and path; the ID wins, so moving a file with its `.meta` keeps references working |
| Play | Exists: `play`, `pause`, `step`, `stop`, `restart` on an isolated runtime copy, so Stop restores the scene exactly. Game view through the primary camera, HUD in `GameHud.cpp` |
| Text config | No JSON library. `.meta`, `.material` and scenes are small line-based text formats |

### 1.8 Camera, gizmo, grid

- **Camera:** `OrbitCamera` (target, yaw, pitch, distance). No fly mode, smoothing, persistence or animation. Focus snaps. FOV is fixed at 60 degrees, and `pan` assumes it.
- **Projection:** perspective only. `extract_frame` rejects anything else, so orthographic views need a small change there.
- **Gizmo:** ImGuizmo from `box3d/extern/imguizmo`, already linked. Hover highlight, plane handles, constant screen size, snapping parameters and `GetHoveredMoveType` are available. Mode and space are chosen in toolbar combos.
- **Gizmo gap:** the transform commits on release, so **the object does not move during a drag**, only the gizmo does. The matrix also passes through `float`.
- **Picking:** ray against built-in shapes; models by bounding box. `pick` returns no distance.
- **Grid:** fixed 1 m lines to ±20 m, built once in `DesktopRenderer`; on or off through `show_grid`.

### 1.9 Panels

| Panel | Does today |
|---|---|
| Toolbar | New, + Cube, Create menu, Duplicate, Delete, Focus, Undo, Redo, Save, Load, Project menu, Play controls, scene path field, gizmo combos, Colliders checkbox |
| Scene Hierarchy | Tree, click select, double-click focus, drag to reparent, context menu with create child, duplicate, move to root, focus, delete |
| Inspector | Name, transform, parent, world position, mesh, camera, light, environment, rigid body, colliders, behaviors, Add Component |
| Lighting | Presets, environment source, sky, exposure, tone mapping, AO, bloom, shadows |
| Material Editor | Live preview sphere, all PBR factors and map slots, saves on release, undoable |
| 3D Viewport | Filament image, gizmo, picking, light and camera markers, collider outlines, game HUD |
| Asset Browser | Import path, Rescan, New/Duplicate material, Assign, Add to scene, Reload, flat text list |
| Console | Scrolling text messages (128). No input, no severity beyond an `Error:` prefix |

### 1.10 Code style

- `Engine/Editor`, `Scene`, `Commands`: 4 spaces, `.hpp`, PascalCase files and types, snake_case functions and members, trailing `_` on private members.
- `Engine/Assets`, `Gameplay`, `Physics`: tabs, `.h`, snake_case files.
- Errors: `Result<T>` and `fail(ErrorCode::..., message)`. Exceptions only at host edges.
- Comments are short and explain why.
- Warnings: `/W4 /permissive- /utf-8`, plus `/WX` on the session library and its tests. Not on the `engine_editor` executable, so warnings there must be checked by hand.
- Tests: `Tests/Framework` with `ENGINE_TEST` and `gameengine_add_test_executable`, one CTest entry per case. `engine.editor.acceptance` is a single executable with its own `main`. `Tests/RealInput` drives the real editor.

## 2. Design decisions

**D1. Name: `EditorCommand`, not `Command`.** `engine::commands::Command` is already the scene mutation type. Scene commands change data; an `EditorCommand` is what a user can trigger, and usually issues scene commands.

**D2. Testable core without ImGui.** Registry, keymap, chord parsing, fuzzy scoring, selection, transactions, camera controller, CVars and property metadata go in `engine_editor_session` or a sibling library, with no ImGui or GLFW. A thin adapter maps ImGui keys to the editor's own `Key` enum.

**D3. Undo stays delta-based, with transactions added.** The bus already stores per-entity before/after records, which is the "snapshot" approach at entity size. A full-scene snapshot would be wasteful and command-pair undo would discard working code.

- `CommandBus` gains `begin_transaction(name)`, `end_transaction()` and `abort_transaction()`. While one is open, each `execute` still validates, commits and bumps the revision, so the renderer and physics see live changes. Deltas merge per entity (first `before`, last `after`).
- A transaction that nets to nothing adds no step. Multi-entity edits past the 256-per-call limit split across calls inside one transaction.
- `EditorSession` wraps this as `begin_transaction` and a scoped `ScopedTransaction` guard. Nested guards join the outer one.
- A session-level `HistoryEntry` list (name, kind) replaces the `scene_depth` ordering, so scene, material and folder edits share one ordered history. It follows the bus when old steps are evicted.
- History panel jumps are repeated undo or redo.

**D4. Editor-only data goes in a sidecar, `<scene>.editor`.** It holds folders, hidden and locked flags, and bookmarks, in a line-based text format. The scene format stays at v6, so scene files keep their exact bytes and a missing sidecar is harmless. Folder edits are undoable; hide and lock are view state, as in Unreal.

**D5. Prefs in `%APPDATA%\GameEngine\`.** Keymap, layouts, Details favorites and collapse state, camera speed, snap values. Today `imgui.ini` lands in the current folder, which is how an untracked `imgui.ini` appeared in the repo root. Format is `key = value` text, matching existing conventions and needing no new dependency. A new `--config-dir <folder>` lets tests run with clean settings. The folder name is one constant, so renaming the engine stays easy.

**D6. Reflection by registration tables in the editor layer.** `Scene.hpp` stays plain data, so runtime code gains nothing. Each component gets a table of `PropertyInfo` (name, display name, type, category, tooltip, min, max, step, read-only, hidden, typed get and set, condition). A property reads and writes a component copy, and a component-level function turns that copy into its `Set*` command, which is what makes generic multi-edit possible.

- `editCondition` becomes a predicate, because many conditions are enums (spot cone only for spot lights).
- Default values come from default-constructed components.
- Behaviors stay dynamic rows without per-key metadata for now.

**D7. No new third-party dependencies.** ImGui docking and ImGuizmo are already here. Native file dialogs use the Win32 API.

## 3. Keymap conflicts

Nothing below is overridden silently; each row is the proposed resolution.

| Input | Today | Requested | Proposed resolution |
|---|---|---|---|
| Ctrl+S, N, Z, Y, Shift+Z, D, Delete | Same meaning | Same | Move to commands, behavior unchanged |
| F | Focus, viewport hover only | Focus | Same; animated |
| RMB drag | Orbit | Look | Follow the request. Orbit moves to Alt+LMB |
| Wheel | Zoom | Only RMB+Wheel is specified | Keep: wheel alone dollies, RMB+Wheel sets speed |
| LMB | Click selects | Drag moves and yaws | Click (under the drag threshold) selects; gizmo and markers take priority over drag |
| **Esc while playing** | Frees mouse and pauses (M4 tested) | Stop | First Esc frees the mouse and pauses, second Esc stops. Approved 2026-10-30; built in Phase 4 |
| Esc while editing | Nothing | Deselect | Gizmo drag or typed value cancels first, then deselect |
| R | Game: restart after win or loss | Scale gizmo | Possessed game owns it; once ejected (F8) it is scale |
| WASD, Space, Shift, mouse | Game reads them while it holds the mouse | Editor actions | Registry is suspended while captured, except Esc, Shift+F1, F8 |
| Q, E, W, A, S, D, Z, C | None | Fly while RMB is held; W, E gizmo otherwise | Camera state machine owns them during navigation |
| Space | ImGui keyboard nav activates the focused widget | Cycle gizmo | Cycle only when the viewport is the active context; elsewhere it stays nav |
| Alt | ImGui nav may focus a menu bar on Alt release, once one exists | Many Alt+ chords | Test in Phase 1. If it interferes, claim Alt with `SetKeyOwner` |
| Alt+LMB on a gizmo handle | None | Both orbit and duplicate drag | A press on a handle duplicates; anywhere else orbits |
| Alt+2, Alt+3 | None | Wireframe, Unlit | Renderer has neither. Registered but greyed out with a tooltip, per "no new render modes". Alt+4 Lit works |
| Ctrl+Space | None | Asset drawer | Toggles input method on some Windows setups. Keep; rebindable |
| `` ` ``, `[`, `]` | None | Console, grid size | Position differs on non-US layouts. Keep; rebindable |

**Deviation:** key chords are all rebindable, including held fly keys. Mouse gestures (Alt+LMB, RMB look, Alt+drag, Shift+drag) are listed in the Shortcuts panel but read-only at first, because they belong to the camera and gizmo state machines rather than to chords.

## 4. Feature map by phase

Paths are relative to `Engine/Editor`. Size is relative effort.

**Enabling work, first commit of Phase 1:** split `EditorUI` out of `Main.cpp` into `src/ui/` with no behavior change, then keep `Main.cpp` as the host loop and test hooks. Every later change would otherwise pile onto one file.

### Phase 1: Foundation

| Feature | Status | Work and files | Size |
|---|---|---|---|
| `EditorCommand` and `CommandRegistry` | MISSING | New `include/engine/editor/EditorCommands.hpp`, `src/EditorCommands.cpp` | M |
| Menus, toolbar, context menus from commands | MISSING | Add a main menu bar; replace hand-written items in `src/ui/` | M |
| Context routing, text-field guard | PARTIAL | New `src/ui/InputRouter.cpp`; replaces both hard-coded blocks. Hovered viewport first, then the focused panel, then Global | M |
| Keymap file, overrides, import, export, reset | MISSING | `EditorCommands.cpp`, D5 | S |
| Keyboard Shortcuts panel | MISSING | New `src/ui/ShortcutsPanel.cpp`: search, groups, click to rebind, conflicts, reset | M |
| `docs/EDITOR_SHORTCUTS.md` | MISSING | Generated by `engine_editor --dump-shortcuts`; a test checks it is current | S |
| Transactions and guard | PARTIAL | `CommandBus`, `EditorSession`, D3 | M |
| All mutations named and transacted | PARTIAL | All already pass through the bus. Add names at each call site | S |
| Drags merge to one step | PARTIAL | Already one step by committing on release. Make them live through a transaction: gizmo first, scrubbing in Phase 3 | M |
| Undo History panel | MISSING | New `src/ui/HistoryPanel.cpp` | S |
| Dirty flag, `*`, Ctrl+S clears | EXISTS | Keep; add `*` to the viewport tab | S |
| `SelectionSet` | PARTIAL | New `include/engine/editor/Selection.hpp`: set, primary, change event. `selection()` stays as the primary, so existing callers keep working | M |
| Multi-select gestures | MISSING | Ctrl+click toggle in viewport and tree, Shift+click range in tree | S |
| Reflection metadata | MISSING | New `PropertyMeta.hpp`, `src/PropertyTables.cpp`, D6. Panel uses it in Phase 3 | M |
| `--config-dir`, `--ui-map` | MISSING | Test support, see section 5 | S |

Tests: registry, chord parsing, keymap round trip and conflicts, routing order, text-field guard, transaction merge, abort, nesting, empty transaction, eviction, history jump, material and scene order, selection events and reconcile, metadata against components.

### Phase 2: Viewport feel

| Feature | Status | Work and files | Size |
|---|---|---|---|
| Camera state machine | MISSING | New `CameraController.hpp` and `.cpp`, no ImGui, unit tested. `OrbitCamera` state becomes position and orientation with a pivot | L |
| RMB+WASD, Q/E, RMB+Wheel, Shift, smoothing, speed overlay | MISSING | Same | M |
| Z/C temporary FOV | MISSING | Controller supplies FOV to `frame()`; `pan` stops assuming 60 degrees | S |
| Alt+LMB, Alt+RMB, Alt+MMB; LMB, RMB, MMB drag; LMB+RMB up and down | PARTIAL | RMB orbit and MMB pan exist. LMB+RMB is from the reference, not the prompt; included because it is cheap | M |
| Hide and lock cursor, restore position | MISSING | Reuse the capture approach in `GameInput`; restore with `glfwSetCursorPos` | S |
| Camera speed saved | MISSING | Prefs | S |
| F animated framing | PARTIAL | `focus()` snaps. Ease over 0.2 s to selection bounds, multi-select included | S |
| Bookmarks Ctrl+0-9, 0-9 | MISSING | Sidecar, animated recall | S |
| Alt+G/H/J/K views | MISSING | Orthographic lens in `RenderSnapshot`, `extract_frame`, `ImGuizmo::SetOrthographic`; render test added. Touches the renderer boundary | M |
| W/E/R, Space, Ctrl+` | PARTIAL | Combos exist; add commands | S |
| Hover highlight, plane handles, constant size | EXISTS | From ImGuizmo; check they are enabled | S |
| Object moves during a drag | MISSING | Live transaction, D3. Keep the matrix in `double` | M |
| Live delta readout | MISSING | Text near cursor | S |
| Alt+drag duplicate | MISSING | One transaction, multi-select aware | S |
| Shift+drag camera follow | MISSING | Controller follows the delta | S |
| Hold V vertex snap | MISSING | Feasible: `MeshData` vertices are on the CPU; built-in shapes are analytic. Nearest vertex to the cursor | M |
| Snap overlay, `[` `]`, camera speed widget | MISSING | `src/ui/ViewportOverlay.cpp`; pass `snap` to `Manipulate`. Grid mesh needs a scale so lines match the size | M |
| End snap to surface | MISSING | Ray downward against rendered shapes, `pick` extended with distance. Models hit by bounding box, as picking does today | S |
| Drag asset to viewport | PARTIAL | `place_asset` puts it at y = 0. Add drag source and spawn at ray hit, ground plane as fallback | S |
| G game view | MISSING | Hide gizmo, markers, grid, outlines | S |
| F11 maximize | MISSING | Viewport covers the main window | S |
| H, Ctrl+H | MISSING | `MeshBinding::visible` exists; skip hidden lights; sidecar | S |
| Show flags dropdown | PARTIAL | Colliders exists. Add grid, markers, shadows, AO, bloom, skybox as viewport overrides | S |

### Phase 3: Panels

| Feature | Status | Work and files | Size |
|---|---|---|---|
| Generated Details panel | PARTIAL | Rewrite `component_sections`, `physics_sections`, inspector body over D6 tables; custom drawers for Vec3, color, enum, Euler, AssetRef; list editors for colliders and behaviors | L |
| Categories, search | PARTIAL | Headings exist, not collapsible or searchable | S |
| Drag-to-scrub, Shift fine, Ctrl coarse, click to type | PARTIAL | `DragFloat` covers drag and typing; add modifiers | S |
| Reset arrow, Show only modified | MISSING | Compare with default | S |
| Multi-edit, "Multiple Values" | MISSING | Set on each component in one transaction | M |
| Copy, Paste, Reset menu | MISSING | Clipboard text per type | S |
| Lock, Favorites | MISSING | Prefs for favorites | S |
| editCondition | PARTIAL | Hand-coded `if`s become predicates | S |
| X/Y/Z colors, tooltips | MISSING | Drawer and metadata | S |
| Asset fields: thumbnail, picker, browse, use selected | PARTIAL | Combo exists; depends on thumbnails below | M |
| Outliner drag reparent, double-click focus, two-way sync, context menu | EXISTS | Rebuild context menu from commands | S |
| Runtime entities in Play | EXISTS | `scene()` is the runtime copy | S |
| Folders | MISSING | Sidecar, undoable, D4 | M |
| Eye, lock | MISSING | Table columns; locked skips picking | S |
| Search, type filter | MISSING | | S |
| F2 rename, Ctrl+G group, Ctrl+C/V, Ctrl+A | MISSING | Group: new Empty parent plus reparent in one transaction. Copy and paste reuse `write_scene` text on the system clipboard | M |
| Folder tree, grid and list | MISSING | Tree derived from asset paths | M |
| Thumbnails | PARTIAL | Material preview renders one sphere today. Textures: GPU token. Materials and meshes: offscreen queue, cached to disk. Type icons until ready | L |
| Color strip, chips, search, favorites, tooltips | MISSING | `AssetMetadata` has size; dimensions and triangles from loaded data | S |
| Drag to Details | MISSING | Drop target on asset fields | S |
| Ctrl+B, Ctrl+E | MISSING | Ctrl+E opens Material Editor for materials; others greyed out | S |
| Rename and move | MISSING | New `AssetDatabase::move`; moves file and `.meta`, then updates stale paths in the scene in one transaction. Material map paths too | M |
| Ctrl+Space drawer | MISSING | Same browser content in a slide-up overlay | S |
| Dockable tabs, floating inside the window | EXISTS | | S |
| Tear-off to separate OS windows | MISSING | See section 5. Approved as a follow-up after Phase 5 | L |
| Named layouts, Reset | PARTIAL | Layout persists through `imgui.ini`; `LoadIniSettingsFromMemory` and files under prefs | S |
| Window menu from commands | MISSING | | S |

### Phase 4: Workflow

| Feature | Status | Work and files | Size |
|---|---|---|---|
| Alt+P Play, exact restore | EXISTS | Add command; Stop restores by design | S |
| Alt+S Simulate | MISSING | `play(Simulate)`: game stays in its Ready phase, where the world simulates and the timer does not; no capture | S |
| Shift+F1 | PARTIAL | Esc frees the mouse today; add command that does not pause | S |
| F8 eject and possess | MISSING | `frame()` override to the editor camera; replace the `!game` gates in `viewport()` with "possessed"; clear held input; allow selection and gizmo on the runtime scene | M |
| Border tint | PARTIAL | Green border exists when no game runs; show it always | S |
| Command palette Ctrl+Shift+P | MISSING | New `FuzzyMatch.hpp`, `src/ui/CommandPalette.cpp`, recents in prefs | M |
| Quick open Ctrl+P | MISSING | Same matcher over assets and entities | S |
| Console input, autocomplete, history | MISSING | `src/ui/ConsolePanel.cpp`; backtick opens it | M |
| CVars | MISSING | New `CVar.hpp`. Built-ins: `stat fps`, `stat timings` (`SimulationTiming`, `RendererStats`), `show collision`, `show grid`. `r.vsync` only if Filament's OpenGL backend allows it; to be checked | M |

### Phase 5: Feedback and polish

| Feature | Status | Work and files | Size |
|---|---|---|---|
| Toasts, progress, Cancel | MISSING | New `src/ui/Toasts.cpp`. Hook into `report()`, save, import | M |
| Non-blocking tasks | PARTIAL | Import is background. Add a small task list with progress; thumbnails use it. Cancel means discarding the result, since a decode in progress cannot be interrupted | M |
| Message Log, severity filters, links | PARTIAL | `messages` becomes `LogEntry{severity, text, link}`; `Error:` prefixes migrate; links select an entity or reveal an asset | M |
| Rich tooltips with shortcut | PARTIAL | From commands; toolbar has only "Focus [F]" | S |
| Cheat sheet, hold ? or F1 | MISSING | Overlay generated from the active context | S |
| Typed values during a gizmo drag | MISSING | Digits and axis letter, Enter confirms, Esc cancels; extends the transaction | M |

## 5. Risks

1. **Real-input scripts use fixed screen coordinates.** `editor_workflow.ps1` has 23 `Click-At` calls and `shooter_acceptance.ps1` has 5. Adding a menu bar breaks them. Fix in Phase 1: `--ui-map <file>` writes named widget rectangles each frame, and the scripts look controls up by name. Runs keep their focus and idle guards, and need a free desktop.
2. **Live edits cost more.** `CommandBus::execute` copies the whole scene per call, and `dirty()` compares all entities per revision. A live drag makes this a per-frame cost, fine for 73 entities but not for thousands. Measure in Phase 1 on a generated 10,000-entity scene. If needed, make `dirty()` lazy and update only changed entities.
3. **Edit-mode physics during live drags.** The bridge syncs each frame, and collider rebuilds may be slow. Measure in Phase 2.
4. **Separate OS windows.** They need a Filament swap chain per ImGui viewport and `RendererHasViewports` in `ImGuiRenderer`. Floating inside the main window works today. Approved: OS windows are a follow-up after Phase 5, because this is the one item that risks renderer stability.
5. **Orthographic lens** touches `extract_frame`'s validation, which existing tests cover. Perspective behavior must stay identical.
6. **Details rewrite** replaces working UI, and every field needs re-checking. It lands component by component, with a test that each table covers its struct's fields.
7. **Alt handling** in ImGui navigation and Windows needs testing early, as Phase 2 depends on it.
8. **Size.** This is larger than Milestone 4. Thumbnails, Details and the camera are the largest items. Anything cut is reported as cut.

## 6. Out of scope

From the reference but not the phases: Alt+MMB temporary pivot, Live Coding, 4-up viewports, Reference Viewer, Size Map, Collections, Map Check, source control icons, Tip of the Day, compact density, unified settings window. Blueprints. New render modes.

## 7. Verification per phase

Both MSVC presets build into fresh `build-ux-windows-msvc-release` and `-ci` folders, with no new warnings in `Engine` or `Tests`. CTest passes, with the baseline 207 entries kept. `--smoke` and `--render-test` pass. Existing scenes load and save, with the sample scene byte-identical after load and save. Real-input scripts pass. Each phase ends with a summary and manual test steps, then stops for approval.

## 8. Docs to add

`docs/EDITOR_SHORTCUTS.md`, and "How to add a new command / new inspectable property" in `docs/architecture/008-editor-ux.md`, both in Phase 1 and kept current.

## 9. Changelog

- 2026-10-30: Step 0 audit and plan written. No build run, no code changed. Waiting for approval.
- 2026-10-30: Plan approved, with the three recommendations: first Esc pauses and second Esc stops, separate OS windows after Phase 5, Alt+2 and Alt+3 greyed out and mouse gestures read-only at first.
- 2026-10-30: Phase 1 done.
- 2026-10-30: Phase 2 done.

### Phase 1 checklist

- [x] `EditorUI` split out of `Main.cpp` into `src/ui/`
- [x] `EditorCommand`, `CommandRegistry`, chords, default table with handlers bound by ID
- [x] Main menu bar, toolbar and hierarchy context menu built from commands, with shortcuts, enabled and checked state, tooltips
- [x] Context routing, text-field guard and other input blocks
- [x] Keymap file with overrides, import, export, per-command and full reset
- [x] Keyboard Shortcuts panel: search, groups, click to rebind, conflict warning, right-click to remove
- [x] `docs/EDITOR_SHORTCUTS.md`, generated, with a test that it is current
- [x] Transactions, nesting, scoped guard, cancel; every mutation named
- [x] Gizmo drags live and one undo step, for the whole selection
- [x] Undo History panel with jumps; scene and material edits in one order
- [x] `*` on the viewport tab as well as the title bar
- [x] `SelectionSet` with primary entity and change event; Ctrl+click, Shift+click, Ctrl+A, Esc; multi-select duplicate, delete and row drag
- [x] Reflection metadata for all seven component types, with tests (the Inspector adopts it in Phase 3)
- [x] `--config-dir`, `--ui-map`, `--dump-shortcuts`; settings moved to `%APPDATA%\GameEngine\`
- [x] Real-input scripts find controls by name
- [x] `docs/architecture/008-editor-ux.md`, with how to add a command and a property

### Phase 1 deviations

- **Scrubbing stays commit-on-release.** Inspector and Material Editor drags are still one undo step, but nothing moves until release, except materials. Live scrubbing comes with the Details rewrite in Phase 3, as planned.
- **Named `EditorCommand`,** because `engine::commands::Command` already exists (D1).
- **A byte-identical sample was the wrong test.** The plan asked for the sample to be byte-identical after load and save. It never was: the Python generator writes shortest-form numbers and the engine writes 17 digits, for the same values. What holds, and what is now checked, is that the scene code is unchanged in this work and that saving, playing, stopping and saving again gives identical bytes.
- **Save All only saves the scene,** because materials already save themselves on release. Nothing else is unsaved.
- **Layouts reset once.** Moving settings means the first run ignores any old `imgui.ini`, which can be deleted.

### Phase 1 known limits

- **Large scenes:** one live drag step with its dirty check takes about 32 ms at 10,000 entities, measured in Release, so a drag would fall below 60 Hz at that size. It scales with entity count, and the sample has 73. `CommandBus::execute` copying the scene is the cause; not fixed yet.
- **Not driven with real input:** the system file dialogs (Ctrl+O, Save As, keymap Import and Export), because the driver stops by design when focus leaves the editor; Ctrl+click in the viewport; Keep both; `Edit.MoveToRoot`. Keymap parsing behind Import has unit tests. The dialogs have only been compiled, so try them by hand.
- **The Alt question is still open:** no default uses Alt yet. It is tested when Phase 2 adds Alt+LMB.

### Phase 1 bugs found by running it

- The key press that finished a rebinding also ran the command it had just been bound to.
- Esc that closed a menu, or cancelled a rebinding, also cleared the selection.
- A keymap line ending in a lone `,` key failed to parse.

### Phase 2 checklist

- [x] Camera state machine: `CameraController`, with no ImGui or GLFW, unit tested
- [x] RMB + WASD fly, Q/E, Z/C temporary field of view, RMB + wheel speed with an overlay, Shift faster, smooth acceleration
- [x] Alt+LMB orbit around the selection, Alt+RMB dolly, Alt+MMB pan; LMB drag walk, RMB look, MMB pan, LMB + RMB up and sideways
- [x] Cursor hidden and held during navigation, and put back where it was
- [x] Camera speed saved between sessions
- [x] F frames the whole selection with a 0.2 s ease
- [x] Bookmarks: Ctrl+0-9 store, 0-9 ease back, saved beside the scene
- [x] Alt+G/H/J/K perspective, front, top, side
- [x] W/E/R, Space, Ctrl+`; hover highlight, plane handles and constant size come from ImGuizmo
- [x] Live readout near the cursor
- [x] Alt+drag copies and moves the copies in one step; Shift+drag takes the camera along; hold V for a vertex; Esc cancels
- [x] Snapping overlay in the top right: toggle, presets, `[` and `]`, camera speed; the grid follows the size
- [x] End snaps to the surface below; models dragged from the Asset Browser land at the cursor's ray
- [x] G game view, F11 maximize, H and Ctrl+H hide and unhide
- [x] View and show-flags menus in the top left

### Phase 2 deviations

- **`OrbitCamera` stays a pivot, two angles and a distance.** The plan expected it to become a position and an orientation. Flying just moves the pivot along with the eye, so nothing needed replacing and every existing use kept working.
- **Clicks select on release, not on press,** because a press may turn out to be an LMB drag.
- **Hidden entities and bookmarks are in `<scene>.scene.editor`,** beside the scene and ignored by git: they are one person's view of a scene. Scene files are untouched.
- **Hiding takes the children along,** and only hides in the editor: Play mode ignores it.
- **The gizmo disappears during Shift+drag.** Its maths needs a view that does not move (see the bug below), so it works from a frozen view and would be drawn in the wrong place. The selection box and readout stay.
- **An orthographic view has no sky, no orbiting and no flying.** A sky means nothing in a parallel view. RMB pans, as in Unreal.
- **End does nothing for an object already at or below the ground plane with nothing under it,** and says so. Models are tested by their bounding box, as picking is.
- **Wireframe and Unlit are grey,** as agreed: the renderer has neither.

### Phase 2 known limits

- **The grid is 40 by 40 cells around the world origin.** At a 0.01 m size it covers only 40 cm, and it does not follow the camera.
- **Not driven with real input:** rotation and scale snapping, the show flags, Alt+drag on a multi-selection, and navigating during Play in a scene with no game. S, A, D, Q and C while flying are covered by unit tests; the script drives W, E and Z.
- **Large scenes:** the 32 ms per drag step at 10,000 entities from Phase 1 is unchanged.

### Phase 2 bugs found by running it

- **Shift+drag ran away:** an 80-pixel move sent the object 34 m, because the gizmo measured the cursor against a camera that was moving with the object. Only real input showed it.
- ImGuizmo's own delta text doubled the new readout.
- The procedural sky drew as a dark bull's-eye in orthographic views.
