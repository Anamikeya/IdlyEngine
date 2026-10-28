# Render snapshot milestone

Date: 2026-10-28. Continues the headless core milestone on agent/codex-core.

## Delivered boundary

`engine::renderer` is a separate, headless static library depending on `engine::core`.
It extracts a camera, visible mesh instances and directional/point lights into a
`shared_ptr<const FrameSnapshot>`. No Filament headers, GPU objects, native Flecs IDs,
or borrowed scene pointers appear in the frame. A retained frame can be read on a
consumer thread after the originating scene has changed or been destroyed.

The trusted host supplies mesh/light bindings, an active camera and a FrameStamp at
a quiescent scene boundary. Extraction must run on the scene owner thread. The stamp
contains a nonzero host-issued scene/session ID, the authoring revision, and a frame
number. The host is responsible for their accuracy and for assigning a different
scene ID to each session. Render identity is `(stamp.scene, entity)`, not EntityId
alone. Mesh and light output is sorted by stable entity ID, independent of input order.

Bindings are explicit runtime input. They are not yet ECS authoring components,
command payloads, or serialized scene data. They must be reconciled by the host when
entities are deleted/replaced: stale references fail the whole extraction. Hidden
mesh bindings are validated but omitted from output. Duplicate mesh bindings or
duplicate light bindings are rejected; an entity may have one of each. A failed
extraction leaves the scene and previously published frames intact.

MeshHandle and MaterialHandle are distinct types containing index and generation;
zero in either field is invalid. This milestone validates handle shape and copies
generations. It does not allocate assets or verify resource liveness. The future
asset owner/adapter must reject stale generations and retain referenced resources
until queued frames and GPU work finish. Owning a CPU snapshot alone does not pin
GPU assets. No texture, material import, or resource registry is claimed here.

## Coordinates and validation

Matrices use doubles, column-major storage, right-handed coordinates and meters.
Camera forward is local -Z, up is +Y. Perspective projection uses vertical FOV in
radians and normalized clip depth [-1,+1]. The backend must explicitly convert
projection/depth conventions if required. Camera view is inverse world transform.
Camera world transforms must be rigid and unreflected, including inherited scale;
scaled or sheared cameras are rejected rather than silently changing the lens.

The camera requires finite positive aspect/near plane, far > near, and FOV in (0, pi).
Non-finite or unrepresentable output matrices fail extraction. Directional intensity
is lux and point intensity is lumens; linear RGB and intensity must be finite and
nonnegative. Point range is a finite positive world-space influence radius.
Directional range is ignored and canonicalized to zero in output. Light direction
is normalized transformed local -Z; position comes from the scene world matrix.
Each binding list is limited to Scene::max_entities. Extraction costs O(b log b +
b log n) for b bindings and n managed scene entities; it does not snapshot unrelated
scene names or hierarchy records. No frustum culling or performance claim is made.

## Build and integration

Standalone, using the existing read-only dependency checkout:

```powershell
cmake -S Engine/Renderer -B out/renderer-msvc -G "Visual Studio 17 2022" -A x64 -DENGINE_DEPENDENCY_ROOT=D:/AI/GameEngine/resources
cmake --build out/renderer-msvc --config Debug --parallel 6
ctest --test-dir out/renderer-msvc -C Debug --output-on-failure
cmake --build out/renderer-msvc --config Release --parallel 6
ctest --test-dir out/renderer-msvc -C Release --output-on-failure
```

A root build can add_subdirectory(Engine/Core), then add_subdirectory(Engine/Renderer).
The renderer adds Core itself only when engine::core does not already exist.
ENGINE_RENDERER_BUILD_TESTS controls the renderer acceptance executable; Core's
existing options continue to control its tests/example. No root/shared build files
or dependency sources are modified.

Acceptance coverage includes near/far clip mapping, aspect ratio, rotated camera
inverse, hierarchical mesh transforms, visibility, deterministic ordering, handle
generation preservation, light orientation/units, frame lifetime across scene
mutation/destruction, undo identity, bad inputs, list limits and thread ownership.

Verified on Windows x64 with MSVC 19.44.35228 and CMake 3.31.2: Debug and Release
builds passed with /W4 /WX on renderer sources/tests. In each configuration both
CTest suites passed (19 core scenarios plus 8 renderer scenarios). The Release
headless sample also completed 120 fixed ticks and serialized its two entities.
No graphics, sanitizer, performance, or cross-platform verification was performed.

## Next integration gate

The saved ownership contract still assigns root dependency builds, CRT alignment,
platform window ownership and shared CI to Claude. Those inputs are absent in the
shared baseline at this continuation. This milestone can be tested without them.
The next step is a real Filament adapter after the dependency/window contract exists,
followed by the ImGui hierarchy/inspector/viewport. No window, GPU rendering,
Filament link test or editor is implemented by this milestone.
