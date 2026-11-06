#include "engine/scripting/script_runtime.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace engine::scripting {

ValueType type_of(const Value& value) noexcept { return static_cast<ValueType>(value.index()); }

const char* to_string(ValueType type) noexcept
{
	switch (type) {
	case ValueType::Number: return "number";
	case ValueType::Boolean: return "boolean";
	case ValueType::Text: return "text";
	case ValueType::Vector: return "Vec3";
	case ValueType::Color: return "Color";
	case ValueType::Entity: return "Entity";
	case ValueType::Asset: return "Asset";
	}
	return "unknown";
}

bool ScriptInfo::defines(std::string_view callback) const
{
	return std::find(callbacks.begin(), callbacks.end(), callback) != callbacks.end();
}

const PropertyDecl* ScriptInfo::property(std::string_view wanted) const
{
	for (const auto& decl : properties)
		if (decl.name == wanted) return &decl;
	return nullptr;
}

std::string ScriptError::located() const
{
	if (script.empty()) return message;
	if (line > 0) return script + ":" + std::to_string(line) + ": " + message;
	return script + ": " + message;
}

namespace {

constexpr int hook_interval = 1000; // Instructions between two looks at the budget.

bool is_identifier_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool is_lua_keyword(std::string_view name)
{
	static constexpr std::array<std::string_view, 22> keywords{"and", "break", "do", "else", "elseif", "end", "false",
		"for", "function", "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until",
		"while"};
	return std::find(keywords.begin(), keywords.end(), name) != keywords.end();
}

bool is_property_name(std::string_view name)
{
	if (name.empty() || name.size() > 64) return false;
	if (std::isdigit(static_cast<unsigned char>(name[0]))) return false;
	return std::all_of(name.begin(), name.end(), is_identifier_char) && !is_lua_keyword(name);
}

// Counts what the state holds and refuses to grow past the limit. Lua then collects garbage and tries again before it
// raises "not enough memory". Shrinking never fails, as Lua requires.
struct Allocator {
	std::size_t used = 0;
	std::size_t limit = 0;
};

void* limited_alloc(void* data, void* block, std::size_t old_size, std::size_t new_size)
{
	auto& allocator = *static_cast<Allocator*>(data);
	const std::size_t old_bytes = block ? old_size : 0; // Without a block, old_size is the kind of object.
	if (new_size == 0) {
		std::free(block);
		allocator.used -= old_bytes;
		return nullptr;
	}
	if (new_size > old_bytes && allocator.used - old_bytes + new_size > allocator.limit) return nullptr;
	void* grown = std::realloc(block, new_size);
	if (!grown) return new_size <= old_bytes ? block : nullptr;
	allocator.used = allocator.used - old_bytes + new_size;
	return grown;
}

std::string format_number(double value)
{
	char text[32];
	std::snprintf(text, sizeof text, "%.14g", value);
	return text;
}

// The sandbox's own versions of what it keeps from the base library. Runs once, before any script.
// A call over its budget must end even when the script catches the error, so everything that catches errors passes
// that one on: pcall, xpcall, coroutine.resume and close, and load (which catches its reader's errors).
constexpr const char* sandbox_setup = R"lua(
local raw_load, raw_collect, raw_seed = load, collectgarbage, math.randomseed
local raw_pcall, raw_xpcall, raw_resume, raw_close = pcall, xpcall, coroutine.resume, coroutine.close
local over_budget, type, error, tostring = __over_budget, type, error, tostring
__over_budget = nil
dofile, loadfile, string.dump, warn = nil, nil, nil, nil
local function pass_on(ok, ...)
	if not ok and over_budget() then error((...), 0) end
	return ok, ...
end
pcall = function(...) return pass_on(raw_pcall(...)) end
xpcall = function(...) return pass_on(raw_xpcall(...)) end
coroutine.resume = function(...) return pass_on(raw_resume(...)) end
coroutine.close = function(...) return pass_on(raw_close(...)) end
load = function(chunk, name, mode, env)
	if type(chunk) ~= "string" and type(chunk) ~= "function" then
		error("bad argument #1 to 'load' (string or function expected)", 2)
	end
	local loaded, why = raw_load(chunk, name, "t", env)
	if loaded == nil and over_budget() then error(why, 0) end
	return loaded, why
end
collectgarbage = function(option, ...)
	option = option or "collect"
	if option == "count" or option == "collect" or option == "step" then return raw_collect(option, ...) end
	error("collectgarbage(\"" .. tostring(option) .. "\") is not allowed in scripts", 2)
end
math.randomseed = function(x, y)
	if type(x) ~= "number" then error("math.randomseed needs a number: scripts cannot seed from the clock", 2) end
	return raw_seed(x, y)
end
)lua";

} // namespace

struct ScriptRuntime::Impl {
	struct LoadedScript {
		ScriptInfo info;
		sol::table cls;           // What the script returned.
		sol::table instance_meta; // {__index = cls}, shared by its instances.
	};
	struct Instance {
		std::string script;
		sol::table self;
		bool enabled = true;
	};
	// Where the last error happened, as the message handler saw the stack.
	struct ErrorPlace {
		std::string script;
		int line = 0;
		std::string traceback;
	};

	ScriptLoader loader;
	RuntimeLimits limits;
	Allocator allocator; // Before the state, which it outlives.
	sol::state lua;
	// After the state: these hold references into it, so they go first.
	std::map<std::string, LoadedScript> scripts;
	std::map<std::string, sol::object> modules;
	std::vector<std::string> requiring;
	std::map<EntityId, Instance> instances;
	std::vector<std::string> chunk_names; // Every script and module ever compiled, for reading error messages.
	std::vector<ScriptError> errors;
	std::vector<ScriptOutput> output;
	std::uint64_t instructions = 0;
	ErrorPlace last_place;

	Impl(ScriptLoader script_loader, RuntimeLimits runtime_limits, std::uint64_t seed)
		: loader(std::move(script_loader))
		, limits(runtime_limits)
		, allocator{0, runtime_limits.memory_bytes}
		, lua(sol::default_at_panic, &limited_alloc, &allocator)
	{
		lua_State* L = lua.lua_state();
		*static_cast<Impl**>(lua_getextraspace(L)) = this; // Copied into every coroutine the state makes.
		lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::math, sol::lib::string, sol::lib::table,
			sol::lib::utf8);
		bind_values();
		lua_pushcfunction(L, &is_over_budget);
		lua_setglobal(L, "__over_budget"); // Taken into the sandbox's own functions, then removed.
		const auto setup = lua.safe_script(sandbox_setup, sol::script_pass_on_error, "=sandbox");
		if (!setup.valid()) throw std::runtime_error("the script sandbox could not be set up");
		const sol::protected_function seed_random = lua["math"]["randomseed"];
		const auto seeded = seed_random(static_cast<lua_Integer>(seed & 0x7fff'ffff'ffff'ffffu));
		if (!seeded.valid()) throw std::runtime_error("the script random generator could not be seeded");
		lua.set_function("print", [this](sol::variadic_args args, sol::this_state state) { print(state, args); });
		lua.set_function("require", [this](const std::string& name) { return require(name); });
		lua_sethook(L, &count_instructions, LUA_MASKCOUNT, hook_interval);
	}

	static Impl& of(lua_State* L) { return **static_cast<Impl**>(lua_getextraspace(L)); }

	// ---- Values -------------------------------------------------------------------------------------------------

	// Vec3 and Color are values: their fields cannot be assigned, so sharing one never changes another's.
	void bind_values()
	{
		lua.new_usertype<Vector3>("Vec3",
			sol::call_constructor,
			sol::factories([] { return Vector3{}; }, [](double x, double y, double z) { return Vector3{x, y, z}; }),
			"x", sol::readonly(&Vector3::x), "y", sol::readonly(&Vector3::y), "z", sol::readonly(&Vector3::z),
			"length", [](const Vector3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); },
			"normalized",
			[](const Vector3& v) {
				const double length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
				return length > 0 ? Vector3{v.x / length, v.y / length, v.z / length} : Vector3{};
			},
			"dot", [](const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; },
			"cross",
			[](const Vector3& a, const Vector3& b) {
				return Vector3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
			},
			sol::meta_function::addition, [](const Vector3& a, const Vector3& b) { return Vector3{a.x + b.x, a.y + b.y, a.z + b.z}; },
			sol::meta_function::subtraction, [](const Vector3& a, const Vector3& b) { return Vector3{a.x - b.x, a.y - b.y, a.z - b.z}; },
			sol::meta_function::multiplication,
			sol::overload([](const Vector3& v, double s) { return Vector3{v.x * s, v.y * s, v.z * s}; },
				[](double s, const Vector3& v) { return Vector3{v.x * s, v.y * s, v.z * s}; }),
			sol::meta_function::division, [](const Vector3& v, double s) { return Vector3{v.x / s, v.y / s, v.z / s}; },
			sol::meta_function::unary_minus, [](const Vector3& v) { return Vector3{-v.x, -v.y, -v.z}; },
			sol::meta_function::equal_to, [](const Vector3& a, const Vector3& b) { return a == b; },
			sol::meta_function::to_string,
			[](const Vector3& v) { return "Vec3(" + format_number(v.x) + ", " + format_number(v.y) + ", " + format_number(v.z) + ")"; });
		lua.new_usertype<Color>("Color",
			sol::call_constructor,
			sol::factories([] { return Color{}; }, [](double r, double g, double b) { return Color{r, g, b, 1}; },
				[](double r, double g, double b, double a) { return Color{r, g, b, a}; }),
			"r", sol::readonly(&Color::r), "g", sol::readonly(&Color::g), "b", sol::readonly(&Color::b),
			"a", sol::readonly(&Color::a),
			sol::meta_function::equal_to, [](const Color& a, const Color& b) { return a == b; },
			sol::meta_function::to_string,
			[](const Color& c) {
				return "Color(" + format_number(c.r) + ", " + format_number(c.g) + ", " + format_number(c.b) + ", " + format_number(c.a) + ")";
			});
		lua.new_usertype<EntityRef>("Entity",
			sol::call_constructor, sol::factories([] { return EntityRef{}; }),
			"id", sol::property([](const EntityRef& e) { return static_cast<lua_Integer>(e.id); }),
			sol::meta_function::equal_to, [](const EntityRef& a, const EntityRef& b) { return a == b; },
			sol::meta_function::to_string,
			[](const EntityRef& e) { return e.id ? "Entity(" + std::to_string(e.id) + ")" : std::string("Entity(none)"); });
		lua.new_usertype<AssetRef>("Asset",
			sol::call_constructor,
			sol::factories([] { return AssetRef{}; }, [](const std::string& name) { return AssetRef{0, name}; }),
			"name", sol::property([](const AssetRef& a) { return a.name; }),
			sol::meta_function::equal_to, [](const AssetRef& a, const AssetRef& b) { return a == b; },
			sol::meta_function::to_string,
			[](const AssetRef& a) { return a.name.empty() ? std::string("Asset(none)") : "Asset(" + a.name + ")"; });
	}

	// Whole numbers go in as Lua integers, so a property of 3 prints as 3, not 3.0.
	sol::object make_object(const Value& value)
	{
		return std::visit(
			[this](const auto& v) -> sol::object {
				using T = std::decay_t<decltype(v)>;
				if constexpr (std::is_same_v<T, double>) {
					if (std::floor(v) == v && std::fabs(v) < 9007199254740992.0)
						return sol::make_object(lua, static_cast<lua_Integer>(v));
					return sol::make_object(lua, v);
				} else {
					return sol::make_object(lua, v);
				}
			},
			value);
	}

	static std::optional<Value> to_value(const sol::object& object)
	{
		switch (object.get_type()) {
		case sol::type::number: return Value{object.as<double>()};
		case sol::type::boolean: return Value{object.as<bool>()};
		case sol::type::string: return Value{object.as<std::string>()};
		case sol::type::userdata:
			if (object.is<Vector3>()) return Value{object.as<Vector3>()};
			if (object.is<Color>()) return Value{object.as<Color>()};
			if (object.is<EntityRef>()) return Value{object.as<EntityRef>()};
			if (object.is<AssetRef>()) return Value{object.as<AssetRef>()};
			return std::nullopt;
		default: return std::nullopt;
		}
	}

	// ---- Running ------------------------------------------------------------------------------------------------

	static void count_instructions(lua_State* L, lua_Debug*)
	{
		Impl& self = of(L);
		self.instructions += hook_interval;
		// Once over, every later look raises again, so a script that catches the error still stops.
		if (self.instructions > self.limits.instructions_per_call)
			luaL_error(L, "took too long: more than %I instructions in one call",
				static_cast<lua_Integer>(self.limits.instructions_per_call));
	}

	static int is_over_budget(lua_State* L)
	{
		const Impl& self = of(L);
		lua_pushboolean(L, self.instructions > self.limits.instructions_per_call);
		return 1;
	}

	// Records where the error happened, before the stack unwinds. Runs no Lua: a __tostring could raise again.
	static int message_handler(lua_State* L)
	{
		Impl& self = of(L);
		self.last_place = {};
		lua_Debug frame{};
		for (int level = 1; lua_getstack(L, level, &frame); ++level) {
			if (!lua_getinfo(L, "Sl", &frame) || frame.currentline <= 0) continue;
			std::string source = frame.source ? frame.source : "";
			if (!source.empty() && (source[0] == '@' || source[0] == '=')) source.erase(0, 1);
			self.last_place.script = std::move(source);
			self.last_place.line = frame.currentline;
			break;
		}
		luaL_traceback(L, L, nullptr, 1);
		self.last_place.traceback = lua_tostring(L, -1);
		lua_pop(L, 1);
		if (!lua_isstring(L, 1)) lua_pushfstring(L, "(the error is a %s value, not a message)", luaL_typename(L, 1));
		else lua_pushvalue(L, 1);
		return 1;
	}

	// Calls what is on the stack: a function and `nargs` arguments. The budget starts again.
	int protected_call(int nargs, int nresults)
	{
		lua_State* L = lua.lua_state();
		const int base = lua_gettop(L) - nargs;
		lua_pushcfunction(L, &message_handler);
		lua_insert(L, base);
		instructions = 0;
		last_place = {};
		const int status = lua_pcall(L, nargs, nresults, base);
		lua_remove(L, base);
		return status;
	}

	// "/Game/Scripts/Gate:12: boom" -> the script, 12 and "boom", when the front names a script compiled here. Lua
	// shortens long names to "..." and their end.
	bool split_location(std::string& message, std::string& script, int& line) const
	{
		const auto colon = message.find(": ");
		if (colon == std::string::npos) return false;
		const auto line_start = message.rfind(':', colon - 1);
		if (line_start == std::string::npos || line_start + 1 >= colon) return false;
		const std::string where = message.substr(0, line_start);
		const std::string digits = message.substr(line_start + 1, colon - line_start - 1);
		if (!std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
			return false;
		for (const auto& name : chunk_names) {
			const bool same = where == name;
			const bool shortened = where.starts_with("...") && name.ends_with(where.substr(3));
			if (!same && !shortened) continue;
			script = name;
			line = std::atoi(digits.c_str());
			message.erase(0, colon + 2);
			return true;
		}
		return false;
	}

	ScriptError error_from_stack(int status, std::string_view fallback_script)
	{
		lua_State* L = lua.lua_state();
		ScriptError error;
		if (status == LUA_ERRMEM) {
			error.message = "ran out of memory: the scripts of a game may use at most "
				+ std::to_string(limits.memory_bytes >> 20) + " MB";
		} else {
			error.message = lua_isstring(L, -1) ? lua_tostring(L, -1) : "unknown error";
		}
		lua_pop(L, 1);
		if (!split_location(error.message, error.script, error.line)) {
			error.script = last_place.script;
			error.line = last_place.line;
		}
		if (error.script.empty()) error.script = fallback_script;
		error.traceback = last_place.traceback;
		return error;
	}

	void remember_chunk(const std::string& name)
	{
		if (std::find(chunk_names.begin(), chunk_names.end(), name) == chunk_names.end()) chunk_names.push_back(name);
	}

	// Compiles a script's text with its own globals, which fall back to the shared libraries, and leaves the chunk on
	// the stack. On a syntax error, gives the error and leaves nothing.
	std::optional<ScriptError> compile(const ScriptSource& source)
	{
		lua_State* L = lua.lua_state();
		remember_chunk(source.name);
		if (source.text.size() > max_script_bytes) {
			ScriptError error;
			error.script = source.name;
			error.message = "is larger than " + std::to_string(max_script_bytes >> 20) + " MB";
			return error;
		}
		const std::string chunk = "@" + source.name;
		const int status = luaL_loadbufferx(L, source.text.data(), source.text.size(), chunk.c_str(), "t");
		if (status != LUA_OK) {
			last_place = {};
			return error_from_stack(status, source.name);
		}
		sol::table globals = lua.create_table();
		sol::table fallback = lua.create_table();
		fallback["__index"] = lua.globals();
		globals[sol::metatable_key] = fallback;
		globals.push(L);
		lua_setupvalue(L, -2, 1); // The chunk's _ENV.
		return std::nullopt;
	}

	void print(sol::this_state state, const sol::variadic_args& args)
	{
		lua_State* L = state;
		ScriptOutput line;
		lua_Debug frame{};
		if (lua_getstack(L, 1, &frame) && lua_getinfo(L, "Sl", &frame)) {
			std::string source = frame.source ? frame.source : "";
			if (!source.empty() && (source[0] == '@' || source[0] == '=')) source.erase(0, 1);
			line.script = std::move(source);
			line.line = frame.currentline;
		}
		for (std::size_t i = 0; i < args.size(); ++i) {
			if (i) line.text += '\t';
			std::size_t length = 0;
			const char* text = luaL_tolstring(L, args.stack_index() + static_cast<int>(i), &length);
			line.text.append(text, length);
			lua_pop(L, 1);
		}
		output.push_back(std::move(line));
	}

	// `require`: runs a module once and keeps what it returned. Errors become Lua errors at the require.
	sol::object require(const std::string& name)
	{
		auto source = loader(name);
		if (!source) throw std::runtime_error("require \"" + name + "\": " + source.error().message);
		const std::string canonical = source.value().name;
		if (const auto known = modules.find(canonical); known != modules.end()) return known->second;
		if (std::find(requiring.begin(), requiring.end(), canonical) != requiring.end()) {
			std::string chain;
			for (const auto& step : requiring) chain += step + " -> ";
			throw std::runtime_error("require cycle: " + chain + canonical);
		}
		if (auto failed = compile(source.value())) throw std::runtime_error(failed->located());
		requiring.push_back(canonical);
		struct Pop {
			std::vector<std::string>& stack;
			~Pop() { stack.pop_back(); }
		} pop{requiring};
		lua_State* L = lua.lua_state();
		lua_call(L, 0, 1); // Its errors pass on to the script that required it.
		sol::object result = sol::stack::pop<sol::object>(L);
		if (result.get_type() == sol::type::lua_nil) result = sol::make_object(lua, true);
		modules.emplace(canonical, result);
		return result;
	}

	// ---- Scripts ------------------------------------------------------------------------------------------------

	Result<ScriptInfo> fail_load(ScriptError error)
	{
		const std::string message = error.located();
		errors.push_back(std::move(error));
		return fail(ErrorCode::ParseError, message);
	}

	Result<ScriptInfo> load(std::string_view name)
	{
		auto source = loader(name);
		if (!source) return source.error();
		const ScriptSource& script = source.value();
		if (const auto known = scripts.find(script.name); known != scripts.end()) return known->second.info;

		lua_State* L = lua.lua_state();
		const int top = lua_gettop(L);
		if (auto failed = compile(script)) return fail_load(std::move(*failed));
		if (const int status = protected_call(0, 1); status != LUA_OK) return fail_load(error_from_stack(status, script.name));
		auto invalid = [&](std::string message) {
			lua_settop(L, top);
			ScriptError error;
			error.script = script.name;
			error.message = std::move(message);
			return fail_load(std::move(error));
		};
		if (!lua_istable(L, -1))
			return invalid(std::string("must return a table, its class; it returned ") + luaL_typename(L, -1));
		sol::table cls(L, -1);
		lua_settop(L, top);

		ScriptInfo info;
		info.name = script.name;
		const sol::object properties = cls.raw_get<sol::object>("properties");
		if (properties.get_type() != sol::type::lua_nil) {
			if (properties.get_type() != sol::type::table) return invalid("its properties must be a table of names and defaults");
			for (const auto& [key, value] : properties.as<sol::table>()) {
				if (key.get_type() != sol::type::string) return invalid("its properties must be named by text");
				const std::string property = key.as<std::string>();
				if (!is_property_name(property) || property == "entity")
					return invalid("'" + property + "' cannot be a property name: use a letter or '_', then letters, digits or '_' (not a Lua word or 'entity')");
				auto converted = to_value(value);
				if (!converted)
					return invalid("property '" + property + "' must be a number, boolean, text, Vec3, Color, Entity or Asset; it is a " + std::string(sol::type_name(L, value.get_type())));
				if (const auto* text = std::get_if<std::string>(&*converted); text && text->size() > max_text_property_bytes)
					return invalid("property '" + property + "' is longer than " + std::to_string(max_text_property_bytes) + " bytes");
				info.properties.push_back({property, std::move(*converted)});
			}
			if (info.properties.size() > max_properties)
				return invalid("has more than " + std::to_string(max_properties) + " properties");
			std::sort(info.properties.begin(), info.properties.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
			order_by_source(info.properties, script.text);
		}
		for (const char* callback : callback_names) {
			const sol::object function = cls.raw_get<sol::object>(callback);
			if (function.get_type() == sol::type::lua_nil) continue;
			if (function.get_type() != sol::type::function) return invalid(std::string("its '") + callback + "' must be a function");
			info.callbacks.emplace_back(callback);
		}
		sol::table meta = lua.create_table();
		meta["__index"] = cls;
		scripts[script.name] = LoadedScript{info, cls, meta};
		return info;
	}

	Result<void> attach(EntityId entity, std::string_view name, std::span<const PropertyOverride> overrides)
	{
		auto loaded = load(name);
		if (!loaded) return loaded.error();
		LoadedScript& script = scripts.at(loaded.value().name);
		sol::table self = lua.create_table();
		for (const auto& decl : script.info.properties) {
			const Value* value = &decl.default_value;
			for (const auto& [key, given] : overrides) {
				if (key != decl.name) continue;
				if (type_of(given) == type_of(decl.default_value)) {
					value = &given;
				} else {
					ScriptError error;
					error.script = script.info.name;
					error.entity = entity;
					error.message = "property '" + decl.name + "' is a " + to_string(type_of(decl.default_value))
						+ ", but the scene gives a " + to_string(type_of(given)) + "; the default is used";
					errors.push_back(std::move(error));
				}
			}
			self[decl.name] = make_object(*value);
		}
		self["entity"] = EntityRef{entity};
		self[sol::metatable_key] = script.instance_meta;
		instances[entity] = Instance{script.info.name, std::move(self), true};
		return {};
	}

	bool call(EntityId entity, Instance& instance, std::string_view callback, std::span<const Value> args)
	{
		if (!instance.enabled) return false;
		const LoadedScript& script = scripts.at(instance.script);
		if (!script.info.defines(callback)) return false;
		lua_State* L = lua.lua_state();
		const int top = lua_gettop(L);
		const sol::object function = script.cls.raw_get<sol::object>(callback);
		function.push(L);
		instance.self.push(L);
		for (const auto& arg : args) make_object(arg).push(L);
		const int status = protected_call(1 + static_cast<int>(args.size()), 0);
		if (status != LUA_OK) {
			ScriptError error = error_from_stack(status, instance.script);
			error.callback = std::string(callback);
			error.entity = entity;
			errors.push_back(std::move(error));
			instance.enabled = false;
		}
		lua_settop(L, top);
		return status == LUA_OK;
	}
};

ScriptRuntime::ScriptRuntime(ScriptLoader loader, RuntimeLimits limits, std::uint64_t random_seed)
	: impl_(std::make_unique<Impl>(std::move(loader), limits, random_seed))
{
}

ScriptRuntime::~ScriptRuntime() = default;

Result<ScriptInfo> ScriptRuntime::load(std::string_view name) { return impl_->load(name); }

Result<void> ScriptRuntime::attach(EntityId entity, std::string_view script, std::span<const PropertyOverride> overrides)
{
	return impl_->attach(entity, script, overrides);
}

void ScriptRuntime::detach(EntityId entity) { impl_->instances.erase(entity); }

bool ScriptRuntime::has_instance(EntityId entity) const { return impl_->instances.contains(entity); }

bool ScriptRuntime::enabled(EntityId entity) const
{
	const auto found = impl_->instances.find(entity);
	return found != impl_->instances.end() && found->second.enabled;
}

std::vector<EntityId> ScriptRuntime::instances() const
{
	std::vector<EntityId> ids;
	ids.reserve(impl_->instances.size());
	for (const auto& [id, instance] : impl_->instances) ids.push_back(id);
	return ids;
}

std::size_t ScriptRuntime::call(std::string_view callback, std::span<const Value> args)
{
	std::size_t succeeded = 0;
	for (auto& [entity, instance] : impl_->instances)
		if (impl_->call(entity, instance, callback, args)) ++succeeded;
	return succeeded;
}

bool ScriptRuntime::call(EntityId entity, std::string_view callback, std::span<const Value> args)
{
	const auto found = impl_->instances.find(entity);
	return found != impl_->instances.end() && impl_->call(entity, found->second, callback, args);
}

std::optional<Value> ScriptRuntime::field(EntityId entity, std::string_view name) const
{
	const auto found = impl_->instances.find(entity);
	if (found == impl_->instances.end()) return std::nullopt;
	return Impl::to_value(found->second.self.raw_get<sol::object>(name));
}

std::vector<ScriptError> ScriptRuntime::take_errors() { return std::exchange(impl_->errors, {}); }

std::vector<ScriptOutput> ScriptRuntime::take_output() { return std::exchange(impl_->output, {}); }

std::size_t ScriptRuntime::memory_used() const noexcept { return impl_->allocator.used; }

const RuntimeLimits& ScriptRuntime::limits() const noexcept { return impl_->limits; }

Result<ScriptInfo> inspect_script(std::string_view name, const ScriptLoader& loader)
{
	ScriptRuntime runtime(loader, RuntimeLimits{std::size_t{32} << 20, 1'000'000});
	return runtime.load(name);
}

void order_by_source(std::vector<PropertyDecl>& properties, std::string_view text)
{
	auto first_assignment = [&](std::string_view name) {
		for (std::size_t at = text.find(name); at != std::string_view::npos; at = text.find(name, at + 1)) {
			if (at > 0 && (is_identifier_char(text[at - 1]) || text[at - 1] == '.')) continue;
			std::size_t next = at + name.size();
			if (next < text.size() && is_identifier_char(text[next])) continue;
			while (next < text.size() && (text[next] == ' ' || text[next] == '\t')) ++next;
			if (next < text.size() && text[next] == '=' && (next + 1 >= text.size() || text[next + 1] != '=')) return at;
		}
		return std::string_view::npos;
	};
	std::vector<std::pair<std::size_t, PropertyDecl>> placed;
	placed.reserve(properties.size());
	for (auto& decl : properties) placed.emplace_back(first_assignment(decl.name), std::move(decl));
	std::stable_sort(placed.begin(), placed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	properties.clear();
	for (auto& [at, decl] : placed) properties.push_back(std::move(decl));
}

} // namespace engine::scripting
