# Scene file format

Implemented in `Engine/Scene/src/Serialization.cpp` (`write_scene`, `read_scene`). The writer always emits the
current version, `engine::scene_format_version` (2). The reader accepts versions 1 to 2.

## Version 2

Line-based UTF-8 text, tokens separated by whitespace, numbers in the classic C locale. Doubles are written with
`max_digits10`, so a save/load round trip is exact.

```
ENGINE_SCENE 2 <next_id> <entity_count>
ENTITY <id> <parent> "<name>" <px> <py> <pz> <qx> <qy> <qz> <qw> <sx> <sy> <sz>
...                                    (exactly entity_count ENTITY lines)
COMPONENTS <component_count>
<KIND> <entity> <fields...>            (exactly component_count lines)
```

- `next_id`: the next stable entity ID the scene will allocate. IDs are never recycled.
- `ENTITY`: stable ID, parent ID (0 = root), quoted name (`\"` and `\\` escapes, at most 256 bytes, no control
  characters), local translation, rotation quaternion (x, y, z, w) and scale.
- `COMPONENTS`: per-entity component records. No component kinds are defined yet, so the count is always 0.
  A reader meets an unknown kind only in a file from a newer engine; it fails with
  `Unsupported component kind '<KIND>'` rather than dropping data. Milestone 3 slices add kinds here (mesh
  renderer, material, camera, light, rigid body, collider), each with a documented field list.

Example:

```
ENGINE_SCENE 2 3 1
ENTITY 2 0 "Cube" 1.9840141534805298 0.5 -1.25 0 0.65291614660035469 0 0.75743019843979287 1 1.94444739818573 1
COMPONENTS 0
```

## Version 1

Identical, but without the `COMPONENTS` section. Version 1 files load unchanged and are written back as version 2
on the next save.

## Validation and limits

Loading is all-or-nothing: a file that fails any check leaves the open scene untouched.

- Input is capped at 64 MiB and every token at a fixed length; numbers must parse completely and be finite.
- At most `Scene::max_entities` (100,000) entities.
- After parsing, `Scene::validate` requires: `next_id` nonzero; entities in strictly ascending ID order (so no
  duplicates), every ID below `next_id`; names of 1–256 non-control bytes; finite transforms with a unit-length
  quaternion and no scale component below 1e-8 in magnitude; existing parents and no parent cycles.
- Trailing content after the last section is an error.

Tests: `Engine/Core/tests/CoreTests.cpp` (`serialization_roundtrip`, `malformed_serialization`,
`bounded_parser_and_stream_flags`) and the editor's `persistence`, `documents` and `acceptance_workflow` groups.
