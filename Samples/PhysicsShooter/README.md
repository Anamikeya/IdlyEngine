# Physics Shooter

A small first-person game built on the engine: Filament rendering, Box3D physics, Flecs components and the reusable
systems in `Engine/Gameplay`. Nothing in the engine or the editor is specific to it; everything shooter-specific is the
data in this folder.

Destroy all 12 targets within 90 seconds, three of them enemies walking the arena. Shots push what they hit, so crates
topple, dominoes fall, and a crate that lands in the green zone is worth points. Shot, an enemy flinches; killed, it
falls as a ragdoll, pushed the way the shot went. Glass breaks into shards that bounce and settle, shots flash at the
muzzle and throw sparks, crates raise dust when they land hard, and the enemies' feet kick up dust.

## Running it

In the editor: **Project... > Open Physics Shooter sample**, then **Play**, then click the view. Or start it directly:

```powershell
build-m4-windows-msvc-release\bin\engine_editor.exe --sample
```

| Input | Action |
|---|---|
| W A S D | Move |
| Mouse | Look |
| Left mouse button | Fire |
| Space | Jump |
| Shift | Sprint |
| R | Reload |
| Esc | Release the mouse and pause |
| Left click on the view | Take the mouse again and resume |

**Stop** returns to the unchanged editor scene.

## What is here

| Path | What |
|---|---|
| `physics_shooter.scene` | The arena, targets, player and rules, as behaviors on entities |
| `stress_test.scene` | The arena with ten enemies in view and ten fountains keeping 5,000 particles alive, for performance runs |
| `lighting_test.scene` | Metallic, rough, non-metal, textured and transparent surfaces, for checking lighting |
| `Assets/materials/` | Material assets, editable in the Material Editor |
| `Assets/effects/` | Particle effects gameplay fires (sparks, muzzle flash, crate and footstep dust, glass slivers), editable in the Effect Editor. Their templates sit below the floor |
| `Assets/textures/`, `environments/`, `models/` | Downloaded CC0 files; see `ASSET_LICENSES.md` and `asset_manifest.json` |
| `tools/generate_project.py` | Writes the scenes, materials and `.meta` files. Asset IDs come from paths, so rerunning it keeps references valid |

## Targets

| Kind | Body | How to destroy it |
|---|---|---|
| Static (3, red) | Static | Two shots. It does not move |
| Moving (2, blue) | Kinematic, on a path | Two shots |
| Dynamic (2, orange) | Dynamic, on a pyramid and on the platform | Three shots, or a hard enough fall: collision events hurt |
| Chain (1, orange) | Dynamic, at the end of the dominoes | Shoot the first domino from the front; the row falls on it |
| Water bottle (1) | Dynamic, on a plinth | Two shots, or knock it off |
| Enemies (3) | Kinematic capsules patrolling back and forth, drawn as Quaternius' mannequin | Three shots. They fall as ragdolls and stay where they fell |
