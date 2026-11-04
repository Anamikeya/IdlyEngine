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
    return std::abs(m[0][3]) < tolerance &&
           std::abs(m[1][3]) < tolerance &&
           std::abs(m[2][3]) < tolerance &&
           std::abs(m[3][3] - 1) < tolerance &&
           std::abs(glm::dot(x, x) - 1) < tolerance &&
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
bool unit(double value) {
    return nonnegative(value) && value <= 1;
}
} // namespace
Result<Frame> extract_frame(const Scene &scene, FrameStamp stamp, const PerspectiveCamera &camera,
                            std::span<const MeshBinding> meshes,
                            std::span<const LightBinding> lights,
                            const RenderEnvironment &environment, bool show_grid, double grid_spacing,
                            std::vector<RenderParticles> particles) {
    if (!scene.on_owner_thread())
        return fail(ErrorCode::WrongThread, "Frame extraction requires the scene owner thread");
    const auto camera_world = scene.world_transform(camera.entity);
    if (!camera_world)
        return fail(ErrorCode::NotFound, "Camera entity does not exist");
    return extract_frame(scene, stamp, camera, *camera_world, meshes, lights, environment, show_grid, grid_spacing,
                         std::move(particles));
}
std::vector<RenderParticles> clean_particles(std::vector<RenderParticles> particles) {
    std::stable_sort(particles.begin(), particles.end(), [](const auto &a, const auto &b) { return a.key < b.key; });
    std::vector<RenderParticles> out;
    out.reserve(particles.size());
    std::size_t total = 0;
    const auto finite3 = [](const std::array<float, 3> &v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); };
    for (auto &batch : particles) {
        if (!out.empty() && out.back().key == batch.key)
            continue;
        if ((batch.blend != ParticleBlendMode::Additive && batch.blend != ParticleBlendMode::Alpha) ||
            !batch.texture.index != !batch.texture.generation || !nonnegative(batch.strength) || !nonnegative(batch.stretch))
            continue;
        std::erase_if(batch.particles, [&](const RenderParticle &p) {
            return !finite3(p.position) || !finite3(p.velocity) || !std::isfinite(p.size) || p.size < 0 || !std::isfinite(p.rotation) ||
                   !std::all_of(p.color.begin(), p.color.end(), [](float c) { return std::isfinite(c) && c >= 0; });
        });
        if (total + batch.particles.size() > max_frame_particles)
            batch.particles.resize(max_frame_particles - total);
        total += batch.particles.size();
        out.push_back(std::move(batch));
    }
    return out;
}
Result<Frame> extract_frame(const Scene &scene, FrameStamp stamp, const PerspectiveCamera &camera,
                            const WorldTransform &camera_world,
                            std::span<const MeshBinding> meshes,
                            std::span<const LightBinding> lights,
                            const RenderEnvironment &environment, bool show_grid, double grid_spacing,
                            std::vector<RenderParticles> particles) {
    if (!scene.on_owner_thread())
        return fail(ErrorCode::WrongThread, "Frame extraction requires the scene owner thread");
    if (!stamp.scene || meshes.size() > Scene::max_entities || lights.size() > Scene::max_entities)
        return fail(ErrorCode::InvalidArgument, "Invalid scene identity or binding count");
    if (!std::isfinite(camera.vertical_fov_radians) || camera.vertical_fov_radians <= 0 ||
        camera.vertical_fov_radians >= std::numbers::pi ||
        !std::isfinite(camera.aspect_ratio) || camera.aspect_ratio <= 0 ||
        !std::isfinite(camera.near_plane) || !std::isfinite(camera.far_plane) ||
        camera.near_plane <= 0 || camera.far_plane <= camera.near_plane ||
        !std::isfinite(camera.orthographic_half_height) || camera.orthographic_half_height < 0)
        return fail(ErrorCode::InvalidArgument, "Invalid perspective camera lens");
    if (!std::isfinite(grid_spacing) || grid_spacing <= 0)
        return fail(ErrorCode::InvalidArgument, "Grid spacing must be positive");
    const auto world = glm::make_mat4(camera_world.matrix.data());
    if (!finite(camera_world) || !rigid(world))
        return fail(ErrorCode::InvalidArgument, "Camera world transform must be rigid and unreflected");
    const auto view = matrix(glm::inverse(world));
    const double half_height = camera.orthographic_half_height, half_width = half_height * camera.aspect_ratio;
    const auto projection = matrix(half_height > 0
        ? glm::orthoRH_NO(-half_width, half_width, -half_height, half_height, camera.near_plane, camera.far_plane)
        : glm::perspectiveRH_NO(camera.vertical_fov_radians, camera.aspect_ratio, camera.near_plane, camera.far_plane));
    if (!finite(view) || !finite(projection) || projection.matrix[0] <= 0 ||
        projection.matrix[5] <= 0 || projection.matrix[14] >= 0)
        return fail(ErrorCode::InvalidArgument, "Camera matrix is not numerically representable");
    if (!nonnegative(environment.intensity) || !std::isfinite(environment.rotation_radians) || !std::isfinite(environment.ev100) ||
        !(environment.shadow_distance > 0) || !std::isfinite(environment.shadow_distance))
        return fail(ErrorCode::InvalidArgument, "Invalid environment parameters");
    FrameSnapshot frame{stamp, {camera, camera_world, view, projection}, {}, {}, environment, show_grid, grid_spacing};
    frame.objects.reserve(meshes.size());
    frame.lights.reserve(lights.size());
    std::set<EntityId> seen;
    for (const auto &binding : meshes) {
        if (!seen.insert(binding.entity).second || !binding.mesh.index || !binding.mesh.generation ||
            !binding.material.index || !binding.material.generation)
            return fail(ErrorCode::InvalidArgument, "Duplicate mesh entity or invalid asset handle");
        const auto &surface = binding.surface;
        if (!unit(surface.linear_color.x) || !unit(surface.linear_color.y) ||
            !unit(surface.linear_color.z) || !unit(surface.roughness) || !unit(surface.metallic))
            return fail(ErrorCode::InvalidArgument, "Surface parameters must be within [0, 1]");
        if (!surface.texture.index != !surface.texture.generation)
            return fail(ErrorCode::InvalidArgument, "Invalid texture handle");
        if (!std::isfinite(surface.uv_scale_u) || !std::isfinite(surface.uv_scale_v) || surface.uv_scale_u <= 0 || surface.uv_scale_v <= 0)
            return fail(ErrorCode::InvalidArgument, "UV scale must be positive");
        for (const auto &part : binding.parts)
            if (!part.material.index || !part.material.generation)
                return fail(ErrorCode::InvalidArgument, "Invalid part material handle");
        if (binding.pose && (binding.pose->size() > max_pose_joints ||
                             !std::all_of(binding.pose->begin(), binding.pose->end(), [](const JointMatrix &m) {
                                 return std::all_of(m.begin(), m.end(), [](float v) { return std::isfinite(v); });
                             })))
            return fail(ErrorCode::InvalidArgument, "Joint pose is too large or not finite");
        const auto transform = scene.world_transform(binding.entity);
        if (!transform)
            return fail(ErrorCode::NotFound, "Mesh entity does not exist");
        if (!finite(*transform))
            return fail(ErrorCode::InvalidArgument, "Non-finite mesh world transform");
        if (binding.visible)
            frame.objects.push_back({binding.entity, binding.mesh, binding.material, *transform, binding.surface, binding.parts, binding.pose});
    }
    seen.clear();
    for (const auto &binding : lights) {
        if (!seen.insert(binding.entity).second ||
            (binding.kind != LightKind::Point && binding.kind != LightKind::Directional && binding.kind != LightKind::Spot) ||
            !nonnegative(binding.linear_color.x) || !nonnegative(binding.linear_color.y) ||
            !nonnegative(binding.linear_color.z) || !nonnegative(binding.intensity) ||
            (binding.kind != LightKind::Directional && (!std::isfinite(binding.range) || binding.range <= 0)) ||
            (binding.kind == LightKind::Spot && (!nonnegative(binding.inner_cone_radians) || binding.outer_cone_radians < binding.inner_cone_radians ||
                                                 !std::isfinite(binding.outer_cone_radians) || binding.outer_cone_radians > std::numbers::pi / 2 + 1e-9)))
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
            binding.intensity, binding.kind != LightKind::Directional ? binding.range : 0,
            {m[12], m[13], m[14]}, {-m[8] / length, -m[9] / length, -m[10] / length},
            binding.inner_cone_radians, binding.outer_cone_radians, binding.cast_shadows});
    }
    std::sort(frame.objects.begin(), frame.objects.end(),
              [](const auto &a, const auto &b) { return a.entity < b.entity; });
    std::sort(frame.lights.begin(), frame.lights.end(),
              [](const auto &a, const auto &b) { return a.entity < b.entity; });
    frame.particles = clean_particles(std::move(particles));
    return Frame(std::make_shared<const FrameSnapshot>(std::move(frame)));
}
} // namespace engine::renderer
