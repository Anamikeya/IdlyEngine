#pragma once
#include <array>
#include <cstdint>
#include <engine/core/Result.hpp>
#include <engine/ecs/World.hpp>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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
// A reference to an imported asset: its stable ID, and its path in the asset folder for when the ID is gone
// (a deleted .meta). Both empty, or both set. The scene never loads anything; hosts resolve references.
struct AssetRef {
    std::uint64_t id = 0;
    std::string path;
    bool empty() const noexcept { return id == 0; }
    bool operator==(const AssetRef &) const = default;
};
inline constexpr std::size_t max_asset_path_bytes = 1024;
// A material asset for one part of a model, overriding `MeshComponent::material` and the part's own material.
struct MaterialSlot {
    std::uint32_t slot = 0; // Index of the model's part; 0 for a built-in mesh.
    AssetRef material;
    bool operator==(const MaterialSlot &) const = default;
};
inline constexpr std::size_t max_material_slots = 64;
struct MeshComponent {
    BuiltinMesh mesh = BuiltinMesh::Cube; // Ignored while mesh_asset is set.
    Vec3 color{0.53, 0.78, 0.96}; // Tints the material.
    double roughness = 0.65, metallic = 0; // Models carry their own; these apply to built-in meshes and OBJ files.
    AssetRef mesh_asset; // Empty: draw the built-in mesh.
    AssetRef texture;    // Empty: the model's own base color image, if any.
    AssetRef material;   // Material asset for the whole mesh. Empty: the model's own, or the values above.
    std::vector<MaterialSlot> material_slots; // Ascending by slot, each at most once.
    double uv_scale_u = 1, uv_scale_v = 1; // Tiling, multiplied with the material's.
    bool operator==(const MeshComponent &) const = default;
};
// Looks down the entity's local -Z axis. The entity's world transform must be rigid (no scale) to render.
struct CameraComponent {
    double vertical_fov_degrees = 60, near_plane = 0.1, far_plane = 1000;
    bool primary = true; // Play mode renders through the first primary camera, by entity ID.
    bool operator==(const CameraComponent &) const = default;
};
enum class LightType : std::uint8_t { Directional = 1, Point = 2, Spot = 3 };
// Directional and spot lights shine along the entity's local -Z axis; point and spot lights emit from its position.
struct LightComponent {
    LightType type = LightType::Directional;
    Vec3 color{1, 1, 1};
    double intensity = 100000; // Directional: lux. Point and spot: lumens.
    double range = 10;         // Point and spot falloff radius in meters.
    double inner_cone_degrees = 25, outer_cone_degrees = 35; // Spot half angles; 0 <= inner <= outer <= 90.
    bool cast_shadows = false;
    bool operator==(const LightComponent &) const = default;
};
// How a scene is lit and exposed; the first one by entity ID applies. A scene without one uses the defaults below.
enum class EnvironmentSource : std::uint8_t { None = 0, Procedural = 1, Image = 2 };
enum class ToneMapping : std::uint8_t { Aces = 1, Filmic = 2, Agx = 3, PbrNeutral = 4, Linear = 5 };
enum class ShadowQuality : std::uint8_t { Off = 0, Low = 1, Medium = 2, High = 3 };
struct EnvironmentComponent {
    EnvironmentSource source = EnvironmentSource::Procedural;
    AssetRef image; // Equirectangular .hdr, for EnvironmentSource::Image.
    Vec3 sky_color{0.32, 0.5, 0.82}, horizon_color{0.78, 0.84, 0.92}, ground_color{0.3, 0.27, 0.24}; // Procedural sky.
    double intensity = 25000;  // Image-based lighting strength, in lux for a white sky.
    double rotation_degrees = 0; // Around +Y.
    bool show_skybox = true;
    double ev100 = 15; // Camera exposure. Sunny 16 is about 15; a lit room about 6.
    ToneMapping tone_mapping = ToneMapping::Aces;
    ShadowQuality shadow_quality = ShadowQuality::Medium;
    double shadow_distance = 60; // Meters from the camera that directional shadows cover.
    bool ambient_occlusion = true, bloom = true;
    bool operator==(const EnvironmentComponent &) const = default;
};
// Physics body simulated in Play mode. An entity with colliders but no body is a static collider.
enum class BodyKind : std::uint8_t { Static = 1, Kinematic = 2, Dynamic = 3 };
struct RigidBodyComponent {
    BodyKind kind = BodyKind::Dynamic;
    Vec3 linear_velocity, angular_velocity; // Initial world-space velocities (m/s, rad/s) when Play starts.
    double linear_damping = 0, angular_damping = 0, gravity_scale = 1;
    bool enable_sleep = true, is_bullet = false;
    bool lock_rotation = false; // Keeps a dynamic body upright, as a character's capsule must be.
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
    bool events = false; // Reports contact, hit and sensor events for this collider.
    // Collision filter: two colliders touch when each one's mask has a bit of the other's category. Raycasts use it too.
    std::uint64_t category = ~std::uint64_t(0), mask = ~std::uint64_t(0);
    bool operator==(const ColliderComponent &) const = default;
};
inline constexpr std::size_t max_colliders_per_entity = 64;
// Named numeric settings a runtime system reads when Play starts. The scene gives them no meaning, so a project
// pays nothing for systems it does not use. Type and keys: 1..32 of [a-z0-9_].
struct BehaviorComponent {
    std::string type;
    std::vector<std::pair<std::string, double>> values; // Each key at most once.
    bool operator==(const BehaviorComponent &) const = default;
};
inline constexpr std::size_t max_behaviors_per_entity = 16, max_behavior_values = 32;
// Plays a particle effect asset (.effect) at the entity. Particles are visual only: they never touch physics,
// gameplay or the scene, so a run plays the same with or without them.
struct ParticleEmitterComponent {
    AssetRef effect;       // Empty: nothing is emitted.
    bool auto_play = true; // Starts by itself in Play and in the editor's preview. Off: only gameplay fires it.
    bool loop = true;      // Starts again each time the effect's duration ends.
    double rate_scale = 1; // Multiplies the effect's rate and bursts; within [0, 1000].
    bool operator==(const ParticleEmitterComponent &) const = default;
};
// Plays a clip of a skinned or animated model on the entity's own model. The pose is visual and lives in the animation
// runtime, never in the scene (Milestone 5, D1); gameplay changes clips at run time without editing this.
struct AnimatorComponent {
    AssetRef clips;        // The model whose clips play. Empty: the entity's own model. Another model's clips play on
                           // the joints whose names match.
    std::string clip;      // Played from the start. Empty: the model rests until gameplay plays something.
    double speed = 1;      // Multiplies time; within [-10, 10]. Negative plays backwards.
    bool loop = true;
    double start_time = 0; // Seconds into the clip where playing starts; within [0, 3600].
    bool preview = true;   // Plays in the editor while editing. Off: it holds still (at its start when first shown).
    bool operator==(const AnimatorComponent &) const = default;
};
inline constexpr std::size_t max_clip_name_bytes = 256;
// Draws the entity, and what is under it, on a joint of its parent's skeleton: its transform is relative to that joint.
// Resolved when a frame is drawn, never written to the scene (D2). Physics and gameplay see the plain parent transform.
struct AttachToBoneComponent {
    std::string bone; // A joint name of the parent's model: 1 to 256 bytes. Unknown names leave it on the parent.
    bool operator==(const AttachToBoneComponent &) const = default;
};
// How one bone of a ragdoll is treated, over what is generated for it (D7).
enum class RagdollBody { Auto, Own, None };
struct RagdollBone {
    std::string bone;              // A joint of the entity's model: 1 to 256 bytes.
    RagdollBody body = RagdollBody::Auto; // Own: a body of its own. None: neither body nor capsule; it follows its parent.
    double radius = 0;             // Of its capsule, meters within [0, 100]. 0: sized from the skin.
    double density = 0;            // kg/m^3 within [0, 100000]. 0: the ragdoll's.
    double swing_degrees = -1;     // How far it swings from its rest direction, within [0, 180]. -1: the ragdoll's.
    double twist_degrees = -1;     // How far it turns about itself either way, within [0, 178]. -1: the ragdoll's.
    bool operator==(const RagdollBone &) const = default;
};
inline constexpr std::size_t max_ragdoll_bones = 256;
enum class RagdollTrigger { OnDeath, OnDemand, Never };
// Turns the entity's skinned model into jointed bodies that fall from its pose (Milestone 5, D7). The bodies are
// generated from the skeleton when it happens and live in the run's physics, never in the scene: a capsule per long
// enough bone, ball joints with cone and twist limits. Hands and feet ride on the bones above them.
struct RagdollComponent {
    RagdollTrigger trigger = RagdollTrigger::OnDeath; // On demand: only when gameplay or the editor asks.
    double density = 1000;       // kg/m^3 of every part, within [1, 100000]: water's, which a body is close to.
    double swing_degrees = 40;   // Within [0, 180].
    double twist_degrees = 20;   // Within [0, 178].
    double joint_friction = 0.5; // How much the joints resist turning, within [0, 10]: a fraction of each part's weight.
    std::vector<RagdollBone> bones; // Settings for single bones, each at most once.
    bool operator==(const RagdollComponent &) const = default;
};
// A script property's value as the scene stores it (Milestone 6, D6). The scene gives it no meaning beyond its type:
// number, boolean, text, vector, color, entity or asset, in that order of the variant.
struct ScriptColor {
    double r = 1, g = 1, b = 1, a = 1; // sRGB in [0, 1]; alpha is linear.
    bool operator==(const ScriptColor &) const = default;
};
struct EntityLink {
    EntityId id = 0; // 0: none. May name an entity that no longer exists; the runtime reports it when used.
    bool operator==(const EntityLink &) const = default;
};
using ScriptValue = std::variant<double, bool, std::string, Vec3, ScriptColor, EntityLink, AssetRef>;
struct ScriptProperty {
    std::string name; // A Lua name: a letter or '_', then letters, digits or '_'; 1 to 64 bytes.
    ScriptValue value;
    bool operator==(const ScriptProperty &) const = default;
};
inline constexpr std::size_t max_script_properties = 64, max_script_text_bytes = 4096;
// Runs a Lua script asset (.lua) on the entity in Play and Simulate (Milestone 6). It holds only the values that differ
// from the script's defaults, so a default changed in the script reaches every entity that kept it. Text values are
// one line: no control characters.
struct ScriptComponent {
    AssetRef script;                        // Empty: nothing runs.
    std::vector<ScriptProperty> properties; // Each name at most once.
    const ScriptProperty *find(std::string_view property) const noexcept {
        for (const auto &p : properties)
            if (p.name == property)
                return &p;
        return nullptr;
    }
    bool operator==(const ScriptComponent &) const = default;
};
struct Components {
    std::optional<MeshComponent> mesh;
    std::optional<CameraComponent> camera;
    std::optional<LightComponent> light;
    std::optional<RigidBodyComponent> body;
    std::vector<ColliderComponent> colliders;
    std::optional<EnvironmentComponent> environment;
    std::vector<BehaviorComponent> behaviors; // Each type at most once.
    std::optional<ParticleEmitterComponent> emitter;
    std::optional<AnimatorComponent> animator;
    std::optional<AttachToBoneComponent> attach;
    std::optional<RagdollComponent> ragdoll;
    std::optional<ScriptComponent> script;
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
    const ecs::World &ecs() const noexcept {
        return world_;
    }
    // The Flecs entity of a stable ID, for trusted systems attaching runtime components; 0 when there is none.
    flecs::entity_t native(EntityId id) const noexcept {
        const auto it = entities_.find(id);
        return it == entities_.end() ? 0 : it->second;
    }
    static Result<void> validate(const SceneSnapshot &);
};
// Text scene format, docs/architecture/006-scene-format.md. Writes the current version; reads 1..current.
inline constexpr unsigned scene_format_version = 8;
Result<void> write_scene(std::ostream &, const SceneSnapshot &);
Result<SceneSnapshot> read_scene(std::istream &);
} // namespace engine
