# Scene file format

Implemented in `Engine/Scene/src/Serialization.cpp` (`write_scene`, `read_scene`). The writer always emits the
current version, `engine::scene_format_version` (7). The reader accepts versions 1 to 7.

## Version 7

Line-based UTF-8 text, tokens separated by whitespace, numbers in the classic C locale. Doubles are written with
`max_digits10`, so a save/load round trip is exact.

```
ENGINE_SCENE 7 <next_id> <entity_count>
ENTITY <id> <parent> "<name>" <px> <py> <pz> <qx> <qy> <qz> <qw> <sx> <sy> <sz>
...                                    (exactly entity_count ENTITY lines)
COMPONENTS <component_count>
<KIND> <entity> <fields...>            (exactly component_count lines)
```

- `next_id`: the next stable entity ID the scene will allocate. IDs are never recycled.
- `ENTITY`: stable ID, parent ID (0 = root), quoted name (`\"` and `\\` escapes, at most 256 bytes, no control
  characters), local translation, rotation quaternion (x, y, z, w) and scale.
- `COMPONENTS`: at most one `MESH`, `CAMERA`, `LIGHT`, `BODY`, `ENVIRONMENT` and `EMITTER` per entity, and up to 64 `COLLIDER`,
  64 `MATSLOT` and 16 `BEHAVIOR` records per entity. The writer sorts them by entity, then `MESH` with its `MATSLOT`s,
  `CAMERA`, `LIGHT`, `BODY`, `COLLIDER`, `ENVIRONMENT`, `BEHAVIOR`, `EMITTER`; the reader accepts any order, keeping colliders
  and behaviors in file order and slots sorted.

### Component records

Colors are sRGB in [0, 1], as picked in the editor. Renderers convert them to linear.

| Record | Fields | Meaning |
|---|---|---|
| `MESH <entity> <mesh> <r> <g> <b> <roughness> <metallic> <model> <texture> <material> <u> <v>` | `mesh`: `cube`, `sphere` or `plane`; roughness and metallic in [0, 1]; `model`, `texture` and `material`: asset references (below); `u`, `v` > 0 | Without a `model`, the built-in unit mesh centered at the origin (the plane lies in XZ facing +Y). With one, the imported mesh; `mesh` is ignored. The color tints the mesh. Roughness and metallic apply to built-in meshes and to OBJ files without a material; a GLB model uses its own. `texture` replaces the base color image of the whole mesh. `material` replaces the model's own materials. `u`, `v` multiply the material's UV scale |
| `MATSLOT <entity> <slot> <material>` | part index; material reference, not `-` | A material for one part of the mesh, overriding `material` there. Needs a `MESH` on the entity; if none is read, a default one is created |
| `CAMERA <entity> <fov> <near> <far> <primary>` | vertical field of view in degrees (1–179); 0 < near < far; primary `0` or `1` | Perspective camera looking along the entity's local −Z. Play mode views through the first primary camera by entity ID; its world transform must be rigid (no scale) |
| `LIGHT <entity> <type> <r> <g> <b> <intensity> <range> <inner> <outer> <shadows>` | `type`: `directional`, `point` or `spot`; intensity ≥ 0 (lux for directional, lumens otherwise); range > 0 m; cone half angles in degrees, 0 ≤ inner ≤ outer ≤ 90; shadows `0` or `1` | Directional and spot lights shine along the entity's local −Z; point and spot lights emit from its position with the given falloff radius. Every field is written for every type |
| `BODY <entity> <kind> <vx> <vy> <vz> <wx> <wy> <wz> <linear_damping> <angular_damping> <gravity_scale> <can_sleep> <continuous> <lock_rotation>` | `kind`: `static`, `kinematic` or `dynamic`; initial world-space velocity (m/s) and spin (rad/s); damping ≥ 0; flags `0` or `1` | Physics body simulated in Play mode. `lock_rotation` keeps it upright, as a character's capsule must be |
| `COLLIDER <entity> <shape> <cx> <cy> <cz> <qx> <qy> <qz> <qw> <hx> <hy> <hz> <radius> <half_height> <density> <friction> <restitution> <sensor> <events> <category> <mask>` | `shape`: `sphere`, `box` or `capsule`; local center and unit rotation; half extents and radius > 0, half height ≥ 0; density and friction ≥ 0; restitution in [0, 1]; flags `0` or `1`; category and mask: 64-bit unsigned integers | Collision shape in the entity's unscaled local frame; world scale is applied when the body is built. A box uses the half extents, a sphere the radius, a capsule the radius and half height (segment along local Y). Every field is written for every shape. `sensor` detects overlaps without colliding; `events` reports contact, hit and sensor events. Two colliders touch when each one's mask has a bit of the other's category |
| `ENVIRONMENT <entity> <source> <image> <sky r g b> <horizon r g b> <ground r g b> <intensity> <rotation> <skybox> <ev100> <tone> <shadows> <distance> <occlusion> <bloom>` | `source`: `none`, `procedural` or `image`; `image`: asset reference, required for `image`; colors in [0, 1]; intensity ≥ 0; rotation in degrees; EV100 in [−20, 30]; `tone`: `aces`, `filmic`, `agx`, `pbr_neutral` or `linear`; `shadows`: `off`, `low`, `medium` or `high`; distance > 0 m; flags `0` or `1` | How the scene is lit and exposed. The first by entity ID applies. Without one, a procedural sky at EV100 15 is used. An image that cannot be used is reported, and the procedural sky lights the scene instead |
| `BEHAVIOR <entity> <type> <count> [<key> <value>]...` | type and keys: 1–32 of `[a-z0-9_]`, each at most once; at most 32 finite values | Named numbers a runtime system reads when Play starts. The scene gives them no meaning. See `Engine/Gameplay/include/engine/gameplay/game.h` for the ones the gameplay module understands |
| `EMITTER <entity> <effect> <auto_play> <loop> <rate_scale>` | `effect`: asset reference to a `.effect` file, or `-`; flags `0` or `1`; rate scale in [0, 1000] | Plays a particle effect at the entity. `auto_play` starts it by itself in Play and in the editor's preview; otherwise only gameplay fires it. `loop` starts it again when the effect's duration ends. The rate scale multiplies its rate and bursts. Particles are visual only and never change the scene |

An asset reference is `-` (none), or the 16 hex digit asset ID followed by the quoted path inside the project's
`Assets` folder (at most 1024 bytes, no control characters): `MESH 1 cube 1 1 1 0.65 0 6a1f0c3e9b2d4a01 "models/helmet.glb" - - 1 1`.
A host resolves the ID first and the path if the ID is unknown, so a renamed asset with its `.meta` and a recreated
`.meta` both keep working. Scene files never embed asset data; a reference that resolves to nothing, or to a file
that fails to import, loads and saves unchanged and renders as a magenta placeholder cube.

An entity with `COLLIDER` records but no `BODY` is a static collider.

When a scene has no `LIGHT` records, the editor viewport uses its own two editor lights; as soon as one light
exists, only scene lights are used.

Example:

```
ENGINE_SCENE 7 5 4
ENTITY 1 0 "Ball" 0 0.5 0 0 0 0 1 1 1 1
ENTITY 2 0 "Sun" 0 4 0 0 0 0 1 1 1 1
ENTITY 3 0 "Camera" 0 1.5 6 0 0 0 1 1 1 1
ENTITY 4 0 "Floor" 0 0 0 0 0 0 1 10 1 10
COMPONENTS 6
MESH 1 sphere 1 0.25 0.125 0.5 0 - - - 1 1
BODY 1 dynamic 0 0 0 0 0 0 0 0 1 1 0 0
COLLIDER 1 sphere 0 0 0 0 0 0 1 0.5 0.5 0.5 0.5 0.5 1000 0.75 0 0 0 18446744073709551615 18446744073709551615
LIGHT 2 directional 1 1 1 100000 10 25 35 0
CAMERA 3 60 0.10000000000000001 1000 1
COLLIDER 4 box 0 -0.5 0 0 0 0 1 0.5 0.5 0.5 0.5 0.5 1000 0.75 0 0 0 18446744073709551615 18446744073709551615
```

## Material files

A `.material` file in the asset folder (`engine::assets::write_material`, `parse_material`) is text: the line
`ENGINE_MATERIAL 1`, then one `key values` line per field. Every key is optional and at most once; unknown keys and
out-of-range values are errors.

```
ENGINE_MATERIAL 1
base_color 0.9 0.3 0.1 1
metallic 0
roughness 0.65
emissive 0 0 0
emissive_strength 1
normal_scale 1
occlusion_strength 1
alpha_mode opaque
alpha_cutoff 0.5
double_sided 0
uv_scale 1 1
base_color_map 84995e8779017a21 "textures/wood_base.jpg"
metallic_roughness_map -
normal_map -
occlusion_map -
emissive_map -
```

Colors are sRGB in [0, 1] (alpha is linear). `alpha_mode` is `opaque`, `mask` or `blend`. Metallic-roughness maps read green
as roughness and blue as metalness, multiplied by the factors; occlusion maps read red.

## Effect files

A `.effect` file in the asset folder (`engine::assets::write_effect`, `parse_effect`) is text like a material: the line
`ENGINE_EFFECT 1`, then one `key values` line per field. Every key is optional and at most once; unknown keys and
out-of-range values are errors.

```
ENGINE_EFFECT 1
duration 1
rate 30
burst 0
lifetime 0.6 1
speed 1 2
cone 20
spawn_radius 0
acceleration 0 -9.81 0
drag 0
spin 0 0
stretch 0
size 0 0.1 1 0.1
color 0 1 1 1 1 1 1 1 1 0
emissive_strength 1
blend additive
texture -
collide_floor 0
floor_height 0
bounce 0.3
world_space 1
max_particles 500
```

Pairs are min and max (`lifetime`, `speed`, `spin` in degrees per second). Directions spread around the emitter's +Y
axis, or the direction a one-shot effect is fired along, within the `cone` half angle (180 is every direction).
`size` is 1 to 8 pairs of life fraction and quad width in metres; `color` is 1 to 8 groups of life fraction and an
sRGB color with linear alpha. Both are interpolated linearly. `stretch` above 0 draws a quad along the velocity,
speed times that many seconds long, for sparks. `blend` is `additive` or `alpha`. `texture` is an image asset
reference or `-` for a soft round dot. `max_particles` is 1 to 20000.

## Earlier versions

- **Version 6** (Milestone 4): no `EMITTER` record.
- **Version 5** (Milestone 3 slice S5): `MESH` has no material or UV scale, `LIGHT` no cones or shadows, `BODY` no rotation
  lock, `COLLIDER` no filter; `MATSLOT`, `ENVIRONMENT`, `BEHAVIOR` and the `spot` type are rejected.
- **Version 4** (Milestone 3 slice S4): `MESH` has no asset reference fields.
- **Version 3** (Milestone 3 slice S2): `MESH`, `CAMERA` and `LIGHT` records only; `BODY` and `COLLIDER` are
  rejected as unsupported kinds.
- **Version 2** (Milestone 3 slice S1): identical to version 3, but no component kinds existed, so
  `COMPONENTS` must be 0.
- **Version 1** (Milestone 2): no `COMPONENTS` section.

Before components existed the editor drew every entity as the built-in cube, so loading a version 1 or 2 file
gives every entity a default `MESH` (cube, default color). Old files look the same, and the next save writes
the current version.

## Validation and limits

Loading is all-or-nothing: a file that fails any check leaves the open scene untouched.

- Input is capped at 64 MiB and every token at a fixed length; numbers must parse completely and be finite.
- At most `Scene::max_entities` (100,000) entities.
- Component records must name an existing entity, use a known kind and enumerator, have every field, and not
  repeat a kind for one entity. Unknown kinds fail with `Unsupported component kind '<KIND>'`, never silently.
- After parsing, `Scene::validate` requires: `next_id` nonzero; entities in strictly ascending ID order (so no
  duplicates), every ID below `next_id`; names of 1–256 non-control bytes; finite transforms with a unit-length
  quaternion and no scale component below 1e-8 in magnitude; existing parents and no parent cycles; and the
  component ranges in the table above. The same validation guards every command, so the editor cannot create a
  scene it could not save.

Tests: `Engine/Core/tests/CoreTests.cpp` (`serialization_roundtrip`, `components`, `malformed_serialization`,
`bounded_parser_and_stream_flags`, `physics_components`, `asset_references`) and the editor's `persistence`,
`documents`, `components_and_rendering`, `physics_play`, `asset_workflow` and `acceptance_workflow` groups.
