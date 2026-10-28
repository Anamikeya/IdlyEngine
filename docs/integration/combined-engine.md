# Combined engine foundation

Verified on 2026-10-28 on Windows x64 with MSVC 19.44, Ninja and the existing shared dependencies.

Integration branch: `integration/engine-mvp-20261028`.
The Codex and Claude branches are merged here. The original main branch remains at its baseline.
The combined root CMake build includes Core, Renderer, Physics, Networking and OnlineServices.

## Reproduce

From PowerShell:

```powershell
cd D:/AI/GameEngine-Integration
$env:GAMEENGINE_RESOURCES_DIR = 'D:/AI/GameEngine/resources'
./cmake/scripts/dev-shell.ps1 cmake --preset windows-msvc-debug
./cmake/scripts/dev-shell.ps1 cmake --build --preset windows-msvc-debug --parallel 6
./cmake/scripts/dev-shell.ps1 ctest --preset windows-msvc-debug
./build/windows-msvc-debug/bin/engine_headless.exe
```

## Results and scope

- Debug configure and all 90 build steps succeeded.
- CTest passed 81/81 entries: core acceptance suite, render snapshot suite, 31 physics cases, 41 networking cases and 7 online service cases.
- Both module sets coexist in a single root build. Existing module tests passed together; this does not establish runtime ECS/physics/network bridges.
- Renderer currently extracts headless CPU frame snapshots. A Filament GPU adapter and ImGui editor are future work.
- Real UDP GameNetworkingSockets backend is disabled (`GAMEENGINE_WITH_GNS=OFF`); networking tests use loopback transport.
- Combined Release, other platforms, sanitizers and cross-compiler determinism were not tested in this integration pass.
- Nothing was pushed or published by this integration. Review the branch before promoting it to main.

Test detail: `build/windows-msvc-debug/Testing/Temporary/LastTest.log`.
