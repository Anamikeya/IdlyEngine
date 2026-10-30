# 008: Editor UX foundation

How the editor's commands, undo, selection and property metadata fit together, and how to extend them. The plan and its
changelog are in `docs/EDITOR_UX_PLAN.md`.

## Commands

`engine::editor::EditorCommand` is something a user can trigger. It is separate from `engine::commands::Command`, which
is a validated change to the scene and is what an `EditorCommand` usually issues.

```text
default_command_table()      Pure data: id, label, tooltip, category, context, default keys. Read by tests and the docs.
CommandRegistry              Current keys, conflicts, routing, keymap text. No ImGui or GLFW.
EditorUI::bind_commands()    Gives each ID its behavior: execute, can_execute, is_checked.
command_item/button/checkbox Draw a command: label, shortcut, enabled and checked state, tooltip.
route_shortcuts()            Turns key presses into chords and dispatches them.
```

**Routing.** The viewport answers while the mouse is over it, other panels while they have the focus, then Global. A
command that cannot run now is passed over. Nothing fires while a text field has the keyboard, a popup or dialog is open,
a drag is in progress, the game holds the mouse, or the Keyboard Shortcuts panel is waiting for keys.

**Settings** live in `%APPDATA%\GameEngine\`: `keymap.ini` holds only what differs from the defaults, and `layout.ini` is
ImGui's layout. `--config-dir <folder>` points somewhere else. `--smoke` and `--render-test` use none.

### How to add a command

1. Add a row to `default_command_table()` in `src/EditorCommands.cpp`. IDs are `Category.Name`. Check the key is free in
   that context; a test fails on a conflict in the default table.
2. Bind it in `EditorUI::bind_commands()` (`src/ui/CommandUI.cpp`). Startup throws if any command has no behavior.
   Use `can_execute` rather than checks inside `execute`, so menus grey it out and shortcuts pass it by.
3. Draw it with `command_item`, `command_button` or `command_checkbox`. Do not write a label or a shortcut by hand.
4. Regenerate the list: `engine_editor --dump-shortcuts docs/EDITOR_SHORTCUTS.md`. A test fails while it is out of date.

```cpp
{"Viewport.ToggleGrid", "Show Grid", "Draw the grid under the scene.", "Viewport", viewport, {chord(Key::G, alt)}},

bind("Viewport.ToggleGrid", [this] { show_grid = !show_grid; }, {}, [this] { return show_grid; });

command_item("Viewport.ToggleGrid");
```

## Undo

`CommandBus` keeps per-entity before and after records. A full-scene snapshot would waste memory, and do/undo pairs would
discard code that works, so transactions were added to what was there.

- `EditorSession::begin_transaction(name)` and `end_transaction()`, or a `ScopedTransaction` guard. Everything between is
  one undo step. Nested ones join the outer one, whose name wins.
- Each `execute` inside still validates and commits, so a drag is drawn live. `cancel_transaction()` restores everything.
- A transaction that changes nothing adds no step and keeps the redo history.
- `execute(command, "Name")` names a single edit. With no name, the command's own label is used.
- `EditorSession::undo_steps_` holds scene and material steps in one order. Scene data stays in the bus.
- In Play mode the runtime scene keeps no history, so transactions do nothing there.

```cpp
ScopedTransaction transaction(session, "Align 3 Entities");
for (const auto id : session.selection_roots())
    if (const auto moved = session.execute(commands::SetTransform{id, aligned}); !moved) { transaction.cancel(); return; }
```

A gizmo drag uses `begin_drag`, `update_drag` and `end_drag`, or `cancel_drag`.

Known limit: `CommandBus::execute` copies the scene on each call. One live drag step with its dirty check takes about
32 ms at 10,000 entities, which is too slow for 60 Hz at that size. The sample has 73.

## Selection

`SelectionSet` (`engine/editor/Selection.hpp`), owned by `EditorSession`: a set with a primary entity and a change event.
Panels read and write it and never talk to each other. `selection()` is the primary one. `selection_roots()` drops
entities whose ancestor is also selected: use it for anything that moves, copies or deletes. Selecting is not an edit.

## Property metadata

`engine/editor/PropertyMeta.hpp` describes each property: name, type, category, tooltip, range, step, read-only, hidden,
and a condition. Tables are written by hand in `src/PropertyTables.cpp`, in the editor layer, so `Scene.hpp` stays plain
data. `Property::condition` is a predicate rather than Unreal's "another bool", because many conditions depend on an enum.
The Inspector starts using these tables in Phase 3.

### How to add an inspectable property

1. Add the field to the struct in `Engine/Scene/include/engine/scene/Scene.hpp`, with its validation and scene file
   handling as usual.
2. Add a row to the component's table in `src/PropertyTables.cpp`:

   ```cpp
   when(field<T>("range", "Range (m)", "Light", "How far a point or spot light reaches.", PropertyType::Number, &T::range, {0.01, 1000, 0.05}), local),
   ```

3. Add the field to the matching `*_table_covers_every_field` test in `Tests/Editor/editor_ux_tests.cpp`.

Use `choice<T>` for an enum and `asset<T>` for an `AssetRef`.

## Real-input runs

`--ui-map <file>` writes where named controls are, so scripts find them by name, not by a position that changes with the
layout. Commands are named by ID. Other controls use `mark("Asset.Assign")` after the widget, panels are `Panel.<name>`
and their tabs `Tab.<name>`. It costs nothing unless the file is set.
