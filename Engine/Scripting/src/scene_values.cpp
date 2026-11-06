#include "engine/scripting/scene_values.h"

#include <type_traits>
#include <variant>

namespace engine::scripting {

static_assert(std::variant_size_v<Value> == std::variant_size_v<engine::ScriptValue>, "the two value types must match");

Value to_script_value(const engine::ScriptValue& value, const AssetNamer& name_of)
{
	return std::visit(
		[&](const auto& v) -> Value {
			using T = std::decay_t<decltype(v)>;
			if constexpr (std::is_same_v<T, engine::Vec3>)
				return Vector3{v.x, v.y, v.z};
			else if constexpr (std::is_same_v<T, engine::ScriptColor>)
				return Color{v.r, v.g, v.b, v.a};
			else if constexpr (std::is_same_v<T, engine::EntityLink>)
				return EntityRef{v.id};
			else if constexpr (std::is_same_v<T, engine::AssetRef>)
				return v.empty() ? AssetRef{} : AssetRef{v.id, name_of ? name_of(v) : std::string()};
			else
				return v;
		},
		value);
}

engine::ScriptValue to_scene_value(const Value& value, const AssetFinder& find)
{
	return std::visit(
		[&](const auto& v) -> engine::ScriptValue {
			using T = std::decay_t<decltype(v)>;
			if constexpr (std::is_same_v<T, Vector3>)
				return engine::Vec3{v.x, v.y, v.z};
			else if constexpr (std::is_same_v<T, Color>)
				return engine::ScriptColor{v.r, v.g, v.b, v.a};
			else if constexpr (std::is_same_v<T, EntityRef>)
				return engine::EntityLink{v.id};
			else if constexpr (std::is_same_v<T, AssetRef>)
				return (v.id == 0 && v.name.empty()) || !find ? engine::AssetRef{} : find(v);
			else
				return v;
		},
		value);
}

std::vector<PropertyOverride> overrides_of(const engine::ScriptComponent& component, const AssetNamer& name_of)
{
	std::vector<PropertyOverride> overrides;
	overrides.reserve(component.properties.size());
	for (const auto& property : component.properties) overrides.emplace_back(property.name, to_script_value(property.value, name_of));
	return overrides;
}

} // namespace engine::scripting
