# Milestone 3 report

Branch `agent/claude-milestone-3`, worktree `D:\AI\GameEngine-Integration`. Plan: `docs/MILESTONE_3_PLAN.md`
(approved 2026-10-29).

**Status: in progress. Slice S1 is complete and verified; S2–S6 are not started.** Milestone 3 is not complete:
components, assets and physics in Play mode are still missing.

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
