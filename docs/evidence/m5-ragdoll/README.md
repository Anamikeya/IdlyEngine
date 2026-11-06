# Milestone 5, slice 4: ragdoll and debris evidence

Release build, 2026-11-04.

## Render test

Captures from `engine_editor --render-test`, at half size. The check lines are in `render_test_results.txt` (71 of 71).

| Image | What it shows |
|---|---|
| `m5_s4_r1_fox_standing.jpg` | Fox at 1:100 in Play on a floor that collides, with a Ragdoll, playing its first clip |
| `m5_s4_r2_fox_fallen.jpg` | 150 ticks after it turned into a ragdoll from that pose: it lies on the floor, drawn where its bodies are. The skeleton's top went from 0.71 m to 0.18 m |
| `m5_s4_r3_debris.jpg` | Eight pieces of debris like the small red cube, falling: drawn by the GPU, never in the scene |

## Real input

`Tests/RealInput/milestone5_ragdoll.ps1` drove the editor with the real mouse and keyboard. Its check lines are in
`m5_ragdoll_results.txt` (18 of 18). Each check reads the editor's `--game-log` state or a file on disk.

| Image | What it shows |
|---|---|
| `m5_s4_01_ragdoll_hidden.jpg`, `m5_s4_02_ragdoll_shown.jpg` | Fox walking after Add Component > Ragdoll, then with Show ragdolls: its 17 planned capsules |
| `m5_s4_03_bone_settings.jpg` | Falls set to On demand, and settings for the tail tip with Body set to None: 16 bodies |
| `m5_s4_04_walking.jpg` | Simulate, with Fox walking from its animator |
| `m5_s4_05_fallen.jpg` | After Ragdoll now in Details: Fox lies on the floor, its capsules over it, and the button now says Stand up |
| `m5_s4_06_debris.jpg` | After Play > Ragdoll Selected and Play > Drop Debris with the cube selected: pieces like it landing around Fox |

The first two runs stopped or failed for faults found on the way, both fixed before this run:

- The script picked combo rows without opening the combo first. That is a fault in the script.
- Ragdoll now was the third button on its row and clipped off the right of the docked Inspector. That is a fault in the editor:
  Details buttons now go beside the last one only when they fit. The script now checks that an item is wholly inside its
  panel.

## Fox

Fox is read from the dependency tree (`filament/third_party/models/Fox/Fox.glb`). It comes from the
[glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Fox):

- Model by PixelMannen, [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/legalcode).
- Rigging and animation by tomkranis, [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/legalcode).
- Conversion to glTF by @AsoboStudio and @scurest, [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/legalcode).

The images are renders of that model by this engine, unchanged otherwise.
