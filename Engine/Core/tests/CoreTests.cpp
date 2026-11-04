#include <algorithm>
#include <cmath>
#include <engine/core/Application.hpp>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
using namespace engine;
using namespace engine::commands;
using namespace std::chrono_literals;
namespace {
Context owner{"test-host", 3};
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class T> void ok(const Result<T> &r) {
    if (!r)
        throw std::runtime_error(r.error().message);
}
template <class T> void error(const Result<T> &r, ErrorCode code) {
    check(!r, "Expected failure");
    check(r.error().code == code, "Wrong structured error code");
}
EntityId create(CommandBus &bus, std::string name = "Entity", EntityId parent = 0,
                Transform t = {}) {
    auto r = bus.execute(owner, bus.revision(), CreateEntity{std::move(name), t, parent});
    ok(r);
    return r.value().created.at(0);
}
void creation_and_flecs() {
    Scene scene;
    CommandBus bus(scene);
    auto id = create(bus);
    check(id != 0 && scene.size() == 1, "Create failed");
    int count = 0;
    scene.ecs().native().each([&](const StableId &stable, const Transform &t) {
        check(stable.value == id && t.scale.x == 1, "Native components mismatch");
        ++count;
    });
    check(count == 1, "Entity not stored in Flecs");
    check(registry().size() == 17, "Registry incomplete");
    check(describe(Command{DeleteEntity{id}}).name == "entity.delete", "Typed registry mismatch");
}
void permissions_and_revision() {
    Scene s;
    CommandBus b(s);
    error(b.execute({"agent", 0}, 0, CreateEntity{"No", {}, 0, 0}), ErrorCode::PermissionDenied);
    error(b.inspect({"agent", 2}), ErrorCode::PermissionDenied);
    error(b.execute({"", 3}, 0, CreateEntity{"No", {}, 0, 0}), ErrorCode::InvalidArgument);
    create(b);
    error(b.execute(owner, 0, RenameEntity{1, "Stale"}), ErrorCode::Conflict);
    check(s.inspect(1)->name == "Entity" && b.revision() == 1, "Stale request mutated scene");
    ok(b.inspect({"reader", 1}));
}
void atomic_batch() {
    Scene s;
    CommandBus b(s);
    auto id = create(b);
    const auto before = s.snapshot();
    Command commands[] = {RenameEntity{id, "Changed"}, SetParent{id, 999}};
    error(b.execute(owner, b.revision(), commands), ErrorCode::NotFound);
    check(s.snapshot() == before && b.revision() == 1 && b.undo_size() == 1,
          "Failed batch was not atomic");
    Command valid[] = {RenameEntity{id, "Changed"}, CreateEntity{"Child", {}, id, 0}};
    auto r = b.execute(owner, b.revision(), valid);
    ok(r);
    check(b.revision() == 2 && s.size() == 2, "Transaction revision count");
    ok(b.undo(owner, 2));
    check(s.size() == 1 && s.inspect(id)->name == "Entity", "Transaction undo");
    ok(b.redo(owner, 3));
    check(s.size() == 2 && s.inspect(id)->name == "Changed", "Transaction redo");
}
void hierarchy_and_cycles() {
    Scene s;
    CommandBus b(s);
    Transform root;
    root.position = {10, 0, 0};
    root.scale = {2, 2, 2};
    auto a = create(b, "Root", 0, root);
    Transform child;
    child.position = {1, 3, 0};
    auto c = create(b, "Child", a, child);
    auto m = s.world_transform(c)->matrix;
    check(m[12] == 12 && m[13] == 6, "Parent/local world composition");
    const auto before = s.snapshot();
    error(b.execute(owner, b.revision(), SetParent{a, c}), ErrorCode::Cycle);
    check(before == s.snapshot(), "Cycle changed scene");
    ok(b.execute(owner, b.revision(), SetParent{c, 0}));
    check(s.world_transform(c)->matrix[12] == 1, "Reparent should preserve local transform");
    ok(b.undo(owner, b.revision()));
    check(s.world_transform(c)->matrix[12] == 12, "Undo reparent");
}
void rotation_composition() {
    Scene s;
    CommandBus b(s);
    Transform t;
    const double h = std::sqrt(0.5);
    t.rotation = {0, 0, h, h};
    auto a = create(b, "Rotated", 0, t);
    Transform child;
    child.position = {1, 0, 0};
    auto c = create(b, "Child", a, child);
    auto m = s.world_transform(c)->matrix;
    check(std::abs(m[12]) < 1e-10 && std::abs(m[13] - 1) < 1e-10, "Quaternion convention");
}
void delete_restore_and_ids() {
    Scene s;
    CommandBus b(s);
    auto root = create(b, "Root"), child = create(b, "Child", root);
    ok(b.execute(owner, b.revision(), DeleteEntity{root}));
    check(s.size() == 0, "Subtree not deleted");
    ok(b.undo(owner, b.revision()));
    check(s.inspect(child)->parent == root, "Subtree IDs not restored");
    ok(b.redo(owner, b.revision()));
    auto newer = create(b, "New");
    check(newer > child, "Stable ID recycled");
    error(b.redo(owner, b.revision()), ErrorCode::InvalidState);
    error(b.execute(owner, b.revision(), CreateEntity{"Recycled", {}, 0, root}),
          ErrorCode::InvalidArgument);
}
void transform_validation() {
    Scene s;
    CommandBus b(s);
    auto id = create(b);
    Transform t;
    t.position.x = std::numeric_limits<double>::quiet_NaN();
    error(b.execute(owner, b.revision(), SetTransform{id, t}), ErrorCode::InvalidArgument);
    t = {};
    t.rotation.w = 2;
    error(b.execute(owner, b.revision(), SetTransform{id, t}), ErrorCode::InvalidArgument);
    t = {};
    t.scale.x = 0;
    error(b.execute(owner, b.revision(), SetTransform{id, t}), ErrorCode::InvalidArgument);
    t = {};
    t.position = {2, 3, 4};
    ok(b.execute(owner, b.revision(), SetTransform{id, t}));
    ok(b.undo(owner, b.revision()));
    check(s.inspect(id)->transform.position.x == 0, "Transform undo");
    error(b.execute(owner, b.revision(), RenameEntity{id, std::string(257, 'x')}),
          ErrorCode::InvalidArgument);
}
void serialization_roundtrip() {
    Scene s;
    CommandBus b(s);
    auto id = create(b, "Quoted \"name\" and \\ path");
    Transform t;
    t.position = {1.23456789012345, -2, 8};
    create(b, "Child", id, t);
    std::ostringstream out;
    ok(write_scene(out, s.snapshot()));
    std::istringstream in(out.str());
    auto loaded = read_scene(in);
    ok(loaded);
    check(loaded.value() == s.snapshot(), "Lossy serialization");
    Scene dest;
    CommandBus bus(dest);
    ok(bus.execute(owner, 0, ReplaceScene{loaded.value()}));
    check(dest.snapshot() == s.snapshot(), "Atomic scene import mismatch");
    std::ostringstream again;
    ok(write_scene(again, dest.snapshot()));
    check(again.str() == out.str(), "Serialization not deterministic");
}
void physics_components() {
    Scene s;
    CommandBus b(s);
    RigidBodyComponent body;
    body.kind = BodyKind::Kinematic;
    body.linear_velocity = {1, 0.5, 0};
    body.angular_damping = 0.25;
    body.enable_sleep = false;
    ColliderComponent floor;
    floor.half_extents = {5, 0.5, 5};
    floor.friction = 0.75;
    ColliderComponent probe;
    probe.shape = ColliderShape::Capsule;
    probe.center = {0, 1, 0};
    probe.radius = 0.25;
    probe.half_height = 0.5;
    probe.sensor = true;
    probe.events = true;
    const auto id = create(b, "Platform");
    ok(b.execute(owner, b.revision(), SetRigidBody{id, body}));
    ok(b.execute(owner, b.revision(), SetColliders{id, {floor, probe}}));
    check(s.inspect(id)->components.body == body && s.inspect(id)->components.colliders == std::vector{floor, probe},
          "Body and colliders stored");
    int mirrored = 0;
    s.ecs().native().each([&](const StableId &stable, const RigidBodyComponent &r) { mirrored += stable.value == id && r == body; });
    check(mirrored == 1, "Rigid body mirrored into Flecs");
    ok(b.execute(owner, b.revision(), SetColliders{id, {}}));
    check(s.inspect(id)->components.colliders.empty(), "Empty list removes colliders");
    ok(b.undo(owner, b.revision()));
    check(s.inspect(id)->components.colliders.size() == 2, "Collider removal undoable");
    // Text round trip, written as documented.
    std::ostringstream out;
    ok(write_scene(out, s.snapshot()));
    check(out.str().find("BODY 1 kinematic 1 0.5 0 0 0 0 0 0.25 1 0 0 0\n") != std::string::npos &&
              out.str().find("COLLIDER 1 box 0 0 0 0 0 0 1 5 0.5 5 0.5 0.5 1000 0.75 0 0 0 18446744073709551615 18446744073709551615\n") != std::string::npos &&
              out.str().find("COLLIDER 1 capsule 0 1 0 0 0 0 1 0.5 0.5 0.5 0.25 0.5 1000 0.59999999999999998 0 1 1 18446744073709551615 18446744073709551615\n") != std::string::npos,
          "Physics records not written as documented");
    std::istringstream in(out.str());
    auto loaded = read_scene(in);
    ok(loaded);
    check(loaded.value() == s.snapshot(), "Lossy physics serialization");
    const std::string head = "ENGINE_SCENE 4 2 1 ENTITY 1 0 \"A\" 0 0 0 0 0 0 1 1 1 1 ";
    const std::string ok_body = "BODY 1 dynamic 0 0 0 0 0 0 0 0 1 1 0";
    const std::string ok_collider = "COLLIDER 1 sphere 0 0 0 0 0 0 1 0.5 0.5 0.5 0.5 0.5 1000 0.6 0 0 0";
    for (const std::string &body_text : std::vector<std::string>
         {"COMPONENTS 1 BODY 1 floating 0 0 0 0 0 0 0 0 1 1 0", "COMPONENTS 1 BODY 1 dynamic 0 0 0 0 0 0 -1 0 1 1 0",
          "COMPONENTS 1 BODY 1 dynamic 0 0 0 0 0 0 0 0 1 2 0", "COMPONENTS 2 " + ok_body + " " + ok_body,
          "COMPONENTS 1 COLLIDER 1 cone 0 0 0 0 0 0 1 0.5 0.5 0.5 0.5 0.5 1000 0.6 0 0 0",
          "COMPONENTS 1 COLLIDER 1 sphere 0 0 0 0 0 0 1 0.5 0.5 0.5 0 0.5 1000 0.6 0 0 0",
          "COMPONENTS 1 COLLIDER 1 sphere 0 0 0 0 0 0 0 0.5 0.5 0.5 0.5 0.5 1000 0.6 0 0 0",
          "COMPONENTS 1 COLLIDER 1 sphere 0 0 0 0 0 0 1 0.5 0.5 0.5 0.5 0.5 1000 0.6 2 0 0",
          "COMPONENTS 1 COLLIDER 1 sphere 0 0 0 0 0 0 1 0.5 0.5", "COMPONENTS 70"}) {
        std::istringstream bad(head + body_text);
        check(!read_scene(bad), "Malformed physics record accepted");
    }
    std::istringstream two(head + "COMPONENTS 3 " + ok_collider + " " + ok_body + " " + ok_collider);
    auto parsed = read_scene(two);
    ok(parsed);
    check(parsed.value().entities[0].components.colliders.size() == 2 && parsed.value().entities[0].components.body,
          "Several colliders and one body per entity");
    // The example in docs/architecture/006-scene-format.md, verbatim: it loads and is what the writer emits.
    const std::string documented = "ENGINE_SCENE 7 5 4\n"
                                   "ENTITY 1 0 \"Ball\" 0 0.5 0 0 0 0 1 1 1 1\n"
                                   "ENTITY 2 0 \"Sun\" 0 4 0 0 0 0 1 1 1 1\n"
                                   "ENTITY 3 0 \"Camera\" 0 1.5 6 0 0 0 1 1 1 1\n"
                                   "ENTITY 4 0 \"Floor\" 0 0 0 0 0 0 1 10 1 10\n"
                                   "COMPONENTS 6\n"
                                   "MESH 1 sphere 1 0.25 0.125 0.5 0 - - - 1 1\n"
                                   "BODY 1 dynamic 0 0 0 0 0 0 0 0 1 1 0 0\n"
                                   "COLLIDER 1 sphere 0 0 0 0 0 0 1 0.5 0.5 0.5 0.5 0.5 1000 0.75 0 0 0 18446744073709551615 18446744073709551615\n"
                                   "LIGHT 2 directional 1 1 1 100000 10 25 35 0\n"
                                   "CAMERA 3 60 0.10000000000000001 1000 1\n"
                                   "COLLIDER 4 box 0 -0.5 0 0 0 0 1 0.5 0.5 0.5 0.5 0.5 1000 0.75 0 0 0 18446744073709551615 18446744073709551615\n";
    std::istringstream example(documented);
    auto example_scene = read_scene(example);
    ok(example_scene);
    std::ostringstream example_out;
    ok(write_scene(example_out, example_scene.value()));
    check(example_out.str() == documented, "Documented scene example differs from the writer's output");
    // Physics records did not exist before version 4.
    std::istringstream v3("ENGINE_SCENE 3 2 1 ENTITY 1 0 \"A\" 0 0 0 0 0 0 1 1 1 1 COMPONENTS 1 " + ok_body);
    check(!read_scene(v3), "BODY accepted in a version 3 file");
}
void duplication() {
    Scene s;
    CommandBus b(s);
    const auto holder = create(b, "Holder");
    Transform t; t.position = {1, 2, 3};
    const auto root = create(b, "Root", holder, t);
    const auto child = create(b, "Child", root);
    const auto grandchild = create(b, "Grandchild", child);
    const auto sibling = create(b, "Sibling", root);
    const MeshComponent red{BuiltinMesh::Sphere, {1, 0, 0}, 0.2, 0.3};
    ok(b.execute(owner, b.revision(), SetMesh{grandchild, red}));
    const auto before = s.snapshot();
    auto r = b.execute(owner, b.revision(), DuplicateEntity{root, "Root (copy)"});
    ok(r);
    const auto &created = r.value().created;
    check(created.size() == 4 && s.size() == before.entities.size() + 4, "Whole subtree copied");
    for (const auto id : created)
        check(id >= before.next_id, "Copies get fresh stable IDs");
    const auto copy = *s.inspect(created[0]);
    check(copy.name == "Root (copy)" && copy.parent == holder && copy.transform == t, "Root copy keeps parent and transform, takes the name");
    // Breadth first: the root's children (by ID) come before the grandchild.
    const auto child_copy = *s.inspect(created[1]), sibling_copy = *s.inspect(created[2]), grand_copy = *s.inspect(created[3]);
    check(child_copy.name == "Child" && child_copy.parent == created[0] && sibling_copy.name == "Sibling" &&
              sibling_copy.parent == created[0] && grand_copy.name == "Grandchild" && grand_copy.parent == created[1],
          "Copies are wired to copied parents");
    check(grand_copy.components.mesh == red, "Components copied");
    for (const auto id : {holder, root, child, grandchild, sibling})
        check(*s.inspect(id) == *std::find_if(before.entities.begin(), before.entities.end(), [&](const EntityRecord &e) { return e.id == id; }),
              "Originals untouched");
    ok(b.undo(owner, b.revision()));
    check(s.snapshot().entities == before.entities, "One undo removes every copy");
    ok(b.redo(owner, b.revision()));
    check(s.inspect(created[3]) && s.inspect(created[3])->parent == created[1], "Redo restores the same IDs");
    ok(b.execute(owner, b.revision(), DuplicateEntity{grandchild, ""}));
    check(s.inspect(s.snapshot().entities.back().id)->name == "Grandchild", "Empty name keeps the original");
    error(b.execute(owner, b.revision(), DuplicateEntity{999, ""}), ErrorCode::NotFound);
    error(b.execute(owner, b.revision(), DuplicateEntity{root, std::string(257, 'x')}), ErrorCode::InvalidArgument);
    // Inside a batch, later commands see the copies.
    const auto revision = b.revision();
    const auto next = s.snapshot().next_id;
    const Command batch[] = {DuplicateEntity{sibling, "Twin"}, RenameEntity{next, "Twin renamed"}};
    ok(b.execute(owner, revision, std::span<const Command>(batch)));
    check(s.inspect(next)->name == "Twin renamed" && b.revision() == revision + 1, "Duplicate composes in one transaction");
}
void components() {
    Scene s;
    CommandBus b(s);
    const MeshComponent red{BuiltinMesh::Sphere, {1, 0, 0}, 0.25, 0.5};
    auto created = b.execute(owner, 0, CreateEntity{"Ball", {}, 0, 0, {red, {}, {}}});
    ok(created);
    const auto ball = created.value().created.at(0);
    check(s.inspect(ball)->components == Components{red, {}, {}}, "CreateEntity components");
    const auto lamp = create(b, "Lamp");
    check(s.inspect(lamp)->components == Components{}, "Entities start without components");
    const CameraComponent lens{45, 0.5, 200, false};
    const LightComponent point{LightType::Point, {1, 0.5, 0.25}, 1500, 4};
    ok(b.execute(owner, b.revision(), SetCamera{lamp, lens}));
    ok(b.execute(owner, b.revision(), SetLight{lamp, point}));
    check(s.inspect(lamp)->components == Components{{}, lens, point}, "Set camera and light");
    // The scene mirrors components into Flecs; inspect() reads them back from there.
    int meshes = 0, lights = 0;
    s.ecs().native().each([&](const StableId &id, const MeshComponent &m) { meshes += id.value == ball && m == red; });
    s.ecs().native().each([&](const StableId &id, const LightComponent &l) { lights += id.value == lamp && l == point; });
    check(meshes == 1 && lights == 1, "Components not mirrored into Flecs");
    ok(b.execute(owner, b.revision(), SetLight{lamp, std::nullopt}));
    check(!s.inspect(lamp)->components.light, "Remove light");
    lights = 0;
    s.ecs().native().each([&](const StableId &, const LightComponent &) { ++lights; });
    check(lights == 0, "Removed component still in Flecs");
    ok(b.undo(owner, b.revision()));
    check(s.inspect(lamp)->components.light == point, "Undo remove light");
    ok(b.redo(owner, b.revision()));
    ok(b.undo(owner, b.revision()));
    ok(b.execute(owner, b.revision(), DeleteEntity{ball}));
    ok(b.undo(owner, b.revision()));
    check(s.inspect(ball)->components.mesh == red, "Delete undo restores components");
    // Invalid values are rejected without changing the scene.
    const auto revision = b.revision();
    const auto before = s.snapshot();
    MeshComponent bright = red; bright.color.x = 1.5;
    CameraComponent flat = lens; flat.far_plane = flat.near_plane;
    LightComponent dark = point; dark.intensity = -1;
    MeshComponent unknown = red; unknown.mesh = BuiltinMesh(9);
    error(b.execute(owner, revision, SetMesh{ball, bright}), ErrorCode::InvalidArgument);
    error(b.execute(owner, revision, SetMesh{ball, unknown}), ErrorCode::InvalidArgument);
    error(b.execute(owner, revision, SetCamera{lamp, flat}), ErrorCode::InvalidArgument);
    error(b.execute(owner, revision, SetLight{lamp, dark}), ErrorCode::InvalidArgument);
    error(b.execute(owner, revision, SetMesh{999, red}), ErrorCode::NotFound);
    check(b.revision() == revision && s.snapshot() == before, "Rejected component edit changed the scene");
    // Exact text round trip with every component kind, and a deterministic writer.
    std::ostringstream out;
    ok(write_scene(out, s.snapshot()));
    check(out.str().find("MESH 1 sphere 1 0 0 0.25 0.5 - - - 1 1\n") != std::string::npos &&
              out.str().find("CAMERA 2 45 0.5 200 0\n") != std::string::npos &&
              out.str().find("LIGHT 2 point 1 0.5 0.25 1500 4 25 35 0\n") != std::string::npos,
          "Component lines not written as documented");
    std::istringstream in(out.str());
    auto loaded = read_scene(in);
    ok(loaded);
    check(loaded.value() == s.snapshot(), "Lossy component serialization");
    std::ostringstream again;
    ok(write_scene(again, loaded.value()));
    check(again.str() == out.str(), "Component serialization not deterministic");
    const std::string head = "ENGINE_SCENE 3 3 2 ENTITY 1 0 \"A\" 0 0 0 0 0 0 1 1 1 1 ENTITY 2 0 \"B\" 0 0 0 0 0 0 1 1 1 1 ";
    for (const std::string body :
         {"COMPONENTS 1 MESH 7 cube 1 1 1 0.5 0", "COMPONENTS 2 MESH 1 cube 1 1 1 0.5 0 MESH 1 cube 1 1 1 0.5 0",
          "COMPONENTS 1 MESH 1 cone 1 1 1 0.5 0", "COMPONENTS 1 MESH 1 cube 1 1", "COMPONENTS 1 MESH 1 cube 2 1 1 0.5 0",
          "COMPONENTS 1 CAMERA 1 60 0.1 100 2", "COMPONENTS 1 CAMERA 1 60 1 1 1", "COMPONENTS 1 LIGHT 1 spot 1 1 1 1 1",
          "COMPONENTS 1 LIGHT 1 point 1 1 1 1 0", "COMPONENTS 1 AUDIO 1", "COMPONENTS 7", "COMPONENTS 0 MESH 1 cube 1 1 1 0.5 0"}) {
        std::istringstream bad(head + body);
        check(!read_scene(bad), "Malformed component record accepted");
    }
    std::istringstream good(head + "COMPONENTS 2 LIGHT 2 directional 1 1 1 0 10 MESH 1 plane 0 0 0 0 1");
    auto parsed = read_scene(good);
    ok(parsed);
    check(parsed.value().entities[0].components.mesh->mesh == BuiltinMesh::Plane && parsed.value().entities[1].components.light &&
              !parsed.value().entities[0].components.light,
          "Component records in any order");
}
void asset_references() {
    Scene s;
    CommandBus b(s);
    const auto id = create(b, "Model");
    MeshComponent model;
    model.mesh_asset = {0x6a1f0c3e9b2d4a01ull, "models/hel \"met\".glb"};
    model.texture = {0x0000000000000abcull, "t/wood.png"};
    ok(b.execute(owner, b.revision(), SetMesh{id, model}));
    // References are part of the record: undo, snapshot and exact text round trip.
    std::ostringstream out;
    ok(write_scene(out, s.snapshot()));
    check(out.str().find("6a1f0c3e9b2d4a01 \"models/hel \\\"met\\\".glb\" 0000000000000abc \"t/wood.png\" - 1 1\n") != std::string::npos,
          "Asset references not written as documented");
    std::istringstream in(out.str());
    auto loaded = read_scene(in);
    ok(loaded);
    check(loaded.value() == s.snapshot(), "Asset references lost in a round trip");
    ok(b.undo(owner, b.revision()));
    check(!s.inspect(id)->components.mesh, "Undo did not remove the reference");
    ok(b.redo(owner, b.revision()));
    check(s.inspect(id)->components.mesh->texture.path == "t/wood.png", "Redo did not restore the reference");
    // Half a reference, control characters and oversized paths are refused by commands and by the reader.
    const auto revision = b.revision();
    for (const AssetRef &bad : {AssetRef{1, ""}, AssetRef{0, "a.glb"}, AssetRef{1, "a\nb.glb"},
                               AssetRef{1, std::string(max_asset_path_bytes + 1, 'a')}}) {
        MeshComponent wrong;
        wrong.mesh_asset = bad;
        error(b.execute(owner, revision, SetMesh{id, wrong}), ErrorCode::InvalidArgument);
        wrong = {};
        wrong.texture = bad;
        error(b.execute(owner, revision, SetMesh{id, wrong}), ErrorCode::InvalidArgument);
    }
    const std::string head = "ENGINE_SCENE 5 2 1 ENTITY 1 0 \"A\" 0 0 0 0 0 0 1 1 1 1 COMPONENTS 1 MESH 1 cube 1 1 1 0.5 0 ";
    for (const std::string &body : std::vector<std::string>
         {"", "-", "- x", "0000000000000001 - -", "0000000000000001", "1 \"a.glb\" -", "000000000000000g \"a.glb\" -",
          "0000000000000000 \"a.glb\" -", "- 0000000000000001 \"\"", "00000000000000001 \"a.glb\" -",
          "0000000000000001 \"" + std::string(max_asset_path_bytes + 1, 'a') + "\" -"}) {
        std::istringstream bad(head + body);
        check(!read_scene(bad), "Malformed asset reference accepted");
    }
    // Version 4 files have no reference fields.
    std::istringstream v4("ENGINE_SCENE 4 2 1 ENTITY 1 0 \"A\" 0 0 0 0 0 0 1 1 1 1 COMPONENTS 1 MESH 1 sphere 1 1 1 0.5 0");
    auto old = read_scene(v4);
    ok(old);
    check(old.value().entities[0].components.mesh->mesh == BuiltinMesh::Sphere &&
              old.value().entities[0].components.mesh->mesh_asset.empty(),
          "Version 4 mesh record");
    std::istringstream v4_extra("ENGINE_SCENE 4 2 1 ENTITY 1 0 \"A\" 0 0 0 0 0 0 1 1 1 1 COMPONENTS 1 MESH 1 sphere 1 1 1 0.5 0 - -");
    check(!read_scene(v4_extra), "Reference fields accepted in a version 4 file");
}
void format_v6() {
    Scene scene;
    CommandBus b(scene);
    Components all;
    MeshComponent mesh;
    mesh.material = {0x1122334455667788, "materials/a b.material"};
    mesh.material_slots = {{0, {1, "materials/one.material"}}, {3, {2, "materials/two.material"}}};
    mesh.uv_scale_u = 2;
    mesh.uv_scale_v = 0.5;
    all.mesh = mesh;
    LightComponent spot;
    spot.type = LightType::Spot;
    spot.inner_cone_degrees = 10;
    spot.outer_cone_degrees = 20;
    spot.cast_shadows = true;
    all.light = spot;
    RigidBodyComponent body;
    body.lock_rotation = true;
    all.body = body;
    ColliderComponent collider;
    collider.category = 4;
    collider.mask = 3;
    all.colliders = {collider};
    EnvironmentComponent environment;
    environment.source = EnvironmentSource::Image;
    environment.image = {9, "environments/sky.hdr"};
    environment.tone_mapping = ToneMapping::PbrNeutral;
    environment.shadow_quality = ShadowQuality::High;
    environment.ev100 = 7.5;
    environment.bloom = false;
    all.environment = environment;
    all.behaviors = {{"health", {{"max", 50}, {"regen", 0.25}}}, {"target", {}}};
    auto created = b.execute(owner, 0, CreateEntity{"All", {}, 0, 0, all});
    ok(created);
    const auto id = created.value().created.front();
    check(scene.inspect(id)->components == all, "Every new component is stored");
    // Round trip, exact and deterministic.
    std::ostringstream out;
    ok(write_scene(out, scene.snapshot()));
    const std::string text = out.str();
    for (const char *expected : {"ENGINE_SCENE 7 ", "MATSLOT 1 3 0000000000000002 \"materials/two.material\"\n",
                                 "LIGHT 1 spot 1 1 1 100000 10 10 20 1\n", " 1 0 1\n", "ENVIRONMENT 1 image 0000000000000009 \"environments/sky.hdr\" ",
                                 " 7.5 pbr_neutral high 60 1 0\n", "BEHAVIOR 1 health 2 max 50 regen 0.25\n", "BEHAVIOR 1 target 0\n", " 0 0 4 3\n"})
        check(text.find(expected) != std::string::npos, expected);
    std::istringstream in(text);
    auto parsed = read_scene(in);
    ok(parsed);
    check(parsed.value().entities[0].components == all, "Format 6 round trip");
    std::ostringstream again;
    ok(write_scene(again, parsed.value()));
    check(again.str() == text, "Format 6 writes the same text twice");
    // Records in any order: a slot before its mesh.
    std::istringstream shuffled("ENGINE_SCENE 6 2 1 ENTITY 1 0 \"A\" 0 0 0 0 0 0 1 1 1 1 COMPONENTS 2 MATSLOT 1 2 0000000000000002 \"b\" MESH 1 cube 1 1 1 0.5 0 - - - 1 1");
    auto reordered = read_scene(shuffled);
    ok(reordered);
    check(reordered.value().entities[0].components.mesh->material_slots.size() == 1, "A slot record may come first");
    // New commands, undoable.
    ok(b.execute(owner, b.revision(), SetBehaviors{id, {}}));
    check(scene.inspect(id)->components.behaviors.empty(), "Behaviors removed");
    ok(b.undo(owner, b.revision()));
    check(scene.inspect(id)->components.behaviors.size() == 2, "Behaviors restored by undo");
    ok(b.execute(owner, b.revision(), SetEnvironment{id, std::nullopt}));
    check(!scene.inspect(id)->components.environment, "Environment removed");
    ok(b.undo(owner, b.revision()));
    check(scene.inspect(id)->components.environment == environment, "Environment restored by undo");
    // Validation guards commands.
    const auto rejected = [&](Components c) { error(b.execute(owner, b.revision(), CreateEntity{"Bad", {}, 0, 0, std::move(c)}), ErrorCode::InvalidArgument); };
    MeshComponent bad_uv; bad_uv.uv_scale_u = 0;
    rejected({bad_uv, {}, {}});
    MeshComponent bad_slots; bad_slots.material_slots = {{1, {1, "a"}}, {1, {2, "b"}}};
    rejected({bad_slots, {}, {}});
    MeshComponent empty_slot; empty_slot.material_slots = {{0, {}}};
    rejected({empty_slot, {}, {}});
    MeshComponent half_ref; half_ref.material = {0, "a"};
    rejected({half_ref, {}, {}});
    LightComponent wide; wide.type = LightType::Spot; wide.outer_cone_degrees = 100;
    rejected({{}, {}, wide});
    LightComponent crossed; crossed.inner_cone_degrees = 40; crossed.outer_cone_degrees = 30;
    rejected({{}, {}, crossed});
    const auto with_environment = [&](EnvironmentComponent e) { Components c; c.environment = e; rejected(c); };
    EnvironmentComponent no_image; no_image.source = EnvironmentSource::Image;
    with_environment(no_image);
    EnvironmentComponent dark; dark.intensity = -1;
    with_environment(dark);
    EnvironmentComponent far_ev; far_ev.ev100 = 99;
    with_environment(far_ev);
    EnvironmentComponent no_distance; no_distance.shadow_distance = 0;
    with_environment(no_distance);
    const auto with_behaviors = [&](std::vector<BehaviorComponent> list) { Components c; c.behaviors = std::move(list); rejected(c); };
    with_behaviors({{"Bad Type", {}}});
    with_behaviors({{"", {}}});
    with_behaviors({{"same", {}}, {"same", {}}});
    with_behaviors({{"a", {{"k", 1}, {"k", 2}}}});
    with_behaviors({{"a", {{"k", std::nan("")}}}});
    with_behaviors({{"a", {{"Bad Key", 1}}}});
    // Malformed records are refused by the reader.
    const std::string head = "ENGINE_SCENE 6 2 1 ENTITY 1 0 \"A\" 0 0 0 0 0 0 1 1 1 1 ";
    for (const std::string &record : std::vector<std::string>{
             "COMPONENTS 1 MESH 1 cube 1 1 1 0.5 0 - -",                         // Version 5's fields only.
             "COMPONENTS 1 MESH 1 cube 1 1 1 0.5 0 - - - 0 1",                   // UV scale.
             "COMPONENTS 1 LIGHT 1 cone 1 1 1 1 1 10 20 0",
             "COMPONENTS 1 LIGHT 1 spot 1 1 1 1 1 10 20",
             "COMPONENTS 1 LIGHT 1 spot 1 1 1 1 1 10 20 2",
             "COMPONENTS 1 BODY 1 dynamic 0 0 0 0 0 0 0 0 1 1 0",                // No lock flag.
             "COMPONENTS 1 COLLIDER 1 box 0 0 0 0 0 0 1 1 1 1 1 1 1000 0.6 0 0 0", // No filter.
             "COMPONENTS 1 MATSLOT 1 0 -",
             "COMPONENTS 2 MATSLOT 1 0 0000000000000001 \"a\" MATSLOT 1 0 0000000000000002 \"b\"",
             "COMPONENTS 1 ENVIRONMENT 1 candle - 1 1 1 1 1 1 1 1 1 1 0 1 15 aces medium 60 1 1",
             "COMPONENTS 1 ENVIRONMENT 1 procedural - 1 1 1 1 1 1 1 1 1 1 0 1 15 blurry medium 60 1 1",
             "COMPONENTS 1 ENVIRONMENT 1 procedural - 1 1 1 1 1 1 1 1 1 1 0 1 15 aces ultra 60 1 1",
             "COMPONENTS 1 ENVIRONMENT 1 procedural - 1 1 1",
             "COMPONENTS 2 ENVIRONMENT 1 procedural - 1 1 1 1 1 1 1 1 1 1 0 1 15 aces medium 60 1 1 ENVIRONMENT 1 procedural - 1 1 1 1 1 1 1 1 1 1 0 1 15 aces medium 60 1 1",
             "COMPONENTS 1 BEHAVIOR 1 a 1 k",
             "COMPONENTS 1 BEHAVIOR 1 a 99 k 1",
             "COMPONENTS 1 BEHAVIOR 1 a 1 k nan",
             "COMPONENTS 1 BEHAVIOR 7 a 0"}) {
        std::istringstream bad(head + record);
        check(!read_scene(bad), ("Malformed version 6 record accepted: " + record).c_str());
    }
    // New records and values do not exist in older versions.
    for (const char *old : {"ENGINE_SCENE 5 2 1 ENTITY 1 0 \"A\" 0 0 0 0 0 0 1 1 1 1 COMPONENTS 1 BEHAVIOR 1 a 0",
                            "ENGINE_SCENE 5 2 1 ENTITY 1 0 \"A\" 0 0 0 0 0 0 1 1 1 1 COMPONENTS 1 MATSLOT 1 0 0000000000000001 \"a\"",
                            "ENGINE_SCENE 5 2 1 ENTITY 1 0 \"A\" 0 0 0 0 0 0 1 1 1 1 COMPONENTS 1 LIGHT 1 spot 1 1 1 1 1"}) {
        std::istringstream bad(old);
        check(!read_scene(bad), old);
    }
    // Version 5 records keep their meaning, and the defaults fill the new fields.
    std::istringstream v5("ENGINE_SCENE 5 2 1 ENTITY 1 0 \"A\" 0 0 0 0 0 0 1 1 1 1 COMPONENTS 3 MESH 1 cube 1 1 1 0.5 0 - - BODY 1 dynamic 0 0 0 0 0 0 0 0 1 1 0 LIGHT 1 point 1 1 1 5 6");
    auto loaded = read_scene(v5);
    ok(loaded);
    const auto &old_components = loaded.value().entities[0].components;
    check(old_components.mesh->material.empty() && old_components.mesh->uv_scale_u == 1 && !old_components.body->lock_rotation && !old_components.light->cast_shadows, "Version 5 defaults");
}
void malformed_serialization() {
    for (const auto *text :
         {"WRONG 1 1 0", "ENGINE_SCENE 2 1 0", "ENGINE_SCENE 1 0 0", "ENGINE_SCENE 1 1 100001",
          "ENGINE_SCENE 1 1 0 trailing", "ENGINE_SCENE 1 2 1 ENTITY 1 0 \"x\" 0 0",
          "ENGINE_SCENE 0 1 0", "ENGINE_SCENE 8 1 0 COMPONENTS 0", "ENGINE_SCENE 2 1 0 COMPONENTS",
          "ENGINE_SCENE 2 1 0 COMPONENTS -1", "ENGINE_SCENE 2 1 0 COMPONENTS 1 MESH 1",
          "ENGINE_SCENE 2 1 0 COMPONENTS 0 trailing", "ENGINE_SCENE 1 1 0 COMPONENTS 0"}) {
        std::istringstream in(text);
        check(!read_scene(in), "Malformed scene accepted");
    }
    // Version 1 files (no COMPONENTS section) still load, and are written back as the current version.
    std::istringstream legacy("ENGINE_SCENE 1 3 1\nENTITY 2 0 \"Legacy\" 1 2 3 0 0 0 1 1 1 1\n");
    auto upgraded = read_scene(legacy);
    ok(upgraded);
    check(upgraded.value().next_id == 3 && upgraded.value().entities.size() == 1 &&
              upgraded.value().entities[0].name == "Legacy" &&
              upgraded.value().entities[0].transform.position == Vec3{1, 2, 3},
          "Version 1 scene not read");
    // Before components existed every entity was drawn as the built-in cube; the upgrade keeps that.
    check(upgraded.value().entities[0].components == Components{MeshComponent{}, {}, {}},
          "Version 1 entities must become default cubes");
    std::ostringstream rewritten;
    ok(write_scene(rewritten, upgraded.value()));
    check(rewritten.str().rfind("ENGINE_SCENE " + std::to_string(scene_format_version) + " 3 1\n", 0) == 0 &&
              rewritten.str().find("\nCOMPONENTS 1\nMESH 2 cube ") != std::string::npos,
          "Scene not written in the current format");
    std::istringstream v2("ENGINE_SCENE 2 3 1\nENTITY 2 0 \"S1\" 0 0 0 0 0 0 1 1 1 1\nCOMPONENTS 0\n");
    auto from_v2 = read_scene(v2);
    ok(from_v2);
    check(from_v2.value().entities[0].components.mesh == MeshComponent{}, "Version 2 entities must become default cubes");
    std::istringstream huge("ENGINE_SCENE 1 2 1 ENTITY 1 0 \"" + std::string(1000, 'x') +
                            "\" 0 0 0 0 0 0 1 1 1 1");
    check(!read_scene(huge), "Unbounded name accepted");
    SceneSnapshot cycle{3, {{1, "A", 2, {}}, {2, "B", 1, {}}}};
    error(Scene::validate(cycle), ErrorCode::Cycle);
    SceneSnapshot duplicate{3, {{1, "A", 0, {}}, {1, "B", 0, {}}}};
    error(Scene::validate(duplicate), ErrorCode::InvalidArgument);
}
void history_bound_and_noop() {
    Scene s;
    CommandBus b(s, 2);
    auto id = create(b);
    ok(b.execute(owner, b.revision(), RenameEntity{id, "B"}));
    ok(b.execute(owner, b.revision(), RenameEntity{id, "C"}));
    check(b.undo_size() == 2, "History limit ignored");
    auto rev = b.revision();
    ok(b.execute(owner, rev, RenameEntity{id, "C"}));
    check(b.revision() == rev, "No-op advanced revision");
    ok(b.undo(owner, b.revision()));
    ok(b.undo(owner, b.revision()));
    error(b.undo(owner, b.revision()), ErrorCode::InvalidState);
    check(s.inspect(id)->name == "Entity", "History restoration");
}
void thread_boundary() {
    Scene s;
    CommandBus b(s);
    bool rejected = false;
    std::thread t([&] {
        auto r = b.execute(owner, 0, CreateEntity{"No", {}, 0, 0});
        rejected = !r && r.error().code == ErrorCode::WrongThread;
    });
    t.join();
    check(rejected && s.size() == 0, "Wrong-thread mutation accepted");
}
struct Counter {
    int value = 0;
};
struct Client : ApplicationClient {
    int starts = 0, stops = 0, updates = 0, native_ticks = 0;
    bool throw_update = false, stop_fixed = false;
    std::vector<std::uint64_t> ticks;
    flecs::entity_t counter = 0;
    Result<void> on_start(Application &a) override {
        ++starts;
        auto &w = a.scene()->ecs().native();
        counter = w.entity().set<Counter>({0}).id();
        w.system<Counter>().each([](Counter &c) { ++c.value; });
        return {};
    }
    void on_fixed_update(Application &a, const FixedUpdate &f) override {
        ticks.push_back(f.tick);
        if (stop_fixed)
            a.request_stop();
    }
    void on_update(Application &, const FrameUpdate &) override {
        ++updates;
        if (throw_update)
            throw std::runtime_error("Injected callback failure");
    }
    void on_stop(Application &a) noexcept override {
        ++stops;
        if (a.scene())
            native_ticks = a.scene()->ecs().native().entity(counter).get<Counter>().value;
    }
};
void deterministic_loop() {
    Client a, b;
    Application one(a), two(b);
    ok(one.start());
    ok(two.start());
    for (auto delta : {3ms, 7ms, 25ms, 5ms})
        ok(one.advance(delta));
    for (auto delta : {10ms, 10ms, 10ms, 10ms})
        ok(two.advance(delta));
    check(a.ticks == b.ticks && one.tick() == 4, "Equal elapsed inputs gave different fixed ticks");
    auto f = one.advance(5ms);
    ok(f);
    check(f.value().interpolation == 0.5, "Interpolation wrong");
    ok(one.shutdown());
    ok(one.shutdown());
    ok(two.shutdown());
    check(a.stops == 1 && b.stops == 1 && a.native_ticks == 4,
          "Shutdown or Flecs scheduling failed");
    check(!one.scene() && !one.commands(), "World not released");
    error(one.start(), ErrorCode::InvalidState);
}
void catchup_and_invalid_time() {
    Client c;
    Application app(c, LoopConfig{10ms, 50ms, 2});
    ok(app.start());
    auto f = app.advance(100ms);
    ok(f);
    check(f.value().fixed_steps == 2 && f.value().dropped == 80ms, "Catch-up limit incorrect");
    error(app.advance(-1ns), ErrorCode::InvalidArgument);
    check(app.tick() == 2, "Negative time changed tick");
    auto next = app.advance(5ms);
    ok(next);
    check(next.value().fixed_steps == 0 && next.value().interpolation == 0.5,
          "Residual time incorrect");
    ok(app.shutdown());
    Client invalid;
    Application bad(invalid, LoopConfig{0ns, 50ms, 2});
    error(bad.start(), ErrorCode::InvalidArgument);
}
void callback_failure_and_stop() {
    Client c;
    c.throw_update = true;
    Application app(c);
    ok(app.start());
    error(app.advance(10ms), ErrorCode::Internal);
    check(app.state() == ApplicationState::Stopped && c.stops == 1,
          "Exception did not safely shut down");
    Client stop;
    stop.stop_fixed = true;
    Application other(stop);
    ok(other.start());
    ok(other.advance(30ms));
    check(other.tick() == 1 && stop.stops == 1 && stop.updates == 0, "Stop request ignored");
}
void replace_keeps_native_systems() {
    Scene s;
    CommandBus b(s);
    auto &w = s.ecs().native();
    int executions = 0;
    w.entity().set<Counter>({0});
    w.system<Counter>().each([&](Counter &) { ++executions; });
    SceneSnapshot loaded{11, {{10, "Loaded", 0, {}}}};
    ok(b.execute(owner, 0, ReplaceScene{loaded}));
    check(s.ecs().progress(0.01), "World progress failed");
    check(executions == 1 && s.inspect(10).has_value(),
          "Scene replacement destroyed native systems");
    ok(b.undo(owner, b.revision()));
    check(s.size() == 0, "Undo scene replacement failed");
    check(create(b) > 10, "Scene import allocator regressed");
}

void bounded_parser_and_stream_flags() {
    for (const auto &text :
         std::vector<std::string>{std::string(4096, 'X'), "ENGINE_SCENE 1 -1 0",
                                  "ENGINE_SCENE 1 2 1 " + std::string(4096, 'E'),
                                  "ENGINE_SCENE 1 2 1 ENTITY 1 0 \"x\" " + std::string(4096, '1'),
                                  "ENGINE_SCENE 1 2 1 ENTITY 1 0 \"x\" nan 0 0 0 0 0 1 1 1 1"}) {
        std::istringstream in(text);
        check(!read_scene(in), "Hostile input accepted");
    }
    Scene s;
    CommandBus b(s);
    create(b, "Escaped \"x\"");
    std::ostringstream out;
    out << std::hex << std::showpos << std::scientific;
    const auto flags = out.flags();
    ok(write_scene(out, s.snapshot()));
    check(out.flags() == flags, "Caller stream flags changed");
    std::istringstream in(out.str());
    auto parsed = read_scene(in);
    ok(parsed);
    check(parsed.value() == s.snapshot(), "Caller formatting corrupted scene format");
    std::ostringstream broken;
    broken.setstate(std::ios::badbit);
    check(!write_scene(broken, s.snapshot()), "Failed stream reported success");
}
struct StartClient : ApplicationClient {
    int mode = 0, stops = 0;
    bool resources_survived = false;
    Result<void> on_start(Application &a) override {
        if (mode == 0)
            return fail(ErrorCode::InvalidArgument, "Injected startup rejection");
        if (mode == 1)
            throw std::runtime_error("Injected startup exception");
        if (mode == 2)
            throw 17;
        error(a.advance(10ms), ErrorCode::InvalidState);
        ok(a.shutdown());
        resources_survived = a.scene() != nullptr && a.commands() != nullptr;
        return {};
    }
    void on_stop(Application &a) noexcept override {
        ++stops;
        auto ignored = a.shutdown();
        (void)ignored;
    }
};
void startup_failure_and_reentry() {
    for (int mode = 0; mode < 4; ++mode) {
        StartClient c;
        c.mode = mode;
        Application app(c);
        auto result = app.start();
        if (mode == 0)
            error(result, ErrorCode::InvalidArgument);
        else if (mode < 3)
            error(result, ErrorCode::Internal);
        else {
            ok(result);
            check(c.resources_survived, "Startup shutdown destroyed live callback resources");
        }
        check(c.stops == 1 && app.state() == ApplicationState::Stopped && !app.scene(),
              "Startup cleanup incomplete");
        ok(app.shutdown());
        check(c.stops == 1, "Stop callback invoked more than once");
    }
}
void transaction_limits_and_replace_rejection() {
    Scene s;
    CommandBus b(s);
    auto id = create(b);
    const auto before = s.snapshot();
    std::vector<Command> many(257, RenameEntity{id, "x"});
    error(b.execute(owner, b.revision(), std::span<const Command>(many)),
          ErrorCode::InvalidArgument);
    error(b.execute(owner, b.revision(), std::span<const Command>{}), ErrorCode::InvalidArgument);
    SceneSnapshot bad{2, {{1, "Broken", 99, {}}}};
    error(b.execute(owner, b.revision(), ReplaceScene{bad}), ErrorCode::NotFound);
    check(s.snapshot() == before && b.revision() == 1, "Rejected request mutated live state");
    error(b.undo({"reader", 1}, b.revision()), ErrorCode::PermissionDenied);
}
void timing_extremes() {
    Client c;
    Application app(c);
    ok(app.start());
    error(app.advance(Duration::max()), ErrorCode::InvalidArgument);
    check(app.tick() == 0 && app.state() == ApplicationState::Running,
          "Invalid large duration changed state");
    auto frame = app.advance(0ns);
    ok(frame);
    check(frame.value().fixed_steps == 0 && frame.value().interpolation == 0,
          "Zero delta advanced simulation");
    ok(app.shutdown());
    Client other;
    Application invalid(other, LoopConfig{1ns, 10ms, 1025});
    error(invalid.start(), ErrorCode::InvalidArgument);
}

// Version 7: one EMITTER per entity. Older versions refuse it; values are checked.
void format_v7() {
    Scene scene;
    CommandBus b(scene);
    Components all;
    all.emitter = ParticleEmitterComponent{{0x00000000000000ab, "effects/spark s.effect"}, false, true, 2.5};
    auto created = b.execute(owner, 0, CreateEntity{"Sparks", {}, 0, 0, all});
    ok(created);
    const auto id = created.value().created.front();
    check(scene.inspect(id)->components == all, "The emitter is stored");
    std::ostringstream out;
    ok(write_scene(out, scene.snapshot()));
    const std::string text = out.str();
    check(text.find("EMITTER 1 00000000000000ab \"effects/spark s.effect\" 0 1 2.5\n") != std::string::npos, text.c_str());
    std::istringstream in(text);
    auto parsed = read_scene(in);
    ok(parsed);
    check(parsed.value().entities[0].components == all, "Format 7 round trip");
    // Changed and removed through its command, with undo.
    auto changed = all.emitter;
    changed->rate_scale = 0;
    ok(b.execute(owner, b.revision(), SetParticleEmitter{id, changed}));
    check(scene.inspect(id)->components.emitter->rate_scale == 0, "The emitter command changes it");
    ok(b.execute(owner, b.revision(), SetParticleEmitter{id, std::nullopt}));
    check(!scene.inspect(id)->components.emitter, "An empty emitter command removes it");
    ok(b.undo(owner, b.revision()));
    check(scene.inspect(id)->components.emitter == changed, "Undo restores it");
    const std::string head = " 2 1 ENTITY 1 0 \"A\" 0 0 0 0 0 0 1 1 1 1 COMPONENTS ";
    for (const std::string bad : {"ENGINE_SCENE 6" + head + "1 EMITTER 1 - 1 1 1",
                                  "ENGINE_SCENE 7" + head + "2 EMITTER 1 - 1 1 1 EMITTER 1 - 1 1 1",
                                  "ENGINE_SCENE 7" + head + "1 EMITTER 1 - 2 1 1",
                                  "ENGINE_SCENE 7" + head + "1 EMITTER 1 - 1 1 -1",
                                  "ENGINE_SCENE 7" + head + "1 EMITTER 1 - 1 1 1001",
                                  "ENGINE_SCENE 7" + head + "1 EMITTER 1 0000000000000001 1 1 1"}) {
        std::istringstream rejected(bad);
        check(!read_scene(rejected), bad.c_str());
    }
    std::istringstream empty_effect("ENGINE_SCENE 7" + head + "1 EMITTER 1 - 1 0 0");
    auto plain = read_scene(empty_effect);
    ok(plain);
    check(plain.value().entities[0].components.emitter == ParticleEmitterComponent{{}, true, false, 0},
          "An emitter without an effect is allowed");
}

// Version 7 also has one ANIMATOR and one ATTACH per entity (Milestone 5 slice 3).
void format_v7_animation() {
    Scene scene;
    CommandBus b(scene);
    Components character;
    character.animator = AnimatorComponent{{0x00000000000000cd, "chars/walk \"cycle\".glb"}, "Walk \"fast\"", -1.5, false, 0.25, false};
    auto made = b.execute(owner, 0, CreateEntity{"Hero", {}, 0, 0, character});
    ok(made);
    const auto hero = made.value().created.front();
    Components sword;
    sword.attach = AttachToBoneComponent{"hand.R"};
    auto held = b.execute(owner, b.revision(), CreateEntity{"Sword", {}, hero, 0, sword});
    ok(held);
    const auto blade = held.value().created.front();
    check(scene.inspect(hero)->components == character && scene.inspect(blade)->components == sword, "Animator and attachment are stored");
    std::ostringstream out;
    ok(write_scene(out, scene.snapshot()));
    const std::string text = out.str();
    check(text.find("ANIMATOR 1 00000000000000cd \"chars/walk \\\"cycle\\\".glb\" \"Walk \\\"fast\\\"\" -1.5 0 0.25 0\n") != std::string::npos, text.c_str());
    check(text.find("ATTACH 2 \"hand.R\"\n") != std::string::npos, text.c_str());
    std::istringstream in(text);
    auto parsed = read_scene(in);
    ok(parsed);
    check(parsed.value() == scene.snapshot(), "Animator and attachment round trip");
    // The defaults: the model's own clips, no clip yet, speed 1, looping, from the start, previewed.
    auto plain_animator = AnimatorComponent{};
    ok(b.execute(owner, b.revision(), SetAnimator{hero, plain_animator}));
    check(scene.inspect(hero)->components.animator == plain_animator, "The animator command changes it");
    ok(b.execute(owner, b.revision(), SetAttachToBone{blade, std::nullopt}));
    check(!scene.inspect(blade)->components.attach, "An empty attach command removes it");
    ok(b.undo(owner, b.revision()));
    check(scene.inspect(blade)->components.attach == sword.attach, "Undo restores it");
    std::ostringstream again;
    ok(write_scene(again, scene.snapshot()));
    check(again.str().find("ANIMATOR 1 - \"\" 1 1 0 1\n") != std::string::npos, again.str().c_str());
    // Invalid values are refused by the command and by the reader.
    auto fast = plain_animator;
    fast.speed = 11;
    error(b.execute(owner, b.revision(), SetAnimator{hero, fast}), ErrorCode::InvalidArgument);
    error(b.execute(owner, b.revision(), SetAttachToBone{blade, AttachToBoneComponent{""}}), ErrorCode::InvalidArgument);
    const std::string head = " 2 1 ENTITY 1 0 \"A\" 0 0 0 0 0 0 1 1 1 1 COMPONENTS ";
    for (const std::string bad : {"ENGINE_SCENE 6" + head + "1 ANIMATOR 1 - \"\" 1 1 0 1",
                                  "ENGINE_SCENE 6" + head + "1 ATTACH 1 \"hand\"",
                                  "ENGINE_SCENE 7" + head + "2 ANIMATOR 1 - \"\" 1 1 0 1 ANIMATOR 1 - \"\" 1 1 0 1",
                                  "ENGINE_SCENE 7" + head + "2 ATTACH 1 \"a\" ATTACH 1 \"b\"",
                                  "ENGINE_SCENE 7" + head + "1 ANIMATOR 1 - \"\" 1 2 0 1",
                                  "ENGINE_SCENE 7" + head + "1 ANIMATOR 1 - \"\" 1 1 -1 1",
                                  "ENGINE_SCENE 7" + head + "1 ANIMATOR 1 - \"\" -10.5 1 0 1",
                                  "ENGINE_SCENE 7" + head + "1 ANIMATOR 1 - Walk 1 1 0 1",
                                  "ENGINE_SCENE 7" + head + "1 ATTACH 1 \"\"",
                                  "ENGINE_SCENE 7" + head + "1 ATTACH 1"}) {
        std::istringstream rejected(bad);
        check(!read_scene(rejected), bad.c_str());
    }
}

} // namespace
int main() {
    const std::pair<const char *, std::function<void()>> tests[] = {
        {"creation_and_flecs", creation_and_flecs},
        {"format_v6", format_v6},
        {"format_v7", format_v7},
        {"format_v7_animation", format_v7_animation},
        {"permissions_and_revision", permissions_and_revision},
        {"atomic_batch", atomic_batch},
        {"hierarchy_and_cycles", hierarchy_and_cycles},
        {"rotation_composition", rotation_composition},
        {"delete_restore_and_ids", delete_restore_and_ids},
        {"transform_validation", transform_validation},
        {"serialization_roundtrip", serialization_roundtrip},
        {"components", components},
        {"duplication", duplication},
        {"physics_components", physics_components},
        {"asset_references", asset_references},
        {"malformed_serialization", malformed_serialization},
        {"history_bound_and_noop", history_bound_and_noop},
        {"thread_boundary", thread_boundary},
        {"deterministic_loop", deterministic_loop},
        {"catchup_and_invalid_time", catchup_and_invalid_time},
        {"callback_failure_and_stop", callback_failure_and_stop},
        {"replace_keeps_native_systems", replace_keeps_native_systems},
        {"bounded_parser_and_stream_flags", bounded_parser_and_stream_flags},
        {"startup_failure_and_reentry", startup_failure_and_reentry},
        {"transaction_limits_and_replace_rejection", transaction_limits_and_replace_rejection},
        {"timing_extremes", timing_extremes}};
    int failed = 0;
    for (const auto &[name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &e) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    }
    std::cout << std::size(tests) - failed << "/" << std::size(tests) << " tests passed\n";
    return failed ? 1 : 0;
}
