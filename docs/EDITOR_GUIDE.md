# How to use the editor

A guide to working in the editor, from starting it to playing your scene. Every key named here is the default, and all of them can be
changed. The full list is in `EDITOR_SHORTCUTS.md`. To build the editor, see `build/building.md`.

## Start it

A preset build puts the editor in `build\<preset>\bin`:

```powershell
build\windows-msvc-release\bin\engine_editor.exe --sample
```

| Option | What it does |
|---|---|
| (none) | Opens an untitled scene with one cube, and treats the current folder as the project |
| `--sample` | Opens the Physics Shooter sample |
| `--project <folder>` | Uses this folder as the project |
| `--scene <file>` | Loads this scene at the start |
| `--config-dir <folder>` | Keeps settings in this folder instead of `%APPDATA%\GameEngine\` |

With no options, the current folder matters: imports go to its `Assets` folder, and `Untitled.scene` saves there too. Choose a project
first if you mean to keep your work.

Closing the main window asks about unsaved changes. Closing the black console window behind it ends the editor at once, without asking.

## What's on screen

| Panel | Use it to |
|---|---|
| **Toolbar** | Create, undo, save and play. It also holds the scene file path and the Move, Rotate or Scale choice |
| **Scene Hierarchy** | See, find and arrange everything in the scene |
| **3D Viewport** | Look around, select and move things, and play |
| **Inspector** | Change the values of what's selected. It shares its place with the next two, so click its tab if another is in front |
| **Lighting** | Set the sky, exposure and shadows for the scene |
| **Material Editor** | Change the material selected in the Asset Browser |
| **Asset Browser** | Import, find and use models, images, materials and HDR images |
| **Message Log** | Read what the editor has told you, filtered by severity |
| **Console** | Type commands and change settings |

Drag a tab to move a panel. **Window** turns panels on and off, saves a layout with **Save Layout...**, and restores the original with
**Reset Layout**. Panels stay inside the main window, because separate OS windows aren't supported yet.

**To learn the keys as you go:** hover over a button to see what it does and its shortcut, and hold `F1` or `?` to see the keys that
work where the mouse is. You can also press `Ctrl+Shift+P` and type part of a command's name.

## Make your first scene

1. Open **Project... > New Empty Project...** in the toolbar, type a folder path and press Enter. This creates the folder, an `Assets`
   folder and `main.scene`.
2. Open **Create...** and add a **Floor**, then a **Cube**. The cube is selected and sits on the floor.
3. With the mouse over the viewport, press `W` and drag the red arrow to move the cube to one side.
4. Open **Create...** and add a **Falling Sphere**. It appears 3 m above the center of the floor.
5. Press `Ctrl+S` to save. A message in the bottom-right corner confirms it.
6. Press `Alt+S` to simulate. The sphere falls and comes to rest on the floor. Press `Esc` to stop, and it goes back to where it started.

A plain **Cube** has no collider, so the sphere would fall straight through it. To make something solid, use **Falling Cube** or **Floor**,
or add a collider with **Add Component** in the Inspector.

## Move around the viewport

The mouse must be over the viewport.

| Input | Action |
|---|---|
| Hold RMB, then `W` `A` `S` `D` | Fly |
| Hold RMB, then `Q` / `E` | Fly down / up |
| Hold RMB, then `Z` / `C` | Widen / narrow the view while held |
| RMB + wheel | Change flying speed |
| Shift while flying | Move three times as fast |
| Alt + LMB drag | Orbit around the selection |
| Alt + RMB drag | Dolly |
| MMB drag, or Alt + MMB drag | Pan |
| LMB drag | Move forward and back, and turn |
| LMB + RMB drag | Move up, down and sideways |
| Wheel | Zoom |
| `F` | Frame the selection |

| Key | Action |
|---|---|
| `Alt+G` / `Alt+H` / `Alt+J` / `Alt+K` | Perspective / front / top / side view |
| `Alt+4` / `Alt+3` / `Alt+2` | Lit / unlit / wireframe |
| `G` | Game view, which hides the editor's helpers |
| `F11` | Maximize the viewport |
| `Ctrl+0` to `Ctrl+9` | Save this camera position as a bookmark |
| `0` to `9` | Go back to a bookmark |

The two buttons at the viewport's top left choose the view and what it shows, such as the grid, shadows and colliders. The controls at
the top right set snapping and camera speed.

## Select and change things

Click to select, and Ctrl+click to add to or remove from the selection. In the Scene Hierarchy, Shift+click selects a range. `Ctrl+A`
selects everything and `Esc` deselects.

Press `W` to move, `E` to rotate and `R` to scale, or `Space` to go to the next one. ``Ctrl+` `` switches between world and local axes.

| While dragging the gizmo | Effect |
|---|---|
| Hold Alt when you start | Copy the selection and move the copies |
| Hold Shift | The camera follows what moves |
| Hold `V` | Snap to the nearest vertex |
| Press `Esc` | Cancel the drag |

**Snapping:** use **Grid**, **Rot** and **Scale** at the viewport's top right, with the value beside each one. `[` and `]` change the
grid size.

**Exact values:** keep the mouse button down on the gizmo and type a number.

| Type | Result |
|---|---|
| `x3` | Moves 3 m along X from where the drag began |
| `z-1.5` | Moves 1.5 m back along Z |
| `y90` on a rotate ring | Turns 90 degrees around Y |
| `2` on the center of the scale gizmo | Scales to twice the size |
| `2` on an arrow or ring | Goes along that handle's axis |

Press Enter or release the button to confirm, `Esc` to cancel, and Backspace to edit. Deleting everything you typed hands control back to
the mouse. The number is always an amount from where the drag began, not a final position. To set a final position, use the Inspector.
A plane handle needs an axis letter first, and the whole typed drag is one undo step.

**Other edits:** `Ctrl+D` duplicates, `Ctrl+C` and `Ctrl+V` copy and paste, `F2` renames, `Delete` deletes, `Ctrl+G` groups, `H` hides and
`Ctrl+H` unhides everything. `End` drops the selection onto the surface below it.

**Undo:** `Ctrl+Z` and `Ctrl+Y`. **Window > Undo History** shows every step, and clicking one jumps there. Selecting, hiding and bookmarks
aren't undo steps.

## Arrange the scene

In the Scene Hierarchy:

- Drag a row onto another to make it a child. **Move to Root** in the right-click menu undoes that.
- Double-click a row to frame it in the viewport.
- Use the search field and the **Mesh**, **Light**, **Camera** and **Physics** buttons to filter the list.
- Use the eye icon to hide a row, and the lock icon to protect it from selection in the viewport.
- Right-click empty space for **New Folder**. Folders only organize the list here, and don't affect the scene or the game.

## Edit values in the Inspector

Drag a number to change it live, holding Shift for fine steps or Ctrl for coarse ones. Click a number to type one. With several objects
selected, an edit reaches all of them, and values that differ show as "Multiple Values".

- **Add Component** and **Remove** add and take away parts such as lights, bodies and colliders.
- **Search properties** and **Modified** narrow down what's listed.
- The arrow beside a changed value resets it.
- Right-click a value to copy, paste, reset or add it to Favorites.
- **Lock** keeps showing this object when the selection changes.

## Use assets

**To import**, drop a file on the window, or type its path at the top of the Asset Browser and press Enter. Imports run in the
background, and a long one shows a progress bar with Cancel. Files are copied into the project's `Assets` folder.

Supported files are `.glb`, `.obj`, `.png`, `.jpg`, `.tga`, `.ppm` and `.hdr`, up to 64 MB. Re-export `.gltf` models as self-contained
`.glb` files. Models import without animation: a skinned model comes in its bind pose.

If you add files to `Assets` yourself, press **Rescan**.

| To | Do this |
|---|---|
| Put a model in the scene | Drag it to the viewport, or double-click it |
| Use an asset on the selected object | Select it, then press **Assign to selection** |
| Fill an Inspector field | Drag the asset onto the field, or pick from the field's list |
| Find the asset an object uses | Press `Ctrl+B` |
| Find anything by name | Press `Ctrl+P` |
| Rename or move an asset | Press `F2`, or right-click it. Slashes move it to another folder, and references follow |
| See the browser over everything | Press `Ctrl+Space`, and `Esc` to close it |

**Materials:** type a name beside **New material** and press it. Double-click a material to open it in the Material Editor, or press
`Ctrl+E` to open the selected object's material. Material changes save themselves when you release a control, and they can be undone.

## Add particle effects

A particle effect is a `.effect` file in the asset folder: how particles are born, move, change and look. An entity
plays one through a **Particle Emitter** component. Particles are only for show: they never push anything, score or
change the scene, and a run plays the same with them or without them.

1. Type a name in the Asset Browser's name field and press **New effect**. The effect is selected and its Effect Editor
   opens with a double-click.
2. Press **Place in scene** in the Effect Editor, or drag the effect onto the viewport. An emitter appears and starts
   playing at once. **Create... > Particle Emitter** does the same with the effect selected in the Asset Browser.
3. Change values in the Effect Editor. Dragging shows the change on every emitter using the effect, and releasing saves
   it to the file as one undo step.

| Part of the Effect Editor | What it sets |
|---|---|
| Emission | How long one cycle lasts, particles per second, a burst at the start of each cycle, and how many may live at once |
| Motion | Lifetime and speed (min and max), the spread cone around the emitter's up axis, a start sphere, acceleration (gravity by default), drag, spin, and stretch, which draws streaks along the velocity for sparks |
| Floor | Particles can bounce on a flat floor at a height, keeping some of their speed |
| Look | **Additive** adds light (sparks, flashes); **Alpha** covers what is behind (smoke, dust). Strength above 1 blooms. An image replaces the soft round dot |
| Size and color over life | Up to eight keys each: a time from 0 (birth) to 1 (death), then a width or a color with alpha |

**Restart** starts every emitter again. **View > Play Effects** turns the preview off while editing; Play and Simulate
always run their own particles from the beginning, and Stop throws them away. **View > Restart Effects** is also in the
viewport's view menu.

In the Inspector, a Particle Emitter can be told not to play by itself, in which case gameplay fires it, not to loop, and
to scale its rate. The shooter's **Impact Effect Template** works this way: give it an emitter and every hit fires that
effect along the surface it struck.

## Light the scene

In the **Lighting** panel, start with **Outdoor** or **Indoor**, then adjust the sky or HDR image, exposure, tone mapping and shadows.
**Add sun** and **Add key light** create lights. Other lights are under **Create...**, and each light's settings are in the Inspector.

A scene with no lights uses two default editor lights. Once you add a light, only your own lights apply.

## Play

| Key | Action |
|---|---|
| `Alt+P` | Play, or carry on a paused run |
| `Alt+S` | Simulate: the world runs and the camera stays yours |
| `F8` | Leave the player to look around, or go back to the player |
| `Shift+F1` | Free the mouse without pausing |
| `Esc` | Free the mouse and pause. Press again to stop |

Clicking the view takes the mouse back. Stopping discards everything that changed during the run, including your own edits, and restores
the scene. The viewport's edge shows the mode: green when playing, blue when simulating, yellow when you've left the player. Saving and
undo are off until you stop.

### The Physics Shooter sample

Open **Project... > Open Physics Shooter Sample**, press **Play**, then click the view. Destroy all 9 targets in 90 seconds.

| Input | Action |
|---|---|
| `W` `A` `S` `D` | Move |
| Mouse | Look |
| LMB | Fire |
| `Space` | Jump |
| `Shift` | Sprint |
| `R` | Reload, or restart once the game has ended |

The sample opens from its folder in the source tree, so saving changes those tracked files. To experiment freely, copy
`Samples\PhysicsShooter` somewhere else and open the copy:

```powershell
engine_editor.exe --project <copy> --scene <copy>\physics_shooter.scene
```

## Check messages

Corner messages appear for saves, loads, imports, warnings and errors. Hover over one to keep it open, and press `x` to close it. They
all stay in the **Message Log**, which also records smaller actions. Use its buttons to filter by severity, and click a link in an entry
to select the object or show the asset it names.

## Use the Console

Press the backtick key to open the Console. Suggestions appear as you type, `Tab` completes, and Up and Down recall earlier lines.

| Type | Effect |
|---|---|
| `help` | List everything, or add a word to search |
| `stat fps`, `stat timings`, `stat none` | Show or hide performance numbers |
| `show collision` | Toggle collider outlines |
| `camera.speed 5` | Set a value. A name alone shows the current value |
| `r.shadows off` | Turn a view setting off. `r.bloom`, `r.ambient_occlusion` and `r.skybox` work the same way |
| `Viewport.FocusSelected` | Run any command by its ID, as listed in `EDITOR_SHORTCUTS.md` |

## Change your shortcuts

Open **Window > Keyboard Shortcuts**. Search for a command, click its key and press the new one. The panel warns you about a conflict, and
right-clicking removes a key. Mouse gestures can't be changed yet.

## Where your work is kept

| What | Where |
|---|---|
| A scene | The `.scene` file named in the toolbar's **Scene file** field |
| Assets, materials and effects | `Assets\` in the project folder |
| Hidden objects and bookmarks | `<scene file>.editor`, beside the scene |
| Keys, layout and editor settings | `keymap.ini`, `layout.ini` and `editor.ini` in `%APPDATA%\GameEngine\` |

**Opening things:** `Ctrl+O` picks a scene file. **Project... > Open Project...** only loads a scene by itself if the folder has a
`main.scene`, so use `Ctrl+O` for any other name. In the Asset Browser, **Scenes** lists the scene files beside the current one: click one,
then press **Load**.

## If something goes wrong

| Problem | Try this |
|---|---|
| A shortcut does nothing | It may belong to another panel, so move the mouse there. Keys are also off while a dialog is open, a drag is in progress, or a text field has the keyboard. Hold `F1` to see what works |
| The mouse is stuck in the game | Press `Esc`, or `Shift+F1` to free it without pausing |
| "Stop Play mode first" | Press `Esc` until the run stops, or press **Stop** |
| "Nothing to snap to below the selection" | `End` had nothing to do: the object is already resting on a surface, or it's at ground level or lower with nothing under it |
| Things fall through an object | It has no collider. Add one with **Add Component** in the Inspector |
| An import fails | Check the file's type and size above. The error stays in the Message Log |
| The asset list is empty | The wrong folder may be open as the project. Assets are read from its `Assets` folder |
| A panel is missing or the layout is a mess | Use the **Window** menu, or **Window > Reset Layout** |
| The sample's scene doesn't load | Use **Open Physics Shooter Sample**, since **Open Project...** only looks for `main.scene` |
| A corner message covers part of the Inspector | It goes by itself, or press `x` |
