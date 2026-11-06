#pragma once

// Plain values that cross between scripts and the engine, with no Lua in sight. Scenes store script properties as
// these, the editor shows them, and the runtime turns them into Lua values and back.

#include <engine/core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace engine::scripting {

using EntityId = std::uint64_t;

struct Vector3 {
	double x = 0, y = 0, z = 0;
	friend bool operator==(const Vector3&, const Vector3&) = default;
};
struct Color {
	double r = 1, g = 1, b = 1, a = 1; // sRGB in [0, 1]; alpha is linear.
	friend bool operator==(const Color&, const Color&) = default;
};
// An entity of the scene by its stable ID; 0 is none.
struct EntityRef {
	EntityId id = 0;
	friend bool operator==(const EntityRef&, const EntityRef&) = default;
};
// An asset by its stable ID and the package name it was given by; both empty is none.
struct AssetRef {
	std::uint64_t id = 0;
	std::string name;
	friend bool operator==(const AssetRef&, const AssetRef&) = default;
};

enum class ValueType : std::uint8_t { Number, Boolean, Text, Vector, Color, Entity, Asset };
using Value = std::variant<double, bool, std::string, Vector3, Color, EntityRef, AssetRef>;

[[nodiscard]] ValueType type_of(const Value& value) noexcept;
[[nodiscard]] const char* to_string(ValueType type) noexcept;

// A property a script declares: its name and default. The type is the default's.
struct PropertyDecl {
	std::string name; // A Lua name: a letter or '_', then letters, digits or '_'; 1 to 64 bytes.
	Value default_value;
	friend bool operator==(const PropertyDecl&, const PropertyDecl&) = default;
};
inline constexpr std::size_t max_properties = 64;
inline constexpr std::size_t max_text_property_bytes = 4096;

// The functions the engine calls on a script's instances, in the order a tick calls them.
inline constexpr const char* callback_names[] = {"start", "tick", "late_tick", "on_hit", "on_trigger_enter",
	"on_trigger_exit", "on_damage", "on_death", "on_clip_event", "on_stop"};

// What a script declares.
struct ScriptInfo {
	std::string name;                     // As the loader named it: a package name such as "/Game/Scripts/Gate".
	std::vector<PropertyDecl> properties; // In the order the script's text first names them.
	std::vector<std::string> callbacks;   // Those of callback_names it defines, in that list's order.
	[[nodiscard]] bool defines(std::string_view callback) const;
	[[nodiscard]] const PropertyDecl* property(std::string_view wanted) const;
};

// Something a script got wrong, with where.
struct ScriptError {
	std::string script;    // The script's name; empty when it is not known.
	int line = 0;          // 0 when it is not known.
	std::string message;   // Without the "name:line:" Lua puts in front.
	std::string traceback; // Lua's stack traceback, when the error happened while running.
	std::string callback;  // The function the engine called ("tick"); empty while loading.
	EntityId entity = 0;   // The instance's entity; 0 while loading.
	// "Gate:12: attempt to index a nil value" or, without a line, the message alone.
	[[nodiscard]] std::string located() const;
};

// A line a script printed, with where it printed it.
struct ScriptOutput {
	std::string script;
	int line = 0;
	std::string text;
};

// A script's text and the name to show for it.
struct ScriptSource {
	std::string name; // The canonical name: two names for one script load it once.
	std::string text;
};
inline constexpr std::size_t max_script_bytes = 1u << 20;

// Finds a script by the name a scene or `require` gives. NotFound when there is none.
using ScriptLoader = std::function<Result<ScriptSource>(std::string_view name)>;

} // namespace engine::scripting
