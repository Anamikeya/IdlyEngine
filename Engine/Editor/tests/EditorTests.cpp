#include <engine/editor/EditorSession.hpp>
#include <cmath>
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
}
int main() {
    try { editing(); persistence(); camera_and_picking(); world_edits(); documents(); play_mode(); acceptance_workflow();
        external_camera_validation();
        std::cout << "8 editor acceptance groups passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
