# Scripting

Game behavior can be written in Lua 5.4 scripts. A script is an asset, a `.lua` file in the project's `Assets` folder,
known by its package name like any other (`/Game/Scripts/Gate`). An entity runs a script when its **Script** component
names it. Scripts run in Play and Simulate, not while editing.

To begin one, type a name in the Asset Browser and press **New script**: it makes `Scripts/<name>.lua` from a template.
To run it, select an entity and use **Add Component > Script** (with the script selected in the Asset Browser), or
**Assign to selection**.

## Editing and changing scripts while playing

Double-click a script in the Asset Browser to open it in the **Script Editor**. Its text is checked a moment after you
stop typing, and a mistake is marked at its line before anything is saved. **Ctrl+S** saves it. Any other editor works
too: **Open in External Editor** opens the file at the cursor's line, VS Code by default.

Saving while playing changes the running game within half a second, with no restart: every entity running the script
keeps its `self`, so what it has counted or stored carries on; a property that is new gets its default; and the new
functions run from the next call. A module that another script `require`s can be changed the same way: the scripts that
require it are run again so that each uses the new one. A script saved with a mistake keeps the last good code running,
and the error goes to the Message Log and stays marked in the Script Editor. Stop and Play again for a clean start.

## A script is a class

A script returns a table. Its `properties` are what Details shows; its functions are what the engine calls. Each entity
running the script gets its own instance, `self`, which holds the properties' values and `self.entity`.

```lua
local Gate = {
  properties = {
    open_height = 3.0,
    speed = 1.5,
    trigger = Entity(),
    message = "The gate opens",
  },
}

function Gate:start()
  self.closed_y = self.entity.position.y
end

function Gate:tick(dt)
  if engine.game.targets_left() == 0 then
    local p = self.entity.position
    local top = self.closed_y + self.open_height
    self.entity.position = Vec3(p.x, math.min(p.y + self.speed * dt, top), p.z)
  end
end

return Gate
```

Anything else in the table is the script's own: helper functions, constants. The script's top level runs once when it
is loaded, so it should only declare things; it has no world yet.

### Properties

A property is a name and a default. The default's type is the property's type:

| Type | Default written as | Details shows |
|---|---|---|
| number | `1.5` | A number field |
| boolean | `false` | A checkbox |
| text | `"hello"` | A text field, one line, at most 4096 bytes |
| Vec3 | `Vec3(0, 1, 0)` | Three number fields |
| Color | `Color(1, 0.5, 0)` or `Color(1, 0.5, 0, 0.8)` | A color picker with alpha, sRGB |
| Entity | `Entity()` | A list of the scene's entities |
| Asset | `Asset()` or `Asset("/Game/Sounds/Creak")` | An asset field |

Names are Lua names of 1 to 64 bytes, not Lua's own words and not `entity`; a script has at most 64. Details lists them
in the order the script's text first assigns them.

An entity keeps only the values that differ from the defaults. Change a default in the script and every entity that
kept it changes too. The arrow beside a value takes it back to the default. A value for a property the script no longer
declares, after a rename, is kept and shown dimmed with **Remove**, so nothing is lost by accident.

### What the engine calls

All are optional. They are called on the instances in the order of their entities' IDs.

| Function | When |
|---|---|
| `start()` | Once, on the first tick the instance runs: the first of Play, or the one after it was spawned |
| `tick(dt)` | Every fixed step, 60 times a second, before physics. `dt` is the step in seconds |
| `late_tick(dt)` | Every step, after physics and the game's rules |
| `on_hit(other, speed, point, normal)` | A collider of this entity hit another hard enough to report (colliders with **Events** on) |
| `on_trigger_enter(other)` | Another entity's collider entered this entity's sensor, or this one entered another's |
| `on_trigger_exit(other)` | It left |
| `on_damage(amount, cause, point)` | This entity's health was hurt: cause `"weapon"`, `"impact"` or `"script"` |
| `on_death()` | Its health ran out. The entity may already be gone, so this should not use `self.entity` |
| `on_clip_event(name, point)` | Its animation crossed a clip event, such as a footstep, at `point` |
| `on_stop()` | Play is stopping |

## Values

`Vec3` and `Color` are values: their fields cannot be assigned, so `v.x = 1` is an error and sharing one never changes
another. Make a new one instead.

| | |
|---|---|
| `Vec3(x, y, z)`, `.x .y .z` | `+ - * /` (by a number), unary `-`, `==`, `tostring` |
| `v:length()`, `v:normalized()`, `a:dot(b)`, `a:cross(b)` | |
| `Color(r, g, b[, a])`, `.r .g .b .a` | `==`, `tostring` |
| `Quat(x, y, z, w)`, `Quat.from_euler(x, y, z)`, `Quat.axis_angle(axis, degrees)` | Euler angles in degrees, as Details shows them |
| `q:euler()`, `q:rotate(v)`, `q:inverse()`, `a * b` | |
| `Entity()` | No entity |
| `Asset(name)`, `.name` | An asset by package name |

## Entities

`self.entity`, and every entity a property, a call or an event gives, is a handle. Using one whose entity is gone is an
error at that line; `entity.valid` says whether it is still there.

| | |
|---|---|
| `id`, `name`, `valid` | Read only |
| `position`, `rotation`, `scale` | Relative to its parent; can be set |
| `world_position` | In the world; can be set |
| `world_rotation` | Read only |
| `parent`, `children()` | `nil` for no parent |
| `destroy()` | Removes it and what is under it, at the end of the phase |
| `velocity` | Its body's speed and direction, `nil` without one; can be set on one that moves |
| `dynamic` | It has a body that physics moves |
| `apply_impulse(impulse[, point])` | A push in N·s, at its position or a world point |
| `play(clip[, blend])` | Crossfades its animator to a clip, over `blend` seconds (0.2) |
| `fire_effect([point[, direction[, strength]]])` | Fires its particle emitter's effect once |
| `ragdoll()` | Falls as its Ragdoll from the pose it is in |
| `damage(amount)`, `health` | Through the game's rules: needs a health behavior |
| `script` | Its own script instance, to read its fields or call its functions; `nil` when it runs none |

What a script changes shows at once to every script, and reaches the scene at the end of the phase it was made in: at
the end of the ticks before physics, and at the end of the events and late ticks after it.

## The engine

| | |
|---|---|
| `engine.find(name)` | The first entity, by ID, with this name, or `nil`. Keep the result: finding looks through the scene |
| `engine.spawn(entity[, position[, rotation]])` | A copy of the entity and what is under it, at the root; its scripts start on the next tick |
| `engine.raycast(origin, direction[, distance[, ignore]])` | The first collider along the ray, as `{entity, point, normal, distance}`, or `nil` |
| `engine.ticks()`, `engine.time()` | Steps, and seconds, since Play began |
| `engine.input.held(action)`, `engine.input.pressed(action)` | `MoveForward`, `MoveBack`, `MoveLeft`, `MoveRight`, `Jump`, `Sprint`, `Fire`, `Reload`, `Interact` (E). Only while the game has the mouse |
| `engine.game.active()` | The scene runs a game: an entity has a `game_mode` or `player` behavior |
| `engine.game.score()`, `add_score(points[, message])`, `announce(message)` | |
| `engine.game.time_left()`, `add_time(seconds)`, `give_ammo(rounds)`, `targets_left()` | |
| `engine.game.player()`, `phase()`, `win()`, `lose()` | `phase()` is `"ready"`, `"playing"`, `"paused"`, `"won"` or `"lost"` |
| `print(...)`, `log.info(...)`, `log.warn(...)` | To the Message Log, with the script and line |
| `require "/Game/Scripts/util"` | Another script, run once; returns what it returned |

The rest of Lua that a script gets: the base functions, `math`, `string`, `table`, `utf8` and `coroutine`.

## The sandbox

Scripts cannot reach files, other programs or the network: there is no `io`, `os`, `debug` or `package`, `load` takes
text only, and `require` finds only the project's scripts. Each script has its own globals.

- **Runaway loops** are stopped: a call that runs more than 10 million Lua instructions (about 40 ms) fails, even if it
  catches the error.
- **Memory**: the scripts of a game share 256 MB.
- **Errors** stop only the instance that made them. The Message Log shows the script, line and function, and a toast
  appears; the other instances and the game go on. Stop and Play again to start it afresh.

## Determinism

A run repeats exactly, which tests rely on. No clock reaches a script; `dt` is the fixed step. `math.random` is seeded
the same way every run, and `math.randomseed` needs a number. Table order is the same every run. Scripts run inside the
fixed tick, at the same points of it as the game's own behaviors.
