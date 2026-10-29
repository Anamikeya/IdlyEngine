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
    check(registry().size() == 9, "Registry incomplete");
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
    check(out.str().find("MESH 1 sphere 1 0 0 0.25 0.5\n") != std::string::npos &&
              out.str().find("CAMERA 2 45 0.5 200 0\n") != std::string::npos &&
              out.str().find("LIGHT 2 point 1 0.5 0.25 1500 4\n") != std::string::npos,
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
void malformed_serialization() {
    for (const auto *text :
         {"WRONG 1 1 0", "ENGINE_SCENE 2 1 0", "ENGINE_SCENE 1 0 0", "ENGINE_SCENE 1 1 100001",
          "ENGINE_SCENE 1 1 0 trailing", "ENGINE_SCENE 1 2 1 ENTITY 1 0 \"x\" 0 0",
          "ENGINE_SCENE 0 1 0", "ENGINE_SCENE 4 1 0 COMPONENTS 0", "ENGINE_SCENE 2 1 0 COMPONENTS",
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

} // namespace
int main() {
    const std::pair<const char *, std::function<void()>> tests[] = {
        {"creation_and_flecs", creation_and_flecs},
        {"permissions_and_revision", permissions_and_revision},
        {"atomic_batch", atomic_batch},
        {"hierarchy_and_cycles", hierarchy_and_cycles},
        {"rotation_composition", rotation_composition},
        {"delete_restore_and_ids", delete_restore_and_ids},
        {"transform_validation", transform_validation},
        {"serialization_roundtrip", serialization_roundtrip},
        {"components", components},
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
