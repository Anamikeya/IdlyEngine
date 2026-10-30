#pragma once
#include <array>
#include <cstdint>
#include <engine/core/Result.hpp>
#include <engine/ecs/World.hpp>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <vector>
namespace engine::commands {
class CommandBus;
}
namespace engine {
using EntityId = std::uint64_t;
struct Vec3 {
    double x = 0, y = 0, z = 0;
    bool operator==(const Vec3 &) const = default;
};
struct Quaternion {
    double x = 0, y = 0, z = 0, w = 1;
    bool operator==(const Quaternion &) const = default;
};
struct Transform {
    Vec3 position;
    Quaternion rotation;
    Vec3 scale{1, 1, 1};
    bool operator==(const Transform &) const = default;
};
struct StableId {
    EntityId value;
};
struct Name {
    std::string value;
};
struct Parent {
    EntityId value = 0;
};
struct WorldTransform {
    std::array<double, 16> matrix{};
};
// ---- Authoring components. Each is optional per entity and part of the entity record, so commands,
// undo/redo, play-mode copies and scene files carry them like the transform. Colors are sRGB in [0, 1]
// as picked in the editor; hosts convert to linear for rendering.
enum class BuiltinMesh : std::uint8_t { Cube = 1, Sphere = 2, Plane = 3 }; // Unit size, centered at the origin.
struct MeshComponent {
    BuiltinMesh mesh = BuiltinMesh::Cube;
    Vec3 color{0.53, 0.78, 0.96};
    double roughness = 0.65, metallic = 0;
    bool operator==(const MeshComponent &) const = default;
};
// Looks down the entity's local -Z axis. The entity's world transform must be rigid (no scale) to render.
struct CameraComponent {
    double vertical_fov_degrees = 60, near_plane = 0.1, far_plane = 1000;
    bool primary = true; // Play mode renders through the first primary camera, by entity ID.
    bool operator==(const CameraComponent &) const = default;
};
enum class LightType : std::uint8_t { Directional = 1, Point = 2 };
// Directional lights shine along the entity's local -Z axis; point lights emit from its position.
struct LightComponent {
    LightType type = LightType::Directional;
    Vec3 color{1, 1, 1};
    double intensity = 100000; // Directional: lux. Point: lumens.
    double range = 10;         // Point light falloff radius in meters.
    bool operator==(const LightComponent &) const = default;
};
// Physics body simulated in Play mode. An entity with colliders but no body is a static collider.
enum class BodyKind : std::uint8_t { Static = 1, Kinematic = 2, Dynamic = 3 };
struct RigidBodyComponent {
    BodyKind kind = BodyKind::Dynamic;
    Vec3 linear_velocity, angular_velocity; // Initial world-space velocities (m/s, rad/s) when Play starts.
    double linear_damping = 0, angular_damping = 0, gravity_scale = 1;
    bool enable_sleep = true, is_bullet = false;
    bool operator==(const RigidBodyComponent &) const = default;
};
// Collision shape placed in the entity's unscaled local frame; the physics bridge applies world scale.
// Box uses half_extents; sphere uses radius; capsule uses radius and half_height (segment along local Y).
enum class ColliderShape : std::uint8_t { Sphere = 1, Box = 2, Capsule = 3 };
struct ColliderComponent {
    ColliderShape shape = ColliderShape::Box;
    Vec3 center;
    Quaternion rotation;
    Vec3 half_extents{0.5, 0.5, 0.5};
    double radius = 0.5, half_height = 0.5;
    double density = 1000, friction = 0.6, restitution = 0; // kg/m^3; restitution in [0, 1].
    bool sensor = false; // Detects overlaps without colliding.
    bool events = false; // Reports contact and sensor events for this collider.
    bool operator==(const ColliderComponent &) const = default;
};
inline constexpr std::size_t max_colliders_per_entity = 64;
struct Components {
    std::optional<MeshComponent> mesh;
    std::optional<CameraComponent> camera;
    std::optional<LightComponent> light;
    std::optional<RigidBodyComponent> body;
    std::vector<ColliderComponent> colliders;
    bool operator==(const Components &) const = default;
};
struct EntityRecord {
    EntityId id = 0;
    std::string name;
    EntityId parent = 0;
    Transform transform;
    Components components;
    bool operator==(const EntityRecord &) const = default;
};
struct SceneSnapshot {
    EntityId next_id = 1;
    std::vector<EntityRecord> entities;
    bool operator==(const SceneSnapshot &) const = default;
};
class Scene final {
    ecs::World world_;
    std::map<EntityId, flecs::entity_t> entities_;
    EntityId next_id_ = 1;
    friend class commands::CommandBus;
    Result<void> commit(const SceneSnapshot &);

  public:
    static constexpr std::size_t max_entities = 100000;
    Scene();
    Scene(const Scene &) = delete;
    Scene &operator=(const Scene &) = delete;
    bool on_owner_thread() const noexcept {
        return world_.on_owner_thread();
    }
    std::size_t size() const noexcept {
        return entities_.size();
    }
    std::optional<EntityRecord> inspect(EntityId) const;
    SceneSnapshot snapshot() const;
    std::optional<WorldTransform> world_transform(EntityId) const;
    ecs::World &ecs() noexcept {
        return world_;
    } // Trusted engine systems only.
    static Result<void> validate(const SceneSnapshot &);
};
// Text scene format, docs/architecture/006-scene-format.md. Writes the current version; reads 1..current.
inline constexpr unsigned scene_format_version = 4;
Result<void> write_scene(std::ostream &, const SceneSnapshot &);
Result<SceneSnapshot> read_scene(std::istream &);
} // namespace engine
