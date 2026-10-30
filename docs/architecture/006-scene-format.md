# Scene file format

Implemented in `Engine/Scene/src/Serialization.cpp` (`write_scene`, `read_scene`). The writer always emits the
current version, `engine::scene_format_version` (4). The reader accepts versions 1 to 4.

## Version 4

Line-based UTF-8 text, tokens separated by whitespace, numbers in the classic C locale. Doubles are written with
`max_digits10`, so a save/load round trip is exact.

```
ENGINE_SCENE 4 <next_id> <entity_count>
ENTITY <id> <parent> "<name>" <px> <py> <pz> <qx> <qy> <qz> <qw> <sx> <sy> <sz>
...                                    (exactly entity_count ENTITY lines)
COMPONENTS <component_count>
<KIND> <entity> <fields...>            (exactly component_count lines)
```

- `next_id`: the next stable entity ID the scene will allocate. IDs are never recycled.
- `ENTITY`: stable ID, parent ID (0 = root), quoted name (`\"` and `\\` escapes, at most 256 bytes, no control
  characters), local translation, rotation quaternion (x, y, z, w) and scale.
- `COMPONENTS`: at most one `MESH`, `CAMERA`, `LIGHT` and `BODY` per entity, and up to 64 `COLLIDER` records per
  entity. The writer sorts them by entity, then `MESH`, `CAMERA`, `LIGHT`, `BODY`, `COLLIDER` (colliders in list
  order); the reader accepts any order, keeping colliders in file order.

### Component records

Colors are sRGB in [0, 1], as picked in the editor. Renderers convert them to linear.

| Record | Fields | Meaning |
|---|---|---|
| `MESH <entity> <mesh> <r> <g> <b> <roughness> <metallic>` | `mesh`: `cube`, `sphere` or `plane`; roughness and metallic in [0, 1] | Built-in unit mesh centered at the origin (the plane lies in XZ facing +Y) drawn with the lit material |
| `CAMERA <entity> <fov> <near> <far> <primary>` | vertical field of view in degrees (1–179); 0 < near < far; primary `0` or `1` | Perspective camera looking along the entity's local −Z. Play mode views through the first primary camera by entity ID; its world transform must be rigid (no scale) |
| `LIGHT <entity> <type> <r> <g> <b> <intensity> <range>` | `type`: `directional` or `point`; intensity ≥ 0 (lux for directional, lumens for point); range > 0 m | Directional lights shine along the entity's local −Z; point lights emit from its position with the given falloff radius |
| `BODY <entity> <kind> <vx> <vy> <vz> <wx> <wy> <wz> <linear_damping> <angular_damping> <gravity_scale> <can_sleep> <continuous>` | `kind`: `static`, `kinematic` or `dynamic`; initial world-space velocity (m/s) and spin (rad/s); damping ≥ 0; flags `0` or `1` | Physics body simulated in Play mode |
| `COLLIDER <entity> <shape> <cx> <cy> <cz> <qx> <qy> <qz> <qw> <hx> <hy> <hz> <radius> <half_height> <density> <friction> <restitution> <sensor> <events>` | `shape`: `sphere`, `box` or `capsule`; local center and unit rotation; half extents and radius > 0, half height ≥ 0; density and friction ≥ 0; restitution in [0, 1]; flags `0` or `1` | Collision shape in the entity's unscaled local frame; world scale is applied when the body is built. A box uses the half extents, a sphere the radius, a capsule the radius and half height (segment along local Y). Every field is written for every shape. `sensor` detects overlaps without colliding; `events` reports contact and sensor events |

An entity with `COLLIDER` records but no `BODY` is a static collider.

When a scene has no `LIGHT` records, the editor viewport uses its own two editor lights; as soon as one light
exists, only scene lights are used.

Example:

```
ENGINE_SCENE 4 5 4
ENTITY 1 0 "Ball" 0 0.5 0 0 0 0 1 1 1 1
ENTITY 2 0 "Sun" 0 4 0 0 0 0 1 1 1 1
ENTITY 3 0 "Camera" 0 1.5 6 0 0 0 1 1 1 1
ENTITY 4 0 "Floor" 0 0 0 0 0 0 1 10 1 10
COMPONENTS 6
MESH 1 sphere 1 0.25 0.125 0.5 0
BODY 1 dynamic 0 0 0 0 0 0 0 0 1 1 0
COLLIDER 1 sphere 0 0 0 0 0 0 1 0.5 0.5 0.5 0.5 0.5 1000 0.75 0 0 0
LIGHT 2 directional 1 1 1 100000 10
CAMERA 3 60 0.10000000000000001 1000 1
COLLIDER 4 box 0 -0.5 0 0 0 0 1 0.5 0.5 0.5 0.5 0.5 1000 0.75 0 0 0
```

## Earlier versions

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
`bounded_parser_and_stream_flags`, `physics_components`) and the editor's `persistence`, `documents`,
`components_and_rendering`, `physics_play` and `acceptance_workflow` groups.
