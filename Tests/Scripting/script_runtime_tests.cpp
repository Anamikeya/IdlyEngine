// Engine/Scripting's runtime without a scene: script classes and properties, instances, the sandbox, budgets,
// require, errors and where they happened, and determinism (Milestone 6, slice 1).
#include "engine/test/test.h"

#include <engine/assets/virtual_paths.h>
#include <engine/scripting/script_loader.h>
#include <engine/scripting/script_runtime.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::scripting;

namespace {

using Scripts = std::map<std::string, std::string, std::less<>>;

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const char* gate_script = R"lua(local Gate = {
	properties = {
		open_height = 3.0,
		speed = 1.5,
		trigger = Entity(),
		message = "The gate opens",
		locked = false,
		offset = Vec3(0, 1, 0),
		tint = Color(1, 0.5, 0),
		sound = Asset("/Game/Sounds/creak"),
	},
}

function Gate:start()
	self.started = true
end

function Gate:tick(dt)
	self.height = (self.height or 0) + self.speed * dt
end

return Gate
)lua";

double number_field(const ScriptRuntime& runtime, EntityId entity, const char* name)
{
	const auto value = runtime.field(entity, name);
	if (!value || !std::holds_alternative<double>(*value)) return -1e300;
	return std::get<double>(*value);
}

bool bool_field(const ScriptRuntime& runtime, EntityId entity, const char* name)
{
	const auto value = runtime.field(entity, name);
	return value && std::holds_alternative<bool>(*value) && std::get<bool>(*value);
}

} // namespace

ENGINE_TEST(a_script_declares_its_properties_in_its_own_order)
{
	const Scripts scripts{{"/Game/Scripts/Gate", gate_script}};
	ScriptRuntime runtime(memory_loader(scripts));
	const auto info = runtime.load("/Game/Scripts/Gate");
	ENGINE_REQUIRE(info);
	const ScriptInfo& gate = info.value();
	ENGINE_CHECK_EQ(gate.name, std::string("/Game/Scripts/Gate"));
	std::vector<std::string> names;
	for (const auto& p : gate.properties) names.push_back(p.name);
	const std::vector<std::string> expected{"open_height", "speed", "trigger", "message", "locked", "offset", "tint", "sound"};
	ENGINE_CHECK(names == expected);
	ENGINE_REQUIRE(gate.properties.size() == expected.size());
	ENGINE_CHECK(type_of(gate.properties[0].default_value) == ValueType::Number);
	ENGINE_CHECK_NEAR(std::get<double>(gate.properties[0].default_value), 3.0, 0);
	ENGINE_CHECK(type_of(gate.properties[2].default_value) == ValueType::Entity);
	ENGINE_CHECK(std::get<EntityRef>(gate.properties[2].default_value).id == 0);
	ENGINE_CHECK(std::get<std::string>(gate.properties[3].default_value) == "The gate opens");
	ENGINE_CHECK(std::get<bool>(gate.properties[4].default_value) == false);
	ENGINE_CHECK(std::get<Vector3>(gate.properties[5].default_value) == (Vector3{0, 1, 0}));
	ENGINE_CHECK(std::get<Color>(gate.properties[6].default_value) == (Color{1, 0.5, 0, 1}));
	ENGINE_CHECK(std::get<AssetRef>(gate.properties[7].default_value).name == "/Game/Sounds/creak");
	ENGINE_CHECK(gate.callbacks == (std::vector<std::string>{"start", "tick"}));
	ENGINE_CHECK(gate.defines("tick"));
	ENGINE_CHECK(!gate.defines("on_hit"));
	ENGINE_CHECK(runtime.take_errors().empty());
}

ENGINE_TEST(instances_get_defaults_overrides_and_their_entity)
{
	const Scripts scripts{{"/Game/Scripts/Gate", gate_script}};
	ScriptRuntime runtime(memory_loader(scripts));
	const std::vector<PropertyOverride> overrides{{"speed", 4.0}, {"message", std::string("Open!")}, {"locked", std::string("yes")}};
	ENGINE_REQUIRE(runtime.attach(7, "/Game/Scripts/Gate", overrides));
	ENGINE_REQUIRE(runtime.attach(3, "/Game/Scripts/Gate"));
	// The wrong type is reported, and the default kept.
	const auto errors = runtime.take_errors();
	ENGINE_REQUIRE(errors.size() == 1);
	ENGINE_CHECK(contains(errors[0].message, "'locked'"));
	ENGINE_CHECK_EQ(errors[0].entity, EntityId{7});
	ENGINE_CHECK(!bool_field(runtime, 7, "locked"));

	ENGINE_CHECK(runtime.instances() == (std::vector<EntityId>{3, 7}));
	ENGINE_CHECK_EQ(runtime.call("start"), std::size_t{2});
	const std::vector<Value> half{0.5};
	runtime.call("tick", half);
	runtime.call("tick", half);
	ENGINE_CHECK(bool_field(runtime, 7, "started"));
	ENGINE_CHECK_NEAR(number_field(runtime, 7, "height"), 4.0, 1e-12);
	ENGINE_CHECK_NEAR(number_field(runtime, 3, "height"), 1.5, 1e-12);
	const auto message = runtime.field(7, "message");
	ENGINE_REQUIRE(message.has_value());
	ENGINE_CHECK(std::get<std::string>(*message) == "Open!");
	const auto entity = runtime.field(7, "entity");
	ENGINE_REQUIRE(entity.has_value());
	ENGINE_CHECK(std::get<EntityRef>(*entity) == EntityRef{7});
	// A callback no script defines calls nothing.
	ENGINE_CHECK_EQ(runtime.call("on_hit"), std::size_t{0});
	runtime.detach(3);
	ENGINE_CHECK(!runtime.has_instance(3));
	ENGINE_CHECK(runtime.take_errors().empty());
}

ENGINE_TEST(instances_run_in_entity_order_and_print_says_where)
{
	const Scripts scripts{{"/Game/Scripts/Counter", "local C = {}\nfunction C:tick()\n\tprint(\"entity\", self.entity.id)\nend\nreturn C\n"}};
	ScriptRuntime runtime(memory_loader(scripts));
	for (EntityId id : {30, 10, 20}) ENGINE_REQUIRE(runtime.attach(id, "/Game/Scripts/Counter"));
	runtime.call("tick");
	const auto lines = runtime.take_output();
	ENGINE_REQUIRE(lines.size() == 3);
	ENGINE_CHECK_EQ(lines[0].text, std::string("entity\t10"));
	ENGINE_CHECK_EQ(lines[1].text, std::string("entity\t20"));
	ENGINE_CHECK_EQ(lines[2].text, std::string("entity\t30"));
	ENGINE_CHECK_EQ(lines[0].script, std::string("/Game/Scripts/Counter"));
	ENGINE_CHECK_EQ(lines[0].line, 3);
}

ENGINE_TEST(a_failing_call_stops_only_its_instance_and_says_where)
{
	const Scripts scripts{{"/Game/Scripts/Flaky", R"lua(local Flaky = { properties = { bad = false } }
function Flaky:tick()
	if self.bad then
		local door = nil
		return door.height
	end
	self.count = (self.count or 0) + 1
end
return Flaky
)lua"}};
	ScriptRuntime runtime(memory_loader(scripts));
	const std::vector<PropertyOverride> bad{{"bad", true}};
	ENGINE_REQUIRE(runtime.attach(1, "/Game/Scripts/Flaky", bad));
	ENGINE_REQUIRE(runtime.attach(2, "/Game/Scripts/Flaky"));
	ENGINE_CHECK_EQ(runtime.call("tick"), std::size_t{1});
	ENGINE_CHECK_EQ(runtime.call("tick"), std::size_t{1});
	const auto errors = runtime.take_errors();
	ENGINE_REQUIRE(errors.size() == 1); // Reported once: the instance is stopped.
	const ScriptError& error = errors[0];
	ENGINE_CHECK_EQ(error.script, std::string("/Game/Scripts/Flaky"));
	ENGINE_CHECK_EQ(error.line, 5);
	ENGINE_CHECK_EQ(error.callback, std::string("tick"));
	ENGINE_CHECK_EQ(error.entity, EntityId{1});
	ENGINE_CHECK(contains(error.message, "attempt to index a nil value"));
	ENGINE_CHECK(!contains(error.message, "Flaky:5:")); // The place is in its own fields.
	ENGINE_CHECK(contains(error.located(), "/Game/Scripts/Flaky:5: attempt to index"));
	ENGINE_CHECK(contains(error.traceback, "Flaky"));
	ENGINE_CHECK(!runtime.enabled(1));
	ENGINE_CHECK(runtime.enabled(2));
	ENGINE_CHECK_NEAR(number_field(runtime, 2, "count"), 2.0, 0);
	// Attaching again gives a fresh, running instance.
	ENGINE_REQUIRE(runtime.attach(1, "/Game/Scripts/Flaky"));
	ENGINE_CHECK(runtime.enabled(1));
}

ENGINE_TEST(load_errors_name_the_script_and_line)
{
	const Scripts scripts{
		{"/Game/Scripts/Broken", "local T = {}\nfunction T:tick(\nreturn T\n"},
		{"/Game/Scripts/Number", "return 5\n"},
		{"/Game/Scripts/BadName", "return { properties = { [\"two words\"] = 1 } }\n"},
		{"/Game/Scripts/BadValue", "return { properties = { speed = {} } }\n"},
		{"/Game/Scripts/Entity", "return { properties = { entity = 1 } }\n"},
		{"/Game/Scripts/BadTick", "return { tick = 5 }\n"},
		{"/Game/Scripts/Throws", "local T = {}\nerror(\"not ready\")\nreturn T\n"},
	};
	ScriptRuntime runtime(memory_loader(scripts));

	const auto broken = runtime.load("/Game/Scripts/Broken");
	ENGINE_CHECK(!broken);
	ENGINE_CHECK(broken.error().code == ErrorCode::ParseError);
	auto errors = runtime.take_errors();
	ENGINE_REQUIRE(errors.size() == 1);
	ENGINE_CHECK_EQ(errors[0].script, std::string("/Game/Scripts/Broken"));
	ENGINE_CHECK_EQ(errors[0].line, 3); // The unclosed parameter list meets 'return' on line 3.
	ENGINE_CHECK(contains(errors[0].message, "near 'return'"));

	const auto expect = [&](const char* name, const char* says) {
		const auto loaded = runtime.load(name);
		ENGINE_CHECK(!loaded);
		if (!loaded) ENGINE_CHECK(contains(loaded.error().message, says));
		const auto reported = runtime.take_errors();
		ENGINE_CHECK(reported.size() == 1);
	};
	expect("/Game/Scripts/Number", "must return a table");
	expect("/Game/Scripts/BadName", "cannot be a property name");
	expect("/Game/Scripts/BadValue", "must be a number, boolean, text");
	expect("/Game/Scripts/Entity", "'entity' cannot be a property name");
	expect("/Game/Scripts/BadTick", "'tick' must be a function");
	expect("/Game/Scripts/Throws", "not ready");
	const auto missing = runtime.load("/Game/Scripts/Nowhere");
	ENGINE_CHECK(!missing);
	ENGINE_CHECK(missing.error().code == ErrorCode::NotFound);
}

ENGINE_TEST(the_sandbox_gives_no_way_out)
{
	const Scripts scripts{{"/Game/Scripts/Escape", R"lua(local E = {}
function E:start()
	self.no_io = io == nil
	self.no_os = os == nil
	self.no_debug = debug == nil
	self.no_package = package == nil
	self.no_dofile = dofile == nil and loadfile == nil
	self.no_dump = string.dump == nil
	local chunk, why = load("\27Lua", "binary", "b")
	self.no_bytecode = chunk == nil and why:find("binary") ~= nil
	self.text_loads = load("return 2 + 3")() == 5
	self.no_stop = not pcall(collectgarbage, "stop")
	self.counts = type(collectgarbage("count")) == "number"
	self.no_clock_seed = not pcall(math.randomseed)
end
return E
)lua"}};
	ScriptRuntime runtime(memory_loader(scripts));
	ENGINE_REQUIRE(runtime.attach(1, "/Game/Scripts/Escape"));
	ENGINE_REQUIRE(runtime.call(1, "start"));
	for (const char* check : {"no_io", "no_os", "no_debug", "no_package", "no_dofile", "no_dump", "no_bytecode",
			 "text_loads", "no_stop", "counts", "no_clock_seed"}) {
		const bool passed = bool_field(runtime, 1, check);
		if (!passed) std::printf("sandbox check failed: %s\n", check);
		ENGINE_CHECK(passed);
	}
	ENGINE_CHECK(runtime.take_errors().empty());
}

ENGINE_TEST(each_script_keeps_its_own_globals)
{
	const Scripts scripts{
		{"/Game/Scripts/A", "shared_value = 1\nlocal A = {}\nfunction A:start() self.mine = shared_value end\nreturn A\n"},
		{"/Game/Scripts/B", "local B = {}\nfunction B:start() self.seen = shared_value == nil end\nreturn B\n"},
	};
	ScriptRuntime runtime(memory_loader(scripts));
	ENGINE_REQUIRE(runtime.attach(1, "/Game/Scripts/A"));
	ENGINE_REQUIRE(runtime.attach(2, "/Game/Scripts/B"));
	runtime.call("start");
	ENGINE_CHECK_NEAR(number_field(runtime, 1, "mine"), 1.0, 0);
	ENGINE_CHECK(bool_field(runtime, 2, "seen"));
}

ENGINE_TEST(require_runs_a_module_once_through_the_loader)
{
	const Scripts scripts{
		{"/Game/Scripts/Util", "print(\"util loaded\")\nlocal U = { calls = 0 }\nfunction U.add(a, b) U.calls = U.calls + 1 return a + b end\nreturn U\n"},
		{"/Game/Scripts/A", "local util = require \"/Game/Scripts/Util\"\nlocal A = {}\nfunction A:start() self.sum = util.add(2, 3) end\nreturn A\n"},
		{"/Game/Scripts/B", "local util = require \"/Game/Scripts/Util\"\nlocal B = {}\nfunction B:start() util.add(1, 1) self.calls = util.calls end\nreturn B\n"},
		{"/Game/Scripts/Missing", "local m = require \"/Game/Scripts/Nowhere\"\nreturn {}\n"},
		{"/Game/Scripts/Loop1", "require \"/Game/Scripts/Loop2\"\nreturn {}\n"},
		{"/Game/Scripts/Loop2", "require \"/Game/Scripts/Loop1\"\nreturn {}\n"},
	};
	ScriptRuntime runtime(memory_loader(scripts));
	ENGINE_REQUIRE(runtime.attach(1, "/Game/Scripts/A"));
	ENGINE_REQUIRE(runtime.attach(2, "/Game/Scripts/B"));
	runtime.call("start");
	ENGINE_CHECK_NEAR(number_field(runtime, 1, "sum"), 5.0, 0);
	ENGINE_CHECK_NEAR(number_field(runtime, 2, "calls"), 2.0, 0); // One module, shared.
	const auto lines = runtime.take_output();
	ENGINE_REQUIRE(lines.size() == 1);
	ENGINE_CHECK_EQ(lines[0].script, std::string("/Game/Scripts/Util"));

	const auto missing = runtime.load("/Game/Scripts/Missing");
	ENGINE_CHECK(!missing);
	if (!missing) ENGINE_CHECK(contains(missing.error().message, "/Game/Scripts/Nowhere"));
	const auto loop = runtime.load("/Game/Scripts/Loop1");
	ENGINE_CHECK(!loop);
	if (!loop) ENGINE_CHECK(contains(loop.error().message, "require cycle"));
	const auto errors = runtime.take_errors();
	ENGINE_REQUIRE(errors.size() == 2);
	ENGINE_CHECK_EQ(errors[0].script, std::string("/Game/Scripts/Missing"));
	ENGINE_CHECK_EQ(errors[0].line, 1);
}

ENGINE_TEST(a_runaway_script_is_stopped_even_if_it_catches_the_error)
{
	const Scripts scripts{
		{"/Game/Scripts/Spin", "local S = {}\nfunction S:tick()\n\twhile true do end\nend\nreturn S\n"},
		{"/Game/Scripts/Sly", "local S = {}\nfunction S:tick()\n\twhile true do pcall(function() while true do end end) end\nend\nreturn S\n"},
		{"/Game/Scripts/Handler", "local S = {}\nfunction S:tick()\n\twhile true do xpcall(function() while true do end end, function(e) return e end) end\nend\nreturn S\n"},
		{"/Game/Scripts/Coy", "local S = {}\nfunction S:tick()\n\twhile true do coroutine.resume(coroutine.create(function() while true do end end)) end\nend\nreturn S\n"},
		{"/Game/Scripts/Reader", "local S = {}\nfunction S:tick()\n\twhile true do load(function() while true do end end) end\nend\nreturn S\n"},
		{"/Game/Scripts/Calm", "local S = {}\nfunction S:tick() self.ticks = (self.ticks or 0) + 1 end\nreturn S\n"},
	};
	ScriptRuntime runtime(memory_loader(scripts), RuntimeLimits{std::size_t{64} << 20, 200'000});
	const char* runaways[] = {"/Game/Scripts/Spin", "/Game/Scripts/Sly", "/Game/Scripts/Handler", "/Game/Scripts/Coy", "/Game/Scripts/Reader"};
	EntityId next = 1;
	for (const char* name : runaways) ENGINE_REQUIRE(runtime.attach(next++, name));
	const EntityId calm = next;
	ENGINE_REQUIRE(runtime.attach(calm, "/Game/Scripts/Calm"));
	const auto start = std::chrono::steady_clock::now();
	ENGINE_CHECK_EQ(runtime.call("tick"), std::size_t{1});
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	ENGINE_CHECK(seconds < 2.0);
	const auto errors = runtime.take_errors();
	ENGINE_REQUIRE(errors.size() == std::size(runaways));
	for (const auto& error : errors) ENGINE_CHECK(contains(error.message, "took too long"));
	ENGINE_CHECK_EQ(errors[0].line, 3);
	for (EntityId id = 1; id < calm; ++id) ENGINE_CHECK(!runtime.enabled(id));
	runtime.call("tick");
	ENGINE_CHECK_NEAR(number_field(runtime, calm, "ticks"), 2.0, 0);
}

// The default budget, as a game gets it: an endless loop is stopped well within a second. Prints how long it took.
ENGINE_TEST(the_default_budget_stops_a_loop_quickly)
{
	const Scripts scripts{{"/Game/Scripts/Spin", "local S = {}\nfunction S:tick()\n\tlocal n = 0\n\twhile true do n = n + 1 end\nend\nreturn S\n"}};
	ScriptRuntime runtime(memory_loader(scripts));
	ENGINE_REQUIRE(runtime.attach(1, "/Game/Scripts/Spin"));
	const auto start = std::chrono::steady_clock::now();
	ENGINE_CHECK_EQ(runtime.call("tick"), std::size_t{0});
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	std::printf("%llu instructions took %.1f ms\n", static_cast<unsigned long long>(runtime.limits().instructions_per_call), ms);
	ENGINE_CHECK(ms < 1000.0);
	const auto errors = runtime.take_errors();
	ENGINE_REQUIRE(errors.size() == 1);
	ENGINE_CHECK(contains(errors[0].message, "took too long"));
}

ENGINE_TEST(scripts_cannot_use_more_memory_than_the_limit)
{
	const Scripts scripts{
		{"/Game/Scripts/Hog", "local H = {}\nfunction H:tick()\n\tlocal t = {}\n\tfor i = 1, 200 do t[i] = string.rep(\"x\", 1 << 20) .. i end\nend\nreturn H\n"},
		{"/Game/Scripts/Calm", "local S = {}\nfunction S:tick() self.ticks = (self.ticks or 0) + 1 end\nreturn S\n"},
	};
	const std::size_t limit = std::size_t{16} << 20;
	ScriptRuntime runtime(memory_loader(scripts), RuntimeLimits{limit, 10'000'000});
	ENGINE_REQUIRE(runtime.attach(1, "/Game/Scripts/Hog"));
	ENGINE_REQUIRE(runtime.attach(2, "/Game/Scripts/Calm"));
	runtime.call("tick");
	ENGINE_CHECK(runtime.memory_used() <= limit);
	const auto errors = runtime.take_errors();
	ENGINE_REQUIRE(errors.size() == 1);
	ENGINE_CHECK(contains(errors[0].message, "ran out of memory"));
	ENGINE_CHECK_EQ(errors[0].entity, EntityId{1});
	runtime.call("tick");
	ENGINE_CHECK_NEAR(number_field(runtime, 2, "ticks"), 2.0, 0);
}

ENGINE_TEST(random_numbers_repeat_with_the_seed)
{
	const Scripts scripts{{"/Game/Scripts/Dice", "local D = {}\nfunction D:start() self.a = math.random(1, 1000000) self.b = math.random() end\nreturn D\n"}};
	auto roll = [&](std::uint64_t seed) {
		ScriptRuntime runtime(memory_loader(scripts), {}, seed);
		ENGINE_REQUIRE(runtime.attach(1, "/Game/Scripts/Dice"));
		runtime.call("start");
		return std::pair{number_field(runtime, 1, "a"), number_field(runtime, 1, "b")};
	};
	const auto first = roll(42), again = roll(42), other = roll(43);
	ENGINE_CHECK(first == again);
	ENGINE_CHECK(first != other);
}

ENGINE_TEST(vectors_and_colors_are_values)
{
	const Scripts scripts{{"/Game/Scripts/Math", R"lua(local M = { properties = { offset = Vec3(1, 2, 3) } }
function M:start()
	local v = self.offset * 2 + Vec3(1, 0, 0)
	self.moved = v
	self.length = Vec3(3, 4, 0):length()
	self.text = tostring(v)
	self.same = Vec3(1, 2, 3) == self.offset
	self.frozen = not pcall(function() self.offset.x = 5 end)
	self.tint = Color(0.5, 0.25, 1)
end
return M
)lua"}};
	ScriptRuntime runtime(memory_loader(scripts));
	ENGINE_REQUIRE(runtime.attach(1, "/Game/Scripts/Math"));
	ENGINE_REQUIRE(runtime.call(1, "start"));
	const auto moved = runtime.field(1, "moved");
	ENGINE_REQUIRE(moved.has_value());
	ENGINE_CHECK(std::get<Vector3>(*moved) == (Vector3{3, 4, 6}));
	ENGINE_CHECK_NEAR(number_field(runtime, 1, "length"), 5.0, 1e-12);
	const auto text = runtime.field(1, "text");
	ENGINE_REQUIRE(text.has_value());
	ENGINE_CHECK_EQ(std::get<std::string>(*text), std::string("Vec3(3, 4, 6)"));
	ENGINE_CHECK(bool_field(runtime, 1, "same"));
	ENGINE_CHECK(bool_field(runtime, 1, "frozen"));
	const auto offset = runtime.field(1, "offset");
	ENGINE_REQUIRE(offset.has_value());
	ENGINE_CHECK(std::get<Vector3>(*offset) == (Vector3{1, 2, 3}));
	const auto tint = runtime.field(1, "tint");
	ENGINE_REQUIRE(tint.has_value());
	ENGINE_CHECK(std::get<Color>(*tint) == (Color{0.5, 0.25, 1, 1}));
}

// Milestone 6, slice 6: Vec3, Color, Quat and Entity own nothing, so their values have no finalizer. Lua walks every
// object that has one each cycle and frees it a cycle after it dies, and scripts make these by the thousand each
// tick: 500 movers paused for 36 to 51 ms. Asset holds a name, so it keeps its finalizer.
ENGINE_TEST(values_made_by_the_thousand_have_no_finalizer_and_are_freed)
{
	const Scripts scripts{{"/Game/Scripts/Churn", R"lua(local C = {}
function C:start()
	local function finalized(value) return rawget(getmetatable(value), "__gc") ~= nil end
	self.plain = not (finalized(Vec3(1, 2, 3)) or finalized(Color(1, 1, 1)) or finalized(Quat.from_euler(0, 90, 0))
		or finalized(Entity()) or finalized(Vec3(1, 0, 0) + Vec3(0, 1, 0)))
	self.asset_finalized = finalized(Asset("/Game/Sounds/Creak"))
	collectgarbage("collect")
	self.before = collectgarbage("count")
	local sum = Vec3(0, 0, 0)
	for i = 1, 50000 do
		sum = sum + Vec3(i, 0, 0) * 0.5
		local c, q, e = Color(1, 1, 1), Quat.from_euler(0, i, 0), Entity()
	end
	self.sum = sum
	collectgarbage("collect")
	self.after = collectgarbage("count")
end
return C
)lua"}};
	ScriptRuntime runtime(memory_loader(scripts));
	ENGINE_REQUIRE(runtime.attach(1, "/Game/Scripts/Churn"));
	ENGINE_REQUIRE(runtime.call(1, "start"));
	ENGINE_CHECK(runtime.take_errors().empty());
	ENGINE_CHECK(bool_field(runtime, 1, "plain"));
	ENGINE_CHECK(bool_field(runtime, 1, "asset_finalized"));
	const auto sum = runtime.field(1, "sum");
	ENGINE_REQUIRE(sum.has_value());
	ENGINE_CHECK(std::get<Vector3>(*sum) == (Vector3{625012500, 0, 0})); // Half of 1 + ... + 50,000.
	// About 20 MB of values made, and gone after one collection: less than 256 KB more than before.
	ENGINE_CHECK(number_field(runtime, 1, "after") - number_field(runtime, 1, "before") < 256);
}

ENGINE_TEST(inspecting_a_script_runs_only_its_top_level_within_a_budget)
{
	const Scripts scripts{
		{"/Game/Scripts/Gate", gate_script},
		{"/Game/Scripts/Forever", "while true do end\nreturn {}\n"},
	};
	const auto gate = inspect_script("/Game/Scripts/Gate", memory_loader(scripts));
	ENGINE_REQUIRE(gate);
	ENGINE_CHECK_EQ(gate.value().properties.size(), std::size_t{8});
	const auto forever = inspect_script("/Game/Scripts/Forever", memory_loader(scripts));
	ENGINE_CHECK(!forever);
	if (!forever) ENGINE_CHECK(contains(forever.error().message, "took too long"));
}

ENGINE_TEST(properties_follow_the_order_the_text_names_them)
{
	std::vector<PropertyDecl> properties{{"alpha", 1.0}, {"beta", 2.0}, {"gamma", 3.0}, {"unnamed", 4.0}};
	order_by_source(properties, "local T = { properties = { gamma = 1, beta == 2, self.alpha = 0, alpha = 2, beta = 3 } }");
	std::vector<std::string> names;
	for (const auto& p : properties) names.push_back(p.name);
	ENGINE_CHECK(names == (std::vector<std::string>{"gamma", "alpha", "beta", "unnamed"}));
}

ENGINE_TEST(scripts_are_found_by_package_name)
{
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / "engine_script_loader_test";
	fs::remove_all(root);
	fs::create_directories(root / "Scripts");
	{
		std::ofstream(root / "Scripts" / "Gate.lua", std::ios::binary) << gate_script;
		std::ofstream(root / "Scripts" / "Brick.png", std::ios::binary) << "not really an image";
	}
	assets::VirtualFileSystem files;
	ENGINE_REQUIRE(files.mount("/Game", root));
	std::vector<assets::AssetMetadata> catalogue(2);
	catalogue[0].path = "Scripts/Gate.lua";
	catalogue[0].id = assets::asset_id_from_path(catalogue[0].path);
	catalogue[0].type = assets::AssetType::Script;
	catalogue[1].path = "Scripts/Brick.png";
	catalogue[1].id = assets::asset_id_from_path(catalogue[1].path);
	catalogue[1].type = assets::AssetType::Texture;
	ENGINE_REQUIRE(files.index("/Game", catalogue, {}));
	ENGINE_CHECK(assets::asset_type_for("Scripts/Gate.lua") == assets::AssetType::Script);

	const ScriptLoader loader = package_loader(files);
	for (const char* name : {"/Game/Scripts/Gate", "/game/scripts/gate", "/Game/Scripts/Gate.lua"}) {
		const auto found = loader(name);
		ENGINE_REQUIRE(found);
		ENGINE_CHECK_EQ(found.value().name, std::string("/Game/Scripts/Gate"));
		ENGINE_CHECK_EQ(found.value().text, std::string(gate_script));
	}
	const auto texture = loader("/Game/Scripts/Brick");
	ENGINE_CHECK(!texture);
	if (!texture) ENGINE_CHECK(texture.error().code == ErrorCode::InvalidArgument);
	const auto missing = loader("/Game/Scripts/Door");
	ENGINE_CHECK(!missing);
	if (!missing) ENGINE_CHECK(missing.error().code == ErrorCode::NotFound);

	// A runtime on it loads the script once, whichever name it is given.
	ScriptRuntime runtime(loader);
	ENGINE_REQUIRE(runtime.attach(1, "/game/scripts/gate"));
	ENGINE_REQUIRE(runtime.attach(2, "/Game/Scripts/Gate.lua"));
	ENGINE_CHECK_EQ(runtime.call("start"), std::size_t{2});
	fs::remove_all(root);
}

ENGINE_TEST(reloading_a_script_keeps_each_instance_and_its_state)
{
	Scripts scripts{{"/Game/Scripts/Counter", "local C = { properties = { step = 1 } }\nfunction C:tick() self.count = (self.count or 0) + self.step end\nreturn C\n"}};
	ScriptRuntime runtime(memory_loader(scripts));
	const std::vector<PropertyOverride> five{{"step", 5.0}};
	ENGINE_REQUIRE(runtime.attach(1, "/Game/Scripts/Counter"));
	ENGINE_REQUIRE(runtime.attach(2, "/Game/Scripts/Counter", five));
	runtime.call("tick");
	runtime.call("tick");
	ENGINE_CHECK_NEAR(number_field(runtime, 1, "count"), 2.0, 0);
	ENGINE_CHECK_NEAR(number_field(runtime, 2, "count"), 10.0, 0);
	ENGINE_CHECK(runtime.loaded() == (std::vector<std::string>{"/Game/Scripts/Counter"}));

	// New code, and a new property: the counts go on from where they were, and each gains the new default.
	scripts["/Game/Scripts/Counter"] = "local C = { properties = { step = 1, bonus = 100 } }\nfunction C:tick() self.count = self.count + self.step * 10 + self.bonus end\nreturn C\n";
	ENGINE_REQUIRE(runtime.reload("/Game/Scripts/Counter"));
	runtime.call("tick");
	ENGINE_CHECK_NEAR(number_field(runtime, 1, "count"), 112.0, 0);
	ENGINE_CHECK_NEAR(number_field(runtime, 2, "count"), 160.0, 0);
	ENGINE_CHECK_NEAR(number_field(runtime, 2, "bonus"), 100.0, 0);
	const auto info = runtime.load("/Game/Scripts/Counter");
	ENGINE_REQUIRE(info);
	ENGINE_CHECK(info.value().property("bonus") != nullptr);

	// A broken version keeps the last good one running, and says where it is broken.
	scripts["/Game/Scripts/Counter"] = "local C = {\n\tproperties = {\n";
	const auto broken = runtime.reload("/Game/Scripts/Counter");
	ENGINE_CHECK(!broken);
	const auto errors = runtime.take_errors();
	ENGINE_REQUIRE(errors.size() == 1);
	ENGINE_CHECK_EQ(errors[0].script, std::string("/Game/Scripts/Counter"));
	ENGINE_CHECK(errors[0].line > 0);
	runtime.call("tick");
	ENGINE_CHECK_NEAR(number_field(runtime, 1, "count"), 222.0, 0);
	ENGINE_CHECK(runtime.enabled(1));
	// What is not loaded cannot be reloaded.
	scripts["/Game/Scripts/Other"] = "return {}\n";
	ENGINE_CHECK(!runtime.reload("/Game/Scripts/Other"));
}

ENGINE_TEST(reloading_a_module_runs_again_the_scripts_that_require_it)
{
	Scripts scripts{
		{"/Game/Scripts/Tuning", "return { factor = 2 }\n"},
		{"/Game/Scripts/User", "local tuning = require \"/Game/Scripts/Tuning\"\nlocal U = {}\nfunction U:tick() self.factor = tuning.factor end\nreturn U\n"},
	};
	ScriptRuntime runtime(memory_loader(scripts));
	ENGINE_REQUIRE(runtime.attach(1, "/Game/Scripts/User"));
	runtime.call("tick");
	ENGINE_CHECK_NEAR(number_field(runtime, 1, "factor"), 2.0, 0);
	ENGINE_CHECK(runtime.loaded() == (std::vector<std::string>{"/Game/Scripts/Tuning", "/Game/Scripts/User"}));
	scripts["/Game/Scripts/Tuning"] = "return { factor = 3 }\n";
	ENGINE_REQUIRE(runtime.reload("/Game/Scripts/Tuning"));
	runtime.call("tick");
	ENGINE_CHECK_NEAR(number_field(runtime, 1, "factor"), 3.0, 0);
	ENGINE_CHECK(runtime.take_errors().empty());
}
