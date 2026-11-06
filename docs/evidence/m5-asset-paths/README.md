# Milestone 5, slice 7: package names, redirectors and the selection outline

Release build from an empty folder (`build-m5-windows-msvc-release`), commit `cabee69`, 2026-11-05.

## Real input

`Tests/RealInput/milestone5_asset_paths.ps1` drove the editor with the real mouse and keyboard, on a copy of the Physics
Shooter sample. Its check lines are in `m5_asset_paths_results.txt`: 10 of 10, on the first attempt. Each check reads the
editor's `--game-log`, which now reports the primary entity's name and material, the number of Flecs selection tags and the
number of redirectors, or a file on disk.

| Image | What it shows |
|---|---|
| `m5_s7_01_not_selected.jpg` | Static Target 1 framed with F, then Esc: nothing selected, no tags |
| `m5_s7_02_outlined.jpg` | After a click on it in the viewport: an orange line round its outer edge, and the gizmo. The thin lines inside the target are its collider's wireframe, drawn because **Colliders** is on in the toolbar; they are in the first image too |
| `m5_s7_03_renamed.jpg` | F2 on `materials/target_static.material`, renamed to `/Game/materials/targets/target_static`: the file moved, a redirector left, and **Fix Up Redirectors (1)** in the Asset Browser |
| `m5_s7_04_typed_old_name.jpg` | Moving Target 1's Material field with the old name typed, `/game/materials/target_static`, offering `/Game/materials/targets/target_static (through 1 redirector)` |
| `m5_s7_05_fixed_up.jpg` | After Fix Up Redirectors: the button is gone, `stress_test.scene` on disk names the new path, and the redirector file is deleted |

The checks also cover Ctrl+click adding a second selection, which becomes the primary one, and Ctrl+S saving the open scene
with the new path. The same script passed 10 of 10 on the development build first.

## Render test

`selection_outline.jpg` and `selection_outline_hidden.jpg` are `--render-test` captures at half size. The first shows a
selected cube turned so three faces show: the outline follows its silhouette, and none of the edges between its faces is
drawn. The second shows the same cube hidden behind a wall: its whole outline is drawn over the wall, with nothing inside it.
The check lines are in `render_test_results.txt` (75 of 75); the four about the outline count about 2,440 pixels changed,
none inside the silhouette and none away from it.
