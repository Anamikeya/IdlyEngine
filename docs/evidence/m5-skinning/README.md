# Milestone 5, slice 2: skinning evidence

Captures from `engine_editor --render-test` (Release, 2026-11-04), whose check lines are in `render_test_results.txt`.

| Image | What it shows |
|---|---|
| `m5_s2_01_bar_rest.jpg` | The generated skinned bar at rest, drawn like its unskinned twin |
| `m5_s2_02_bar_posed.jpg` | The same bar with its tip joint 1.5 m to the right: the bottom row stays, the middle row moves half as far |
| `m5_s2_03_fox_rest.jpg` | Fox in its rest pose |
| `m5_s2_04_fox_run.jpg` | Fox posed by the keys nearest the middle of its Run clip |

Fox is read in place from the dependency tree (`filament/third_party/models/Fox/Fox.glb`), from the
[glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Fox):

- Model by PixelMannen, [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/legalcode).
- Rigging and animation by tomkranis, [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/legalcode).
- Conversion to glTF by @AsoboStudio and @scurest, [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/legalcode).

The images are renders of that model by this engine, unchanged otherwise.
