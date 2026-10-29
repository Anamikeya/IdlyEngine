# Milestone 2: editor and renderer decision

## Audit and plan

Baseline `d876e42` is present on main, the integration branch, and this worktree.
The clean worktree was branched as `agent/codex-editor-m2`. The full Debug root
build and all 81 CTest entries passed before implementation. Previous architecture
contracts 001–004 remain applicable. Renderer registration in root CMake only
builds real CPU extraction; there was no GPU implementation or desktop host.

Build in small increments: desktop/GPU lifecycle; docking UI; command-driven scene
editing and persistence; camera/picking/gizmos; focused tests and visual validation.

## Rendering decision

Use Windows/MSVC, GLFW with `GLFW_NO_API`, and Filament's explicit OpenGL backend.
Filament's WGL implementation takes an HWND and owns its contexts and submission.
Compile the existing Filament source in the worktree with the dynamic CRT to match
the engine. No shared dependency files are modified or downloaded.

Options considered:

* ImGui Vulkan backend: rejected because Filament does not expose the required
  device, command buffers and render-pass ownership as a supported interop API.
* Separate native child surface: valid Windows fallback, but complicates docking,
  clipping, input routing and gizmo overlays.
* Filament primitives for ImGui: selected, following the checked-out
  `libs/filagui/src/ImGuiHelper.cpp` public API usage. The existing independent
  docking ImGui is linked once. A renderer-owned UI adapter uploads draw lists,
  font atlas and scissor rectangles, using a Filament material and orthographic view.

Render the scene into a Filament color texture using a public RenderTarget, then
sample it in the ImGui viewport through the same Engine. No native texture IDs or
Filament handles enter ECS, commands or scene files. One native window is the MVP;
ImGui docking is supported, OS multi-viewport windows are disabled.

The actual local Engine, Renderer, Camera, RenderTarget, Texture, VertexBuffer,
IndexBuffer and RenderableManager headers and GLFW/ImGui APIs were inspected.
The public native-surface contract is also documented at
https://github.com/google/filament/blob/main/filament/include/filament/SwapChain.h.
ImGuizmo already exists at `resources/box3d/extern/imguizmo`; compile-check it
against the independent docking ImGui, without altering either dependency.

## Build coordination

The owned `Engine/Editor/CMakeLists.txt` entry point includes the unchanged root
foundation build (including Claude's physics/networking tests). Shared root CMake,
dependency locks and physics sources remain under Claude's ownership. Integration
handoff: make this optional editor entry point part of shared presets/CI after
review; keep Filament /MD and /MDd aligned. Do not merge into main for this task.

## Delivered editor

The standalone editor now provides a docked toolbar, scene hierarchy, inspector,
3D viewport, asset browser and console. Managed scene entities render as built-in
blue cubes over a reference grid. Create, rename, transform, reparent, delete,
undo/redo and scene replacement use the existing typed command bus. Save uses a
temporary file followed by atomic replacement; malformed loads preserve the scene.

Editor camera navigation stays outside scene authoring history. Right-drag orbits,
middle-drag pans, the wheel zooms, and F focuses the selected entity. Picking uses
the nearest transformed cube hit. Move/rotate/scale gizmos convert world edits
through the parent transform and reject shear. Inspector and gizmo edits commit
on release as one undo step. Ctrl+Z/Ctrl+Y undo/redo and Ctrl+S saves.

## Reproduce

From the repository root, with the existing dependencies available:

```powershell
./cmake/scripts/dev-shell.ps1 cmake -S Engine/Editor -B out/editor -G Ninja -DCMAKE_BUILD_TYPE=Release -DGAMEENGINE_RESOURCES_DIR=D:/AI/GameEngine/resources
./cmake/scripts/dev-shell.ps1 cmake --build out/editor --parallel 4
ctest --test-dir out/editor --output-on-failure
Push-Location out/editor
./engine_editor.exe --smoke
Pop-Location
```

Launch `out/editor/engine_editor.exe` without arguments for normal editing.
Scene paths are relative to the working directory. The smoke run creates
`smoke.scene` and `editor-smoke.ppm` there. Set `ENGINE_EDITOR_GPU_TESTS=ON` at
configure time to register the visible GPU smoke run in CTest; leave it off on
machines without an interactive desktop.

## Completion verification (2026-10-29)

The Windows x64 Release build succeeds and all 82 CTest entries pass, including
five editor acceptance groups covering commands, persistence, camera/picking,
world-space edits and external camera validation. The GPU smoke run passes on an
NVIDIA RTX 3080 with Filament OpenGL: 93 rendered frames, transform, undo/redo,
save/load, resize, minimize/restore calls, image readback and clean shutdown.

The final audit found an extra vertical flip in capture export. Filament's
readPixels already returns top-to-bottom rows; the exporter now preserves that
order. A fresh smoke capture verifies the correction. Native window capture via
the desktop automation helper timed out, so visual verification uses the GPU
readback artifact. The smoke's immediate minimize/restore calls do not establish
extended minimized-window behavior.

Scope remains the Windows/MSVC single-window editor MVP. Asset import, arbitrary
mesh/material authoring, OS multi-viewport windows, runtime physics/network
bridges and other graphics backends are outside this milestone. The desktop
adapter was verified in Release; the earlier Debug baseline covers the foundation.

## Final review and milestone acceptance

Reviewed on 2026-10-29 against the standalone M2 scope above. The review tightened
external camera validation to reject non-affine homogeneous rows and constrained
picking to the near/far clip segment, with regression cases in the editor suite.
An interrupted GPU smoke run now fails instead of returning success before its
checks finish. The rebuilt Release targets, all 82 CTest entries and the 93-frame
GPU smoke run pass after these changes.

| Milestone requirement | Delivery and evidence |
| --- | --- |
| Desktop/GPU lifecycle | GLFW window, Filament OpenGL, resize, readback, clean smoke shutdown |
| Docked editor UI | Toolbar, hierarchy, inspector, viewport, asset browser and console visible in GPU capture |
| Command-driven authoring | Create, rename, transform, reparent, delete, undo/redo; editor acceptance coverage |
| Scene persistence | Save/load round trip, undoable replacement, malformed-load atomicity tests |
| Camera and selection | Orbit/pan/zoom/focus, nearest visible-range cube picking; acceptance coverage |
| Transform gizmos | ImGuizmo move/rotate/scale, parent-space conversion and shear rejection tests |
| Build and verification | Standalone CMake entry point, optional GPU CTest, 82 passing CPU tests, successful GPU smoke |

The standalone M2 implementation is complete within this scope. Shared root
presets/CI integration remains a separate handoff under the build coordination
contract. Native input-driven gizmo interaction and extended minimize/restore
behavior have not been independently automated. Commit the source, materials,
tests and this record together on `agent/codex-editor-m2`; generated build outputs
and diagnostic captures stay under ignored `out/`.
