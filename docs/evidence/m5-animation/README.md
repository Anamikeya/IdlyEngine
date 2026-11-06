# Milestone 5, slice 3: animation evidence

Release build, 2026-11-04.

## Render test

Captures from `engine_editor --render-test`, at half size. The check lines are in `render_test_results.txt` (66 of 66).

| Image | What it shows |
|---|---|
| `m5_s3_r1_bar_start.jpg` | The generated bar, placed with an animator on its clip "Slide", at 0 s. A green cube is attached 1.3 m above its tip joint |
| `m5_s3_r2_bar_half.jpg` | The same at 0.5 s: the tip has slid 0.75 m right, the top of the bar with it, and the cube on it |
| `m5_s3_r3_fox_walk_start.jpg` | Fox playing Walk through its animator, at 0 s |
| `m5_s3_r4_fox_walk_middle.jpg` | The same at 0.35 s |

## Real input

`Tests/RealInput/milestone5_animation.ps1` drove the editor with the real mouse and keyboard. Its check lines are in
`m5_animation_results.txt` (22 of 22). Each check reads the editor's `--game-log` state or a file on disk.

| Image | What it shows |
|---|---|
| `m5_s3_01_preview_a.jpg` | Fox double-clicked into the scene, playing its first clip (Survey) while editing |
| `m5_s3_03_scrubbed.jpg` | The Animation panel after a crossfade to Walk, a pause and a drag along the timeline |
| `m5_s3_04_event.jpg` | A `footstep` event added at the timeline's time and saved in `Fox.glb.meta`; its mark is on the timeline |
| `m5_s3_05_attached.jpg` | A child cube attached to the `b_Head_05` joint, drawn on the head as it moves |
| `m5_s3_06_bones_off.jpg`, `m5_s3_07_bones_on.jpg` | View > Show Bones off and on, with the preview held still |
| `m5_s3_08_play.jpg` | Play: the animator runs from its default clip, and the cube rides on the head |

## Fox

Fox is read from the dependency tree (`filament/third_party/models/Fox/Fox.glb`). It comes from the
[glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Fox):

- Model by PixelMannen, [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/legalcode).
- Rigging and animation by tomkranis, [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/legalcode).
- Conversion to glTF by @AsoboStudio and @scurest, [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/legalcode).

The images are renders of that model by this engine, unchanged otherwise.
