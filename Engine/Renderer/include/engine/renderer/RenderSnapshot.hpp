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
struct EnvironmentHandle {
    std::uint32_t index = 0, generation = 0;
    bool operator==(const EnvironmentHandle &) const = default;
};
// Built-in resources every renderer provides. Unit size, centered at the origin; the plane lies in XZ facing +Y.
inline constexpr MeshHandle builtin_cube{1, 1}, builtin_sphere{2, 1}, builtin_plane{3, 1};
inline constexpr MaterialHandle builtin_lit{1, 1}; // Lit PBR material driven by Surface.
enum class AlphaMode : std::uint8_t { Opaque, Mask, Blend };
// A material asset, resolved. Colors are linear. Base color and emissive maps are read as sRGB, the others as linear.
struct MaterialDesc {
    Vec3 base_color{1, 1, 1};
    double alpha = 1, roughness = 0.65, metallic = 0;
    Vec3 emissive; // May exceed 1.
    double normal_scale = 1, occlusion_strength = 1, alpha_cutoff = 0.5;
    AlphaMode alpha_mode = AlphaMode::Opaque;
    bool double_sided = false;
    double uv_scale_u = 1, uv_scale_v = 1;
    TextureHandle base_color_map, metallic_roughness_map, normal_map, occlusion_map, emissive_map; // Zero: none.
    bool operator==(const MaterialDesc &) const = default;
};
// Per-object parameters of the material. Color is linear, components in [0, 1]. For a model with its own materials
// the color tints each part, and the part's roughness and metallic win. A texture replaces every base color image.
struct Surface {
    Vec3 linear_color{1, 1, 1};
    double roughness = 0.65, metallic = 0;
    TextureHandle texture; // Zero: none.
    double uv_scale_u = 1, uv_scale_v = 1;
    bool double_sided = false; // For surfaces without a material; models and material assets say for themselves.
    bool operator==(const Surface &) const = default;
};
// A material asset for one part of a mesh, overriding the object's material.
struct PartMaterial {
    std::uint32_t part = 0;
    MaterialHandle material;
    bool operator==(const PartMaterial &) const = default;
};
struct MeshBinding {
    EntityId entity = 0;
    MeshHandle mesh;
    MaterialHandle material; // builtin_lit: the Surface values and the mesh's own materials. Otherwise a material asset.
    bool visible = true;
    Surface surface;
    std::vector<PartMaterial> parts;
};
struct PerspectiveCamera {
    EntityId entity = 0;
    double vertical_fov_radians = 1.0471975511965976;
    double aspect_ratio = 1;
    double near_plane = 0.1, far_plane = 1000;
    double orthographic_half_height = 0; // Above zero: an orthographic view this many meters from the middle to the top.
};
enum class LightKind { Directional, Point, Spot };
struct LightBinding {
    EntityId entity = 0;
    LightKind kind = LightKind::Directional;
    Vec3 linear_color{1, 1, 1};
    double intensity = 1; // Directional: lux; point and spot: lumens.
    double range = 10;    // Point and spot influence radius in meters; ignored for directional.
    double inner_cone_radians = 0.4363, outer_cone_radians = 0.6109; // Spot half angles.
    bool cast_shadows = false;
};
struct RenderObject {
    EntityId entity;
    MeshHandle mesh;
    MaterialHandle material;
    WorldTransform world;
    Surface surface;
    std::vector<PartMaterial> parts;
};
struct RenderLight {
    EntityId entity;
    LightKind kind;
    Vec3 linear_color;
    double intensity, range;
    Vec3 position, direction; // Direction is local -Z transformed and normalized.
    double inner_cone_radians, outer_cone_radians;
    bool cast_shadows;
};
struct RenderCamera {
    PerspectiveCamera lens;
    WorldTransform world, view, projection;
};
// Image-based lighting, sky and exposure. Colors are linear; the renderer rebuilds its IBL when the source changes.
struct RenderEnvironment {
    EnvironmentSource source = EnvironmentSource::Procedural;
    EnvironmentHandle image; // For EnvironmentSource::Image.
    Vec3 sky_color{0.0843, 0.2140, 0.6376}, horizon_color{0.5711, 0.6724, 0.8228}, ground_color{0.0742, 0.0595, 0.0467};
    double intensity = 25000, rotation_radians = 0;
    bool show_skybox = true;
    double ev100 = 15;
    ToneMapping tone_mapping = ToneMapping::Aces;
    ShadowQuality shadow_quality = ShadowQuality::Medium;
    double shadow_distance = 60;
    bool ambient_occlusion = true, bloom = true;
    bool operator==(const RenderEnvironment &) const = default;
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
    RenderEnvironment environment;
    bool show_grid = true;
    double grid_spacing = 1; // Meters between grid lines.
};
using Frame = std::shared_ptr<const FrameSnapshot>;

// Call at a quiescent boundary on the Scene owner thread. The trusted host supplies
// bindings and stamp for this scene. No live world/resource pointers escape.
// All bindings, including hidden objects, must be valid. Failure publishes no frame.
// Matrices: column-major, right-handed, camera -Z forward, clip depth [-1,+1].
// Backend adapters convert projection/depth conventions as necessary.
Result<Frame> extract_frame(const Scene &, FrameStamp, const PerspectiveCamera &,
                            std::span<const MeshBinding>, std::span<const LightBinding>,
                            const RenderEnvironment & = {}, bool show_grid = true, double grid_spacing = 1);
// Editor navigation is host state, not a managed scene entity or undo operation.
// The same rigid-camera validation applies. lens.entity is zero for this overload.
Result<Frame> extract_frame(const Scene &, FrameStamp, const PerspectiveCamera &,
                            const WorldTransform &camera_world,
                            std::span<const MeshBinding>, std::span<const LightBinding>,
                            const RenderEnvironment & = {}, bool show_grid = true, double grid_spacing = 1);
} // namespace engine::renderer
