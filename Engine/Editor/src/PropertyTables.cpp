#include <engine/editor/PropertyMeta.hpp>
#include <cstdio>
#include <sstream>
#include <utility>

namespace engine::editor {
namespace {
// Reads and writes one member directly.
template<class T, class M> void member(Property<T> &p, M T::*field) {
    p.get = [field](const T &owner) { return PropertyValue(owner.*field); };
    p.set = [field](T &owner, const PropertyValue &value) { owner.*field = std::get<M>(value); };
}
// An enum as a zero-based index into its choices. `first` is the enumerator of index 0.
template<class T, class E> void member_enum(Property<T> &p, E T::*field, int first, std::vector<std::string_view> choices) {
    p.type = PropertyType::Enum;
    p.choices = std::move(choices);
    p.get = [field, first](const T &owner) { return PropertyValue(int(owner.*field) - first); };
    p.set = [field, first](T &owner, const PropertyValue &value) { owner.*field = E(std::get<int>(value) + first); };
}
struct Range { double min = 0, max = 0, step = 0.01; std::string_view format = "%.3f"; };
template<class T> Property<T> header(std::string_view id, std::string_view name, std::string_view category, std::string_view tooltip, PropertyType type, Range range = {}) {
    Property<T> p;
    p.id = id; p.display_name = name; p.category = category; p.tooltip = tooltip; p.type = type;
    p.min = range.min; p.max = range.max; p.step = range.step; p.format = range.format;
    return p;
}
template<class T, class M> Property<T> field(std::string_view id, std::string_view name, std::string_view category, std::string_view tooltip, PropertyType type, M T::*at, Range range = {}) {
    auto p = header<T>(id, name, category, tooltip, type, range);
    member(p, at);
    return p;
}
template<class T, class E> Property<T> choice(std::string_view id, std::string_view name, std::string_view category, std::string_view tooltip, E T::*at, int first, std::vector<std::string_view> choices) {
    auto p = header<T>(id, name, category, tooltip, PropertyType::Enum);
    member_enum(p, at, first, std::move(choices));
    return p;
}
template<class T> Property<T> asset(std::string_view id, std::string_view name, std::string_view category, std::string_view tooltip, AssetRef T::*at, std::string_view type) {
    auto p = field<T>(id, name, category, tooltip, PropertyType::Asset, at);
    p.asset_type = type;
    return p;
}
// A mesh renderer's assets: choosing one also whitens the tint, so the asset shows as authored.
Property<MeshComponent> mesh_asset(std::string_view id, std::string_view name, std::string_view category, std::string_view tooltip, AssetRef MeshComponent::*at, std::string_view type) {
    auto p = asset<MeshComponent>(id, name, category, tooltip, at, type);
    p.set = [at](MeshComponent &owner, const PropertyValue &value) {
        const auto &ref = std::get<AssetRef>(value);
        if (!ref.empty() && owner.*at != ref) owner.color = {1, 1, 1};
        owner.*at = ref;
    };
    return p;
}
template<class T> Property<T> when(Property<T> p, std::function<bool(const T &)> condition, bool hides = true) {
    p.condition = std::move(condition);
    p.condition_hides = hides;
    return p;
}
PropertyTable<Transform> make_transform() {
    using T = Transform;
    return {"Transform", {
        field<T>("position", "Position", "Transform", "Position relative to the parent, in meters.", PropertyType::Vector, &T::position, {0, 0, 0.03}),
        field<T>("rotation", "Rotation", "Transform", "Rotation relative to the parent, as XYZ Euler degrees.", PropertyType::Rotation, &T::rotation, {0, 0, 0.5}),
        field<T>("scale", "Scale", "Transform", "Scale relative to the parent.", PropertyType::Vector, &T::scale, {0, 0, 0.01}),
    }};
}
PropertyTable<MeshComponent> make_mesh() {
    using T = MeshComponent;
    const auto builtin = [](const T &m) { return m.mesh_asset.empty(); };
    const auto no_material = [](const T &m) { return m.material.empty(); };
    return {"Mesh Renderer", {
        mesh_asset("mesh_asset", "Model", "Mesh", "An imported model. Empty draws the built-in mesh.", &T::mesh_asset, "mesh"),
        when(choice<T>("mesh", "Mesh", "Mesh", "The built-in shape.", &T::mesh, 1, {"Cube", "Sphere", "Plane"}), std::function<bool(const T &)>(builtin)),
        mesh_asset("material", "Material", "Surface", "A material asset for the whole mesh. Empty uses the model's own, or the values below.", &T::material, "material"),
        mesh_asset("texture", "Texture", "Surface", "Replaces every base color image.", &T::texture, "texture"),
        field<T>("color", "Color", "Surface", "Tints the material.", PropertyType::Color, &T::color),
        when(field<T>("roughness", "Roughness", "Surface", "0 is a mirror, 1 is fully rough. Models and materials carry their own.", PropertyType::Number, &T::roughness, {0, 1, 0.01}), std::function<bool(const T &)>(no_material), false),
        when(field<T>("metallic", "Metallic", "Surface", "0 is a dielectric, 1 a metal. Models and materials carry their own.", PropertyType::Number, &T::metallic, {0, 1, 0.01}), std::function<bool(const T &)>(no_material), false),
        field<T>("uv_scale_u", "UV scale U", "Surface", "Texture tiling across, multiplied with the material's.", PropertyType::Number, &T::uv_scale_u, {0.001, 1000, 0.01}),
        field<T>("uv_scale_v", "UV scale V", "Surface", "Texture tiling down, multiplied with the material's.", PropertyType::Number, &T::uv_scale_v, {0.001, 1000, 0.01}),
    }};
}
PropertyTable<CameraComponent> make_camera() {
    using T = CameraComponent;
    return {"Camera", {
        field<T>("vertical_fov_degrees", "Field of view", "Lens", "Vertical angle of view.", PropertyType::Number, &T::vertical_fov_degrees, {1, 179, 0.2, "%.1f deg"}),
        field<T>("near_plane", "Near", "Lens", "Nothing closer than this is drawn, in meters.", PropertyType::Number, &T::near_plane, {0.001, 1e6, 0.01}),
        field<T>("far_plane", "Far", "Lens", "Nothing farther than this is drawn, in meters.", PropertyType::Number, &T::far_plane, {0.001, 1e6, 1, "%.1f"}),
        field<T>("primary", "Primary", "Lens", "Play mode looks through the first primary camera, by entity ID.", PropertyType::Bool, &T::primary),
    }};
}
PropertyTable<LightComponent> make_light() {
    using T = LightComponent;
    const std::function<bool(const T &)> local = [](const T &l) { return l.type != LightType::Directional; };
    const std::function<bool(const T &)> spot = [](const T &l) { return l.type == LightType::Spot; };
    return {"Light", {
        choice<T>("type", "Type", "Light", "Directional and spot lights shine along local -Z.", &T::type, 1, {"Directional", "Point", "Spot"}),
        field<T>("color", "Light color", "Light", "The light's color.", PropertyType::Color, &T::color),
        field<T>("intensity", "Intensity", "Light", "Directional lights in lux; point and spot lights in lumens.", PropertyType::Number, &T::intensity, {0, 1e7, 500, "%.0f"}),
        when(field<T>("range", "Range (m)", "Light", "How far a point or spot light reaches.", PropertyType::Number, &T::range, {0.01, 1000, 0.05}), local),
        when(field<T>("inner_cone_degrees", "Inner cone (deg)", "Cone", "Half angle of a spot light's full-strength part.", PropertyType::Number, &T::inner_cone_degrees, {0, 90, 0.2, "%.1f"}), spot),
        when(field<T>("outer_cone_degrees", "Outer cone (deg)", "Cone", "Half angle where a spot light fades out.", PropertyType::Number, &T::outer_cone_degrees, {0, 90, 0.2, "%.1f"}), spot),
        field<T>("cast_shadows", "Cast shadows", "Light", "Whether this light casts shadows.", PropertyType::Bool, &T::cast_shadows),
    }};
}
PropertyTable<RigidBodyComponent> make_body() {
    using T = RigidBodyComponent;
    return {"Rigid Body", {
        choice<T>("kind", "Body type", "Body", "Static never moves, kinematic follows the scene, dynamic is simulated.", &T::kind, 1, {"Static", "Kinematic", "Dynamic"}),
        field<T>("linear_velocity", "Initial velocity", "Motion", "World-space velocity when Play starts, in m/s.", PropertyType::Vector, &T::linear_velocity, {0, 0, 0.05}),
        field<T>("angular_velocity", "Initial spin", "Motion", "World-space spin when Play starts, in rad/s.", PropertyType::Vector, &T::angular_velocity, {0, 0, 0.05}),
        field<T>("linear_damping", "Linear damping", "Motion", "Slows movement over time.", PropertyType::Number, &T::linear_damping, {0, 100, 0.01}),
        field<T>("angular_damping", "Angular damping", "Motion", "Slows spin over time.", PropertyType::Number, &T::angular_damping, {0, 100, 0.01}),
        field<T>("gravity_scale", "Gravity scale", "Motion", "Multiplies gravity for this body.", PropertyType::Number, &T::gravity_scale, {-100, 100, 0.01}),
        field<T>("enable_sleep", "Can sleep", "Body", "Lets the body rest when it stops moving.", PropertyType::Bool, &T::enable_sleep),
        field<T>("is_bullet", "Continuous collision", "Body", "Stops a fast body passing through thin ones, at some cost.", PropertyType::Bool, &T::is_bullet),
        field<T>("lock_rotation", "Lock rotation", "Body", "Keeps a dynamic body upright, as a character's capsule must be.", PropertyType::Bool, &T::lock_rotation),
    }};
}
PropertyTable<ColliderComponent> make_collider() {
    using T = ColliderComponent;
    const std::function<bool(const T &)> box = [](const T &k) { return k.shape == ColliderShape::Box; };
    const std::function<bool(const T &)> round = [](const T &k) { return k.shape != ColliderShape::Box; };
    const std::function<bool(const T &)> capsule = [](const T &k) { return k.shape == ColliderShape::Capsule; };
    const std::function<bool(const T &)> turnable = [](const T &k) { return k.shape != ColliderShape::Sphere; };
    auto category = field<T>("category", "Category bits", "Filter", "Which groups this collider belongs to.", PropertyType::Bits, &T::category);
    auto mask = field<T>("mask", "Mask bits", "Filter", "Which groups this collider touches.", PropertyType::Bits, &T::mask);
    category.hidden = mask.hidden = true; // No panel shows them yet; scenes and the shooter sample set them.
    return {"Collider", {
        choice<T>("shape", "Shape", "Shape", "The collision shape.", &T::shape, 1, {"Sphere", "Box", "Capsule"}),
        field<T>("center", "Center", "Shape", "Offset in the entity's unscaled local frame.", PropertyType::Vector, &T::center, {0, 0, 0.01}),
        when(field<T>("rotation", "Local rotation", "Shape", "Rotation in the entity's local frame.", PropertyType::Rotation, &T::rotation, {0, 0, 0.5}), turnable),
        when(field<T>("half_extents", "Half extents", "Shape", "Half the box's size on each axis.", PropertyType::Vector, &T::half_extents, {0.001, 10000, 0.01}), box),
        when(field<T>("radius", "Radius", "Shape", "The sphere's or capsule's radius.", PropertyType::Number, &T::radius, {0.001, 10000, 0.01}), round),
        when(field<T>("half_height", "Half height", "Shape", "Half the capsule's segment, along local Y.", PropertyType::Number, &T::half_height, {0, 10000, 0.01}), capsule),
        field<T>("density", "Density", "Material", "Mass per volume, in kg/m3.", PropertyType::Number, &T::density, {0, 100000, 10, "%.0f kg/m3"}),
        field<T>("friction", "Friction", "Material", "How much it resists sliding.", PropertyType::Number, &T::friction, {0, 10, 0.01}),
        field<T>("restitution", "Restitution", "Material", "How much it bounces, from 0 to 1.", PropertyType::Number, &T::restitution, {0, 1, 0.01}),
        field<T>("sensor", "Sensor", "Events", "Detects overlaps without colliding.", PropertyType::Bool, &T::sensor),
        field<T>("events", "Report events", "Events", "Reports contact, hit and sensor events for this collider.", PropertyType::Bool, &T::events),
        std::move(category), std::move(mask),
    }};
}
PropertyTable<EnvironmentComponent> make_environment() {
    using T = EnvironmentComponent;
    const std::function<bool(const T &)> image = [](const T &e) { return e.source == EnvironmentSource::Image; };
    const std::function<bool(const T &)> procedural = [](const T &e) { return e.source == EnvironmentSource::Procedural; };
    return {"Environment", {
        choice<T>("source", "Source", "Environment", "What lights the scene from all around.", &T::source, 0, {"None", "Procedural sky", "HDR image"}),
        when(asset<T>("image", "Image", "Environment", "An equirectangular .hdr image.", &T::image, "environment"), image),
        when(field<T>("sky_color", "Sky", "Environment", "The procedural sky above.", PropertyType::Color, &T::sky_color), procedural),
        when(field<T>("horizon_color", "Horizon", "Environment", "The procedural sky at the horizon.", PropertyType::Color, &T::horizon_color), procedural),
        when(field<T>("ground_color", "Ground", "Environment", "The procedural sky below.", PropertyType::Color, &T::ground_color), procedural),
        field<T>("intensity", "Ambient intensity", "Environment", "Image-based lighting strength, in lux for a white sky.", PropertyType::Number, &T::intensity, {0, 200000, 100, "%.0f"}),
        field<T>("rotation_degrees", "Rotation (deg)", "Environment", "Turns the environment around +Y.", PropertyType::Number, &T::rotation_degrees, {-360, 360, 1, "%.0f"}),
        field<T>("show_skybox", "Show skybox", "Environment", "Draws the environment behind the scene.", PropertyType::Bool, &T::show_skybox),
        field<T>("ev100", "Exposure (EV100)", "Camera", "Sunny 16 is about 15; a lit room about 6.", PropertyType::Number, &T::ev100, {-6, 20, 0.05, "%.2f"}),
        choice<T>("tone_mapping", "Tone mapping", "Camera", "How bright values map to the screen.", &T::tone_mapping, 1, {"ACES", "Filmic", "AgX", "PBR Neutral", "Linear"}),
        field<T>("ambient_occlusion", "Ambient occlusion", "Camera", "Darkens creases and contact points.", PropertyType::Bool, &T::ambient_occlusion),
        field<T>("bloom", "Bloom", "Camera", "Bright areas glow.", PropertyType::Bool, &T::bloom),
        choice<T>("shadow_quality", "Quality", "Shadows", "Shadow map size; Off disables them.", &T::shadow_quality, 0, {"Off", "Low", "Medium", "High"}),
        field<T>("shadow_distance", "Distance (m)", "Shadows", "How far from the camera directional shadows reach.", PropertyType::Number, &T::shadow_distance, {1, 1000, 1, "%.0f"}),
    }};
}
PropertyTable<ParticleEmitterComponent> make_emitter() {
    using T = ParticleEmitterComponent;
    return {"Particle Emitter", {
        asset<T>("effect", "Effect", "Emitter", "A particle effect (.effect). Edit it in the Effect Editor.", &T::effect, "effect"),
        field<T>("auto_play", "Play by itself", "Emitter", "Starts on its own in Play and in the editor's preview. Off: only gameplay fires it.", PropertyType::Bool, &T::auto_play),
        field<T>("loop", "Loop", "Emitter", "Starts again each time the effect's duration ends.", PropertyType::Bool, &T::loop),
        field<T>("rate_scale", "Rate scale", "Emitter", "Multiplies the effect's rate and bursts.", PropertyType::Number, &T::rate_scale, {0, 1000, 0.01}),
    }};
}
}
std::string property_to_text(const PropertyValue &value) {
    std::ostringstream out;
    out.precision(17);
    std::visit([&](const auto &v) {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, bool>) out << "bool " << (v ? 1 : 0);
        else if constexpr (std::is_same_v<V, double>) out << "number " << v;
        else if constexpr (std::is_same_v<V, Vec3>) out << "vector " << v.x << ' ' << v.y << ' ' << v.z;
        else if constexpr (std::is_same_v<V, Quaternion>) out << "rotation " << v.x << ' ' << v.y << ' ' << v.z << ' ' << v.w;
        else if constexpr (std::is_same_v<V, int>) out << "choice " << v;
        else if constexpr (std::is_same_v<V, AssetRef>) out << "asset " << v.id << ' ' << v.path;
        else out << "bits " << v;
    }, value);
    return out.str();
}
std::optional<PropertyValue> property_from_text(std::string_view text) {
    std::istringstream in{std::string(text)};
    std::string kind;
    in >> kind;
    if (kind == "bool") { int v = 0; if (in >> v) return PropertyValue(v != 0); }
    else if (kind == "number") { double v = 0; if (in >> v) return PropertyValue(v); }
    else if (kind == "vector") { Vec3 v; if (in >> v.x >> v.y >> v.z) return PropertyValue(v); }
    else if (kind == "rotation") { Quaternion v; if (in >> v.x >> v.y >> v.z >> v.w) return PropertyValue(v); }
    else if (kind == "choice") { int v = 0; if (in >> v) return PropertyValue(v); }
    else if (kind == "asset") {
        AssetRef v;
        if (in >> v.id) { std::getline(in >> std::ws, v.path); return PropertyValue(v); }
        return std::nullopt;
    } else if (kind == "bits") { std::uint64_t v = 0; if (in >> v) return PropertyValue(v); }
    return std::nullopt;
}
const PropertyTable<Transform> &transform_properties() { static const auto table = make_transform(); return table; }
const PropertyTable<MeshComponent> &mesh_properties() { static const auto table = make_mesh(); return table; }
const PropertyTable<CameraComponent> &camera_properties() { static const auto table = make_camera(); return table; }
const PropertyTable<LightComponent> &light_properties() { static const auto table = make_light(); return table; }
const PropertyTable<RigidBodyComponent> &body_properties() { static const auto table = make_body(); return table; }
const PropertyTable<ColliderComponent> &collider_properties() { static const auto table = make_collider(); return table; }
const PropertyTable<EnvironmentComponent> &environment_properties() { static const auto table = make_environment(); return table; }
const PropertyTable<ParticleEmitterComponent> &emitter_properties() { static const auto table = make_emitter(); return table; }
} // namespace engine::editor
