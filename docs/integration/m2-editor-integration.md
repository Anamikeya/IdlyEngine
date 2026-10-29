# Editor integration examples (physics and assets)

Audience: Codex, for the ImGui editor host and panels. Contracts are in [m2-contracts.md](m2-contracts.md).
The snippets use only the public APIs exercised by `Tests/Physics/scene_physics_tests.cpp` and
`Tests/Integration/systems_smoke_tests.cpp`. The smoke test is the compiled reference; when a snippet
and that test disagree, the test is right. No ImGui code is provided here on purpose: panels are Codex's.

Link: `engine::physics_scene` (brings `engine::physics` and `engine::core`) and `engine::assets`.

## 1. Host wiring: edit mode, play, stop

```cpp
#include <engine/physics/scene_physics.h>

struct EditorHost final : engine::ApplicationClient {
    std::optional<engine::physics::ScenePhysics> physics;
    std::unique_ptr<engine::commands::CommandBus> runtime; // history_limit 0: never in undo (C-1)
    std::optional<engine::SceneSnapshot> before_play;
    engine::commands::Context physics_ctx{"physics", /*Inspect|Edit*/ 3};

    engine::Result<void> on_start(engine::Application& app) override {
        engine::physics::WorldConfig config;
        config.fixed_timestep = std::chrono::milliseconds(10); // == LoopConfig::fixed_step (C-3)
        auto created = engine::physics::ScenePhysics::create(config);
        if (!created) return created.error();
        physics.emplace(std::move(created.value()));
        runtime = std::make_unique<engine::commands::CommandBus>(*app.scene(), 0);
        return {};
    }

    // Runs in edit mode too: bodies keep following editor edits; nothing is stepped or published.
    void on_fixed_update(engine::Application& app, const engine::FixedUpdate& tick) override {
        auto report = physics->fixed_update(tick.delta, *app.scene(), *runtime, physics_ctx);
        if (!report) engine::log_error(report.error().message);
    }

    void play(engine::Application& app) {
        before_play = app.scene()->snapshot();
        physics->set_simulation_enabled(true);
    }

    void stop(engine::Application& app) {
        physics->set_simulation_enabled(false);
        // Restore through the runtime bus so the editor's undo history stays valid (tested).
        runtime->execute(physics_ctx, runtime->revision(), engine::commands::ReplaceScene{*before_play});
        physics->reset_simulation(*app.scene());
    }

    void on_stop(engine::Application&) noexcept override {
        runtime.reset(); // Must die before Application destroys the scene.
        physics.reset();
    }
};
```

Pause = `set_simulation_enabled(false)` without restoring; velocities are kept.
"Step one frame" while paused: `set_simulation_enabled(true)`, one `fixed_update`, then `false`.

## 2. Inspector: rigid body and colliders

```cpp
using namespace engine::physics;

// "Add Rigid Body" button. The entity must exist in the scene.
if (auto r = physics.set_rigid_body(scene, entity, RigidBody{BodyType::Dynamic}); !r) show_error(r.error().message);

// Read for widgets. nullopt means "no rigid body".
if (std::optional<RigidBody> body = physics.rigid_body(entity)) {
    RigidBody edited = *body;           // Bind widgets to `edited` ...
    if (edited != *body) physics.set_rigid_body(scene, entity, edited); // ... commit on change.
}

// Colliders: index-addressed list in the entity's unscaled local frame.
std::span<const Collider> list = physics.colliders(entity);
Collider c;
c.shape = BoxShape{{0.5f, 0.5f, 0.5f}, {}};
auto index = physics.add_collider(entity, c);        // Result<size_t>
physics.set_collider(entity, index.value(), c);      // rejects invalid values with InvalidArgument
physics.remove_collider(entity, index.value());
physics.remove_rigid_body(entity);                    // body + colliders + authoring data
```

Edits apply on the next `fixed_update` (or an explicit `physics.sync(scene)`). They rebuild the body
and **are not undoable yet** (C-2a).

## 3. Diagnostics panel and live body read-out

```cpp
const PhysicsDiagnostics d = physics.diagnostics();
// d.simulation_enabled, d.ticks, d.simulated_ticks
// d.world.{body_count, collider_count, contact_count, awake_body_count}
// d.authored_entities, d.active_bodies, d.detached_entities
// d.last_tick.{stepped, scene_updates, rebuilt, published, transactions}
for (const EntityIssue& issue : d.issues) { /* entity id + human-readable reason */ }

if (std::optional<BodyState> s = physics.body_state(entity)) {
    // s->pose (world, unscaled), s->linear_velocity, s->angular_velocity, s->mass, s->awake
}

for (const CollisionEvent& e : physics.events()) { // last tick only; copy if you keep a log
    // e.kind: ContactBegin / ContactEnd / Hit / SensorBegin / SensorEnd; e.a, e.b are EntityIds
}
```

## 4. Collider visualization

```cpp
DebugGeometryOptions options;
options.circle_segments = 24;
options.include_static = show_static_toggle;
const DebugGeometry g = physics.debug_geometry(options);
for (const DebugLine& line : g.lines) {
    const std::array<float, 4> rgba = default_color(line.category); // or your own palette
    viewport_lines.push(line.a, line.b, rgba);                     // world space, metres
}
```

The lines come from the **physics** pose, so during play they show the simulated body even
before the next publish. Route to a Filament line renderable or an ImGui overlay (C-4a).

## 5. Asset browser and the renderer resource boundary

```cpp
#include <engine/assets/asset_database.h>
using namespace engine::assets;

auto db = AssetDatabase::open(project_root / "Assets").value(); // trusted host only
db.scan();
for (const AssetMetadata& a : db.assets()) { /* a.path, a.type, a.id, db.state(a.id) */ }
for (const AssetIssue& i : db.issues())    { /* show path + message */ }

// Renderer resource registry (Codex-owned), keyed by (AssetId, revision) (C-5a):
auto mesh = db.load_mesh(id);
if (!mesh) show_error(mesh.error().message);   // NotFound / ParseError / InvalidArgument
else if (registry.revision(id) != mesh.value().revision)
    registry.upload(id, mesh.value().revision, *mesh.value().data); // bump MeshHandle generation

// "Reimport" button and file-watch hook:
db.reload(id);                                  // next load re-imports with revision + 1
// Periodically, or when the editor gains focus:
for (AssetId missing : db.find_missing()) { /* mark in browser */ }
db.unload_unused();                             // frees CPU data nobody holds
```

`MeshData`: interleaved `MeshVertex {position, normal, uv}` with 32-bit triangle-list indices,
CCW front faces and a top-left UV origin. `TextureData`: RGBA8 rows top to bottom plus `srgb`.
Test content for the editor lives in `Tests/Assets/editor/` (a cube with a `.meta` id, a triangle
with generated normals, an 8x8 RGBA checker TGA, an RLE TGA and a linear PPM gradient).
