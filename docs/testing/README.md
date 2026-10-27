# Testing

## Running

```powershell
./cmake/scripts/dev-shell.ps1 ctest --preset windows-msvc-debug          # everything
./cmake/scripts/dev-shell.ps1 ctest --preset windows-msvc-debug -L physics
./cmake/scripts/dev-shell.ps1 ctest --preset windows-msvc-debug -R connection_manager
build/windows-msvc-debug/bin/physics_tests.exe --list
build/windows-msvc-debug/bin/physics_tests.exe --run free_fall_matches_gravity
```

## Framework

`Tests/Framework` is a small self-registering framework with no third-party dependency, so it
builds wherever the engine builds, including future mobile targets.

```cpp
#include "engine/test/test.h"

ENGINE_TEST(my_case) {
	ENGINE_REQUIRE(setup_ok);         // aborts this case on failure
	ENGINE_CHECK(x == 1);             // records the failure and continues
	ENGINE_CHECK_EQ(a, b);            // prints both values
	ENGINE_CHECK_NEAR(v, 9.81, 1e-3);
}
```

Register an executable with `gameengine_add_test_executable(<target> LABELS ... SOURCES ...
LIBRARIES ...)` (`cmake/GameEngineTesting.cmake`). After each build the executable is asked for
`--list`, and every case becomes its own CTest test named `<target>.<case>`. Each case therefore
runs in a fresh process, which isolates process-global state such as Box3D's world table.
Case names must be unique within an executable.

## Current suites (79 cases)

| Executable | Labels | Cases | Covers |
|---|---|---|---|
| `physics_tests` | physics, unit | 31 | World lifecycle and limits, config validation, move semantics, Box3D memory release, fixed step and catch-up, free-fall accuracy, bitwise determinism, body/collider lifecycle, stale/foreign handles, input validation, mass, rest/sleep, impulses, kinematic targets, filters, moved/contact/hit/sensor events |
| `networking_tests` | networking, unit | 41 | Serialization (endianness, varuint edge cases, bounded strings, sticky failure, size limits), loopback transport (ordering, seeded loss, async connect failure, reject/close semantics, shutdown), connection manager (handshake, dispatch, broadcast, version mismatch, server full, unknown/malformed/premature/oversized/flood violations, handshake timeout, handler-initiated disconnect, diagnostics, shutdown) |
| `online_services_tests` | online, unit | 7 | NullOnlineService lifecycle, identity, auth ticket round trip and rejections, achievements |

Verified passing on 2026-10-27:

| Toolchain | Debug | Release | RelWithDebInfo + `/WX` |
|---|---|---|---|
| MSVC 19.44 (VS 2022 17.14), Ninja | 79/79 | 79/79 | 79/79 |
| MinGW-w64 GCC 13.2, Ninja | 79/79 | not run | not run |

Not covered yet:
- GameNetworkingSockets over real UDP (blocked on protobuf; the backend only compiles).
- Linux, macOS, iOS, Android: nothing has run on them.
- Cross-compiler determinism (MSVC vs GCC bitwise equality) is untested. Determinism tests compare
  two runs within one binary.
- ECS integration tests wait on committed `engine::core` contracts.

## Conventions

- Tests use only public headers of the module under test.
- No wall clock in assertions. Time is passed explicitly (`advance(ns)`, `poll(now)`).
- No real sockets in unit tests. Use `LoopbackTransport`, which is deterministic and seedable.
- Network integration tests that use real UDP will carry the `integration` label and bind
  `127.0.0.1` on ephemeral ports.
