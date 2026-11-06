// Lua 5.4.9 and sol2 3.5.0 as the engine builds them: Lua compiled as C++, a fixed string hash seed, every sol2
// safety check on. These are the guarantees Engine/Scripting is built on; the sandbox and the engine API come later.
#include "engine/test/test.h"

#include <sol/sol.hpp>

#include <stdexcept>
#include <string>

namespace {

struct Vec {
	double x = 0, y = 0, z = 0;
	Vec() = default;
	Vec(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
};

// Opens only what scripts will be given: no io, os, debug or package.
void open_script_libraries(sol::state& lua)
{
	lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::utf8);
}

std::string error_of(const sol::protected_function_result& result)
{
	if (result.valid()) return {};
	const sol::error error = result;
	return error.what();
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

} // namespace

ENGINE_TEST(lua_is_the_pinned_release)
{
	ENGINE_CHECK_EQ(LUA_VERSION_NUM, 504);
	ENGINE_CHECK_EQ(std::string(LUA_RELEASE), std::string("Lua 5.4.9"));
	sol::state lua;
	open_script_libraries(lua);
	const std::string version = lua["_VERSION"];
	ENGINE_CHECK_EQ(version, std::string("Lua 5.4"));
}

ENGINE_TEST(scripts_get_only_the_libraries_opened)
{
	sol::state lua;
	open_script_libraries(lua);
	for (const char* name : {"io", "os", "debug", "package", "require"}) {
		const sol::object value = lua[name];
		ENGINE_CHECK(value.get_type() == sol::type::lua_nil);
	}
	for (const char* name : {"math", "string", "table", "utf8", "pcall"}) {
		const sol::object value = lua[name];
		ENGINE_CHECK(value.get_type() != sol::type::lua_nil);
	}
}

ENGINE_TEST(a_script_calls_into_cpp_and_back)
{
	sol::state lua;
	open_script_libraries(lua);
	lua.new_usertype<Vec>("Vec", sol::constructors<Vec(), Vec(double, double, double)>(),
		"x", &Vec::x, "y", &Vec::y, "z", &Vec::z,
		sol::meta_function::addition, [](const Vec& a, const Vec& b) { return Vec{a.x + b.x, a.y + b.y, a.z + b.z}; });
	int calls = 0;
	lua.set_function("scaled", [&calls](const Vec& v, double by) { ++calls; return Vec{v.x * by, v.y * by, v.z * by}; });

	const auto result = lua.safe_script(R"(
		local v = scaled(Vec.new(1, 2, 3), 2) + Vec.new(0.5, 0, 0)
		function length_squared(a) return a.x * a.x + a.y * a.y + a.z * a.z end
		return v
	)", sol::script_pass_on_error, "@vectors.lua");
	ENGINE_REQUIRE(result.valid());
	const Vec v = result;
	ENGINE_CHECK_NEAR(v.x, 2.5, 1e-12);
	ENGINE_CHECK_NEAR(v.y, 4.0, 1e-12);
	ENGINE_CHECK_NEAR(v.z, 6.0, 1e-12);
	ENGINE_CHECK_EQ(calls, 1);

	// C++ calls a function the script defined.
	const sol::protected_function length_squared = lua["length_squared"];
	const auto length = length_squared(Vec{1, 2, 2});
	ENGINE_REQUIRE(length.valid());
	ENGINE_CHECK_NEAR(length.get<double>(), 9.0, 1e-12);
}

ENGINE_TEST(errors_name_the_script_and_line)
{
	sol::state lua;
	open_script_libraries(lua);
	const auto runtime = lua.safe_script("local gate = nil\nreturn gate.height\n", sol::script_pass_on_error, "@gate.lua");
	ENGINE_CHECK(!runtime.valid());
	const std::string message = error_of(runtime);
	ENGINE_CHECK(contains(message, "gate.lua:2:"));
	ENGINE_CHECK(contains(message, "gate"));

	const auto syntax = lua.safe_script("function open(\n", sol::script_pass_on_error, "@broken.lua");
	ENGINE_CHECK(!syntax.valid());
	ENGINE_CHECK(contains(error_of(syntax), "broken.lua:"));

	// The state is still usable after both.
	const auto after = lua.safe_script("return 1 + 1", sol::script_pass_on_error);
	ENGINE_REQUIRE(after.valid());
	ENGINE_CHECK_EQ(after.get<int>(), 2);
}

ENGINE_TEST(an_engine_exception_becomes_a_lua_error)
{
	sol::state lua;
	open_script_libraries(lua);
	lua.set_function("find_entity", [](int id) -> int { throw std::runtime_error("no entity " + std::to_string(id)); });

	// Uncaught by the script: C++ sees it as a failed call with the exception's message.
	const auto uncaught = lua.safe_script("return find_entity(42)", sol::script_pass_on_error, "@finder.lua");
	ENGINE_CHECK(!uncaught.valid());
	ENGINE_CHECK(contains(error_of(uncaught), "no entity 42"));

	// The script can catch it with pcall, like any Lua error, and carry on.
	const auto caught = lua.safe_script(R"(
		local ok, why = pcall(find_entity, 7)
		return ok, why
	)", sol::script_pass_on_error, "@finder.lua");
	ENGINE_REQUIRE(caught.valid());
	const bool ok = caught[0];
	const std::string why = caught[1];
	ENGINE_CHECK(!ok);
	ENGINE_CHECK(contains(why, "no entity 7"));
}

namespace {
struct Guard {
	bool* destroyed;
	~Guard() { *destroyed = true; }
};

int raise_holding_a_guard(lua_State* L)
{
	Guard guard{static_cast<bool*>(lua_touserdata(L, lua_upvalueindex(1)))};
	return luaL_error(L, "raised while holding a guard");
}
} // namespace

// Lua is compiled as C++, so lua_error throws: a C++ object alive in the function that raised it is destroyed.
// (Linking this file at all also shows it: the Lua functions it calls have C++ names, not C ones.)
ENGINE_TEST(a_lua_error_unwinds_cpp_frames)
{
	sol::state lua;
	lua_State* L = lua.lua_state();
	bool destroyed = false;
	lua_pushlightuserdata(L, &destroyed);
	lua_pushcclosure(L, raise_holding_a_guard, 1);
	const int status = lua_pcall(L, 0, 0, 0);
	ENGINE_CHECK_EQ(status, LUA_ERRRUN);
	ENGINE_CHECK(destroyed);
	const std::string message = lua_tostring(L, -1);
	ENGINE_CHECK(contains(message, "raised while holding a guard"));
	lua_pop(L, 1);
	ENGINE_CHECK_EQ(lua_gettop(L), 0);
}

// Lua seeds string hashing from the time and addresses unless told not to. Two states alive at once sit at different
// addresses, so with a random seed they would visit the same table in different orders.
ENGINE_TEST(table_order_is_the_same_in_every_state)
{
	const char* order = R"(
		local t = {}
		for i = 1, 64 do t["key" .. i] = i end
		local keys = {}
		for k in pairs(t) do keys[#keys + 1] = k end
		return table.concat(keys, ",")
	)";
	sol::state first;
	sol::state second;
	open_script_libraries(first);
	open_script_libraries(second);
	const auto a = first.safe_script(order, sol::script_pass_on_error);
	const auto b = second.safe_script(order, sol::script_pass_on_error);
	ENGINE_REQUIRE(a.valid() && b.valid());
	const std::string first_order = a;
	const std::string second_order = b;
	ENGINE_CHECK_EQ(first_order, second_order);
	ENGINE_CHECK(contains(first_order, "key64"));
}
