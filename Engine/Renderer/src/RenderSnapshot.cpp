#include <engine/renderer/RenderSnapshot.hpp>
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <numbers>
#include <set>

namespace engine::renderer {
namespace {
bool finite(const WorldTransform &m) {
    return std::all_of(m.matrix.begin(), m.matrix.end(), [](double v) { return std::isfinite(v); });
}
WorldTransform matrix(const glm::dmat4 &m) {
    WorldTransform out;
    std::copy_n(glm::value_ptr(m), 16, out.matrix.begin());
    return out;
}
bool rigid(const glm::dmat4 &m) {
    const glm::dvec3 x(m[0]), y(m[1]), z(m[2]);
    constexpr double tolerance = 1e-6;
    return std::abs(glm::dot(x, x) - 1) < tolerance &&
           std::abs(glm::dot(y, y) - 1) < tolerance &&
           std::abs(glm::dot(z, z) - 1) < tolerance &&
           std::abs(glm::dot(x, y)) < tolerance &&
           std::abs(glm::dot(x, z)) < tolerance &&
           std::abs(glm::dot(y, z)) < tolerance &&
           std::abs(glm::dot(glm::cross(x, y), z) - 1) < tolerance;
}
bool nonnegative(double value) {
    return std::isfinite(value) && value >= 0;
}
} // namespace
Result<Frame> extract_frame(const Scene &scene, FrameStamp stamp, const PerspectiveCamera &camera,
                            std::span<const MeshBinding> meshes,
                            std::span<const LightBinding> lights) {
    if (!scene.on_owner_thread())
        return fail(ErrorCode::WrongThread, "Frame extraction requires the scene owner thread");
    if (!stamp.scene || meshes.size() > Scene::max_entities || lights.size() > Scene::max_entities)
        return fail(ErrorCode::InvalidArgument, "Invalid scene identity or binding count");
    if (!std::isfinite(camera.vertical_fov_radians) || camera.vertical_fov_radians <= 0 ||
        camera.vertical_fov_radians >= std::numbers::pi ||
        !std::isfinite(camera.aspect_ratio) || camera.aspect_ratio <= 0 ||
        !std::isfinite(camera.near_plane) || !std::isfinite(camera.far_plane) ||
        camera.near_plane <= 0 || camera.far_plane <= camera.near_plane)
        return fail(ErrorCode::InvalidArgument, "Invalid perspective camera lens");
    const auto camera_world = scene.world_transform(camera.entity);
    if (!camera_world)
        return fail(ErrorCode::NotFound, "Camera entity does not exist");
    const auto world = glm::make_mat4(camera_world->matrix.data());
    if (!finite(*camera_world) || !rigid(world))
        return fail(ErrorCode::InvalidArgument, "Camera world transform must be rigid and unreflected");
    const auto view = matrix(glm::inverse(world));
    const auto projection = matrix(glm::perspectiveRH_NO(camera.vertical_fov_radians,
        camera.aspect_ratio, camera.near_plane, camera.far_plane));
    if (!finite(view) || !finite(projection) || projection.matrix[0] <= 0 ||
        projection.matrix[5] <= 0 || projection.matrix[14] >= 0)
        return fail(ErrorCode::InvalidArgument, "Camera matrix is not numerically representable");
    FrameSnapshot frame{stamp, {camera, *camera_world, view, projection}, {}, {}};
    frame.objects.reserve(meshes.size());
    frame.lights.reserve(lights.size());
    std::set<EntityId> seen;
    for (const auto &binding : meshes) {
        if (!seen.insert(binding.entity).second || !binding.mesh.index || !binding.mesh.generation ||
            !binding.material.index || !binding.material.generation)
            return fail(ErrorCode::InvalidArgument, "Duplicate mesh entity or invalid asset handle");
        const auto transform = scene.world_transform(binding.entity);
        if (!transform)
            return fail(ErrorCode::NotFound, "Mesh entity does not exist");
        if (!finite(*transform))
            return fail(ErrorCode::InvalidArgument, "Non-finite mesh world transform");
        if (binding.visible)
            frame.objects.push_back({binding.entity, binding.mesh, binding.material, *transform});
    }
    seen.clear();
    for (const auto &binding : lights) {
        if (!seen.insert(binding.entity).second ||
            (binding.kind != LightKind::Point && binding.kind != LightKind::Directional) ||
            !nonnegative(binding.linear_color.x) || !nonnegative(binding.linear_color.y) ||
            !nonnegative(binding.linear_color.z) || !nonnegative(binding.intensity) ||
            (binding.kind == LightKind::Point && (!std::isfinite(binding.range) || binding.range <= 0)))
            return fail(ErrorCode::InvalidArgument, "Duplicate light entity or invalid light parameters");
        const auto transform = scene.world_transform(binding.entity);
        if (!transform)
            return fail(ErrorCode::NotFound, "Light entity does not exist");
        if (!finite(*transform))
            return fail(ErrorCode::InvalidArgument, "Non-finite light world transform");
        const auto &m = transform->matrix;
        const double length = std::hypot(m[8], m[9], m[10]);
        if (!std::isfinite(length) || length <= 0)
            return fail(ErrorCode::InvalidArgument, "Light direction is not numerically representable");
        frame.lights.push_back({binding.entity, binding.kind, binding.linear_color,
            binding.intensity, binding.kind == LightKind::Point ? binding.range : 0,
            {m[12], m[13], m[14]}, {-m[8] / length, -m[9] / length, -m[10] / length}});
    }
    std::sort(frame.objects.begin(), frame.objects.end(),
              [](const auto &a, const auto &b) { return a.entity < b.entity; });
    std::sort(frame.lights.begin(), frame.lights.end(),
              [](const auto &a, const auto &b) { return a.entity < b.entity; });
    return Frame(std::make_shared<const FrameSnapshot>(std::move(frame)));
}
} // namespace engine::renderer
