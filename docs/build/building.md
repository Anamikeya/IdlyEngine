# Building

## Requirements

- CMake >= 3.25, Ninja. CMake 3.30 or later while `GAMEENGINE_WITH_ANIMATION` is on (the default), for ozz-animation.
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
| `GAMEENGINE_WITH_ANIMATION` | ON | Build ozz-animation as `gameengine::ozz`. Needs CMake 3.30 |
| `GAMEENGINE_BUILD_EDITOR` | ON | Define `gameengine::glfw` / `gameengine::imgui` and add `Engine/Editor` when it exists |
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
| `engine::physics_scene` | Claude | `engine::physics`, `engine::core` (scene/command bridge, built when core exists) |
| `engine::assets` | Claude | `engine::core` (for `Result`) |
| `gameengine::glfw` | Claude (build) | GLFW from resources, `EXCLUDE_FROM_ALL` |
| `gameengine::imgui` | Claude (build) | ImGui core + GLFW backend, `EXCLUDE_FROM_ALL`; no renderer backend |
| `engine::renderer` | Codex | `engine::core` |
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
It then builds and tests the `windows-msvc-ci` and `windows-msvc-release` presets, plus a non-blocking
Linux GCC job (editor targets off there: GLFW needs X11/Wayland packages and Linux is unverified). The
workflow has been dry-run locally (same script and preset against a fresh fetch), but it has **not
yet run on GitHub-hosted runners**.

## When `vcvars64.bat` stalls

On the reference machine on 2026-10-28, `vcvars64.bat` (and therefore `dev-shell.ps1`) began hanging
inside `Common7/Tools/vsdevcmd/ext/vcvars.bat` after printing its banner, although it had worked
earlier the same day. The root cause was not identified. Builds were completed by exporting the
equivalent environment directly from Git Bash:

```bash
VCT='C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207'
SDK='C:/Program Files (x86)/Windows Kits/10'; SDKV=10.0.26100.0
export INCLUDE="$VCT/include;$SDK/Include/$SDKV/ucrt;$SDK/Include/$SDKV/shared;$SDK/Include/$SDKV/um"
export LIB="$VCT/lib/x64;$SDK/Lib/$SDKV/ucrt/x64;$SDK/Lib/$SDKV/um/x64"
export PATH="$(cygpath -u "$VCT/bin/Hostx64/x64"):$(cygpath -u "$SDK/bin/$SDKV/x64"):$PATH"
cmake --preset windows-msvc-debug && cmake --build --preset windows-msvc-debug && ctest --preset windows-msvc-debug
```

Adjust the toolset and SDK versions to the installed ones. Prefer `dev-shell.ps1` whenever it works.

This recipe only works with `-DGAMEENGINE_BUILD_EDITOR=OFF`. The editor builds Filament, which fails
configure whenever `MSYSTEM` is set ("MSYS2 Subsystem detected"), and Git Bash re-adds `MSYSTEM` to every
Windows process it starts, so `unset` does not help. For editor builds, set the same `INCLUDE`/`LIB`/`PATH`
variables in PowerShell (`$env:INCLUDE = ...`) and run the cmake commands from there.

## Editor build (Filament) requirements

- Configure from an MSVC x64 environment (`vcvars64.bat` or `dev-shell.ps1`) and pass
  `-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl` (the presets do this). Without both, CMake picks the first
  compiler on `PATH`, which on the reference machine is Strawberry GCC, and the editor is skipped.
- Put the build directory on the same drive as `GAMEENGINE_RESOURCES_DIR`. Filament's tool manifest path is
  passed as a path relative to its source root, and a relative path cannot cross drives.
- A build directory first configured before 2026-10-29 may hold the relative `CMAKE_INSTALL_PREFIX=install`
  written by Filament's glslang, which makes every re-configure fail in Box3D's `file(RELATIVE_PATH)`. Re-run
  configure once with an absolute `-DCMAKE_INSTALL_PREFIX=...`, or use a fresh directory.
- `could not load cache` / `ninja: error: loading 'build.ninja'` only mean that the configure step for that
  directory never finished; fix the configure error first.

## Running the sample

The Physics Shooter sample is in `Samples/PhysicsShooter`. Start it with `engine_editor --sample`, or from the toolbar's **Project...** menu. See its `README.md`. For how to use the editor, see `../EDITOR_GUIDE.md`.

