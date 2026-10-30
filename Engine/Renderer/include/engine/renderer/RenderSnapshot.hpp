#pragma once
#include <engine/scene/Scene.hpp>
#include <memory>
#include <span>

namespace engine::renderer {
// Zero is invalid. The eventual asset owner checks generation/liveness and retains
// resources until submitted frames finish; extraction only checks handle shape.
struct MeshHandle {
    std::uint32_t index = 0, generation = 0;
    bool operator==(const MeshHandle &) const = default;
};
struct MaterialHandle {
    std::uint32_t index = 0, generation = 0;
    bool operator==(const MaterialHandle &) const = default;
};
struct TextureHandle {
    std::uint32_t index = 0, generation = 0;
    bool operator==(const TextureHandle &) const = default;
};
// Built-in resources every renderer provides. Unit size, centered at the origin; the plane lies in XZ facing +Y.
inline constexpr MeshHandle builtin_cube{1, 1}, builtin_sphere{2, 1}, builtin_plane{3, 1};
inline constexpr MaterialHandle builtin_lit{1, 1}; // Lit PBR material driven by Surface.
// Per-object parameters of the material. Color is linear, components in [0, 1]. For a model with its own materials
// the color tints each part, and the part's roughness and metallic win. A texture replaces every base color image.
struct Surface {
    Vec3 linear_color{1, 1, 1};
    double roughness = 0.65, metallic = 0;
    TextureHandle texture; // Zero: none.
    bool operator==(const Surface &) const = default;
};
struct MeshBinding {
    EntityId entity = 0;
    MeshHandle mesh;
    MaterialHandle material;
    bool visible = true;
    Surface surface;
};
struct PerspectiveCamera {
    EntityId entity = 0;
    double vertical_fov_radians = 1.0471975511965976;
    double aspect_ratio = 1;
    double near_plane = 0.1, far_plane = 1000;
};
enum class LightKind { Directional, Point };
struct LightBinding {
    EntityId entity = 0;
    LightKind kind = LightKind::Directional;
    Vec3 linear_color{1, 1, 1};
    double intensity = 1; // Directional: lux; point: lumens.
    double range = 10;    // Point influence radius in meters; ignored for directional.
};
struct RenderObject {
    EntityId entity;
    MeshHandle mesh;
    MaterialHandle material;
    WorldTransform world;
    Surface surface;
};
struct RenderLight {
    EntityId entity;
    LightKind kind;
    Vec3 linear_color;
    double intensity, range;
    Vec3 position, direction; // Direction is local -Z transformed and normalized.
};
struct RenderCamera {
    PerspectiveCamera lens;
    WorldTransform world, view, projection;
};
struct FrameStamp {
    std::uint64_t scene = 0; // Host-issued nonzero session identity, never a Flecs ID.
    std::uint64_t revision = 0, frame = 0;
};
struct FrameSnapshot {
    FrameStamp stamp;
    RenderCamera camera;
    std::vector<RenderObject> objects; // Ascending stable entity IDs.
    std::vector<RenderLight> lights;
};
using Frame = std::shared_ptr<const FrameSnapshot>;

// Call at a quiescent boundary on the Scene owner thread. The trusted host supplies
// bindings and stamp for this scene. No live world/resource pointers escape.
// All bindings, including hidden objects, must be valid. Failure publishes no frame.
// Matrices: column-major, right-handed, camera -Z forward, clip depth [-1,+1].
// Backend adapters convert projection/depth conventions as necessary.
Result<Frame> extract_frame(const Scene &, FrameStamp, const PerspectiveCamera &,
                            std::span<const MeshBinding>, std::span<const LightBinding>);
// Editor navigation is host state, not a managed scene entity or undo operation.
// The same rigid-camera validation applies. lens.entity is zero for this overload.
Result<Frame> extract_frame(const Scene &, FrameStamp, const PerspectiveCamera &,
                            const WorldTransform &camera_world,
                            std::span<const MeshBinding>, std::span<const LightBinding>);
} // namespace engine::renderer
