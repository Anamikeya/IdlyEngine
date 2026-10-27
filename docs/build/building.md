# Building

## Requirements

- CMake >= 3.25, Ninja.
- Windows: Visual Studio 2022 with the "Desktop development with C++" workload (MSVC x64).
- The third-party source tree. Point `GAMEENGINE_RESOURCES_DIR` at it (environment variable or
  `-DGAMEENGINE_RESOURCES_DIR=`). If unset, `<repo>/resources` is tried. Nothing is ever written
  into that directory; all build output goes under `build/`.

## Windows (MSVC + Ninja)

Ninja needs the MSVC environment. From a "Developer PowerShell for VS 2022" run the commands
directly; from any other shell prefix them with `cmake/scripts/dev-shell.ps1`, which loads
`vcvars64.bat` through `vswhere`:

```powershell
$env:GAMEENGINE_RESOURCES_DIR = 'D:\AI\GameEngine\resources'
./cmake/scripts/dev-shell.ps1 cmake --preset windows-msvc-debug
./cmake/scripts/dev-shell.ps1 cmake --build --preset windows-msvc-debug
./cmake/scripts/dev-shell.ps1 ctest --preset windows-msvc-debug
```

Presets (`CMakePresets.json`):

| Preset | Build type | Notes |
|---|---|---|
| `windows-msvc-debug` | Debug | Box3D asserts enabled |
| `windows-msvc-release` | Release | |
| `windows-msvc-ci` | RelWithDebInfo | `/WX` on engine targets, strict dependency lock |
| `linux-gcc-debug` | Debug | Authored, **not verified** on Linux |

MinGW-w64 GCC also builds and passes all tests (verified with GCC 13.2, no preset):

```powershell
cmake -S . -B build/mingw-gcc-debug -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Debug
```

## Options

| Option | Default | Meaning |
|---|---|---|
| `GAMEENGINE_RESOURCES_DIR` | env / `<repo>/resources` | Third-party sources |
| `GAMEENGINE_BUILD_TESTS` | ON | Build tests and register them with CTest |
| `GAMEENGINE_WARNINGS_AS_ERRORS` | OFF | `/WX` / `-Werror` on engine-owned targets only |
| `GAMEENGINE_STRICT_DEPENDENCY_LOCK` | OFF | Fail configure if a dependency is not at its locked commit |
| `GAMEENGINE_WITH_PHYSICS` | ON | Build Box3D and `engine::physics` |
| `GAMEENGINE_WITH_GNS` | OFF | Build GameNetworkingSockets and `engine::networking_gns` (needs protobuf) |
| `GAMEENGINE_GNS_COMPILE_CHECK` | ON | When GNS is off, compile (not link) the GNS backend against GNS headers |

Compiler settings: C++20, C17, dynamic CRT (`/MD`, `/MDd`) to match `engine::core`.
Warnings (`cmake/GameEngineCompilerOptions.cmake`) apply only to engine targets via
`gameengine_target_defaults()`; third-party targets keep their own flags.

## Targets

| Target | Owner | Links |
|---|---|---|
| `engine::physics` | Claude | Box3D (private) |
| `engine::networking` | Claude | none |
| `engine::networking_gns` | Claude | `engine::networking`, `GameNetworkingSockets::static` (when enabled) |
| `engine::online_services` | Claude | none |
| `engine::core` | Codex | added automatically once `Engine/Core/CMakeLists.txt` exists; receives `ENGINE_DEPENDENCY_ROOT=${GAMEENGINE_RESOURCES_DIR}` |

## Enabling GameNetworkingSockets

GNS needs protobuf on `CMAKE_PREFIX_PATH` (and OpenSSL off Windows). Until that is provided the
transport layer is fully usable through `LoopbackTransport`, and the GNS backend is compile-checked.
Ways to provide protobuf, none done yet:

1. **Isolated vcpkg**: use the standalone vcpkg tool with the shared `vcpkg` ports read-only and
   `--x-install-root`, `--x-buildtrees-root`, `--x-packages-root` and `--downloads-root` all inside
   `build/`. Leaves the shared directory untouched.
2. **Bootstrap the shared vcpkg** (`bootstrap-vcpkg.bat` in the resources dir). Writes into the shared
   directory, which needs approval.
3. Any prebuilt protobuf matching one `protoc` version, via `CMAKE_PREFIX_PATH`.

Then: `cmake --preset windows-msvc-debug -DGAMEENGINE_WITH_GNS=ON -DCMAKE_PREFIX_PATH=<protobuf>`.

## Continuous integration

`.github/workflows/ci.yml` recreates the resources directory with
`cmake/scripts/fetch_dependencies.py`, which shallow-fetches each dependency at its locked commit.
It then builds and tests the `windows-msvc-ci` preset, plus a non-blocking Linux GCC job. The
workflow has been dry-run locally (same script and preset against a fresh fetch), but it has **not
yet run on GitHub-hosted runners**.
