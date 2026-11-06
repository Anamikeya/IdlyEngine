#pragma once

// Script property values as a scene stores them (engine::ScriptValue, Scene.hpp) and as scripts see them
// (scripting::Value). The two variants hold the same seven types in the same order; only assets differ: a scene
// names an asset by ID and path, a script by ID and package name, so the host says how to go from one to the other.

#include <engine/scene/Scene.hpp>
#include <engine/scripting/script_runtime.h>

#include <functional>
#include <string>
#include <vector>

namespace engine::scripting {

// A scene's asset reference as a package name ("/Game/Sounds/Creak"); empty when it names nothing the host knows.
using AssetNamer = std::function<std::string(const engine::AssetRef&)>;
// A script's asset as a scene reference, by ID first and then by name; empty when it names nothing the host knows.
using AssetFinder = std::function<engine::AssetRef(const AssetRef&)>;

[[nodiscard]] Value to_script_value(const engine::ScriptValue& value, const AssetNamer& name_of);
[[nodiscard]] engine::ScriptValue to_scene_value(const Value& value, const AssetFinder& find);

// The values an entity's Script component holds, as overrides for ScriptRuntime::attach.
[[nodiscard]] std::vector<PropertyOverride> overrides_of(const engine::ScriptComponent& component, const AssetNamer& name_of);

} // namespace engine::scripting
