#include <engine/editor/EditorSession.hpp>
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace engine;
using namespace engine::editor;
namespace {
void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
template<class T> void ok(const Result<T> &result) { if (!result) throw std::runtime_error(result.error().message); }
bool near(double a, double b) { return std::abs(a-b) < 1e-5; }
void editing() {
    EditorSession s;
    ok(s.create("Parent")); const auto parent = s.selection();
    ok(s.create("Child", parent)); const auto child = s.selection();
    check(s.scene().on_owner_thread(), "real ECS owner thread");
    ok(s.execute(commands::RenameEntity{child,"Renamed"}));
    Transform t; t.position = {2,3,4}; t.rotation = {0,0.382683432365,0,0.923879532511}; t.scale = {2,3,4};
    ok(s.execute(commands::SetTransform{child,t}));
    check(s.scene().inspect(child)->name == "Renamed", "rename");
    check(near(s.scene().world_transform(child)->matrix[13],3.5), "parent world composition");
    auto f = s.frame(1.5); ok(f);
    check(f.value()->objects.size() == 2, "actual scene entities rendered");
    check(near(f.value()->objects.back().world.matrix[12],2), "snapshot follows edit");
    ok(s.undo()); check(s.scene().inspect(child)->transform.position != t.position, "undo transform");
    ok(s.redo()); check(s.scene().inspect(child)->transform.position == t.position, "redo transform");
    ok(s.execute(commands::DeleteEntity{parent}));
    check(s.selection() == 0 && s.scene().size() == 0, "subtree deletion clears stale selection");
    auto empty = s.frame(1); ok(empty); check(empty.value()->objects.empty(), "stale bindings removed");
    ok(s.undo()); check(s.scene().inspect(child).has_value(), "stable identity restored");
    s.select(child); check(s.selection() == child, "select restored entity");
    s.select(999999); check(s.selection() == 0, "unknown selection rejected");
    check(f.value()->objects.size() == 2, "old frame remains immutable");
}
void persistence() {
    EditorSession s; ok(s.create("Saved")); const auto id = s.selection();
    const auto directory = std::filesystem::current_path() / "editor-test-files";
    std::filesystem::create_directories(directory);
    const auto path = directory / "roundtrip.scene";
    const auto before = s.scene().snapshot();
    ok(s.save(path)); ok(s.save(path));
    ok(s.execute(commands::DeleteEntity{id})); ok(s.load(path));
    check(s.scene().snapshot().entities == before.entities, "save/load records");
    check(!s.selection(), "load clears selection even on ID collision");
    ok(s.undo()); check(s.scene().size() == 0, "scene load undoable");
    ok(s.redo()); check(s.scene().size() == 1, "scene load redoable");
    const auto revision = s.commands().revision();
    { std::ofstream corrupt(directory / "bad.scene"); corrupt << "not a scene"; }
    check(!s.load(directory / "bad.scene"), "malformed load fails");
    check(s.commands().revision() == revision && s.scene().size() == 1, "failed load atomic");
    check(!s.save(directory / "missing" / "file.scene"), "bad save reports failure");
    std::filesystem::remove(path); std::filesystem::remove(directory / "bad.scene");
    std::filesystem::remove(directory);
}
void camera_and_picking() {
    EditorSession s; ok(s.create()); const auto near_id = s.selection();
    s.camera.yaw = 0; s.camera.pitch = 0; s.camera.target = {0,0.5,0}; s.camera.distance = 8;
    auto f = s.frame(1); ok(f);
    check(s.pick(*f.value(),0,0) == near_id, "center ray selects real ID");
    check(s.pick(*f.value(),0.99,0.99) == 0, "empty-space miss");
    ok(s.create("Far")); const auto far_id = s.selection();
    Transform t; t.position = {0,0.5,-3}; ok(s.execute(commands::SetTransform{far_id,t}));
    f = s.frame(1); ok(f); check(s.pick(*f.value(),0,0) == near_id, "nearest hit wins");
    ok(s.execute(commands::DeleteEntity{near_id}));
    f = s.frame(1); ok(f); check(s.pick(*f.value(),0,0) == far_id, "deleted object is not picked");
    t.position.z = -2 * f.value()->camera.lens.far_plane;
    ok(s.execute(commands::SetTransform{far_id,t}));
    f = s.frame(1); ok(f);
    check(s.pick(*f.value(),0,0) == 0, "object beyond far clip is not picked");
    t.position.z = -3; ok(s.execute(commands::SetTransform{far_id,t}));
    const auto revision = s.commands().revision();
    s.camera.orbit(40,5000); s.camera.pan(20,-30,600); s.camera.zoom(10000);
    check(s.camera.pitch <= 1.5 && s.camera.distance >= 0.2, "orbit/zoom bounds");
    f = s.frame(1); ok(f); check(s.commands().revision() == revision, "navigation not authoring");
    s.select(far_id); s.focus(); check(near(s.camera.target.z,-3), "focus world position");
    check(!s.frame(0), "invalid aspect rejected");
}
void world_edits() {
    EditorSession s; ok(s.create("Parent")); const auto parent = s.selection();
    Transform p; p.position = {3,0,0}; p.scale = {2,2,2}; ok(s.execute(commands::SetTransform{parent,p}));
    ok(s.create("Child",parent)); const auto child = s.selection();
    auto world = *s.scene().world_transform(child); world.matrix[12] = 7;
    ok(s.set_world(child,world)); check(near(s.scene().inspect(child)->transform.position.x,2), "world edit converted through parent inverse");
    world.matrix[4] += 0.5;
    const auto revision = s.commands().revision();
    check(!s.set_world(child,world), "shear rejected"); check(s.commands().revision() == revision, "rejected world edit atomic");
    ok(s.undo()); check(near(s.scene().inspect(child)->transform.position.x,0), "world edit undo");
}
std::filesystem::path test_directory() {
    const auto directory = std::filesystem::current_path() / "editor-test-files";
    std::filesystem::create_directories(directory);
    return directory;
}
void documents() {
    EditorSession s; ok(s.create("Starter")); check(s.dirty(), "new edits are unsaved");
    s.mark_clean(); check(!s.dirty(), "mark_clean");
    const auto starter = s.selection();
    ok(s.new_scene());
    check(s.scene().size() == 0 && !s.selection() && !s.dirty(), "new scene is empty and clean");
    ok(s.create("Fresh")); check(s.selection() > starter, "stable IDs are not recycled by New Scene");
    check(s.dirty(), "edit after New Scene is unsaved");
    ok(s.undo()); check(!s.dirty(), "undo back to the clean state is clean");
    ok(s.undo()); check(s.scene().inspect(starter).has_value() && s.dirty(), "New Scene is undoable");
    const auto path = test_directory() / "documents.scene";
    ok(s.save(path)); check(!s.dirty(), "save cleans");
    Transform t; t.position.x = 4; ok(s.execute(commands::SetTransform{starter, t})); check(s.dirty(), "edit after save");
    ok(s.load(path)); check(!s.dirty(), "load cleans");
    ok(s.undo()); check(s.dirty(), "undoing a load restores unsaved state");
    std::filesystem::remove(path);
}
void play_mode() {
    EditorSession s; ok(s.create("Body")); const auto id = s.selection();
    const auto before = s.editor_scene().snapshot();
    const auto revision = s.commands().revision(); const auto history = s.commands().undo_size();
    check(s.play_state() == PlayState::Editing && !s.stop() && !s.pause() && !s.step(), "play controls need Play mode");
    auto editing_frame = s.frame(1); ok(editing_frame);
    ok(s.play()); check(s.play_state() == PlayState::Playing && s.selection() == id, "play keeps selection");
    check(&s.scene() != &s.editor_scene() && s.scene().snapshot() == before, "runtime copy of the editor scene");
    auto playing_frame = s.frame(1); ok(playing_frame);
    check(playing_frame.value()->stamp.scene != editing_frame.value()->stamp.scene, "runtime scene has its own identity");
    Transform moved; moved.position = {5, 6, 7};
    ok(s.execute(commands::SetTransform{id, moved}));
    ok(s.create("RuntimeOnly")); const auto runtime_only = s.selection();
    WorldTransform world = *s.scene().world_transform(id); world.matrix[12] = -1;
    ok(s.set_world(id, world));
    check(s.scene().size() == 2 && near(s.scene().inspect(id)->transform.position.x, -1), "runtime edits apply to the copy");
    check(s.editor_scene().snapshot() == before && s.commands().revision() == revision && s.commands().undo_size() == history,
          "editor scene, revision and history untouched while playing");
    check(!s.undo() && !s.redo() && !s.save(test_directory() / "x.scene") && !s.load(test_directory() / "x.scene") &&
          !s.new_scene() && !s.execute(commands::ReplaceScene{}), "document and history commands blocked while playing");
    s.update(EditorSession::fixed_step * 3 + EditorSession::fixed_step / 2);
    check(s.runtime_ticks() == 3, "fixed ticks");
    s.update(EditorSession::fixed_step - EditorSession::fixed_step / 2); check(s.runtime_ticks() == 4, "remainder carries over");
    s.update(EditorSession::fixed_step * 100); check(s.runtime_ticks() == 4 + EditorSession::max_ticks_per_update, "tick burst capped");
    ok(s.pause()); s.update(EditorSession::fixed_step * 3);
    const auto paused = s.runtime_ticks(); check(s.play_state() == PlayState::Paused, "paused");
    check(paused == 4 + EditorSession::max_ticks_per_update, "paused time does not advance");
    ok(s.step()); check(s.runtime_ticks() == paused + 1, "step advances one tick");
    ok(s.play()); check(s.play_state() == PlayState::Playing && s.runtime_ticks() == paused + 1, "resume keeps the runtime");
    check(!s.step(), "step only while paused");
    s.select(runtime_only);
    ok(s.stop());
    check(s.play_state() == PlayState::Editing && &s.scene() == &s.editor_scene(), "stop returns to the editor scene");
    check(s.scene().snapshot() == before && s.commands().revision() == revision, "editor scene unchanged after stop");
    check(!s.selection(), "selection of a runtime-only entity cleared");
    check(s.runtime_ticks() == 0, "runtime discarded");
    ok(s.play()); check(s.scene().snapshot() == before, "a second play starts from the editor scene again");
    auto second = s.frame(1); ok(second);
    check(second.value()->stamp.scene != playing_frame.value()->stamp.scene, "each play session is a new scene identity");
    ok(s.stop());
    ok(s.undo()); check(s.scene().size() == 0, "editor history still works after Play");
}
// A dynamic crate dropped on a static floor in Play mode; Stop restores the editor scene exactly.
void physics_play() {
    EditorSession s;
    Components floor; floor.mesh = MeshComponent{BuiltinMesh::Plane};
    ColliderComponent slab; slab.half_extents = {0.5, 0.05, 0.5}; slab.center = {0, -0.05, 0};
    floor.colliders = {slab};
    ok(s.create("Floor", 0, floor)); const auto floor_id = s.selection();
    Transform wide; wide.scale = {10, 1, 10}; ok(s.execute(commands::SetTransform{floor_id, wide}));
    Components crate; crate.mesh = MeshComponent{}; crate.body = RigidBodyComponent{}; crate.colliders = {ColliderComponent{}};
    ok(s.create("Crate", 0, crate)); const auto id = s.selection();
    Transform high; high.position = {0, 3, 0}; ok(s.execute(commands::SetTransform{id, high}));
    const auto y = [&] { return s.scene().world_transform(id)->matrix[13]; };
    // The edit-mode bridge mirrors the scene but never steps it.
    ok(s.frame(1));
    auto d = s.physics_bridge().diagnostics();
    check(d.active_bodies == 2 && !d.simulation_enabled && d.issues.empty(), "edit-mode bridge mirrors the scene");
    check(s.physics_bridge().debug_geometry().lines.size() == 24, "collider outlines available while editing");
    s.update(EditorSession::fixed_step * 3); ok(s.frame(1));
    check(y() == 3 && s.physics_bridge().diagnostics().simulated_ticks == 0, "nothing simulates outside Play");
    const auto before = s.editor_scene().snapshot(); const auto revision = s.commands().revision();
    const auto history = s.commands().undo_size(); const auto was_dirty = s.dirty();
    // Drop and land.
    ok(s.play());
    for (int i = 0; i < 240; ++i) s.update(EditorSession::fixed_step);
    check(s.runtime_ticks() == 240 && s.physics_bridge().diagnostics().simulated_ticks == 240, "one physics step per tick");
    check(std::abs(y() - 0.5) < 0.02, "crate rests on the floor");
    check(s.physics_error().empty(), "no physics failure");
    check(s.editor_scene().snapshot() == before && s.commands().revision() == revision, "editor scene untouched while simulating");
    ok(s.stop());
    check(s.scene().snapshot() == before && s.commands().revision() == revision && s.commands().undo_size() == history && s.dirty() == was_dirty,
          "Stop restores the editor scene exactly");
    check(!s.physics_bridge().diagnostics().simulation_enabled, "edit-mode bridge back in charge");
    // A second Play restarts from the authored pose; Pause freezes it; Step advances one physics tick.
    ok(s.play()); check(y() == 3, "Play restarts from the editor scene");
    for (int i = 0; i < 30; ++i) s.update(EditorSession::fixed_step);
    const auto falling = y(); check(falling < 2.5, "crate falls");
    ok(s.pause());
    s.update(EditorSession::fixed_step * 10); check(y() == falling, "paused physics does not move");
    ok(s.step()); check(y() < falling, "Step advances physics");
    check(s.physics_bridge().body_state(id)->linear_velocity.y < -1, "bridge reports the body's velocity");
    ok(s.play());
    // An edit during Play teleports the body and physics carries on from there.
    auto lifted = s.scene().inspect(id)->transform; lifted.position.y = 10;
    ok(s.execute(commands::SetTransform{id, lifted}));
    s.update(EditorSession::fixed_step); check(y() > 9.5 && y() < 10, "runtime edit teleports the body");
    ok(s.stop()); check(s.scene().snapshot() == before, "runtime edits discarded");
    // Authored initial velocity applies when Play starts, and body edits are undoable.
    RigidBodyComponent thrown; thrown.linear_velocity = {2, 0, 0};
    ok(s.execute(commands::SetRigidBody{id, thrown}));
    ok(s.play()); for (int i = 0; i < 30; ++i) s.update(EditorSession::fixed_step);
    check(s.scene().world_transform(id)->matrix[12] > 0.9, "initial velocity applied");
    ok(s.stop()); ok(s.undo());
    check(s.scene().inspect(id)->components.body == RigidBodyComponent{}, "body edit undone");
    // Unsupported setups are reported by both bridges rather than simulated.
    ok(s.create("Nested", id, crate)); const auto nested = s.selection();
    ok(s.frame(1));
    d = s.physics_bridge().diagnostics();
    check(d.issues.size() == 1 && d.issues[0].entity == nested, "edit-mode bridge reports the issue");
    ok(s.play()); s.update(EditorSession::fixed_step);
    d = s.physics_bridge().diagnostics();
    check(d.issues.size() == 1 && d.issues[0].entity == nested && !s.physics_bridge().body_state(nested), "runtime bridge reports it too");
    ok(s.stop()); ok(s.undo());
    // A world-preserving reparent under a dynamic body is reported too, and clears when undone.
    ok(s.create("Loose", 0, crate)); const auto loose = s.selection();
    ok(s.frame(1)); check(s.physics_bridge().diagnostics().issues.empty(), "loose body is fine");
    ok(s.reparent(loose, id)); ok(s.frame(1));
    d = s.physics_bridge().diagnostics();
    check(d.issues.size() == 1 && d.issues[0].entity == loose, "reparenting under a dynamic body is reported");
    ok(s.undo()); ok(s.frame(1));
    check(s.physics_bridge().diagnostics().issues.empty() && s.physics_bridge().body_state(loose), "undo clears the issue");
    ok(s.undo());
    // Physics components travel with duplicates and scene files.
    const auto path = test_directory() / "physics.scene";
    const auto saved = s.editor_scene().snapshot();
    ok(s.save(path)); ok(s.new_scene()); ok(s.frame(1));
    check(s.physics_bridge().diagnostics().authored_entities == 0, "New Scene drops the bodies");
    ok(s.load(path)); ok(s.frame(1));
    check(s.editor_scene().snapshot().entities == saved.entities && s.physics_bridge().diagnostics().active_bodies == 2, "physics saved and loaded");
    std::filesystem::remove(path);
    ok(s.duplicate(id)); ok(s.frame(1));
    check(s.scene().inspect(s.selection())->components == crate && s.physics_bridge().diagnostics().active_bodies == 3, "duplicates keep physics");
}
// The Milestone 3 acceptance workflow, without a window: new scene, add a cube, move/rotate/scale it with
// world-space (gizmo) edits, edit its local transform (Inspector), save, reopen in a new session, load,
// compare, play, stop, compare.
void acceptance_workflow() {
    const auto path = test_directory() / "acceptance.scene";
    Transform expected;
    {
        EditorSession s; ok(s.create("Starter")); s.mark_clean();
        ok(s.new_scene());
        ok(s.create("Cube")); const auto id = s.selection();
        auto world = *s.scene().world_transform(id);
        world.matrix[12] = 2; ok(s.set_world(id, world));                      // move
        const double c = std::cos(0.5), sn = std::sin(0.5);                     // rotate 0.5 rad about Y
        world = *s.scene().world_transform(id);
        world.matrix[0] = c; world.matrix[2] = -sn; world.matrix[8] = sn; world.matrix[10] = c;
        ok(s.set_world(id, world));
        world = *s.scene().world_transform(id);                                 // scale Y by 2
        for (int r = 0; r < 3; ++r) world.matrix[4 + r] *= 2;
        ok(s.set_world(id, world));
        auto t = s.scene().inspect(id)->transform;
        // q and -q are the same rotation, so compare magnitudes.
        check(near(t.position.x, 2) && near(t.scale.y, 2) && near(std::abs(t.rotation.y), std::sin(0.25)) &&
              near(std::abs(t.rotation.w), std::cos(0.25)), "gizmo-style edits");
        t.position.z = -1.25; ok(s.execute(commands::SetTransform{id, t}));     // Inspector edit
        expected = s.scene().inspect(id)->transform;
        check(s.dirty(), "edited scene is unsaved");
        ok(s.save(path)); check(!s.dirty(), "saved");
    }
    EditorSession s;
    ok(s.load(path));
    check(s.scene().size() == 1, "one entity after reopen");
    const auto record = s.scene().snapshot().entities.front();
    check(record.name == "Cube" && record.transform == expected, "cube keeps its exact transform after reopen");
    const auto before = s.editor_scene().snapshot();
    ok(s.play());
    Transform runtime; runtime.position = {9, 9, 9}; ok(s.execute(commands::SetTransform{record.id, runtime}));
    s.update(EditorSession::fixed_step * 2);
    ok(s.stop());
    check(s.editor_scene().snapshot() == before && s.scene().inspect(record.id)->transform == expected && !s.dirty(),
          "editor scene unchanged by Play/Stop");
    std::filesystem::remove(path);
    std::filesystem::remove(test_directory());
}
// NDC of a world point in a frame, for aiming pick rays at known spots.
std::pair<double, double> ndc(const renderer::FrameSnapshot &f, glm::dvec3 p) {
    const auto clip = glm::make_mat4(f.camera.projection.matrix.data()) * glm::make_mat4(f.camera.view.matrix.data()) * glm::dvec4(p, 1);
    return {clip.x / clip.w, clip.y / clip.w};
}
void components_and_rendering() {
    EditorSession s;
    ok(s.create("Ball", 0, {MeshComponent{BuiltinMesh::Sphere, {0.5, 1, 0}, 0.3, 0.7}, {}, {}})); const auto ball = s.selection();
    ok(s.create("Box")); const auto box = s.selection();
    check(s.scene().inspect(box)->components.mesh == MeshComponent{}, "default create is a cube");
    ok(s.create("Lamp", 0, {{}, {}, LightComponent{LightType::Point, {1, 0.5, 0}, 2000, 6}})); const auto lamp = s.selection();
    ok(s.create("Empty", 0, {}));
    auto f = s.frame(1); ok(f);
    const auto &objects = f.value()->objects;
    check(objects.size() == 2 && objects[0].entity == ball && objects[1].entity == box, "only mesh components are drawn");
    check(objects[0].mesh == renderer::builtin_sphere && objects[1].mesh == renderer::builtin_cube && objects[0].material == renderer::builtin_lit,
          "built-in mesh handles");
    check(near(objects[0].surface.linear_color.x, 0.21404114) && objects[0].surface.linear_color.y == 1 && objects[0].surface.linear_color.z == 0 &&
          objects[0].surface.roughness == 0.3 && objects[0].surface.metallic == 0.7, "sRGB color converted to linear, surface carried");
    check(f.value()->lights.size() == 1 && f.value()->lights[0].entity == lamp && f.value()->lights[0].kind == renderer::LightKind::Point &&
          near(f.value()->lights[0].linear_color.y, 0.21404114) && f.value()->lights[0].intensity == 2000 && f.value()->lights[0].range == 6,
          "light component becomes a scene light");
    // Picking follows the real shape: a ray through a cube-only corner misses the sphere.
    ok(s.execute(commands::DeleteEntity{box}));
    s.camera.yaw = 0; s.camera.pitch = 0; s.camera.target = {0, 0.5, 0}; s.camera.distance = 8;
    f = s.frame(1); ok(f);
    auto [cx, cy] = ndc(*f.value(), {0, 0.5, 0});
    check(s.pick(*f.value(), cx, cy) == ball, "sphere center hit");
    auto [kx, ky] = ndc(*f.value(), {0.45, 0.95, 0.45});
    check(s.pick(*f.value(), kx, ky) == 0, "cube corner is outside the sphere");
    ok(s.execute(commands::SetMesh{ball, MeshComponent{}}));
    f = s.frame(1); ok(f);
    check(s.pick(*f.value(), kx, ky) == ball, "the same ray hits it as a cube");
    ok(s.undo()); check(s.scene().inspect(ball)->components.mesh->mesh == BuiltinMesh::Sphere, "mesh change undoable");
    ok(s.create("Floor", 0, {MeshComponent{BuiltinMesh::Plane}, {}, {}})); const auto floor = s.selection();
    Transform wide; wide.position = {0, 0, 3}; wide.scale = {10, 1, 10}; ok(s.execute(commands::SetTransform{floor, wide}));
    s.camera.pitch = 0.6; s.camera.target = {0, 0, 3};
    f = s.frame(1); ok(f);
    check(s.pick(*f.value(), 0, 0) == floor, "plane hit from above");
    s.camera.pitch = 0;
    f = s.frame(1); ok(f);
    check(s.pick(*f.value(), 0, 0.5) == 0, "ray above the horizon misses the plane");
    // Play mode looks through the first primary camera; edit mode never does.
    Transform eye; eye.position = {0, 1.5, 6};
    ok(s.create("Side", 0, {{}, CameraComponent{40, 0.1, 100, false}, {}}));
    ok(s.create("Main", 0, {{}, CameraComponent{50, 0.2, 300, true}, {}})); const auto main_camera = s.selection();
    ok(s.execute(commands::SetTransform{main_camera, eye}));
    f = s.frame(1.5); ok(f); check(s.view_camera() == 0 && f.value()->camera.lens.entity == 0, "edit mode uses the editor camera");
    ok(s.play());
    f = s.frame(1.5); ok(f);
    check(s.view_camera() == main_camera && f.value()->camera.lens.entity == main_camera && near(f.value()->camera.world.matrix[14], 6) &&
          near(f.value()->camera.lens.vertical_fov_radians, glm::radians(50.0)) && f.value()->camera.lens.far_plane == 300,
          "play mode views through the primary camera");
    eye.scale = {2, 2, 2}; ok(s.execute(commands::SetTransform{main_camera, eye}));
    f = s.frame(1.5); ok(f);
    check(s.view_camera() == 0 && !s.view_camera_issue().empty() && f.value()->camera.lens.entity == 0, "scaled camera falls back with a reason");
    // Component edits in Play mode stay in the runtime copy.
    ok(s.execute(commands::SetLight{lamp, std::nullopt}));
    check(!s.scene().inspect(lamp)->components.light, "runtime light removed");
    ok(s.stop());
    check(s.scene().inspect(lamp)->components.light && s.scene().inspect(main_camera)->transform.scale.x == 1, "editor components untouched by Play");
    // Components round-trip through a scene file exactly.
    const auto path = test_directory() / "components.scene";
    const auto before = s.editor_scene().snapshot();
    ok(s.save(path)); ok(s.new_scene()); ok(s.load(path));
    check(s.editor_scene().snapshot().entities == before.entities, "components saved and loaded");
    std::filesystem::remove(path);
}
void external_camera_validation() {
    Scene scene;
    renderer::PerspectiveCamera lens;
    OrbitCamera camera;
    auto world = camera.world();
    auto f = renderer::extract_frame(scene,{1,0,1},lens,world,{},{}); ok(f);
    world.matrix[0] *= 2; world.matrix[1] *= 2; world.matrix[2] *= 2;
    check(!renderer::extract_frame(scene,{1,0,1},lens,world,{},{}), "external camera scale rejected");
    for (const auto index : {3, 7, 11, 15}) {
        world = camera.world(); world.matrix[index] += 0.25;
        const auto invalid = renderer::extract_frame(scene,{1,0,1},lens,world,{},{});
        check(!invalid && invalid.error().code == ErrorCode::InvalidArgument,
              "external camera must have an affine homogeneous row");
    }
    bool rejected = false;
    std::thread worker([&] { auto r = renderer::extract_frame(scene,{1,0,1},lens,camera.world(),{},{}); rejected = !r && r.error().code == ErrorCode::WrongThread; }); worker.join();
    check(rejected,"external extraction enforces owner thread");
}
bool same_world(const WorldTransform &a, const WorldTransform &b) {
    for (int i = 0; i < 16; ++i) if (!near(a.matrix[i], b.matrix[i])) return false;
    return true;
}
void hierarchy_editing() {
    EditorSession s;
    ok(s.create("A")); const auto a = s.selection();
    Transform ta; ta.position = {2, 0, 0}; ta.rotation = {0, std::sin(0.25), 0, std::cos(0.25)}; ta.scale = {2, 2, 2};
    ok(s.execute(commands::SetTransform{a, ta}));
    ok(s.create("B")); const auto b = s.selection();
    Transform tb; tb.position = {5, 1, 0}; ok(s.execute(commands::SetTransform{b, tb}));
    const auto world_b = *s.scene().world_transform(b);
    const auto history = s.commands().undo_size();
    // Reparenting keeps the world transform and is one undo step.
    ok(s.reparent(b, a));
    check(s.scene().inspect(b)->parent == a && same_world(*s.scene().world_transform(b), world_b) && s.commands().undo_size() == history + 1,
          "reparent keeps world transform");
    check(!(s.scene().inspect(b)->transform == tb), "local transform recomputed under the parent");
    ok(s.reparent(b, 0));
    check(!s.scene().inspect(b)->parent && same_world(*s.scene().world_transform(b), world_b), "move to root keeps world transform");
    ok(s.undo()); check(s.scene().inspect(b)->parent == a, "undo move to root");
    ok(s.undo()); check(!s.scene().inspect(b)->parent && s.scene().inspect(b)->transform == tb, "undo reparent restores the original record");
    // Cycles and self-parenting are refused without touching the scene.
    ok(s.reparent(b, a));
    auto revision = s.commands().revision();
    auto cycle = s.reparent(a, b);
    check(!cycle && cycle.error().code == ErrorCode::Cycle && !s.scene().inspect(a)->parent, "parenting under a descendant refused");
    check(!s.reparent(a, a) && !s.reparent(a, 999) && !s.reparent(999, a), "self, missing parent and missing entity refused");
    check(s.commands().revision() == revision, "refused reparents are atomic");
    // A world pose that cannot be expressed under a non-uniformly scaled parent is refused.
    ok(s.create("Squash")); const auto squash = s.selection();
    Transform tall; tall.scale = {1, 3, 1}; ok(s.execute(commands::SetTransform{squash, tall}));
    ok(s.create("Tilted")); const auto tilted = s.selection();
    Transform tilt; tilt.rotation = {0, 0, std::sin(0.3926990817), std::cos(0.3926990817)}; ok(s.execute(commands::SetTransform{tilted, tilt}));
    revision = s.commands().revision();
    check(!s.reparent(tilted, squash) && s.commands().revision() == revision && !s.scene().inspect(tilted)->parent, "shear refused atomically");
    // Duplication copies the subtree as one undo step and selects the copy.
    ok(s.create("Leaf", b)); const auto leaf = s.selection();
    ok(s.execute(commands::SetMesh{leaf, MeshComponent{BuiltinMesh::Sphere, {0, 1, 0}, 0.5, 0}}));
    const auto size = s.scene().size(); const auto before_copy = s.commands().undo_size();
    ok(s.duplicate(a));
    const auto copy = s.selection();
    check(copy != a && s.scene().inspect(copy)->name == "A (copy)" && !s.scene().inspect(copy)->parent && s.scene().size() == size + 3 &&
          s.commands().undo_size() == before_copy + 1, "subtree duplicated and selected");
    const auto copies = s.scene().snapshot().entities;
    check(std::count_if(copies.begin(), copies.end(), [](const EntityRecord &e) { return e.components.mesh && e.components.mesh->mesh == BuiltinMesh::Sphere; }) == 2,
          "components copied with the subtree");
    ok(s.undo()); check(s.scene().size() == size && !s.scene().inspect(copy), "one undo removes the copies");
    std::string wide(247, 'x'); for (int i = 0; i < 4; ++i) wide += "\xC3\xA9"; // 255 bytes; the 249-byte cut lands in the two-byte tail
    ok(s.execute(commands::RenameEntity{leaf, wide}));
    ok(s.duplicate(leaf));
    const auto trimmed = s.scene().inspect(s.selection())->name;
    check(trimmed.size() <= 256 && trimmed.ends_with(" (copy)") && trimmed.substr(0, trimmed.size() - 7).find_first_not_of('x') == std::string::npos,
          "long names trimmed without splitting a UTF-8 sequence");
    // In Play mode duplicates live only in the runtime copy.
    const auto editor_before = s.editor_scene().snapshot();
    ok(s.play()); ok(s.duplicate(b));
    check(s.scene().size() > editor_before.entities.size(), "runtime duplicate");
    ok(s.stop()); check(s.editor_scene().snapshot() == editor_before, "editor scene untouched");
}
}
int main() {
    try { editing(); persistence(); camera_and_picking(); world_edits(); documents(); play_mode(); components_and_rendering(); hierarchy_editing(); physics_play();
        acceptance_workflow(); external_camera_validation();
        std::cout << "11 editor acceptance groups passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
