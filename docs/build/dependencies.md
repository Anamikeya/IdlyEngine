# Dependency audit

Audited 2026-10-27 on branch `agent/claude-systems` against the shared, read-only checkout at
`GAMEENGINE_RESOURCES_DIR` (on the reference machine, `D:\AI\GameEngine\resources`).
Every entry is a clean git checkout (`git status --porcelain` empty) of the listed origin.
Pinned commits live in [`cmake/DependencyLock.cmake`](../../cmake/DependencyLock.cmake), which is the
machine-readable source of truth. Configure verifies each linked dependency against it (warning by
default, fatal with `GAMEENGINE_STRICT_DEPENDENCY_LOCK=ON`, which CI uses).

| Dependency | Origin | Commit | `git describe` | License | Status in engine |
|---|---|---|---|---|---|
| Box3D | github.com/erincatto/box3d | `f36d061b` | v0.1.0-41 | MIT | **Linked** by `engine::physics` |
| GameNetworkingSockets | github.com/ValveSoftware/GameNetworkingSockets | `d534c19a` | v1.6.1 | BSD-style (Valve) | Headers compile-checked; library **blocked on protobuf** |
| ENet | github.com/lsalzman/enet | `5a9c537f` | v1.3.18-17 | MIT | Not integrated (possible future transport) |
| Flecs | github.com/SanderMertens/flecs | `3988dee7` | v4.1.6-692 | MIT | Codex (`engine::core`) |
| Filament | github.com/google/filament | `4e2df5e2` | rc/1.80.0-cut-6 | Apache-2.0 | Codex (renderer, not yet built) |
| GLFW | github.com/glfw/glfw | `92dcf4ce` | 3.5.1-1 | zlib | **Built** as `gameengine::glfw` (static, `/MD`) when `GAMEENGINE_BUILD_EDITOR=ON`; link-tested headless |
| GLM | github.com/g-truc/glm | `6f14f479` | 1.0.0-139 | MIT | Codex (headers) |
| Dear ImGui | github.com/ocornut/imgui | `d88f238d` | v1.93.0-docking-10 | MIT | **Built** as `gameengine::imgui` (core + GLFW platform backend) for Codex's editor; null-backend smoke test |
| miniaudio | github.com/mackron/miniaudio | `9634bedb` | 0.11.25 | Public domain / MIT-0 | Not yet built |
| spdlog | github.com/gabime/spdlog | `6d9805df` | v1.17.0-66 | MIT | Codex (headers) |
| ozz-animation | github.com/guillaumeblanc/ozz-animation | `744eb9d9` | 0.17.0 | MIT | **Linked** as `gameengine::ozz` (runtime and offline libraries, `/MD`). Added 2026-11-03 for Milestone 5 |
| Lua | github.com/lua/lua | `312b9efa` | v5.4.9 | MIT | **Built** as `gameengine::lua`, compiled as C++. Added 2026-11-06 for Milestone 6 |
| sol2 | github.com/ThePhD/sol2 | `9190880c` | v3.5.0 | MIT | **Header-only** target `gameengine::sol2` on Lua. Added 2026-11-06 for Milestone 6 |
| ImGuiColorTextEdit | github.com/goossens/ImGuiColorTextEdit | `12a5a01d` | development | MIT | **Built** as `gameengine::imgui_text_edit` (`TextEditor.cpp` only). Added 2026-11-06 for Milestone 6 |
| Tracy | github.com/wolfpld/tracy | `1f4c5357` | v0.14.1-262 | BSD-3-Clause | Not yet built |
| glTF-Transform | github.com/donmccurdy/glTF-Transform | `8e2a94a2` | v4.5.1-6 | MIT | JavaScript asset tooling, not a C++ dependency |
| Ninja (source) | github.com/ninja-build/ninja | `3ccb2270` | development | Apache-2.0 | Not used; builds use an installed Ninja binary |
| vcpkg | github.com/microsoft/vcpkg | `5363acb7` | 2026.10.16-184 | MIT | **Not bootstrapped** (no `vcpkg.exe`, no installed ports) |

The M2 asset pipeline (`engine::assets`) adds **no** dependency: OBJ, TGA and PPM are parsed by
engine code. `stb_image`/`cgltf` exist only inside `filament/third_party` and are deliberately unused.

Several checkouts are development commits past their last tag (`-N` suffix). They are pinned by
commit, not by tag, so builds are reproducible but not "release" versions.

## Box3D suitability (verified)

Read `include/box3d/*.h`, `src/CMakeLists.txt`, `src/physics_world.c`, `src/shape.c` and built it.

- Real 3D rigid-body engine in C17: static/kinematic/dynamic bodies, sphere/capsule/convex hull/
  mesh/height-field shapes, joints, sensors, contact/hit/body-move events, continuous collision,
  ray/shape casts, recording/replay, optional internal multithreading.
- Designed for cross-platform determinism (`-ffp-contract=off` is applied by its CMake for GCC/Clang).
- Embeds cleanly with `add_subdirectory`: samples, tests and docs are only built when Box3D is the
  top-level project; it does not force a CRT, so it follows the engine's `/MD` choice.
- Capacity: `B3_MAX_WORLDS` = 128 simultaneous worlds. Note `b3GetMaxWorldCount()` is a
  high-water mark, **not** the capacity.
- Version 0.1.0: the API is young and may change; all use is isolated behind `engine::physics`.
- Process-global hooks (`b3SetAllocator`, `b3SetAssertFcn`, `b3SetLogFcn`) are not set by the engine
  yet. If set, it must be once at startup by the host, before any world exists.

### MinGW-w64 GCC issue (found and worked around)

With GCC 13.2 (Strawberry Perl MinGW-w64 build), any contact between a convex hull and another
shape crashed in Box3D's AVX2 path (`vmovaps` to an unaligned stack slot). Root cause is GCC
bug 54412: GCC does not realign the Win64 stack to 32 bytes. Fix:
`-Wa,-muse-unaligned-vector-move` on the `box3d` target for MinGW GCC (needs binutils >= 2.38;
verified with 2.42). MSVC is unaffected.

## GameNetworkingSockets prerequisites (blocking)

- Requires **protobuf** (`protoc` and `libprotobuf` of the same version, plus abseil for current
  protobuf). Not installed anywhere on the reference machine.
- Crypto: on Windows the engine selects `USE_CRYPTO=BCrypt`, so OpenSSL is **not** required there.
  Other platforms need OpenSSL >= 1.1.1 or libsodium.
- ICE/P2P (`ENABLE_ICE`) is disabled; only direct UDP is in scope.
- Decision 2026-10-27: GNS integration is deferred until protobuf is provided. Options are listed
  in [building.md](building.md#enabling-gamenetworkingsockets).

## Other prerequisites observed

- Visual Studio 2022 Community, MSVC 14.44 (cl 19.44.35228), Windows SDK 10.0.26100: present.
- CMake 3.31.2, Ninja 1.12.0 (Strawberry), MinGW-w64 GCC 13.2 / binutils 2.42: present.
- No clang/clang-cl on PATH.
- Filament defaults `USE_STATIC_CRT=ON`; the engine uses the dynamic CRT. Must be overridden when
  Filament is added to the build (flagged by Codex, not yet relevant).

## ozz-animation (added 2026-11-03)

Read and built on 2026-11-03 at `744eb9d9` (release 0.17.0), approved by the user for Milestone 5.

- Runtime (`ozz_base`, `ozz_animation`) and offline (`ozz_animation_offline`) libraries need nothing else. The engine
  turns its tools, FBX and glTF importers, samples, howtos and tests off, so its bundled GLFW, gtest, jsoncpp and
  tinygltf never enter the build. The engine imports glTF itself and builds ozz data with the offline library.
- It needs **CMake 3.30**, above the engine's own 3.25 minimum, so configure fails early with a message when
  `GAMEENGINE_WITH_ANIMATION` is on and CMake is older.
- It defaults to the static CRT. `ozz_build_msvc_rt_dll=ON` gives `/MD`, like the rest of the engine.
- Its folder sets `/W4` and warnings-as-errors for its own targets. Built with MSVC 19.44 in C++20, it compiles with
  no warnings, so this is left as ozz sets it.
- Its CMake writes only under the binary dir. Its `BUILD_CLANG_FORMAT` target would reformat the read-only sources in
  place; it is not part of `all`, and must never be built.
- Rotation keys are stored as three 16-bit components: a sampled pose is within about 0.5 mm per metre of exact.

## Lua, sol2 and ImGuiColorTextEdit (added 2026-11-06)

Read and built on 2026-11-06, approved by the user for Milestone 6 (scripting).

**Lua 5.4.9** (`312b9efa`, tag `v5.4.9` of github.com/lua/lua, the Lua team's own repository). The final 5.4 release.
- Checked against the release: `lua-5.4.9.tar.gz` from lua.org (373,429 bytes, SHA-256 `2335b6c5...6fb8e6`, as lua.org
  lists it) was compared file by file with the tag: the 60 `.c` and `.h` files of its `src` that the tag also has are
  identical. Only the release has `lua.hpp` and `luac.c`, which the engine does not use; only the repository has
  `ltests.c`, `onelua.c` and the test suite.
- Only the library is built (the 32 files listed in `cmake/GameEngineDependencies.cmake`), not `lua.c`, `onelua.c` or
  `ltests.c`. The repository has no license file; the MIT license is at the end of `lua.h`.
- **Compiled as C++** (`/TP`), so a Lua error is a C++ exception and unwinds engine frames, running their destructors,
  instead of `longjmp`ing over them. A consequence: Lua's functions have C++ names, so code including `lua.h` must not
  wrap it in `extern "C"`. sol2 is told with `SOL_USING_CXX_LUA`.
- **Fixed string hash seed.** Lua normally mixes the time and addresses into it (`luai_makeseed` in `lstate.c`), so
  `pairs` would visit a table in a different order in each run, and a script's run would not repeat.
  `cmake/thirdparty/engine_lua_user.h`, included through `LUA_USER_H`, fixes it. A control build of Lua without that
  header gave two states different orders on three runs out of three; with it, `scripting_tests` checks they match.
  Scripts are the project's own code, so the hash flooding a random seed defends against does not arise.
- Debug builds define `LUA_USE_APICHECK`, so a misuse of Lua's C API in a binding stops at once.
- Built with MSVC 19.44 in C++20 at the compiler's default warning level, the library gives no warnings.

**sol2 3.5.0** (`9190880c`, 2025-02-25). Header-only.
- Documented for Lua 5.1 to 5.4 and LuaJIT. It has had no commits since March 2025.
- `SOL_ALL_SAFETIES_ON`: arguments, types, userdata and numbers are checked on every call, in every build, as scripts
  are the project's to get wrong. `SOL_PRINT_ERRORS=0`, as the safeties would otherwise write every error to
  `std::cerr`. `SOL_EXCEPTIONS_ALWAYS_UNSAFE`: an exception from engine code bound to Lua is caught at the call and
  becomes a Lua error with its message, which a script's `pcall` can catch.
- Its users need `/bigobj` under MSVC; the target adds it.

**ImGuiColorTextEdit** (`12a5a01d`, goossens' fork, a development commit). Maintained in 2026, made for Dear ImGui
1.92.9 and 1.93.0; it builds against the engine's 1.93.1 WIP docking commit. It needs only ImGui and C++17. Only
`TextEditor.cpp` is built: `TextDiff` (which needs the BSD-licensed `dtl.h`) and the extras (a language server bridge and
autocomplete) stay out. It has Lua coloring built in, line markers and squiggles with tooltips, and its own undo.

