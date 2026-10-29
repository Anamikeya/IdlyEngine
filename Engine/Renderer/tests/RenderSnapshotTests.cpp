#include <engine/commands/CommandBus.hpp>
#include <engine/renderer/RenderSnapshot.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>

using namespace engine;
using namespace engine::commands;
using namespace engine::renderer;
namespace {
static_assert(!std::is_convertible_v<MeshHandle, MaterialHandle>);
static_assert(std::is_const_v<Frame::element_type>);
const Context host{"render-test", 3};
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void near(double value, double expected) {
    check(std::abs(value - expected) < 1e-9, "Unexpected matrix/vector value");
}
template <class T> void ok(const Result<T> &result) {
    if (!result)
        throw std::runtime_error(result.error().message);
}
void error(const Result<Frame> &result, ErrorCode code) {
    check(!result && result.error().code == code, "Expected structured extraction error");
}
struct Fixture {
    Scene scene;
    CommandBus bus{scene};
    EntityId camera_id = create();
    PerspectiveCamera camera{camera_id};
    EntityId create(Transform transform = {}, EntityId parent = 0) {
        auto result = bus.execute(host, bus.revision(), CreateEntity{"Entity", transform, parent});
        ok(result);
        return result.value().created.at(0);
    }
    Result<Frame> extract(std::span<const MeshBinding> meshes = {},
                          std::span<const LightBinding> lights = {}) {
        return extract_frame(scene, {42, bus.revision(), 7}, camera, meshes, lights);
    }
};
MeshBinding mesh(EntityId id) { return {id, {1, 2}, {3, 4}, true}; }
void camera_projection() {
    Fixture f;
    Transform pose;
    pose.position = {2, 3, 4};
    ok(f.bus.execute(host, f.bus.revision(), SetTransform{f.camera_id, pose}));
    auto result = f.extract();
    ok(result);
    const auto &frame = *result.value();
    check(frame.stamp.scene == 42 && frame.stamp.revision == f.bus.revision() &&
          frame.stamp.frame == 7, "Frame stamp lost");
    check(frame.objects.empty() && frame.lights.empty(), "Empty scene extraction");
    const auto &view = frame.camera.view.matrix;
    near(view[12], -2); near(view[13], -3); near(view[14], -4);
    const auto &p = frame.camera.projection.matrix;
    const auto depth = [&](double z) { return (p[10] * z + p[14]) / (p[11] * z); };
    near(depth(-f.camera.near_plane), -1);
    near(depth(-f.camera.far_plane), 1);
    near(p[5], 1 / std::tan(f.camera.vertical_fov_radians / 2));
    const double vertical_scale = p[5];
    f.camera.aspect_ratio = 2;
    result = f.extract();
    ok(result);
    near(result.value()->camera.projection.matrix[0], vertical_scale / 2);
    pose.rotation = {0, std::sqrt(0.5), 0, std::sqrt(0.5)};
    ok(f.bus.execute(host, f.bus.revision(), SetTransform{f.camera_id, pose}));
    result = f.extract(); ok(result);
    const auto &rotated = result.value()->camera;
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            double product = 0;
            for (std::size_t k = 0; k < 4; ++k)
                product += rotated.view.matrix[k * 4 + row] * rotated.world.matrix[column * 4 + k];
            near(product, row == column ? 1 : 0);
        }
    }
}
void hierarchy_sort_and_visibility() {
    Fixture f;
    Transform parent_pose; parent_pose.position = {10, 0, 0}; parent_pose.scale = {2, 2, 2};
    auto parent = f.create(parent_pose);
    Transform child_pose; child_pose.position = {1, 2, 3};
    auto child = f.create(child_pose, parent);
    auto hidden = mesh(f.create()); hidden.visible = false;
    MeshBinding bindings[] = {mesh(child), hidden, mesh(parent)};
    const auto before = f.scene.snapshot();
    const auto revision = f.bus.revision();
    auto result = f.extract(bindings);
    ok(result);
    const auto &objects = result.value()->objects;
    check(objects.size() == 2 && objects[0].entity == parent && objects[1].entity == child,
          "Stable ordering or visibility incorrect");
    near(objects[1].world.matrix[12], 12);
    near(objects[1].world.matrix[13], 4);
    near(objects[1].world.matrix[14], 6);
    check(objects[1].mesh == bindings[0].mesh && objects[1].material == bindings[0].material,
          "Asset generations not preserved");
    check(f.scene.snapshot() == before && f.bus.revision() == revision, "Extraction mutated scene");
}
void snapshot_lifetime() {
    Frame retained;
    {
        Fixture f;
        auto id = f.create();
        MeshBinding bindings[] = {mesh(id)};
        auto result = f.extract(bindings); ok(result); retained = result.value();
        Transform moved; moved.position = {99, 0, 0};
        ok(f.bus.execute(host, f.bus.revision(), SetTransform{id, moved}));
        auto updated = f.extract(bindings); ok(updated);
        near(updated.value()->objects[0].world.matrix[12], 99);
        near(retained->objects[0].world.matrix[12], 0);
        ok(f.bus.execute(host, f.bus.revision(), DeleteEntity{id}));
        error(f.extract(bindings), ErrorCode::NotFound);
        ok(f.bus.undo(host, f.bus.revision()));
        auto restored = f.extract(bindings); ok(restored);
        check(restored.value()->objects[0].entity == id, "Undo changed render identity");
    }
    double position = -1;
    std::thread reader([&] { position = retained->objects.at(0).world.matrix[12]; });
    reader.join();
    near(position, 0);
}
void light_extraction() {
    Fixture f;
    Transform pose; pose.position = {4, 5, 6};
    pose.rotation = {0, std::sqrt(0.5), 0, std::sqrt(0.5)};
    auto parent = f.create(pose);
    auto directional = f.create({}, parent);
    auto point = f.create();
    LightBinding lights[] = {{point, LightKind::Point, {1, 0.5, 0.25}, 800, 15},
                             {directional, LightKind::Directional, {1, 1, 1}, 10000, 99}};
    auto result = f.extract({}, lights); ok(result);
    const auto &out = result.value()->lights;
    check(out.size() == 2 && out[0].entity == directional, "Light sorting");
    near(out[0].position.x, 4); near(out[0].position.y, 5); near(out[0].position.z, 6);
    near(out[0].direction.x, -1); near(out[0].direction.y, 0); near(out[0].direction.z, 0);
    near(out[0].range, 0); near(out[0].intensity, 10000);
    near(out[1].range, 15); near(out[1].intensity, 800); near(out[1].linear_color.y, 0.5);
}
void invalid_meshes() {
    Fixture f;
    auto id = f.create();
    MeshBinding bindings[] = {mesh(id), mesh(id)};
    error(f.extract(bindings), ErrorCode::InvalidArgument);
    auto invalid = mesh(id); invalid.mesh.generation = 0;
    error(f.extract({&invalid, 1}), ErrorCode::InvalidArgument);
    invalid = mesh(id); invalid.material.index = 0;
    error(f.extract({&invalid, 1}), ErrorCode::InvalidArgument);
    invalid = mesh(999); invalid.visible = false;
    error(f.extract({&invalid, 1}), ErrorCode::NotFound);
    error(extract_frame(f.scene, {}, f.camera, {}, {}), ErrorCode::InvalidArgument);
    std::vector<MeshBinding> oversized(Scene::max_entities + 1);
    error(f.extract(oversized), ErrorCode::InvalidArgument);
    // Surface parameters are carried per object and must stay within [0, 1].
    auto surfaced = mesh(id); surfaced.surface = {{0.25, 0.5, 1}, 0.1, 0.9};
    auto carried = f.extract({&surfaced, 1});
    ok(carried);
    check(carried.value()->objects.at(0).surface == surfaced.surface, "Surface not carried into the frame");
    for (auto bad : {Surface{{1.5, 0, 0}, 0.5, 0}, Surface{{0, -0.1, 0}, 0.5, 0}, Surface{{0, 0, 0}, 2, 0},
                     Surface{{0, 0, 0}, 0.5, std::numeric_limits<double>::quiet_NaN()}}) {
        invalid = mesh(id); invalid.surface = bad;
        error(f.extract({&invalid, 1}), ErrorCode::InvalidArgument);
    }
}
void invalid_cameras() {
    Fixture f;
    const auto original = f.camera;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    for (double value : {0.0, -1.0, 4.0, nan, infinity}) {
        f.camera = original; f.camera.vertical_fov_radians = value;
        error(f.extract(), ErrorCode::InvalidArgument);
    }
    for (double value : {0.0, -1.0, nan, infinity}) {
        f.camera = original; f.camera.aspect_ratio = value;
        error(f.extract(), ErrorCode::InvalidArgument);
        f.camera = original; f.camera.near_plane = value;
        error(f.extract(), ErrorCode::InvalidArgument);
    }
    for (double value : {0.0, original.near_plane, nan, infinity}) {
        f.camera = original; f.camera.far_plane = value;
        error(f.extract(), ErrorCode::InvalidArgument);
    }
    f.camera = original; f.camera.entity = 999;
    error(f.extract(), ErrorCode::NotFound);
    f.camera = original; f.camera.aspect_ratio = std::numeric_limits<double>::denorm_min();
    error(f.extract(), ErrorCode::InvalidArgument);
    f.camera = original;
    Transform scaled; scaled.scale = {1, 2, 1};
    auto parent = f.create(scaled);
    ok(f.bus.execute(host, f.bus.revision(), SetParent{f.camera_id, parent}));
    error(f.extract(), ErrorCode::InvalidArgument);
    scaled.scale = {-1, 1, 1};
    ok(f.bus.execute(host, f.bus.revision(), SetTransform{parent, scaled}));
    error(f.extract(), ErrorCode::InvalidArgument);
}
void invalid_lights() {
    Fixture f;
    LightBinding binding{f.create(), LightKind::Point};
    const auto original = binding;
    binding.range = 0; error(f.extract({}, {&binding, 1}), ErrorCode::InvalidArgument);
    binding = original; binding.linear_color.x = -1;
    error(f.extract({}, {&binding, 1}), ErrorCode::InvalidArgument);
    binding = original; binding.intensity = std::numeric_limits<double>::quiet_NaN();
    error(f.extract({}, {&binding, 1}), ErrorCode::InvalidArgument);
    binding = original; binding.kind = static_cast<LightKind>(99);
    error(f.extract({}, {&binding, 1}), ErrorCode::InvalidArgument);
    binding = original; binding.entity = 999;
    error(f.extract({}, {&binding, 1}), ErrorCode::NotFound);
    LightBinding duplicates[] = {original, original};
    error(f.extract({}, duplicates), ErrorCode::InvalidArgument);
}
void thread_boundary() {
    Fixture f;
    bool rejected = false;
    std::thread worker([&] {
        auto result = f.extract();
        rejected = !result && result.error().code == ErrorCode::WrongThread;
    });
    worker.join();
    check(rejected, "Non-owner extraction allowed");
    ok(f.extract());
}
} // namespace
int main() {
    const std::pair<const char *, void (*)()> tests[] = {
        {"camera projection", camera_projection},
        {"hierarchy, sort and visibility", hierarchy_sort_and_visibility},
        {"snapshot lifetime and undo", snapshot_lifetime},
        {"light extraction", light_extraction},
        {"invalid meshes", invalid_meshes},
        {"invalid cameras", invalid_cameras},
        {"invalid lights", invalid_lights},
        {"owner thread boundary", thread_boundary},
    };
    int failures = 0;
    for (const auto &[name, run] : tests) {
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception &e) {
            ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    }
    return failures ? 1 : 0;
}
