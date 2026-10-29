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
    try { editing(); persistence(); camera_and_picking(); world_edits(); external_camera_validation();
        std::cout << "5 editor acceptance groups passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
