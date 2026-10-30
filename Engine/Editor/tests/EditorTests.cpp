#include <engine/assets/importers.h>
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
// A one-triangle binary glTF: 36 bytes of positions and 3 u16 indices, translated up by one meter.
std::string triangle_glb() {
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0,"translation":[0,1,0]}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],)"
        R"("materials":[{"pbrMetallicRoughness":{"baseColorFactor":[0,1,0,1],"roughnessFactor":0.25,"metallicFactor":0}}],)"
        R"("animations":[{"channels":[],"samplers":[]}],)"
        R"("buffers":[{"byteLength":44}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[2,2,0]},)"
        R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}]})";
    while (json.size() % 4) json += ' ';
    const float positions[9]{0,0,0, 2,0,0, 0,2,0};
    const std::uint16_t indices[4]{0, 1, 2, 0};
    const auto u32 = [](std::uint32_t v) { return std::string(reinterpret_cast<const char *>(&v), 4); };
    std::string bin(reinterpret_cast<const char *>(positions), sizeof positions);
    bin.append(reinterpret_cast<const char *>(indices), sizeof indices);
    return "glTF" + u32(2) + u32(std::uint32_t(12 + 8 + json.size() + 8 + bin.size())) + u32(std::uint32_t(json.size())) + "JSON" + json +
           u32(std::uint32_t(bin.size())) + std::string("BIN\0", 4) + bin;
}
void write_file(const std::filesystem::path &path, const std::string &bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary); file << bytes;
    check(static_cast<bool>(file), "test file written");
}
void asset_workflow() {
    namespace fs = std::filesystem;
    const auto base = fs::current_path() / "editor-asset-test-files";
    fs::remove_all(base);
    const auto project = base / "project", source = base / "source";
    fs::create_directories(project);
    const std::string red_pixel = "P6\n1 1\n255\n\xff\x01\x01";
    write_file(source / "tri.obj", "v -1 0 0\nv 1 0 0\nv 0 3 0\nf 1 2 3\n");
    write_file(source / "red.ppm", red_pixel);
    write_file(source / "notes.txt", "not an asset");
    write_file(source / "bad.obj", "v 0 0 0\nf 1 2 3\n");

    EditorSession s;
    check(!s.open_project(base / "absent"), "missing project folder rejected");
    check(!s.import_asset(source / "tri.obj"), "import without a project rejected");
    ok(s.open_project(project));
    check(s.assets().empty() && !fs::exists(s.asset_folder()), "opening a project creates nothing");

    // Unsupported and broken files are reported and leave nothing in the catalogue.
    const auto unsupported = s.import_asset(source / "notes.txt");
    check(!unsupported && unsupported.error().message.find(".txt") != std::string::npos, "unsupported file type names itself");
    check(!s.import_asset(source / "bad.obj") && s.assets().empty() && s.asset_issues().empty(), "broken file leaves no asset");

    const auto obj = s.import_asset(source / "tri.obj"); ok(obj);
    const auto ppm = s.import_asset(source / "red.ppm"); ok(ppm);
    check(obj.value().type == assets::AssetType::Mesh && ppm.value().type == assets::AssetType::Texture, "asset types");
    check(s.assets().size() == 2 && fs::exists(s.asset_folder() / "tri.obj") && fs::exists(s.asset_folder() / "red.ppm.meta"), "assets in the folder");

    // Place: named after the file, resting on the floor; one undo step.
    const auto revision = s.commands().revision();
    ok(s.place_asset(obj.value().id));
    const auto placed = s.selection();
    const auto record = *s.scene().inspect(placed);
    check(record.name == "tri" && near(record.transform.position.y, 0) && s.commands().revision() == revision + 1, "model placed");
    check(!s.place_asset(ppm.value().id), "a texture cannot be placed");
    check(record.components.mesh->mesh_asset == AssetRef{obj.value().id.value, "tri.obj"}, "reference has id and path");

    // Frame: the model has a registry handle that finds its data.
    auto f = s.frame(1); ok(f);
    const auto model_handle = f.value()->objects.at(0).mesh;
    check(model_handle.index >= renderer::ResourceRegistry::first_index && s.asset_problems().empty(), "model handle in the frame");
    check(s.resources().find(model_handle)->indices.size() == 3, "handle finds the mesh");
    check(!s.resources().find(renderer::builtin_cube), "built-in handles are not assets");

    // Assign a texture and a model to an existing entity; both are undoable and survive save and load.
    ok(s.create("Box")); const auto box = s.selection();
    ok(s.execute(commands::SetMesh{box, MeshComponent{BuiltinMesh::Cube, {1, 0, 0}, 0.5, 0.25}}));
    ok(s.assign_asset(box, ppm.value().id));
    auto mesh = *s.scene().inspect(box)->components.mesh;
    check(mesh.texture.id == ppm.value().id.value && mesh.mesh_asset.empty() && mesh.color == Vec3{1, 1, 1} && mesh.roughness == 0.5, "texture assigned");
    f = s.frame(1); ok(f);
    check(f.value()->objects.at(1).surface.texture.index >= renderer::ResourceRegistry::first_index && f.value()->objects.at(1).mesh == renderer::builtin_cube, "textured built-in");
    ok(s.undo());
    check(s.scene().inspect(box)->components.mesh->texture.empty() && s.scene().inspect(box)->components.mesh->color == Vec3{1, 0, 0}, "assign undone");
    ok(s.redo());
    check(!s.assign_asset(box, assets::AssetId{12345}) && !s.assign_asset(999999, ppm.value().id), "bad assignments rejected");
    ok(s.create("Empty", 0, {})); const auto empty = s.selection();
    ok(s.assign_asset(empty, obj.value().id));
    check(s.scene().inspect(empty)->components.mesh.has_value(), "assigning adds a mesh renderer");

    // The placed triangle's box spans x -1..1, y 0..3, z flat: a fitted collider matches it, with some thickness.
    const auto fitted = s.model_collider(record.components.mesh->mesh_asset);
    check(fitted && fitted->shape == ColliderShape::Box && near(fitted->center.y, 1.5) && near(fitted->half_extents.x, 1) &&
          near(fitted->half_extents.y, 1.5) && near(fitted->half_extents.z, 0.001), "collider fitted to the model");
    check(!s.model_collider({}) && !s.model_collider({1, "absent.glb"}), "no collider for an unresolved model");
    ok(s.execute(commands::SetColliders{placed, {*fitted}}));
    s.sync_physics();
    check(s.physics_bridge().diagnostics().issues.empty(), "the fitted collider is accepted by physics");
    ok(s.undo());

    // Picking uses the model's bounds, focus its center.
    s.camera.target = {0, 1.5, 0}; s.camera.yaw = 0; s.camera.pitch = 0; s.camera.distance = 8;
    ok(s.execute(commands::DeleteEntity{box})); ok(s.execute(commands::DeleteEntity{empty}));
    f = s.frame(1); ok(f);
    check(s.pick(*f.value(), 0, 0) == placed, "model picked inside its bounds");
    s.select(placed); s.focus();
    check(near(s.camera.target.y, 1.5) && s.camera.distance > 3, "focus frames the model's bounds");
    ok(s.undo()); ok(s.undo());

    const auto scene_file = base / "assets.scene";
    const auto saved = s.scene().snapshot().entities;
    ok(s.save(scene_file));
    {
        std::ifstream in(scene_file); const std::string text{std::istreambuf_iterator<char>(in), {}};
        check(text.find("\"tri.obj\"") != std::string::npos && text.find("\"red.ppm\"") != std::string::npos, "references are in the scene file");
    }

    // A new session: same project, references resolve; ids came from the sidecars.
    {
        EditorSession other;
        ok(other.open_project(project));
        ok(other.load(scene_file));
        check(other.scene().snapshot().entities == saved && !other.dirty(), "references round trip");
        auto reopened = other.frame(1); ok(reopened);
        check(other.asset_problems().empty() && other.resources().find(reopened.value()->objects.at(0).mesh), "references resolve after reopen");

        // Play mode copies them and renders the same assets; Stop leaves them.
        ok(other.play());
        auto playing = other.frame(1); ok(playing);
        check(playing.value()->objects.at(0).mesh == reopened.value()->objects.at(0).mesh, "Play mode draws the same asset");
        ok(other.stop());
        check(other.scene().snapshot().entities == saved, "Play left the references alone");
    }

    // The path is the fallback when the id is gone: a recreated .meta gets a new id.
    {
        fs::remove(s.asset_folder() / "tri.obj.meta");
        EditorSession other;
        ok(other.open_project(project)); ok(other.load(scene_file));
        auto frame = other.frame(1); ok(frame);
        check(other.asset_problems().empty() && frame.value()->objects.at(0).mesh.index >= renderer::ResourceRegistry::first_index, "reference found by path");
    }

    // Missing asset: a magenta cube, a clear problem, and the reference kept for when the file returns.
    fs::rename(s.asset_folder() / "red.ppm", base / "red.ppm.away");
    fs::rename(s.asset_folder() / "tri.obj", base / "tri.obj.away");
    {
        EditorSession other;
        ok(other.open_project(project)); ok(other.load(scene_file));
        auto frame = other.frame(1); ok(frame);
        const auto &objects = frame.value()->objects;
        check(objects.at(0).mesh == renderer::builtin_cube && objects.at(0).surface.linear_color == Vec3{1, 0, 1}, "missing model is a magenta cube");
        check(objects.at(1).surface.linear_color == Vec3{1, 0, 1} && !objects.at(1).surface.texture.index, "missing texture is magenta");
        check(other.asset_problems().size() == 2 && other.asset_problems()[0].find("tri.obj") != std::string::npos, "problems listed");
        check(other.scene().snapshot().entities == saved && !other.dirty(), "missing assets leave the references intact");
        ok(other.save(base / "resaved.scene"));
        auto text = std::ifstream(base / "resaved.scene"); const std::string body{std::istreambuf_iterator<char>(text), {}};
        check(body.find("\"tri.obj\"") != std::string::npos, "saving keeps a missing reference");

        // The file returns: a rescan picks it up.
        fs::rename(base / "tri.obj.away", s.asset_folder() / "tri.obj");
        fs::rename(base / "red.ppm.away", s.asset_folder() / "red.ppm");
        ok(other.rescan_assets());
        frame = other.frame(1); ok(frame);
        check(other.asset_problems().empty() && frame.value()->objects.at(0).mesh.index >= renderer::ResourceRegistry::first_index, "rescan restores the model");
    }

    // No asset folder at all: the same placeholder, never a failure.
    {
        EditorSession other;
        ok(other.load(scene_file));
        auto frame = other.frame(1); ok(frame);
        check(other.asset_problems().size() == 2 && frame.value()->objects.at(0).mesh == renderer::builtin_cube, "no project gives placeholders");
    }

    // A file changed on disk: Reload bumps the generation, the old handle goes stale and the new data is drawn.
    ok(s.rescan_assets());
    f = s.frame(1); ok(f);
    const auto before_reload = f.value()->objects.at(0).mesh;
    write_file(s.asset_folder() / "tri.obj", "v -1 0 0\nv 1 0 0\nv 0 3 0\nv 1 3 0\nf 1 2 3\nf 2 4 3\n");
    const auto meshes = s.assets();
    const auto tri = std::find_if(meshes.begin(), meshes.end(), [](const assets::AssetMetadata &a) { return a.path == "tri.obj"; });
    ok(s.reload_asset(tri->id));
    f = s.frame(1); ok(f);
    const auto after_reload = f.value()->objects.at(0).mesh;
    check(after_reload.index == before_reload.index && after_reload.generation == before_reload.generation + 1, "reload bumps the generation");
    check(!s.resources().find(before_reload) && s.resources().find(after_reload)->indices.size() == 6, "stale handle finds nothing");

    // GLB through the whole path, where the build has the parser.
    if (assets::gltf_import_available()) {
        write_file(source / "tri.glb", triangle_glb());
        write_file(source / "fake.glb", "not a model");
        const auto bad = s.import_asset(source / "fake.glb");
        check(!bad && bad.error().message.find("Invalid GLB") != std::string::npos, "invalid GLB reported");
        const auto glb = s.import_asset(source / "tri.glb"); ok(glb);
        check(glb.value().warnings.size() == 1 && glb.value().warnings[0].find("animations") != std::string::npos, "import warnings returned");
        ok(s.place_asset(glb.value().id));
        const auto entity = s.selection();
        check(near(s.scene().inspect(entity)->transform.position.y, -1), "bounds include the node transform");
        const auto model = s.model(s.scene().inspect(entity)->components.mesh->mesh_asset);
        check(model && model->submeshes.size() == 1 && model->submeshes[0].material.roughness == 0.25f, "model material kept");
        f = s.frame(1); ok(f);
        check(s.asset_problems().empty(), "GLB resolves");
    }
    fs::remove_all(base);
}
std::string read_all(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
std::chrono::nanoseconds fixed_step_of_session() { return EditorSession::fixed_step; }
void materials_workflow() {
    namespace fs = std::filesystem;
    const auto base = fs::current_path() / "editor-material-test-files";
    fs::remove_all(base);
    const auto project = base / "project";
    fs::create_directories(project);
    write_file(base / "red.ppm", "P6\n1 1\n255\n\xff\x01\x01");
    EditorSession s;
    check(!s.create_material("x"), "a material needs a project");
    ok(s.open_project(project));
    auto created = s.create_material("Rust"); ok(created);
    const auto rust = created.value();
    check(fs::exists(project / "Assets/materials/Rust.material"), "material written to the project");
    const auto listed = [&](assets::AssetId id) {
        const auto all = s.assets();
        return std::find_if(all.begin(), all.end(), [&](const assets::AssetMetadata &m) { return m.id == id && m.type == assets::AssetType::Material; }) != all.end();
    };
    check(listed(rust), "material visible in the catalogue");

    // Duplicate: a new ID and the same values.
    auto data = *s.material(rust);
    data.roughness = 0.2;
    ok(s.save_material(rust, data));
    auto copied = s.duplicate_material(rust); ok(copied);
    check(copied.value() != rust && listed(copied.value()) && s.material(copied.value())->roughness == 0.2, "duplicate keeps values under a new ID");

    // Assign to an entity: drawn with the material.
    ok(s.create("Box")); const auto box = s.selection();
    const auto revision = s.commands().revision();
    ok(s.assign_material(box, rust));
    check(s.commands().revision() == revision + 1, "assignment is one step");
    check(s.scene().inspect(box)->components.mesh->material.id == rust.value, "reference stored");
    auto f = s.frame(1); ok(f);
    check(f.value()->objects.at(0).material == s.material_handle(rust), "frame draws the material");
    const auto *desc = s.resources().find(f.value()->objects.at(0).material);
    check(desc && near(desc->roughness, 0.2), "registry holds its values");
    ok(s.undo());
    check(s.scene().inspect(box)->components.mesh->material.empty(), "assignment undone");
    ok(s.redo());
    check(!s.assign_material(box, assets::AssetId{424242}), "unknown material rejected");

    // Per-part: only that slot changes; an empty id clears it; slots stay sorted.
    ok(s.assign_material(box, copied.value(), 3));
    ok(s.assign_material(box, rust, 1));
    auto slots = s.scene().inspect(box)->components.mesh->material_slots;
    check(slots.size() == 2 && slots[0].slot == 1 && slots[1].slot == 3, "slots sorted");
    f = s.frame(1); ok(f);
    check(f.value()->objects.at(0).parts.size() == 2 && f.value()->objects.at(0).parts[1].part == 3, "parts reach the frame");
    ok(s.assign_material(box, assets::AssetId{}, 1));
    check(s.scene().inspect(box)->components.mesh->material_slots.size() == 1, "slot cleared");

    // A live edit shows at once, and neither saves nor adds history.
    auto file_before = read_all(project / "Assets/materials/Rust.material");
    auto draft = data; draft.metallic = 0.75;
    const auto undo_before = s.commands().undo_size();
    ok(s.preview_material(rust, draft));
    f = s.frame(1); ok(f);
    check(near(s.resources().find(f.value()->objects.at(0).material)->metallic, 0.75), "preview reaches the frame");
    check(read_all(project / "Assets/materials/Rust.material") == file_before, "preview does not touch the file");
    check(s.commands().undo_size() == undo_before, "preview adds no history");
    check(s.material(rust)->metallic == 0.75, "the draft is what the editor shows");
    s.discard_material_draft(rust);
    check(s.material(rust)->metallic == 0, "a discarded draft is gone");
    check(!s.preview_material(rust, [&] { auto bad = data; bad.metallic = 3; return bad; }()), "invalid draft rejected");

    // Saving is one undo step, in order with scene edits.
    ok(s.preview_material(rust, draft));
    ok(s.save_material(rust, draft));
    check(read_all(project / "Assets/materials/Rust.material") != file_before, "saved to the file");
    Transform moved; moved.position = {5, 0, 0};
    ok(s.execute(commands::SetTransform{box, moved}));
    auto after_both = s.material(rust)->metallic;
    check(after_both == 0.75, "edit kept");
    check(s.can_undo(), "undo offered");
    ok(s.undo()); // The move, which was last.
    check(s.scene().inspect(box)->transform.position != moved.position && s.material(rust)->metallic == 0.75, "scene edit undone first");
    ok(s.undo()); // Then the material.
    check(s.material(rust)->metallic == 0 && read_all(project / "Assets/materials/Rust.material") == file_before, "material undone, and its file too");
    f = s.frame(1); ok(f);
    check(near(s.resources().find(f.value()->objects.at(0).material)->metallic, 0), "the frame follows undo");
    ok(s.redo());
    check(s.material(rust)->metallic == 0.75, "material redone");
    ok(s.redo());
    check(s.scene().inspect(box)->transform.position == moved.position, "scene edit redone");
    // A new scene edit ends the redo chain for materials too.
    ok(s.undo()); ok(s.undo());
    ok(s.execute(commands::RenameEntity{box, "Renamed"}));
    check(!s.can_redo(), "a new edit clears redo");

    // Texture slots: a linear slot is read back as what was picked.
    const auto image = s.import_asset(base / "red.ppm"); ok(image);
    auto textured = *s.material(rust);
    textured.base_color_map = {image.value().id, image.value().path};
    textured.normal_map = {image.value().id, image.value().path};
    ok(s.save_material(rust, textured));
    f = s.frame(1); ok(f);
    const auto *with_maps = s.resources().find(f.value()->objects.at(0).material);
    check(with_maps && with_maps->base_color_map.index && with_maps->base_color_map == with_maps->normal_map, "maps resolve to handles");

    // Persistence: saved, closed, reopened in a new session.
    const auto scene_file = base / "m.scene";
    ok(s.save(scene_file));
    {
        EditorSession t;
        ok(t.open_project(project));
        ok(t.load(scene_file));
        const auto loaded = t.scene().snapshot().entities.at(0);
        check(loaded.components.mesh->material.id == rust.value, "reference survives save and load");
        check(t.material(rust)->roughness == 0.2 && !t.material(rust)->base_color_map.empty(), "material survives a new session");
        auto g = t.frame(1); ok(g);
        check(t.asset_problems().empty(), "no problems when everything is there");
    }

    // Missing assets: the texture gone, the material gone. Reported; the scene loads and still draws.
    fs::remove(project / "Assets/red.ppm"); fs::remove(project / "Assets/red.ppm.meta");
    {
        EditorSession t;
        ok(t.open_project(project));
        ok(t.load(scene_file));
        auto g = t.frame(1); ok(g);
        check(g.value()->objects.size() == 1 && !t.asset_problems().empty(), "a missing map is reported and the object draws");
        check(t.asset_problems().front().find("red.ppm") != std::string::npos, "the problem names the file");
        check(g.value()->objects.at(0).material != renderer::builtin_lit, "the material still applies");
    }
    fs::remove(project / "Assets/materials/Rust.material"); fs::remove(project / "Assets/materials/Rust.material.meta");
    {
        EditorSession t;
        ok(t.open_project(project));
        ok(t.load(scene_file));
        auto g = t.frame(1); ok(g);
        check(g.value()->objects.at(0).material == renderer::builtin_lit && !t.asset_problems().empty(), "a missing material falls back and is reported");
        check(t.scene().snapshot().entities.at(0).components.mesh->material.id == rust.value, "the reference is kept");
    }
}
void lighting_settings() {
    EditorSession s;
    // No environment entity: the defaults apply.
    check(!s.environment_entity(), "no environment yet");
    auto f = s.frame(1); ok(f);
    check(f.value()->environment.source == EnvironmentSource::Procedural && near(f.value()->environment.ev100, 15), "default environment");
    check(f.value()->show_grid, "the grid shows while editing");
    // Setting one creates an entity, one undo step; the next edit reuses it.
    auto indoor = EditorSession::indoor_environment();
    ok(s.set_environment(indoor));
    const auto owner = s.environment_entity();
    check(owner != 0 && s.scene().size() == 1, "environment entity created");
    indoor.ev100 = 7;
    ok(s.set_environment(indoor));
    check(s.environment_entity() == owner && s.scene().size() == 1, "edit reuses it");
    f = s.frame(1); ok(f);
    check(near(f.value()->environment.ev100, 7) && !f.value()->environment.show_skybox, "frame carries the settings");
    check(f.value()->environment.sky_color.x < 0.55 && f.value()->environment.sky_color.x > 0, "colors become linear");
    ok(s.undo());
    check(near(s.environment().ev100, 6), "undone");
    ok(s.undo());
    check(!s.environment_entity(), "entity gone with its undo");
    // Point light sized for the exposure: a real bulb indoors, far more under daylight exposure.
    check(near(EditorSession::point_light_lumens(6), 3000), "indoor point light");
    check(EditorSession::point_light_lumens(15) > 1e6, "outdoor point light");
    // Lights reach the frame with their new fields.
    Components c;
    LightComponent spot; spot.type = LightType::Spot; spot.inner_cone_degrees = 10; spot.outer_cone_degrees = 20; spot.cast_shadows = true;
    c.light = spot;
    ok(s.execute(commands::CreateEntity{"Spot", {}, 0, 0, c}));
    ok(s.create_preset_lights(true));
    ok(s.create_preset_lights(false));
    f = s.frame(1); ok(f);
    check(f.value()->lights.size() == 3, "lights extracted");
    check(f.value()->lights[0].kind == renderer::LightKind::Spot && f.value()->lights[0].cast_shadows && near(f.value()->lights[0].outer_cone_radians, glm::radians(20.0)), "spot fields");
    // An image environment with a missing file: reported, and lit by the sky instead.
    EnvironmentComponent image;
    image.source = EnvironmentSource::Image;
    image.image = {77, "environments/gone.hdr"};
    ok(s.set_environment(image));
    f = s.frame(1); ok(f);
    check(f.value()->environment.source == EnvironmentSource::Procedural && !s.asset_problems().empty(), "missing environment image falls back");
    // Play mode hides the grid.
    ok(s.play());
    f = s.frame(1); ok(f);
    check(!f.value()->show_grid, "no grid in Play mode");
    ok(s.stop());
}
void background_import() {
    namespace fs = std::filesystem;
    const auto base = fs::current_path() / "editor-import-test-files";
    fs::remove_all(base);
    const auto project = base / "project";
    fs::create_directories(project);
    write_file(base / "tri.obj", "v -1 0 0\nv 1 0 0\nv 0 3 0\nf 1 2 3\n");
    write_file(base / "bad.obj", "v 0 0 0\nf 1 2 3\n");
    EditorSession s;
    ok(s.open_project(project));
    s.begin_import(base / "tri.obj");
    s.begin_import(base / "bad.obj");
    s.begin_import(base / "absent.png");
    check(s.imports_pending() == 3, "imports are pending");
    const auto finished = s.wait_for_imports();
    check(finished.size() == 3 && s.imports_pending() == 0, "all finished");
    check(std::count_if(finished.begin(), finished.end(), [](const FinishedImport &f) { return static_cast<bool>(f.result); }) == 1, "only the good one lands");
    const auto all = s.assets();
    check(all.size() == 1 && all[0].path == "tri.obj" && s.asset_state(all[0].id) == assets::AssetState::Loaded, "catalogued and cached");
    ok(s.place_asset(all[0].id));
    auto f = s.frame(1); ok(f);
    check(f.value()->objects.size() == 1 && s.asset_problems().empty(), "usable at once");
}
void resource_lifetime() {
    namespace fs = std::filesystem;
    const auto base = fs::current_path() / "editor-lifetime-test-files";
    fs::remove_all(base);
    const auto project = base / "project";
    fs::create_directories(project);
    write_file(base / "tri.obj", "v -1 0 0\nv 1 0 0\nv 0 3 0\nf 1 2 3\n");
    EditorSession s;
    ok(s.open_project(project));
    const auto imported = s.import_asset(base / "tri.obj"); ok(imported);
    ok(s.place_asset(imported.value().id));
    const auto placed = s.selection();
    auto f = s.frame(1); ok(f);
    const auto handle = f.value()->objects.at(0).mesh;
    check(s.resources().find(handle) != nullptr, "held while drawn");
    check(s.release_unused_assets() == 0 && s.resources().find(handle) != nullptr, "an asset in use is kept");
    // Play keeps references alive even if only the runtime copy has them.
    ok(s.play());
    ok(s.stop());
    // Removed from the scene: the CPU data goes, and only then. Undo brings the reference back, and it loads again.
    ok(s.execute(commands::DeleteEntity{placed}));
    check(s.release_unused_assets() == 1, "the unused asset is freed");
    check(s.resources().find(handle) == nullptr && s.resources().held_meshes() == 0, "the registry let it go");
    check(s.asset_state(imported.value().id) == assets::AssetState::Unloaded, "the database let it go");
    ok(s.undo());
    f = s.frame(1); ok(f);
    check(s.resources().find(f.value()->objects.at(0).mesh) != nullptr && s.asset_problems().empty(), "reloaded from disk on the next reference");
    check(f.value()->objects.at(0).mesh == handle, "with the same handle: the asset did not change");
}
void shooter_game() {
    namespace fs = std::filesystem;
    const fs::path sample = fs::path(ENGINE_TEST_SAMPLES_DIR) / "PhysicsShooter";
    EditorSession s;
    ok(s.open_project(sample));
    ok(s.load(sample / "physics_shooter.scene"));
    check(s.scene().size() > 60 && !s.dirty(), "sample loads clean");
    check(!s.game(), "no game while editing");
    const auto authored = s.editor_scene().snapshot();
    const auto revision = s.commands().revision();
    auto f = s.frame(1.6); ok(f);
    check(s.asset_problems().empty(), "every reference in the sample resolves");
    check(f.value()->objects.size() > 50 && !f.value()->lights.empty(), "sample draws");

    // Play sets up the game from the scene's behaviors, viewing through the player's camera.
    ok(s.play());
    auto *game = s.game();
    check(game != nullptr && game->phase() == gameplay::GamePhase::Ready, "Play offers the game");
    check(game->setup_issues().empty(), "the scene's behaviors are understood");
    f = s.frame(1.6); ok(f);
    check(s.view_camera() == game->camera(), "looking through the player");
    game->start();
    s.update(std::chrono::milliseconds(500));
    check(s.runtime_ticks() > 0 && game->ticks() == s.runtime_ticks(), "gameplay ticks with physics");
    gameplay::InputFrame input;
    input.held[std::size_t(gameplay::Action::MoveForward)] = true;
    input.held[std::size_t(gameplay::Action::Fire)] = true;
    input.pressed[std::size_t(gameplay::Action::Fire)] = true;
    const auto start_z = s.scene().world_transform(game->player())->matrix[14];
    for (int i = 0; i < 30; ++i) { game->set_input(input); s.update(fixed_step_of_session()); }
    check(s.scene().world_transform(game->player())->matrix[14] < start_z - 0.5, "the player moves");
    check(game->hud().ammo < game->hud().magazine, "the gun fires");

    // Pause stops the world; the toolbar's Pause and the game's agree.
    ok(s.pause());
    check(game->phase() == gameplay::GamePhase::Paused, "game paused");
    const auto ticks = s.runtime_ticks();
    s.update(std::chrono::milliseconds(200));
    check(s.runtime_ticks() == ticks, "no ticks while paused");
    ok(s.play());
    check(game->phase() == gameplay::GamePhase::Playing, "game resumed");

    // Restart: a new game on a fresh copy, without leaving Play mode.
    const auto identity = s.scene_identity();
    ok(s.restart());
    game = s.game();
    check(game != nullptr && game->phase() == gameplay::GamePhase::Ready && game->hud().ammo == game->hud().magazine && game->hud().score == 0, "restart starts over");
    check(s.scene_identity() != identity && s.scene().snapshot().entities == authored.entities, "restart restores the scene");
    check(s.play_state() == PlayState::Playing, "still playing");

    // Stop: the editor scene is untouched, as is its history, and it still works.
    game->start();
    for (int i = 0; i < 30; ++i) { game->set_input(input); s.update(fixed_step_of_session()); }
    ok(s.stop());
    check(!s.game(), "game gone after Stop");
    check(s.editor_scene().snapshot() == authored && s.commands().revision() == revision && !s.dirty(), "Stop restores the editor scene");
    ok(s.create("After")); ok(s.undo());
    check(s.editor_scene().snapshot().entities == authored.entities, "the editor remains usable");
    // Empty scenes have no game and depend on nothing of the sample.
    ok(s.new_scene());
    ok(s.play());
    check(!s.game(), "an empty scene runs no game");
    ok(s.stop());
}
int main() {
    try { editing(); persistence(); camera_and_picking(); world_edits(); documents(); play_mode(); components_and_rendering(); hierarchy_editing(); physics_play();
        acceptance_workflow(); external_camera_validation(); asset_workflow();
        materials_workflow(); lighting_settings(); background_import(); resource_lifetime(); shooter_game();
        std::cout << "17 editor acceptance groups passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
