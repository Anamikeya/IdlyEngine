# Milestone 6 plan: scripting

Branch `milestone-6`, worktree `D:\AI\GameEngine-Integration`, created from `main` at `ea2fa85` (the Milestone 5 merge).
**Status: approved 2026-11-06.** Do not merge into `main` without the user's approval.

Decisions (2026-11-06), the recommended option each time: Lua 5.4.9 with sol2 3.5.0, and goossens' ImGuiColorTextEdit,
go into `resources` at the pinned commits. Scripts are edited in a Script Editor tab, with Open in External Editor.
The shooter keeps its C++ behaviors, gains scripted `mover`, `pickup` and `bonus_zone` that must play the same, and a
scripted gate and button. Text scripts only: node graphs are not part of this milestone.

Goal: game behavior written in Lua scripts, created, edited and hot-reloaded in the editor, as Unreal does with
Blueprints but as text. A script is an asset: an entity's Script component names it, and the Details panel shows the
properties it declares. Scripts run in Play and Simulate, inside the fixed tick, so a run stays reproducible. The
shooter gains scripted parts, and its simple C++ behaviors are shown to play the same when written as scripts.
The standalone player and packaging are Milestone 7; nothing here depends on the editor, so the player runs scripts
the same way.

## 1. Audit

### Git

| Check | Result |
|---|---|
| `main` | `ea2fa85` *Merge Milestone 5: characters and effects*, pushed to `origin` with `milestone-5` (`9c3d7fb`) |
| This worktree | `milestone-6`, new, at `main`. Untracked files from earlier runs (`audit.txt`, `build-log.txt`, `smoke-*`, `imgui.ini`, `docs/UNREAL_UX_REFERENCE.md`) are left alone |
| Tests | 555 CTest entries pass at `cabee69`, whose files `main` matches |

### What exists

| Area | State after Milestone 5 | Reused as |
|---|---|---|
| Gameplay | `Engine/Gameplay`: one `Game` class (`game.cpp`, 1,008 lines) with 17 behavior types from a fixed list. A `BehaviorComponent` is a type and up to 32 named **numbers**, parsed when Play starts. A tick is `pre_physics()`, physics, animation, `post_physics()`; the game reports damage, shots and effect, animation and ragdoll requests for the host to carry out | Scripts run at the same two points of the tick and hear the same events. `Game` gains a small surface for scripts: score, damage, win and lose. Scripts also run in scenes with no `game_mode` |
| Play | `EditorSession::play()` copies the scene into a runtime scene with its own physics and `Game`. `tick()` runs at a fixed step; Stop throws the runtime away | The script runtime lives in that runtime and goes with it, so Play and Stop keep the edit scene untouched |
| Input | Eight actions (move, jump, sprint, fire, reload) from an `InputMap`; gameplay reads actions, never devices | Scripts read actions too. An `Interact` action (E) is added for scripted doors and buttons |
| Physics queries | `ScenePhysics::raycast`, `apply_impulse`, collision and sensor events | Bound for scripts as they are |
| Assets | Mesh, Texture, Material, Environment, Effect. `/Game/...` package names through the `VirtualFileSystem`, redirectors. **Nothing watches files**: the database rescans when asked | `.lua` becomes a Script asset; `require` resolves `/Game/...` names; hot reload watches only the scripts that are loaded |
| Scene | Format 7. Nothing can hold text, booleans or entity references for gameplay | Format 8 adds the Script component and its property values |
| Editor | Details, Asset Browser, Message Log with links to entities and assets, toasts, the Console (`~`) with commands and variables, the Material and Effect Editors as docked tabs. No text editor beyond ImGui's plain `InputText` | A Script Editor tab beside them; Message Log links gain a script line |

### Libraries

None of these is in `D:\AI\GameEngine\resources` yet; adding any of them needs your approval (decision 1). They would be
fetched by `fetch_dependencies.py` at the pinned commits and listed in `cmake/DependencyLock.cmake`.

| Library | State | Use |
|---|---|---|
| Lua 5.4.9 (`312b9efa`, tag `v5.4.9` of github.com/lua/lua, 2026-08-25, MIT) | The final 5.4 release; lua.org plans no more. The GitHub repository is the Lua team's own; slice 0 checks the tag's sources against the lua.org tarball | The language. Compiled as C++ so that Lua errors unwind through C++ code instead of `longjmp`ing over destructors |
| Lua 5.5.1 (`7579fc9d`, 2026-08-03) | The current series: `global` declarations, read-only loop variables, smaller arrays. Its C API changed slightly | The alternative in decision 1 |
| sol2 3.5.0 (`9190880c`, 2025-02-25, MIT) | Header-only C++17 bindings. Documented for Lua 5.1 to 5.4 and LuaJIT; **Lua 5.5 is not listed**, and in 2024 its maintainer said 5.5 was unsupported. No commits since March 2025 | Binds the engine API: classes, functions, tables and errors |
| ImGuiColorTextEdit, goossens' fork (`12a5a01d`, MIT) | Maintained in 2026 and made for Dear ImGui 1.92.9 and 1.93.0 (ours is 1.93.1 WIP, docking). Needs nothing else. Has syntax colors, line numbers, find, and markers and squiggles with tooltips | The Script Editor (decision 2) |

## 2. Architecture

```text
Engine/Assets              Script asset type (.lua); script files read through the VFS
Engine/Scripting (new)     Lua and sol2 behind it: the script runtime, sandbox, budgets, script classes, properties,
                           hot reload, the engine API. Nothing else includes a Lua header
Engine/Scene               ScriptComponent and format 8
Engine/Gameplay            A surface for scripts on Game; the Interact action
Engine/Editor              Script component in Details, Script Editor tab, New Script, Message Log script links,
                           hot reload while editing and in Play
```

**D1. A script is a class.** A `.lua` file returns a table. Its `properties` are what Details shows, with their types
and defaults, and its functions are called by the engine. Each entity with the script gets its own instance, `self`:

```lua
local Gate = {
  properties = { open_height = 3.0, speed = 1.5, trigger = Entity(), message = "The gate opens" },
}
function Gate:start() self.closed_y = self.entity.position.y end
function Gate:tick(dt) ... end
function Gate:on_trigger_enter(other) ... end
return Gate
```

Called when there is one: `start`, `tick(dt)` before physics, `late_tick(dt)` after it, `on_hit(other, speed, point,
normal)`, `on_trigger_enter(other)` and `on_trigger_exit(other)`, `on_damage(amount, cause, point)`, `on_death()`,
`on_clip_event(name, point)`, `on_stop()`. Property types: number, boolean, text, vector, color, entity and asset.

**D2. Scripts run in the fixed tick, one Lua state per Play session.** No wall clock, frame time or render state
reaches a script; `dt` is the fixed step. `math.random` is replaced by a generator seeded per session, and Lua's
string hash seed is fixed at build time, so table order, and with it a whole run, repeats exactly. Instances run in
the order of their entity IDs. Scripts change the scene through the same command batch as `Game`, so their changes
take effect at the next sync and Stop restores the scene.

**D3. The sandbox.** Scripts get `math`, `string`, `table`, `utf8` and the engine API. No `io`, `os`, `debug`,
`package.loadlib` or loading of compiled chunks. `require "/Game/Scripts/util"` loads another script through the VFS,
inside the same sandbox. A script that runs too long in one call (an instruction count, about 50 ms of work) is
stopped with an error naming its line. The state has a memory limit (256 MB at first).

**D4. Errors stop one instance, never the game.** A failing call disables that instance, logs the error with its file,
line and traceback to the Message Log, and shows a toast. Clicking it opens the Script Editor at that line. The other
instances and the C++ behaviors keep running. Errors in `start` are reported once, not every tick.

**D5. The engine API.** One `engine` table plus `self.entity`:

| Part | What |
|---|---|
| Entities | name, position, rotation, scale (local and world), parent, children, find by name, spawn a copy of a template, destroy, enable |
| Physics | raycast, apply impulse, velocity, collision events (D1) |
| Input | `input.held("Fire")`, `input.pressed("Interact")` and the look direction, while the player is possessed |
| Animation | play a clip, crossfade, clip events, joint positions |
| Effects | fire an emitter's effect at a point and direction |
| Game | score and messages, damage an entity, health, win and lose, time left; ragdolls on demand |
| Output | `print` and `log.warn` to the Message Log, with the script's name and line |

Entity handles hold a stable ID and check it on every use, so a destroyed entity gives a clear error, never a crash.
Vectors and quaternions are value types. The API is described in `docs/SCRIPTING.md` and checked by its own tests.

**D6. Script properties in the scene.** `ScriptComponent`: the script asset, then values for the properties that
differ from the script's defaults, each with its type. Format 8; versions 1 to 7 still load. An entity property is a
stable ID, remapped when entities are duplicated or pasted together, like parents are. A property the script no longer
declares is kept, dimmed in Details with a remove button, so renaming a property in a script loses nothing silently.

**D7. Details reads a script without running the game.** When a script is loaded or saved, the editor runs its top
level in a throwaway sandbox with a small budget, to read `properties`. Details then shows a field for each one, typed,
with its default, and a reset arrow on values that differ, as Unreal does. Changes are undoable like any other field.

**D8. Hot reload.** The editor checks the loaded scripts' files twice a second, so a save from the Script Editor or from
VS Code is seen. A script that compiles replaces its functions in every running instance, which keep their `self`
fields; a new property gets its default. A script that does not compile keeps the old code running and marks the error
in the Script Editor. While editing, a reload refreshes Details.

**D9. The Script Editor.** A docked tab beside the Material and Effect Editors: Lua colors, line numbers, find and
replace, error markers with tooltips, Ctrl+S to save, and a list of the script's properties and functions. **Open in
External Editor** runs the editor named in Preferences (`code -g <file>:<line>` by default). **New Script** in the Asset
Browser starts from a template with every callback commented, and **Add Component > Script** attaches one.

**D10. C++ behaviors stay.** The 17 behaviors keep working, and every existing scene plays as before. Scripts and
behaviors can be on the same entity: a script hears the damage and death the C++ `health` behavior decides.

**D11. Out of scope:** node graphs (Blueprints), a step debugger with breakpoints, construction scripts that run while
editing, networked scripts, and scripted editor tools.

## 3. Slices

| # | Slice | Done when |
|---|---|---|
| 0 | **Lua, sol2 and the text editor in the build:** lock entries, `fetch_dependencies.py`, CI fetch list, `gameengine::lua` compiled as C++ with a fixed hash seed, sol2 as an interface target, docs/build/dependencies.md | Both MSVC presets configure from fresh folders. A smoke test runs a script that calls into C++ and catches an error |
| 1 | **The script runtime:** `Engine/Scripting`, the sandbox, budgets, script classes and properties, `require` through the VFS, errors with tracebacks | `scripting_tests`: sandbox escapes refused, a runaway loop stopped, memory limit, errors located, determinism |
| 2 | **Scripts in scenes and assets:** the Script asset type, `ScriptComponent`, format 8, commands, duplicate and paste remapping, New Script, Details fields with defaults and reset | `asset_tests`, `engine.core.acceptance`, `editor_script_tests` without a window |
| 3 | **The engine API:** D5, called from the tick and the game's events, the Interact action, `docs/SCRIPTING.md` | `scripting_tests` and `gameplay_tests`: each API part, events in order, Play and Stop isolation, a run repeated tick for tick |
| 4 | **Script Editor and hot reload:** the tab, markers, Message Log script links, external editor, file checks, reload in Play keeping state | Editor tests; real input writes a script, attaches it, plays, edits it while playing and sees the change |
| 5 | **The shooter:** `mover`, `pickup` and `bonus_zone` rewritten as scripts and shown to play the same as the C++ ones; a scripted gate that opens when its room's targets fall, and a button the player presses with E | `gameplay_tests` compare the C++ and scripted versions tick for tick; the game is won and lost with real input |
| 6 | **Performance, fresh builds, report** | 500 scripted entities ticking in Release, measured; the report from runs made in this milestone |

Cut order if time runs short: the button, then the property list in the Script Editor, then find and replace (the
external editor stays).

## 4. Tests

| Area | Where |
|---|---|
| Sandbox: no `io`, `os`, `debug` or bytecode; `require` stays inside the project; instruction and memory limits | `scripting_tests` (new) |
| Script classes, properties and their types, defaults, overrides, errors with file and line | `scripting_tests` |
| Hot reload keeping state, a failed reload keeping the old code, a new property getting its default | `scripting_tests` |
| Every API part, handles to destroyed entities, call order, a run repeated exactly | `scripting_tests`, `gameplay_tests` |
| Scripted and C++ `mover`, `pickup` and `bonus_zone` playing the same | `gameplay_tests` |
| Format 8, Script commands, remapping entity properties, versions 1 to 7 loading | `engine.core.acceptance` |
| Details fields, New Script, Script Editor save and markers, log links, without a window | `editor_script_tests` (new) |
| Real input | `Tests/RealInput/milestone6.ps1`, with the focus and idle guards |

## 5. Risks

- **sol2 under C++20 and MSVC 19.44** with warnings as errors: it is large and template-heavy, and has had no commits
  since March 2025. Slice 0 finds out first. Fallbacks: its warnings off for the files that include it, which are all
  in `Engine/Scripting`; then plain Lua C API bindings, which are more code but have no dependency.
- **Compile time.** sol2 slows builds. It stays inside `Engine/Scripting`'s sources, behind headers that include no Lua.
- **Determinism.** Lua seeds its string hashing from the time and addresses unless told not to, which would change
  `pairs` order between runs. D2 fixes the seed; a test runs a script that iterates tables twice and compares.
- **Hot reload and state.** A reload that changes what `self` fields mean can leave an instance confused. That is the
  same in every engine with hot reload; Stop and Play, or the game's Restart button, always gives a clean run.
- **Lua errors through C++.** With Lua compiled as C++, errors are exceptions; every call into a script is wrapped, and
  a test throws from deep inside a callback.
- **Real input** needs a free desktop.

## 6. Decisions for the user

1. **The language and its libraries.** Recommended: Lua 5.4.9 with sol2 3.5.0, a long-proven pair, both MIT, added to
   `resources` at the commits above. Alternatives: Lua 5.5.1 with sol2, which is newer but which sol2 does not claim to
   support, so slice 0 might end in bindings of our own; or Luau (Roblox's Lua, with types and a sandbox built in), which
   needs its own bindings and is no longer the Lua expected since Milestone 5.
2. **The Script Editor.** Recommended: a tab in the editor with ImGuiColorTextEdit (goossens' fork, MIT, added to
   `resources`), plus Open in External Editor. Alternatives: the external editor only, with no new library; or ImGui's
   plain text box, with no colors or markers.
3. **What the shooter gets.** Recommended: keep the C++ behaviors, rewrite `mover`, `pickup` and `bonus_zone` as scripts
   with tests that they play the same, and add a scripted gate and button. Alternatives: rewrite every behavior,
   including the player, weapon and enemies, as scripts (much larger, and the shooter's tests would all move); or only
   new scripted content, with no rewrites.
4. **Node graphs.** Recommended: text scripts only in this milestone; node graphs (Blueprints) are a milestone of their
   own if you want them. Alternative: a first node graph editor in this milestone, which would about double it.

## 7. Acceptance

- Both MSVC presets build from fresh folders. Every existing test passes, and none is removed or weakened.
- A script is created in the editor, attached to an entity, and its properties edited in Details and undone. It runs in
  Play and Simulate, and the scene is the same after Stop.
- Saving a script during Play changes the running game within a second, keeping each instance's state. A syntax error
  keeps the old code running and shows the error at its line.
- A runtime error stops only that instance and shows the file, line and traceback; clicking it opens the Script Editor
  at that line. An endless loop is stopped and the editor stays responsive.
- Scripts cannot read or write files outside their `require`s, start processes or use the network.
- The scripted `mover`, `pickup` and `bonus_zone` play the same as the C++ ones, tick for tick.
- The shooter, with its scripted gate, is won and lost with real input.
- 500 scripted entities ticking at 60 Hz cost under 2 ms of CPU a frame in Release, or the report says what they cost.
- The report lists what was verified, how, and what was not.

## 8. Progress

### Slice 0: Lua, sol2 and the text editor in the build (done 2026-11-06)

- **Fetched** with `fetch_dependencies.py` into `resources` at the locked commits; each checkout is clean. The CI fetch
  lists name them (the Linux job, which builds no editor, fetches Lua and sol2 only).
- **Lua's sources are the release's:** `lua-5.4.9.tar.gz` from lua.org, with the SHA-256 lua.org lists, matches the
  GitHub tag file for file (`docs/build/dependencies.md`).
- **`gameengine::lua`:** the 32 library files, compiled as C++ (`/TP`), with `cmake/thirdparty/engine_lua_user.h`
  fixing the string hash seed through `LUA_USER_H`. A control build without it gave two states different table orders
  on three runs out of three. Debug builds check every use of the C API (`LUA_USE_APICHECK`).
- **`gameengine::sol2`:** header-only, all safeties on, errors not printed, engine exceptions turned into Lua errors
  where Lua called the engine, `/bigobj`. It compiled under C++20, MSVC 19.44 and `/WX` with no warnings, so the
  fallbacks in section 5 were not needed.
- **`gameengine::imgui_text_edit`:** `TextEditor.cpp` only, on `gameengine::imgui`.
- **`GAMEENGINE_WITH_SCRIPTING`** (on by default) gates Lua and sol2. Also fixed: a missing space in the
  `GAMEENGINE_WITH_GNS` option, which made every configure print a CMake syntax warning.
- **Tests:** `scripting_tests` (7 cases, `Tests/Scripting/lua_smoke_tests.cpp`) and a third case in
  `editor_dependency_smoke`, which draws a Lua script with a marker in the text editor through ImGui's null backend.
- **Verified:** both MSVC presets configured and built in empty folders (`build-m6-*`, 1,079 steps, 354 s and 340 s)
  and pass 563 of 563 CTest entries. No warning came from the new libraries, the engine or the tests; the CI preset
  builds them with `/WX`.

### Slice 1: the script runtime (done 2026-11-06)

- **`Engine/Scripting`** (`engine::scripting`): `script_types.h` (values, property declarations, script info, errors,
  printed lines, the loader), `script_runtime.h` (`ScriptRuntime`, `inspect_script`, `order_by_source`) and
  `script_loader.h` (`package_loader` through the VFS, `memory_loader`). Lua and sol2 are private to its sources.
- **Script classes:** a script returns a table. Its `properties` map names to defaults of seven types: number, boolean,
  text, `Vec3`, `Color`, `Entity()` and `Asset("/Game/...")`, at most 64. Details will list them in the order the
  script's text first assigns them, as Lua tables keep no order. Its callbacks are found by name. Each instance's
  `self` holds the properties, then whatever the script adds, and falls back to the class for functions. Overrides of
  the wrong type are reported and the default is used.
- **Sandbox:** base without `dofile`, `loadfile`, `warn` or bytecode (`load` is text only), plus `coroutine`, `math`,
  `string` (no `dump`), `table` and `utf8`. `collectgarbage` keeps `count`, `collect` and `step`. `math.randomseed`
  needs a number, and the host seeds it. Each script has its own globals, falling back to the shared libraries.
  `require` loads through the loader once per canonical name, and reports missing modules and cycles.
- **Budgets:** an instruction count hook every 1,000 instructions stops a call past 10 million instructions (36 to 42
  ms of a plain loop here). Found while testing: a script that catches that error with `pcall` in a loop ran forever,
  so `pcall`, `xpcall`, `coroutine.resume`, `coroutine.close` and `load` now pass the error on when the call is over
  its budget. Memory is limited by the state's allocator (256 MB by default); Lua collects and retries before it fails.
- **Errors:** a message handler records the first Lua frame's script and line and a traceback before the stack
  unwinds; `name:line:` is taken off the message when it names a script the runtime compiled. A failed call disables
  that instance only; attaching it again starts afresh.
- **Assets:** `AssetType::Script` for `.lua`. The database catalogues and copies scripts without decoding them; the
  Asset Browser labels them SCRIPT. The type filter chip waits for slice 2, as it moves the browser's layout.
- **Tests:** `scripting_tests` gains 16 cases (23 in all) and `asset_tests` one. Both presets build and pass 580 of 580.
