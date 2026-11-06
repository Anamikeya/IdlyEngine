# Scripting (Milestone 6)

Lua scripts for game behavior: how they are run, where Lua stops, and what keeps a run the same every time. The decisions
are in `../MILESTONE_6_PLAN.md` (D1 to D11); results are in `../MILESTONE_6_REPORT.md`; what script authors need is in
`../SCRIPTING.md`.

```text
Engine/Scripting
  engine::scripting      ScriptRuntime     one Lua state: sandbox, budgets, script classes, instances, hot reload
                         script_types.h    Value (7 types), PropertyDecl, ScriptInfo, ScriptError, ScriptLoader
                         script_loader.h   package_loader: "/Game/Scripts/Gate" through the VirtualFileSystem
                         scene_values.h    ScriptValue (scene) <-> Value (runtime)
                         script_host.h     ScriptHost: what scripts can do, as plain types; no Lua
  engine::script_world   ScriptWorld : ScriptHost, over a play session's Scene, CommandBus, ScenePhysics and Game
Engine/Scene             ScriptComponent (the script and the values that differ from its defaults), format 8
Engine/Editor            EditorSession: play() makes the ScriptWorld, tick() calls it, Stop throws it away;
                         Details (inspect_script), the Script Editor (ImGuiColorTextEdit), reload of changed files
```

## Where Lua is

Lua 5.4.9 and sol2 3.5.0 are compiled into `script_runtime.cpp` and nowhere else; no public header includes a Lua or sol2
header, so sol2's templates are compiled once. Lua is compiled as C++ (`/TP`), so a Lua error is a C++ exception that runs
destructors as it unwinds, instead of a `longjmp` over them. `cmake/thirdparty/engine_lua_user.h` fixes the string hash
seed through `LUA_USER_H` (see Determinism).

The runtime does not know the scene. It binds `ScriptHost`, an abstract interface of plain types (`EntityId`, `Vector3`,
`Quaternion`, `RayHit`), as the methods of `Entity` and the `engine`, `engine.input`, `engine.game` and `log` tables. A
host call that cannot be carried out throws, and the script sees a Lua error at its line. `ScriptWorld` is the host for a
play session; tests give the runtime other hosts, and the standalone player of Milestone 7 can use `ScriptWorld` as the
editor does, since nothing in it depends on the editor.

## A script is a class

A script's top level runs once, with its own globals (a table whose metatable falls back to the shared libraries, set as
the chunk's `_ENV`), and must return a table: the class. Its `properties` table maps names to defaults of seven types
(number, boolean, text, `Vec3`, `Color`, `Entity()`, `Asset(...)`), at most 64. Callbacks are found by name when the class
is loaded, so an instance whose script has no `tick` costs a lookup, not a call.

An instance is a table, `self`: each property's value (the scene's override when its type matches, the default
otherwise) and `self.entity`, with a metatable whose `__index` is the class. Instances are kept by entity ID and called
in ID order; one attached during a call (a spawned copy) waits for the next.

**Details reads a script without a game:** `inspect_script` runs its top level in a throwaway runtime with 32 MB and a
million instructions, and `order_by_source` lists the properties in the order the text first assigns them, as Lua
tables keep none.

## The tick

The session's fixed tick, 60 a second, with scripts at two points after the game's own:

```text
Game::pre_physics()
ScriptWorld::pre_physics()    start() on instances that have not started, then tick(dt); apply()
ScenePhysics::fixed_update()  the step
animation, Game::hear(), ragdoll poses
Game::post_physics()          the game's rules, damage, deaths
ScriptWorld::post_physics()   on_hit, on_trigger_enter/_exit (both entities), on_damage, on_death, on_clip_event,
                              then late_tick(dt); apply()
the session carries out what scripts asked of clips, ragdolls and effects
```

**Writes go through the command bus, once a phase.** A script's change to a transform, or a destroy, goes into an
overlay (`moved_`, `destroyed_`) that every read sees at once, and `apply()` sends it as `SetTransform` and
`DeleteEntity` commands at the end of the phase: the moves first, then the removals, each in transactions of the bus's
maximum size. So physics sees what the ticks
before it moved, the next frame draws it, and Stop throws it away with the session's copy of the scene, as it does the
game's changes. `spawn` applies first, so the copy is of the entity as scripts see it, then duplicates the subtree to the
root and attaches its scripts for the next tick.

**Events go to both entities,** each as `other` to the other, and not at all when the game removed one of them in the same
tick, as the game's own sensors ignore them. Damage and deaths are the game's, from the tick's `post_physics`.

**Entity handles** hold an ID and check it on every use: a destroyed entity is an error at the line that used it, never a
dangling pointer.

## The sandbox and budgets

- **Libraries:** the base functions without `dofile`, `loadfile` or `warn`; `load` takes text only; `math`, `string`
  (without `dump`), `table`, `utf8` and `coroutine`; `collectgarbage` with `count`, `collect` and `step` only. No `io`,
  `os`, `debug` or `package`. `require` loads through the host's loader, once per canonical name, and reports missing
  modules and cycles.
- **Instructions:** a count hook every 1,000 instructions stops a call past 10 million (about 40 ms of a plain loop here).
  `pcall`, `xpcall`, `coroutine.resume`, `coroutine.close` and `load` pass that error on while the call is over budget,
  so a script cannot catch it and carry on.
- **Memory:** the state's allocator refuses to hold more than 256 MB; Lua collects and retries before it fails.
- **Errors:** a message handler records the first Lua frame's script and line and a traceback before the stack unwinds.
  The failing instance is disabled; the others and the game go on. The editor shows it as a toast and a Message Log entry
  whose link opens the Script Editor at the line.

## Determinism

A run repeats exactly, which the equivalence tests rely on. No clock, frame time or render state reaches a script; `dt`
is the fixed step. `math.random` is seeded by the host (the same seed every Play), and `math.randomseed` needs a number.
Lua seeds its string hash from the time and addresses unless told not to, which would change the order of `pairs` between
runs: the fixed seed keeps it. Instances run in ID order, and scripts run at fixed points of the fixed tick.

## Scenes and assets

`.lua` files are `AssetType::Script` assets, catalogued and copied as text. An entity's `ScriptComponent` names one and
keeps only the values that differ from its defaults, each with its type, so a changed default reaches every entity that
kept it. Scene format 8 stores it as `SCRIPT <entity> <script> <count> [<name> <type> <value>]...`; formats 1 to 7 still
load (`006-scene-format.md`). An entity value is a stable ID, pointed at the copy when its target is duplicated or pasted
with it. An asset value is an ID and a path in the scene and a package name in the script; the host says how to go from
one to the other (`AssetNamer`). A value the script no longer declares is kept, dimmed in Details with Remove.

## Editing and hot reload

The Script Editor is goossens' ImGuiColorTextEdit in a docked tab beside the Asset Browser. Typed text is compiled 0.4 s
after typing stops, with the `require`s it names read from disk, and its error is marked at its line before anything is
saved. While Play runs, the session looks at the loaded scripts' files twice a second, so a save from any editor is seen.
`ScriptRuntime::reload` runs the script's top level again; if it returns a class, that class replaces the old one under
every instance, each keeping its `self` and gaining the defaults of new properties. A failure keeps the last good class. A
changed module is dropped from the `require` cache and every script is reloaded, so each requires the new one.

## The C++ behaviors

The 17 C++ behaviors stay, and scripts and behaviors can be on the same entity. The sample's `Mover`, `Pickup` and
`BonusZone` scripts are those behaviors written step for step: the same `std::hypot` for a vector's length (which
`Vec3:length` is), `fmod` along the path and whole numbers toward zero. Played side by side with the C++ versions, every
transform and the HUD agree to the bit after every tick (`Tests/Gameplay/scripted_behavior_tests.cpp`). The one
difference left is the order within a tick: a C++ behavior acts during the game's `post_physics`, before it decides that
time has run out, and a script after it.

## Performance

Measured in slice 6 (the report has the numbers). A tick of a scripted entity costs what its script does: the call
(0.3 µs), the values it makes and the engine calls it makes (0.1 to 0.5 µs each through sol2, with all its safety checks
on), and its moves at the end of the phase. Three things cost more than that, in proportion to the scene rather than to
what the scripts did, until slice 6 removed them:

- **The command bus** copied, validated and committed the whole scene for every transaction, and physics, gameplay and
  scripts each send at least one a tick. A batch that only renames, moves or sets components now runs in place (see
  `001-core-milestone.md`), so a tick's moves cost what was moved.
- **Physics** copied the whole scene every tick to find what has a body. It now mirrors components only when
  `Scene::structure_revision()` has moved, which moves alone never do.
- **sol2 gives every type a finalizer.** Lua keeps each object with one on a list it walks every cycle and frees it only
  in the next, so the thousands of `Vec3`s a tick of movers makes came in pauses of 36 to 51 ms. `Vec3`, `Color`, `Quat`
  and `Entity` own nothing and have none (`without_finalizer`); `Asset`, which holds a name, keeps its own.

Scripts read a transform or a parent from the scene without copying the entity, and send their moves in transactions of
their own, apart from removals, so they take the bus's path for moves.

**Vec3's operators** (`+`, `-`, `*`, `/`, unary `-`, `==`) are what scripts use most, and sol2's checks and overload
resolution made each cost 0.25 to 0.35 µs. They are plain Lua C functions now (`quick_vector_operators`), with Vec3's
metatable and sol2's own function as upvalues: two Vec3 values, or a value and a number, are computed there with the same
arithmetic, and anything else goes to sol2's function, so other operands work or fail as they did. sol2's safety checks
stay on everywhere else; turned off they would have saved another fifth, but they are what turns a wrong argument from a
script into a Lua error instead of a crash.

`Tests/Performance/script_benchmark.cpp` times this without a window; `Tests/Performance/milestone6_performance.ps1` in the
Release editor, where the perf log keeps scripts apart from gameplay.

## Not built

Node graphs (Blueprints), a step debugger with breakpoints, construction scripts that run while editing, networked
scripts and scripted editor tools (D11). One Lua state runs every script of a session on the main thread.
