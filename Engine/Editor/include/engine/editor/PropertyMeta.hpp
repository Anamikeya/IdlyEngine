#pragma once
#include <engine/scene/Scene.hpp>
#include <functional>
#include <string_view>
#include <variant>
#include <vector>

namespace engine::editor {
// What a generic inspector needs to know about each property of a component. Kept in the editor layer: the scene stays
// plain data and runtime code pays nothing. Written by hand per component, so it stays light; see
// docs/architecture/008-editor-ux.md for how to add one.
enum class PropertyType : std::uint8_t {
    Bool,
    Number,
    Vector,   // X, Y, Z.
    Color,    // sRGB in [0, 1].
    Rotation, // A quaternion, shown as Euler degrees.
    Enum,     // An index into `choices`.
    Asset,    // An AssetRef of `asset_type`.
    Bits,     // A 64-bit mask.
};
using PropertyValue = std::variant<bool, double, Vec3, Quaternion, int, AssetRef, std::uint64_t>;
template<class T> struct Property {
    std::string_view id;           // The field, stable: favorites and copied values refer to it.
    std::string_view display_name;
    std::string_view category;
    std::string_view tooltip;
    PropertyType type = PropertyType::Number;
    std::function<PropertyValue(const T &)> get;
    std::function<void(T &, const PropertyValue &)> set;
    double min = 0, max = 0;       // No clamp when equal.
    double step = 0.01;            // Per pixel of drag.
    std::string_view format = "%.3f";
    std::vector<std::string_view> choices; // For Enum.
    std::string_view asset_type;   // For Asset: "mesh", "texture", "material" or "environment".
    bool read_only = false;
    bool hidden = false;           // Kept out of the panel, still copied and saved.
    // Whether it applies now, like Unreal's EditCondition: a predicate because many depend on an enum (a spot light's
    // cone), not only on another bool. Unset: always. False greys it out, or hides it when `condition_hides`.
    std::function<bool(const T &)> condition;
    bool condition_hides = false;
};
template<class T> struct PropertyTable {
    std::string_view name; // "Light"
    std::vector<Property<T>> properties;
    const Property<T> *find(std::string_view id) const {
        for (const auto &property : properties) if (property.id == id) return &property;
        return nullptr;
    }
};
const PropertyTable<Transform> &transform_properties();
const PropertyTable<MeshComponent> &mesh_properties();
const PropertyTable<CameraComponent> &camera_properties();
const PropertyTable<LightComponent> &light_properties();
const PropertyTable<RigidBodyComponent> &body_properties();
const PropertyTable<ColliderComponent> &collider_properties();
const PropertyTable<EnvironmentComponent> &environment_properties();
} // namespace engine::editor
