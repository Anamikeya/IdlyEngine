// Scripts in a running world (Milestone 6, slice 3): the real scene, command bus, Box3D world and game, ticked in the
// order a play session ticks them, with scripts held in memory. The engine API, events, game rules and determinism.
#include "engine/test/test.h"

#include <engine/scripting/script_loader.h>
#include <engine/scripting/script_world.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::scripting;

namespace {

constexpr std::chrono::nanoseconds fixed_step{16'666'667};
using Sources = std::map<std::string, std::string, std::less<>>;

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

Transform at(double x, double y, double z)
{
	Transform t;
	t.position = {x, y, z};
	return t;
}

ColliderComponent box(Vec3 half, bool sensor = false)
{
	ColliderComponent c;
	c.half_extents = half;
	c.events = true;
	c.sensor = sensor;
	return c;
}

RigidBodyComponent body(BodyKind kind)
{
	RigidBodyComponent b;
	b.kind = kind;
	b.enable_sleep = false;
	return b;
}

// A play session's scene with scripts, ticked as EditorSession::tick does.
struct World {
	Sources sources;
	Scene scene;
	commands::CommandBus bus{scene, 0};
	commands::Context context{"test", 3};
	std::unique_ptr<physics::ScenePhysics> physics;
	std::unique_ptr<gameplay::Game> game;
	std::unique_ptr<ScriptWorld> scripts;

	World()
	{
		auto created = physics::ScenePhysics::create();
		ENGINE_REQUIRE(static_cast<bool>(created));
		physics = std::make_unique<physics::ScenePhysics>(std::move(created.value()));
		physics->set_simulation_enabled(true);
	}

	void script(const std::string& name, std::string text) { sources["/Game/Scripts/" + name] = std::move(text); }

	// Components running a script, with values of its own.
	static Components scripted(const std::string& name, std::vector<ScriptProperty> values = {}, Components c = {})
	{
		c.script = ScriptComponent{{std::hash<std::string>{}(name) | 1, "Scripts/" + name + ".lua"}, std::move(values)};
		return c;
	}

	EntityId add(const char* name, Transform t, Components c = {}, EntityId parent = 0)
	{
		auto r = bus.execute(context, bus.revision(), commands::Command(commands::CreateEntity{name, t, parent, 0, std::move(c)}));
		ENGINE_REQUIRE(static_cast<bool>(r));
		return r.value().created.front();
	}

	EntityId floor()
	{
		Components c;
		c.colliders = {box({30, 0.5, 30})};
		return add("Floor", at(0, -0.5, 0), c);
	}

	void rules()
	{
		Components c;
		c.behaviors = {{"game_mode", {{"time_limit", 90}}}};
		add("Rules", {}, c);
	}

	void begin()
	{
		ENGINE_REQUIRE(static_cast<bool>(physics->sync(scene)));
		game = gameplay::Game::create(scene, bus, *physics, fixed_step);
		if (game) game->start();
		ScriptWorld::Setup setup;
		setup.loader = memory_loader(sources);
		setup.names = [](const engine::AssetRef& ref) {
			std::string path = ref.path;
			if (path.ends_with(".lua")) path.resize(path.size() - 4);
			return "/Game/" + path;
		};
		scripts = ScriptWorld::create(scene, bus, *physics, game.get(), fixed_step, std::move(setup));
		ENGINE_REQUIRE(scripts != nullptr);
	}

	void tick(const gameplay::InputFrame& input = {})
	{
		if (game) {
			game->set_input(input);
			game->pre_physics();
		}
		scripts->set_input(input);
		scripts->pre_physics();
		ENGINE_REQUIRE(static_cast<bool>(physics->fixed_update(fixed_step, scene, bus, context)));
		if (game) game->post_physics();
		scripts->post_physics({});
	}

	void ticks(int count)
	{
		for (int i = 0; i < count; ++i) tick();
	}

	std::vector<std::string> lines()
	{
		std::vector<std::string> out;
		for (const auto& line : scripts->runtime().take_output()) out.push_back(line.text);
		return out;
	}

	std::vector<ScriptError> errors() { return scripts->runtime().take_errors(); }

	double number(EntityId id, const char* field)
	{
		const auto value = scripts->runtime().field(id, field);
		return value && std::holds_alternative<double>(*value) ? std::get<double>(*value) : -1e300;
	}

	bool flag(EntityId id, const char* field)
	{
		const auto value = scripts->runtime().field(id, field);
		return value && std::holds_alternative<bool>(*value) && std::get<bool>(*value);
	}

	Vec3 position(EntityId id) { return scene.inspect(id)->transform.position; }
};

} // namespace

ENGINE_TEST(start_runs_once_before_the_first_tick_and_late_tick_after_physics)
{
	World w;
	w.script("Order", R"lua(local S = {}
function S:start() print("start") end
function S:tick(dt) print("tick", dt > 0.016 and dt < 0.017) end
function S:late_tick() print("late") end
return S
)lua");
	w.add("Probe", {}, World::scripted("Order"));
	w.begin();
	w.ticks(2);
	ENGINE_CHECK(w.lines() == (std::vector<std::string>{"start", "tick\ttrue", "late", "tick\ttrue", "late"}));
	ENGINE_CHECK(w.errors().empty());
	// A scene without scripts makes no script world.
	World empty;
	empty.add("Nothing", {});
	ENGINE_CHECK(ScriptWorld::create(empty.scene, empty.bus, *empty.physics, nullptr, fixed_step, {}) == nullptr);
}

ENGINE_TEST(scripts_move_entities_and_see_their_own_changes_at_once)
{
	World w;
	w.script("Slide", R"lua(local S = { properties = { step = Vec3(1, 0, 0) } }
function S:tick()
	self.entity.position = self.entity.position + self.step
	self.seen = self.entity.position.x
	self.under = self.entity:children()[1].world_position.x
end
return S
)lua");
	w.script("Place", R"lua(local S = {}
function S:start()
	self.entity.world_position = Vec3(5, 4, 3)
	self.local_after = self.entity.position
end
return S
)lua");
	Transform scaled = at(0, 0, 0);
	scaled.scale = {2, 2, 2};
	const auto slider = w.add("Slider", scaled, World::scripted("Slide"));
	const auto child = w.add("Child", at(0, 1, 0), World::scripted("Place"), slider);
	w.begin();
	w.ticks(3);
	ENGINE_CHECK_NEAR(w.position(slider).x, 3.0, 1e-12);
	ENGINE_CHECK_NEAR(w.number(slider, "seen"), 3.0, 1e-12);
	// The child was placed in the world by its start(), before any tick moved the slider; the slider then took it along.
	const auto placed = w.scripts->runtime().field(child, "local_after");
	ENGINE_REQUIRE(placed.has_value());
	const auto local = std::get<Vector3>(*placed);
	ENGINE_CHECK_NEAR(local.x, 2.5, 1e-12); // Under a parent at the origin, scaled by 2.
	ENGINE_CHECK_NEAR(local.y, 2.0, 1e-12);
	ENGINE_CHECK_NEAR(w.number(slider, "under"), 3 + local.x * 2, 1e-9);
	const auto world = w.scene.world_transform(child);
	ENGINE_REQUIRE(world.has_value());
	ENGINE_CHECK_NEAR(world->matrix[12], 3 + local.x * 2, 1e-9);
	ENGINE_CHECK(w.errors().empty());
}

ENGINE_TEST(spawned_copies_run_their_scripts_and_destroyed_ones_stop)
{
	World w;
	w.script("Spark", R"lua(local S = {}
function S:start() self.started = true end
function S:tick() self.ticks = (self.ticks or 0) + 1 end
return S
)lua");
	w.script("Spawner", R"lua(local S = {}
function S:tick()
	if engine.ticks() == 1 then self.copy = engine.spawn(engine.find("Spark"), Vec3(1, 2, 3)) end
	if engine.ticks() == 4 then self.copy:destroy() self.gone = not self.copy.valid end
end
return S
)lua");
	const auto original = w.add("Spark", at(0, -40, 0), World::scripted("Spark"));
	const auto spawner = w.add("Spawner", {}, World::scripted("Spawner"));
	w.begin();
	w.tick();
	const auto copy_value = w.scripts->runtime().field(spawner, "copy");
	ENGINE_REQUIRE(copy_value.has_value());
	const EntityId copy = std::get<EntityRef>(*copy_value).id;
	ENGINE_REQUIRE(copy != 0 && copy != original);
	const auto spawned = w.scene.inspect(copy); // By value: the check macros keep references.
	ENGINE_REQUIRE(spawned.has_value());
	ENGINE_CHECK_EQ(spawned->name, std::string("Spark"));
	ENGINE_CHECK(spawned->parent == 0);
	ENGINE_CHECK_NEAR(w.position(copy).y, 2.0, 1e-12);
	ENGINE_CHECK(w.scripts->runtime().has_instance(copy));
	ENGINE_CHECK(!w.flag(copy, "started")); // It starts on the next tick.
	w.tick();
	ENGINE_CHECK(w.flag(copy, "started"));
	ENGINE_CHECK_NEAR(w.number(copy, "ticks"), 1.0, 0);
	w.ticks(2);
	ENGINE_CHECK(!w.scene.inspect(copy));
	ENGINE_CHECK(!w.scripts->runtime().has_instance(copy));
	ENGINE_CHECK(w.flag(spawner, "gone"));
	ENGINE_CHECK(w.scene.inspect(original).has_value());
	ENGINE_CHECK(w.errors().empty());
}

ENGINE_TEST(rays_impulses_and_velocities_reach_the_physics)
{
	World w;
	w.script("Probe", R"lua(local S = {}
function S:start()
	local hit = engine.raycast(Vec3(3, 5, 0), Vec3(0, -2, 0), 20)
	self.floor = hit ~= nil and hit.entity.name == "Floor"
	self.distance = hit and hit.distance or -1
	self.nothing = engine.raycast(Vec3(3, 5, 0), Vec3(0, 1, 0), 20) == nil
end
return S
)lua");
	w.script("Kick", R"lua(local S = {}
function S:tick()
	if engine.ticks() == 1 then
		self.dynamic = self.entity.dynamic
		self.entity:apply_impulse(Vec3(0, 4000, 0)) -- A tonne of crate: 4 m/s up.
	end
	if engine.ticks() == 2 then self.rising = self.entity.velocity.y > 0 end
	if engine.ticks() == 3 then self.entity.velocity = Vec3(4, 0, 0) end
end
return S
)lua");
	w.floor();
	w.add("Probe", {}, World::scripted("Probe"));
	Components crate = World::scripted("Kick");
	crate.body = body(BodyKind::Dynamic);
	crate.colliders = {box({0.5, 0.5, 0.5})};
	const auto kicked = w.add("Crate", at(0, 0.5, 0), crate);
	const auto probe = w.scene.snapshot().entities[1].id;
	w.begin();
	w.ticks(10);
	ENGINE_CHECK(w.flag(probe, "floor"));
	ENGINE_CHECK_NEAR(w.number(probe, "distance"), 5.0, 1e-4);
	ENGINE_CHECK(w.flag(probe, "nothing"));
	ENGINE_CHECK(w.flag(kicked, "dynamic"));
	ENGINE_CHECK(w.flag(kicked, "rising"));
	ENGINE_CHECK(w.position(kicked).x > 0.2);
	ENGINE_CHECK(w.errors().empty());
}

ENGINE_TEST(triggers_and_hits_reach_both_entities)
{
	World w;
	w.script("Zone", R"lua(local S = {}
function S:on_trigger_enter(other) self.entered = other.name end
return S
)lua");
	w.script("Ball", R"lua(local S = {}
function S:on_trigger_enter(other) self.passed = other.name end
function S:on_hit(other, speed, point, normal) if other.name == "Floor" then self.landed = speed end end
return S
)lua");
	w.floor();
	Components zone = World::scripted("Zone");
	zone.colliders = {box({1, 0.5, 1}, true)};
	const auto sensor = w.add("Zone", at(0, 1.5, 0), zone);
	Components ball = World::scripted("Ball");
	ball.body = body(BodyKind::Dynamic);
	ball.colliders = {box({0.25, 0.25, 0.25})};
	const auto falling = w.add("Ball", at(0, 4, 0), ball);
	w.begin();
	w.ticks(120);
	const auto entered = w.scripts->runtime().field(sensor, "entered");
	ENGINE_REQUIRE(entered.has_value());
	ENGINE_CHECK_EQ(std::get<std::string>(*entered), std::string("Ball"));
	const auto passed = w.scripts->runtime().field(falling, "passed");
	ENGINE_REQUIRE(passed.has_value());
	ENGINE_CHECK_EQ(std::get<std::string>(*passed), std::string("Zone"));
	ENGINE_CHECK(w.number(falling, "landed") > 1.0);
	ENGINE_CHECK(w.errors().empty());
}

ENGINE_TEST(game_rules_damage_and_death_reach_scripts)
{
	World w;
	w.script("Watch", R"lua(local S = {}
function S:on_damage(amount, cause) print("hurt", amount, cause) end
function S:on_death() print("dead") end
return S
)lua");
	w.script("Rules", R"lua(local S = {}
function S:tick()
	local t = engine.ticks()
	if t == 1 then
		self.left = engine.game.targets_left()
		engine.find("Target"):damage(30)
		self.time = engine.game.time_left()
		engine.game.add_time(10)
	elseif t == 2 then
		self.health = engine.find("Target").health
		engine.find("Target"):damage(30)
	elseif t == 4 then
		engine.game.add_score(5, "Bonus")
		self.score = engine.game.score()
		self.phase = engine.game.phase()
		engine.game.win()
	elseif t == 5 then
		self.won = engine.game.phase() == "won"
	end
end
return S
)lua");
	w.rules();
	Components target = World::scripted("Watch");
	target.colliders = {box({0.5, 0.5, 0.1})};
	target.behaviors = {{"health", {{"max", 50}}}, {"target", {{"points", 100}}}};
	w.add("Target", at(0, 1.5, -5), target);
	const auto rules = w.add("Referee", {}, World::scripted("Rules"));
	w.begin();
	ENGINE_REQUIRE(w.game != nullptr);
	w.ticks(5);
	ENGINE_CHECK(w.lines() == (std::vector<std::string>{"hurt\t30\tscript", "hurt\t30\tscript", "dead"}));
	ENGINE_CHECK_NEAR(w.number(rules, "left"), 1.0, 0);
	ENGINE_CHECK_NEAR(w.number(rules, "health"), 20.0, 1e-12);
	ENGINE_CHECK_NEAR(w.number(rules, "score"), 105.0, 0);
	ENGINE_CHECK(w.flag(rules, "won"));
	ENGINE_CHECK(w.game->hud().last_event == "Bonus");
	ENGINE_CHECK(w.game->hud().time_limit > 99.9);
	ENGINE_CHECK(w.errors().empty());
}

ENGINE_TEST(input_is_read_as_actions)
{
	World w;
	w.script("Button", R"lua(local S = {}
function S:tick()
	if engine.input.pressed("Interact") then self.pressed = (self.pressed or 0) + 1 end
	if engine.input.held("Interact") then self.held = (self.held or 0) + 1 end
	if engine.ticks() == 3 then engine.input.held("Dance") end
end
return S
)lua");
	const auto button = w.add("Button", {}, World::scripted("Button"));
	w.begin();
	gameplay::InputFrame press;
	press.held[static_cast<std::size_t>(gameplay::Action::Interact)] = true;
	press.pressed = press.held;
	w.tick(press);
	gameplay::InputFrame hold;
	hold.held = press.held;
	w.tick(hold);
	w.tick();
	ENGINE_CHECK_NEAR(w.number(button, "pressed"), 1.0, 0);
	ENGINE_CHECK_NEAR(w.number(button, "held"), 2.0, 0);
	const auto errors = w.errors();
	ENGINE_REQUIRE(errors.size() == 1);
	ENGINE_CHECK(contains(errors[0].message, "there is no action 'Dance'"));
	ENGINE_CHECK_EQ(errors[0].line, 5);
}

ENGINE_TEST(a_gone_entity_and_a_missing_game_are_errors_at_their_line)
{
	World w;
	w.script("Hunter", R"lua(local S = { properties = { prey = Entity() } }
function S:tick()
	if engine.ticks() == 1 then self.prey:destroy() end
	if engine.ticks() == 2 then local p = self.prey.position end
end
return S
)lua");
	w.script("Scorer", R"lua(local S = {}
function S:start()
	self.active = engine.game.active()
	engine.game.add_score(1)
end
return S
)lua");
	const auto prey = w.add("Prey", {});
	w.add("Hunter", {}, World::scripted("Hunter", {{"prey", EntityLink{prey}}}));
	w.add("Scorer", {}, World::scripted("Scorer"));
	w.begin();
	ENGINE_CHECK(w.game == nullptr);
	w.ticks(3);
	auto errors = w.errors();
	ENGINE_REQUIRE(errors.size() == 2);
	ENGINE_CHECK(contains(errors[0].message, "runs no game"));
	ENGINE_CHECK_EQ(errors[0].script, std::string("/Game/Scripts/Scorer"));
	ENGINE_CHECK_EQ(errors[0].line, 4);
	ENGINE_CHECK(contains(errors[1].message, "entity " + std::to_string(prey) + " no longer exists"));
	ENGINE_CHECK_EQ(errors[1].line, 4);
}

ENGINE_TEST(a_run_with_scripts_repeats_tick_for_tick)
{
	const auto run = [] {
		World w;
		w.script("Jitter", R"lua(local S = {}
function S:tick()
	if engine.ticks() % 10 == 1 then
		local push = Vec3(math.random() - 0.5, math.random() * 2, math.random() - 0.5) * 20
		self.entity:apply_impulse(push)
	end
	local t = {}
	for i = 1, 20 do t["k" .. i] = i end
	local order = {}
	for k in pairs(t) do order[#order + 1] = k end
	self.order = table.concat(order, ",")
end
return S
)lua");
		w.floor();
		for (int i = 0; i < 6; ++i) {
			Components c = World::scripted("Jitter");
			c.body = body(BodyKind::Dynamic);
			c.colliders = {box({0.3, 0.3, 0.3})};
			w.add("Crate", at(i * 0.8 - 2, 0.3 + i * 0.7, 0), c);
		}
		w.begin();
		w.ticks(150);
		ENGINE_CHECK(w.errors().empty());
		return w.scene.snapshot();
	};
	const auto first = run();
	const auto second = run();
	ENGINE_CHECK(first == second);
}
