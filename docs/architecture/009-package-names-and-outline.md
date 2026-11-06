# Package names, redirectors and the selection outline (Milestone 5, slice 7)

Three Unreal-style editor features, asked for before Milestone 5 was pushed, and how they fit what the engine already had.
The decisions are in `../MILESTONE_5_PLAN.md` (slice 7); results are in `../MILESTONE_5_REPORT.md`.

```text
AssetDatabase (one root)  --scan-->  assets + redirectors  --index-->  VirtualFileSystem ("/Game" -> <project>/Assets)
   move(..., leave_redirector)             |                                 | resolve("/game/textures/brick")
   writes <old path>.redirect              |                                 v   follows redirectors, any thread
                                           v                          PackageResolution {asset path, file, id, hops}
EditorSession: rename_asset (leaves one), resolve() fallback, fix_up_redirectors (rewrites, then removes)

SelectionSet --apply_selection_tags--> SelectedTag / PrimarySelectedTag on the shown scene's Flecs entities
                                           | read by EditorSession::frame()
                                           v
                                 RenderObject::outline --> DesktopRenderer: mask view, then outline pass on the viewport
```

## Package names

**References do not change.** A scene or material names an asset by a stable ID and a relative path, and the ID wins
(`asset_types.h`). Unreal needs redirectors because its references are paths. Here they are a convenience for whatever
still uses a path: a scene saved before a move whose `.meta` was lost, a typed name, a console command, and scripts in
Milestone 6. So the formats stayed as they were; package names are a lookup and display layer on top.

**`engine::assets::VirtualFileSystem`** (`Engine/Assets/include/engine/assets/virtual_paths.h`) holds mount points
(`/Game`, a slash and up to 64 letters, digits, `_` or `-`) and an index for each. A package name is the mount and the
asset's path under the mount's root without its extension, as Unreal writes it: `/Game/Textures/Brick`. Keys are folded to
ASCII lower case, so names find the same file on every platform while the file keeps its own case; other bytes, UTF-8
included, pass through. Two assets whose bare names collide (`Brick.png` and `Brick.material`) are a conflict reported by
`index()`, not a guess. The name with its extension still finds each, and `name_of()` shows that form for them.

**Thread safety.** `index()` builds the new tables outside the lock and swaps them in under an exclusive lock; `resolve()`
and `name_of()` take a shared one. `asset_tests` resolves from four threads while the index is replaced 400 times or more.
The session rebuilds the index on its own thread whenever `AssetDatabase::scans()` has moved on.

## Redirectors

**A file, as in Unreal.** Moving an asset in the editor leaves `<old path>.redirect` (for example `Brick.png.redirect`), a
text file in the `.meta` style naming the new path and the asset's ID. It lives in the project, so it travels with it and
under version control. Scans catalogue redirectors (`AssetDatabase::redirectors()`) instead of reporting them as unsupported
files. A malformed one is an issue. Moving an asset onto a redirector's name deletes the redirector, because the asset
takes the name.

**Resolving follows them, recursively.** A name that lands on a redirector continues at its target, up to 32 in a row.
A loop is refused with `ErrorCode::Cycle` and named in full (`/Game/A -> /Game/B -> /Game/A`). A redirector hidden by an
asset at its own name is ignored, since the asset wins, and reported.

**Fix Up Redirectors** (`EditorSession::fix_up_redirectors`) follows each redirector from its target, so one whose old name
has since been taken still leads to the right asset. A reference is stale when its path is a redirector's old path, or when
its ID is the moved asset's and its path is not where the asset is now. A reference whose ID has nothing behind it takes
the asset's ID too. The fix-up rewrites:

- every `.scene` under the project folder apart from the open one, through a temporary file (an older version is written
  back as the current version, as any save does);
- the open scene, as one undo step, left unsaved;
- materials' maps and effects' images, saved at once, as a rename saves them.

Then it deletes the redirectors it resolved. One that leads nowhere or goes round in a loop is kept and listed. If a scene
file cannot be written, nothing is removed.

## The selection as Flecs tags

`SelectionSet` stays the source of truth, because it keeps the order and the primary entity, which gizmos and Details
follow. Before each frame the session calls `apply_selection_tags()`, which makes `SelectedTag` and `PrimarySelectedTag`
match it on the entities of the shown scene. That is the runtime copy during Play, so the editor scene is not touched. The
tags are not components the scene saves, copies or undoes: snapshots, files and history stay the same. ECS systems and
queries can use them, and the frame does: a mesh is outlined when it, or the nearest tagged entity above it, carries a tag.
So selecting a group outlines its members.

## The selection outline

The old highlight was the twelve edges of each selected model's bounding box, drawn by ImGui over the viewport, hidden edges
included, so a selected model looked like a wireframe cage. The new one is drawn by the renderer, around the silhouette only:

1. **Mask.** Every outlined object gets a second renderable in a mask scene that shares its vertex and index buffers and
   its bones. The mask view uses the main camera, with post-processing and shadows off and a black skybox to clear it. It
   draws them flat into an RGBA8 target the viewport's size: 1 for the primary entity, 0.5 for the others, both faces.
2. **Outline.** A full-screen triangle (`vertexDomain: device`) in a translucent view, drawn after the main view into the
   viewport's own target, runs `selection_outline.mat`. A pixel inside the mask is left alone. A pixel outside it within
   `width` pixels (2 at 1x scale, times the UI scale) takes the outline color, fading over its last pixel. Filament clears
   a frame's color only before its first view, so the scene under the outline is kept.

Nothing inside the silhouette is drawn, so no edge of the model shows through it. Because the mask has no depth test
against the scene, the whole silhouette is outlined even where another object hides it, as Unreal does. Mask copies are
synchronized before meshes replaced in that sync are freed, since they draw from the same buffers. `readback()` draws the
same views, so render tests see the outline: `--render-test` checks a ring around a selected cube with nothing changed inside
it or away from it, and the same around a cube hidden by a wall.
