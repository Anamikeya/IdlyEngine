# Build and verification

Verified on Windows x64 with MSVC 19.44.35228, Windows SDK 10.0.26100.0,
CMake 3.31.2 and the source revisions listed in 001-core-milestone.md.
The core is headless; no renderer, window, physics or network is linked.

## Reproduce

From D:/AI/GameEngine-Codex:

```powershell
cmake -S Engine/Core -B out/core-msvc -G "Visual Studio 17 2022" -A x64 -DENGINE_DEPENDENCY_ROOT=D:/AI/GameEngine/resources
cmake --build out/core-msvc --config Debug --parallel 6
ctest --test-dir out/core-msvc -C Debug --output-on-failure
cmake --build out/core-msvc --config Release --parallel 6
ctest --test-dir out/core-msvc -C Release --output-on-failure
.\out\core-msvc\Release\engine_headless.exe
```

Core and test sources compile with /W4 /WX; third-party headers are external includes.
Build products stay under this worktree's ignored out/ directory.
The shared resources directory is read-only input.

## Results

- Debug: build succeeded; one CTest suite containing 19 acceptance scenarios passed.
- Release: build succeeded; the same 19 scenarios passed.
- Headless application: created two Flecs entities through commands, advanced
  120 fixed ticks, undid/redid creation, serialized the scene, then shut down.
- One intermediate parser edit failed compilation due to a missing namespace brace;
  corrected before the final successful builds. No unresolved build failure.
- Expected callback-failure tests deliberately log an injected error; their assertions
  verify safe shutdown and pass.

Coverage: native Flecs storage and system execution; typed command descriptors;
permissions and expected revisions; transaction atomicity and limits; transform
composition and cycles; subtree delete/restore and stable IDs; invalid transforms;
bounded, strict serialization and round trip; undo/redo and history limits;
owner-thread enforcement; deterministic tick scheduling; catch-up/drop accounting;
invalid time inputs; startup rejection/exceptions/re-entry; update failure cleanup;
scene replacement retaining native systems.

## Limits and next integration

No performance benchmark, graphics test, sanitizer run, cross-platform run or
Filament link test has been performed. These results do not establish renderer or
physics correctness, cross-platform determinism, or production-scale editing speed.
Flecs owner-thread access is required. Transaction atomicity covers validated managed
scene state at command boundaries, not allocation failure or external effects in
native observers. Scene serialization covers managed authoring components only.

Claude can consume engine::core through add_subdirectory(Engine/Core), integrate the
suite into shared CI, and coordinate dependency targets/CRT before Filament linkage.
No main-branch merge is performed by this milestone.
