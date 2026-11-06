// Milestone 6, slice 5: the Physics Shooter's scripts. Its Mover, Pickup and BonusZone scripts against the C++
// behaviors they stand for, played side by side and compared after every tick: every entity's transform and the HUD,
// exactly. Then the scripted vault: a gate that opens once the targets of its room have fallen, and a button the player
// presses with E. Real Scene, CommandBus, Box3D world, Game and ScriptWorld, ticked in the order a play session ticks
// them; the scripts are the sample's own files.

#include "engine/gameplay/game.h"
#include "engine/test/test.h"

#include <engine/scripting/script_world.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::gameplay;
using engine::scripting::ScriptWorld;
namespace fs = std::filesystem;

#define SCRIPTED_FAIL(text)                                             \
	do {                                                                \
		::engine::test::report_failure(__FILE__, __LINE__, (text));     \
		throw ::engine::test::RequireFailed{};                          \
	} while (false)

namespace {

constexpr std::chrono::nanoseconds fixed_step{16'666'667};
const fs::path sample = fs::path(GAMEENGINE_SAMPLES_DIR) / "PhysicsShooter";

// The sample's scripts by package name: "/Game/Scripts/Gate" is Assets/Scripts/Gate.lua.
scripting::ScriptLoader sample_scripts()
{
	return [](std::string_view name) -> Result<scripting::ScriptSource> {
		constexpr std::string_view mount = "/Game/";
		if (!name.starts_with(mount)) return fail(ErrorCode::NotFound, "not a /Game name: " + std::string(name));
		std::ifstream in(sample / "Assets" / (std::string(name.substr(mount.size())) + ".lua"), std::ios::binary);
		if (!in) return fail(ErrorCode::NotFound, "no script is named " + std::string(name));
		return scripting::ScriptSource{std::string(name), std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>())};
	};
}

std::string package_of(const AssetRef& ref)
{
	std::string path = ref.path;
	if (path.ends_with(".lua")) path.resize(path.size() - 4);
	return "/Game/" + path;
}

Transform at(double x, double y, double z)
{
	Transform t;
	t.position = {x, y, z};
	return t;
}

InputFrame held(std::initializer_list<Action> actions)
{
	InputFrame frame;
	for (const Action action : actions) frame.held[static_cast<std::size_t>(action)] = true;
	return frame;
}

InputFrame pressed(std::initializer_list<Action> actions)
{
	InputFrame frame = held(actions);
	frame.pressed = frame.held;
	return frame;
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

double value_of(const BehaviorComponent& b, const char* key, double fallback)
{
	for (const auto& [name, value] : b.values)
		if (name == key) return value;
	return fallback;
}

ScriptComponent script(const char* name, std::vector<ScriptProperty> values)
{
	return {{std::hash<std::string>{}(name) | 1, std::string("Scripts/") + name + ".lua"}, std::move(values)};
}

// The same scene with each mover, pickup and bonus_zone behavior swapped for the sample's script of that name, given the
// same values, the C++ defaults included. Everything else stays as it is.
SceneSnapshot as_scripts(SceneSnapshot snapshot)
{
	for (auto& e : snapshot.entities) {
		auto& behaviors = e.components.behaviors;
		for (auto it = behaviors.begin(); it != behaviors.end();) {
			const auto number = [&](const char* key, double fallback) { return value_of(*it, key, fallback); };
			const Vec3 p = e.transform.position;
			if (it->type == "mover") {
				ENGINE_REQUIRE(!e.components.script);
				e.components.script = script("Mover", {{"a", Vec3{number("ax", p.x), number("ay", p.y), number("az", p.z)}},
					{"b", Vec3{number("bx", p.x), number("by", p.y), number("bz", p.z)}}, {"speed", number("speed", 2)}});
			} else if (it->type == "pickup") {
				ENGINE_REQUIRE(!e.components.script);
				e.components.script = script("Pickup", {{"ammo", number("ammo", 0)}, {"time", number("time", 0)}});
			} else if (it->type == "bonus_zone") {
				ENGINE_REQUIRE(!e.components.script);
				e.components.script = script("BonusZone", {{"points", number("points", 50)}});
			} else {
				++it;
				continue;
			}
			it = behaviors.erase(it);
		}
	}
	return snapshot;
}

// A play session's runtime: the scene, its physics, the game and the scripts, ticked as EditorSession::tick does.
struct Run {
	Scene scene;
	commands::CommandBus bus{scene, 0};
	commands::Context context{"test", 3};
	std::unique_ptr<physics::ScenePhysics> physics;
	std::unique_ptr<Game> game;
	std::unique_ptr<ScriptWorld> scripts;
	double yaw = 0, pitch = 0;

	explicit Run(const SceneSnapshot& snapshot)
	{
		auto created = physics::ScenePhysics::create();
		ENGINE_REQUIRE(static_cast<bool>(created));
		physics = std::make_unique<physics::ScenePhysics>(std::move(created.value()));
		physics->set_simulation_enabled(true);
		ENGINE_REQUIRE(static_cast<bool>(bus.execute(context, 0, commands::Command(commands::ReplaceScene{snapshot}))));
		ENGINE_REQUIRE(static_cast<bool>(physics->sync(scene)));
		game = Game::create(scene, bus, *physics, fixed_step);
		ENGINE_REQUIRE(game != nullptr);
		ENGINE_CHECK(game->setup_issues().empty());
		ScriptWorld::Setup setup;
		setup.loader = sample_scripts();
		setup.names = package_of;
		scripts = ScriptWorld::create(scene, bus, *physics, game.get(), fixed_step, std::move(setup));
		game->start();
	}

	void tick(const InputFrame& input = {})
	{
		game->set_input(input);
		if (scripts) scripts->set_input(input);
		game->pre_physics();
		if (scripts) scripts->pre_physics();
		ENGINE_REQUIRE(static_cast<bool>(physics->fixed_update(fixed_step, scene, bus, context)));
		game->hear({});
		game->post_physics();
		if (scripts) scripts->post_physics({});
	}

	void ticks(int count, const InputFrame& input = {})
	{
		for (int i = 0; i < count; ++i) tick(input);
	}

	EntityId find(const std::string& name) const
	{
		for (const auto& e : scene.snapshot().entities)
			if (e.name == name) return e.id;
		return 0;
	}

	Vec3 position(EntityId id) const
	{
		const auto w = scene.world_transform(id);
		ENGINE_REQUIRE(w.has_value());
		return {w->matrix[12], w->matrix[13], w->matrix[14]};
	}

	bool exists(EntityId id) const { return scene.inspect(id).has_value(); }

	void teleport(EntityId id, Vec3 to)
	{
		auto t = scene.inspect(id)->transform;
		t.position = to;
		ENGINE_REQUIRE(static_cast<bool>(bus.execute(context, bus.revision(), commands::Command(commands::SetTransform{id, t}))));
	}

	// Turns the view onto a point, as moving the mouse would.
	void aim(Vec3 point)
	{
		const Vec3 eye = position(game->camera());
		const double dx = point.x - eye.x, dy = point.y - eye.y, dz = point.z - eye.z;
		const double want_yaw = std::atan2(-dx, -dz), want_pitch = std::atan2(dy, std::hypot(dx, dz));
		game->look((yaw - want_yaw) / 0.0022, (pitch - want_pitch) / 0.0022);
		yaw = want_yaw;
		pitch = want_pitch;
		game->update_view();
	}

	std::vector<scripting::ScriptError> errors() { return scripts ? scripts->runtime().take_errors() : std::vector<scripting::ScriptError>{}; }

	bool flag(EntityId id, const char* name) const
	{
		const auto value = scripts->runtime().field(id, name);
		return value && std::holds_alternative<bool>(*value) && std::get<bool>(*value);
	}
};

std::string text(const Vec3& v)
{
	std::ostringstream out;
	out.precision(17);
	out << '(' << v.x << ", " << v.y << ", " << v.z << ')';
	return out.str();
}

// What differs between two runs after a tick: every entity, where it is, and what the HUD shows. Empty when nothing.
std::string differences(const Run& cpp, const Run& lua)
{
	const auto a = cpp.scene.snapshot(), b = lua.scene.snapshot();
	if (a.entities.size() != b.entities.size())
		return "entities: " + std::to_string(a.entities.size()) + " in C++, " + std::to_string(b.entities.size()) + " scripted";
	for (std::size_t i = 0; i < a.entities.size(); ++i) {
		const auto &x = a.entities[i], &y = b.entities[i];
		if (x.id != y.id) return "entity " + std::to_string(x.id) + " in C++ where the scripted run has " + std::to_string(y.id);
		if (!(x.transform == y.transform))
			return x.name + ": " + text(x.transform.position) + " in C++, " + text(y.transform.position) + " scripted";
	}
	const auto h = cpp.game->hud(), k = lua.game->hud();
	std::ostringstream out;
	out.precision(17);
	if (h.phase != k.phase) out << " phase " << to_string(h.phase) << '/' << to_string(k.phase);
	if (h.score != k.score) out << " score " << h.score << '/' << k.score;
	if (h.targets_remaining != k.targets_remaining) out << " targets " << h.targets_remaining << '/' << k.targets_remaining;
	if (h.time_limit != k.time_limit) out << " time limit " << h.time_limit << '/' << k.time_limit;
	if (h.time_remaining != k.time_remaining) out << " time " << h.time_remaining << '/' << k.time_remaining;
	if (h.ammo != k.ammo || h.reserve != k.reserve) out << " ammo " << h.ammo << '+' << h.reserve << '/' << k.ammo << '+' << k.reserve;
	if (h.last_event != k.last_event) out << " message '" << h.last_event << "'/'" << k.last_event << "'";
	if (h.last_event_age != k.last_event_age) out << " message age " << h.last_event_age << '/' << k.last_event_age;
	return out.str();
}

// Both runs, given the same input and commands, tick for tick.
struct Twin {
	Run cpp, lua;
	int tick_count = 0, bonuses = 0; // Ticks run, and bonus zone scores announced.

	explicit Twin(const SceneSnapshot& authored) : cpp(authored), lua(as_scripts(authored))
	{
		same();
	}

	void same()
	{
		if (const auto diff = differences(cpp, lua); !diff.empty())
			SCRIPTED_FAIL(("tick " + std::to_string(tick_count) + ": " + diff).c_str());
	}

	void tick(const InputFrame& input = {})
	{
		cpp.tick(input);
		lua.tick(input);
		++tick_count;
		same();
		const auto hud = lua.game->hud();
		if (hud.last_event_age == 0 && hud.last_event == "+50  Bonus") ++bonuses;
	}

	void ticks(int count, const InputFrame& input = {})
	{
		for (int i = 0; i < count; ++i) tick(input);
	}

	void teleport(EntityId id, Vec3 to)
	{
		cpp.teleport(id, to);
		lua.teleport(id, to);
	}

	void aim(Vec3 point)
	{
		cpp.aim(point);
		lua.aim(point);
	}

	// Fires at an entity until `done`, reloading when the magazine is empty, standing at `stand(where it is)` every two
	// seconds in case something is in the way.
	template <typename Done, typename Stand>
	void fire_at(EntityId target, Done done, Stand stand, int limit = 900)
	{
		const EntityId player = lua.game->player();
		for (int i = 0; i < limit && !done(); ++i) {
			if (i % 120 == 0) {
				teleport(player, stand(lua.position(target)));
				tick();
			}
			if (lua.game->hud().ammo == 0) {
				tick(pressed({Action::Reload}));
				ticks(80);
				continue;
			}
			aim(lua.position(target));
			tick(held({Action::Fire}));
		}
	}

	// No script failed, in either run.
	void no_errors()
	{
		for (auto* run : {&cpp, &lua})
			for (const auto& error : run->errors()) SCRIPTED_FAIL((error.script + ":" + std::to_string(error.line) + ": " + error.message).c_str());
	}
};

// A scene built through commands, for small cases.
struct Builder {
	Scene scene;
	commands::CommandBus bus{scene, 0};
	commands::Context context{"test", 3};

	EntityId add(const char* name, Transform t, Components c = {}, EntityId parent = 0)
	{
		auto r = bus.execute(context, bus.revision(), commands::Command(commands::CreateEntity{name, t, parent, 0, std::move(c)}));
		ENGINE_REQUIRE(static_cast<bool>(r));
		return r.value().created.front();
	}

	void floor()
	{
		Components c;
		c.colliders = {box({30, 0.5, 30})};
		add("Floor", at(0, -0.5, 0), c);
	}

	void rules(double time_limit = 90)
	{
		Components c;
		c.behaviors = {{"game_mode", {{"time_limit", time_limit}}}};
		add("Rules", {}, c);
	}

	// A capsule on the floor looking down -Z, with a camera and a gun.
	EntityId player(Vec3 where, double reserve = 10)
	{
		Components c;
		c.body = body(BodyKind::Dynamic);
		c.body->lock_rotation = true;
		ColliderComponent capsule;
		capsule.shape = ColliderShape::Capsule;
		capsule.radius = 0.35;
		capsule.half_height = 0.55;
		capsule.density = 130;
		capsule.friction = 0;
		capsule.events = true;
		c.colliders = {capsule};
		c.behaviors = {{"player", {}}, {"weapon", {{"reserve", reserve}}}};
		const auto id = add("Player", at(where.x, where.y, where.z), c);
		Components view;
		view.camera = CameraComponent{};
		add("Camera", at(0, 0.6, 0), view, id);
		return id;
	}

	EntityId crate(const char* name, Vec3 where, double half = 0.3)
	{
		Components c;
		c.body = body(BodyKind::Dynamic);
		c.colliders = {box({half, half, half})};
		c.colliders[0].density = 100;
		c.colliders[0].friction = 0.6;
		return add(name, at(where.x, where.y, where.z), c);
	}

	EntityId pickup(const char* name, Vec3 where, double ammo, double time)
	{
		Components c;
		ColliderComponent sensor;
		sensor.shape = ColliderShape::Sphere;
		sensor.radius = 0.6;
		sensor.sensor = true;
		sensor.events = true;
		c.colliders = {sensor};
		c.behaviors = {{"pickup", {{"ammo", ammo}, {"time", time}}}};
		return add(name, at(where.x, where.y, where.z), c);
	}

	SceneSnapshot snapshot() const { return scene.snapshot(); }
};

SceneSnapshot read_sample()
{
	std::ifstream file(sample / "physics_shooter.scene", std::ios::binary);
	ENGINE_REQUIRE(static_cast<bool>(file));
	auto snapshot = read_scene(file);
	if (!snapshot) SCRIPTED_FAIL(snapshot.error().message.c_str());
	return snapshot.value();
}

} // namespace

// ---- The scripts play as the C++ behaviors do ----------------------------------------------------

ENGINE_TEST(scripted_mover_moves_like_the_cpp_mover_tick_for_tick)
{
	Builder b;
	b.floor();
	b.rules();
	// A platform carrying a crate, there and back; a sphere on a slant at an odd speed; one at rest; one with only an end.
	Components platform;
	platform.body = body(BodyKind::Kinematic);
	platform.colliders = {box({1.5, 0.25, 1.5})};
	platform.colliders[0].friction = 0.9;
	platform.behaviors = {{"mover", {{"ax", -6}, {"ay", 0.5}, {"az", 0}, {"bx", 6}, {"by", 0.5}, {"bz", 0}, {"speed", 2.5}}}};
	b.add("Platform", at(-6, 0.5, 0), platform);
	const EntityId rider = b.crate("Rider", {-6, 1.06, 0});
	Components sphere;
	sphere.body = body(BodyKind::Kinematic);
	ColliderComponent ball;
	ball.shape = ColliderShape::Sphere;
	ball.radius = 0.5;
	sphere.colliders = {ball};
	sphere.behaviors = {{"mover", {{"ax", 0}, {"ay", 2}, {"az", -5}, {"bx", 4.25}, {"by", 5.5}, {"bz", -9.75}, {"speed", 3.3}}}};
	b.add("Slant", at(0, 2, -5), sphere);
	Components still = sphere;
	still.behaviors = {{"mover", {{"ax", 5}, {"ay", 1}, {"az", 5}, {"bx", 9}, {"by", 1}, {"bz", 5}, {"speed", 0}}}};
	b.add("Still", at(5, 1, 5), still);
	Components defaults = sphere;
	defaults.behaviors = {{"mover", {{"bx", -3}}}}; // From where it stands, at the default speed of 2.
	b.add("Defaults", at(3, 1, 8), defaults);

	Twin twin(b.snapshot());
	double furthest = -100, back = 100;
	for (int i = 0; i < 900; ++i) {
		twin.tick();
		const EntityId platform_id = twin.lua.find("Platform");
		furthest = std::max(furthest, twin.lua.position(platform_id).x);
		if (furthest > 5.9) back = std::min(back, twin.lua.position(platform_id).x);
	}
	twin.no_errors();
	// It did go there and back, carrying the crate, and the others did what their values say.
	ENGINE_CHECK(furthest > 5.9);
	ENGINE_CHECK(back < -5.9);
	ENGINE_CHECK(std::abs(twin.lua.position(rider).x - twin.lua.position(twin.lua.find("Platform")).x) < 1.5);
	const Vec3 still_at = twin.lua.position(twin.lua.find("Still"));
	ENGINE_CHECK(still_at == (Vec3{5, 1, 5}));
	const Vec3 defaults_at = twin.lua.position(twin.lua.find("Defaults"));
	ENGINE_CHECK(defaults_at.x < 3 && defaults_at.x >= -3 && defaults_at.y == 1 && defaults_at.z == 8);
}

ENGINE_TEST(scripted_pickups_collect_like_the_cpp_ones)
{
	Builder b;
	b.floor();
	b.rules(50);
	b.player({0, 0.9, 0}, 10);
	const EntityId ammo = b.pickup("Ammo", {0, 1, -3}, 20, 0);
	const EntityId time = b.pickup("Time", {0, 1, -6}, 0, 15.7);
	const EntityId both = b.pickup("Both", {0, 1, -9}, 5, 4);
	// A crate falling through one is not the player: it stays.
	const EntityId missed = b.pickup("Missed", {4, 1, -3}, 30, 30);
	b.crate("Faller", {4, 4, -3});

	Twin twin(b.snapshot());
	twin.ticks(20);
	std::vector<std::string> messages;
	for (int i = 0; i < 160; ++i) {
		twin.tick(held({Action::MoveForward}));
		const auto message = twin.lua.game->hud().last_event;
		if (messages.empty() || messages.back() != message) messages.push_back(message);
	}
	twin.no_errors();
	for (const EntityId id : {ammo, time, both}) ENGINE_CHECK(!twin.lua.exists(id) && !twin.cpp.exists(id));
	ENGINE_CHECK(twin.lua.exists(missed));
	const auto hud = twin.lua.game->hud();
	ENGINE_CHECK_EQ(hud.reserve, 35);
	ENGINE_CHECK_NEAR(hud.time_limit, 50 + 15.7 + 4, 1e-9);
	ENGINE_CHECK(messages == (std::vector<std::string>{"", "Ammo collected", "+15 seconds", "+4 seconds"}));
}

ENGINE_TEST(scripted_bonus_zone_scores_like_the_cpp_one)
{
	Builder b;
	b.floor();
	b.rules();
	b.player({6.6, 0.9, 0});
	Components zone;
	zone.colliders = {box({2, 1, 2}, true)};
	zone.behaviors = {{"bonus_zone", {{"points", 50}}}};
	b.add("Zone", at(5, 1, -5), zone);
	const EntityId first = b.crate("First", {5, 4, -5});
	b.crate("Second", {5.6, 6, -5.4});
	// A kinematic body passing through scores nothing: physics does not move it.
	Components sweeper;
	sweeper.body = body(BodyKind::Kinematic);
	sweeper.colliders = {box({0.3, 0.3, 0.3})};
	sweeper.behaviors = {{"mover", {{"ax", 3.5}, {"ay", 1}, {"az", -9}, {"bx", 3.5}, {"by", 1}, {"bz", -1}, {"speed", 4}}}};
	b.add("Sweeper", at(3.5, 1, -9), sweeper);

	Twin twin(b.snapshot());
	twin.ticks(150);
	ENGINE_CHECK_EQ(twin.lua.game->hud().score, 100);
	ENGINE_CHECK(twin.lua.game->hud().last_event == "+50  Bonus");
	// Back above it and in again: still once.
	twin.teleport(first, {5, 4, -5});
	twin.ticks(120);
	// The player walks through it: no points either.
	twin.ticks(120, held({Action::MoveForward}));
	twin.no_errors();
	ENGINE_CHECK_EQ(twin.lua.game->hud().score, 100);
	ENGINE_CHECK(twin.lua.position(twin.lua.game->player()).z < -7.5);
}

// The sample as authored against the same scene with its two moving targets, two pickups and bonus zone run by
// scripts: knocking a crate into the zone, walking through both pickups, then shooting both moving targets.
ENGINE_TEST(sample_plays_the_same_with_its_movers_pickups_and_bonus_zone_scripted)
{
	const SceneSnapshot authored = read_sample();
	Twin twin(authored);
	ENGINE_REQUIRE(twin.lua.scripts != nullptr);
	ENGINE_CHECK_EQ(twin.lua.scripts->runtime().instances().size(), std::size_t{7}); // And the vault's gate and button.
	ENGINE_CHECK_EQ(twin.cpp.scripts->runtime().instances().size(), std::size_t{2});
	const EntityId player = twin.lua.game->player();
	twin.ticks(60);

	// A crate off the shelf into the bonus zone.
	const EntityId crate = twin.lua.find("Shelf Crate 3");
	const int bonuses = twin.bonuses;
	twin.fire_at(crate, [&] { return twin.bonuses > bonuses; }, [](Vec3 p) { return Vec3{p.x, 1.0, p.z + 6}; });
	ENGINE_CHECK(twin.bonuses > bonuses);
	twin.ticks(60);

	// Both pickups, walked into from two and a half meters away.
	for (const char* name : {"Ammo Pickup", "Time Pickup"}) {
		const EntityId id = twin.lua.find(name);
		ENGINE_REQUIRE(id != 0);
		const Vec3 p = twin.lua.position(id);
		twin.teleport(player, {p.x, p.y + 0.2, p.z + 2.5});
		twin.tick();
		twin.aim({p.x, twin.lua.position(twin.lua.game->camera()).y, p.z});
		const auto before = twin.lua.game->hud();
		twin.ticks(80, held({Action::MoveForward}));
		ENGINE_CHECK(!twin.lua.exists(id));
		const auto after = twin.lua.game->hud();
		ENGINE_CHECK(name[0] == 'A' ? after.reserve == before.reserve + 45 : after.time_limit == before.time_limit + 15);
	}
	twin.ticks(600); // The movers go on there and back.

	// The moving targets, fired at where they are each tick, from in front of them.
	for (const char* name : {"Moving Target 1", "Moving Target 2"}) {
		const EntityId id = twin.lua.find(name);
		ENGINE_REQUIRE(id != 0);
		twin.fire_at(id, [&] { return !twin.lua.exists(id); },
			[](Vec3 p) { return Vec3{std::clamp(p.x, -12.0, 12.0), 1.0, std::clamp(p.z + 6, -14.0, 14.0)}; });
		ENGINE_CHECK(!twin.lua.exists(id));
	}
	twin.ticks(300);
	twin.no_errors();
	const auto hud = twin.lua.game->hud();
	ENGINE_CHECK(hud.targets_remaining <= 10);
	ENGINE_CHECK_NEAR(hud.time_limit, 105.0, 1e-9);
	ENGINE_CHECK(twin.tick_count > 1200);
}

// ---- The vault --------------------------------------------------------------------------------

ENGINE_TEST(sample_vault_gate_opens_once_the_west_wing_targets_have_fallen)
{
	Run run(read_sample());
	ENGINE_REQUIRE(run.scripts != nullptr);
	const EntityId gate = run.find("Vault Gate"), pillar = run.find("Static Target 3"), enemy = run.find("Enemy 1");
	const EntityId elsewhere = run.find("Static Target 1"), bottle = run.find("Water Bottle Target");
	ENGINE_REQUIRE(gate && pillar && enemy && elsewhere && bottle);
	const Vec3 closed = run.position(gate);
	run.ticks(30);
	ENGINE_CHECK(run.position(gate) == closed);
	ENGINE_CHECK(!run.flag(gate, "opened"));
	// Targets outside its room do not open it, nor does one of the two in it.
	for (const EntityId id : {elsewhere, bottle, pillar}) {
		run.game->damage(id, 1000, {});
		run.ticks(30);
	}
	ENGINE_CHECK(!run.exists(pillar));
	ENGINE_CHECK_EQ(run.game->hud().targets_remaining, 9);
	ENGINE_CHECK(run.position(gate) == closed);
	ENGINE_CHECK(!run.flag(gate, "opened"));
	// The enemy falls, and is still in the scene: fallen is not standing.
	run.game->damage(enemy, 1000, {});
	run.tick();
	ENGINE_CHECK(run.exists(enemy));
	run.tick();
	ENGINE_CHECK(run.flag(gate, "opened"));
	ENGINE_CHECK(run.game->hud().last_event == "The vault is open");
	// It slides 3 m into the wall at 1.5 m/s, and stays there.
	run.ticks(59);
	ENGINE_CHECK_NEAR(run.position(gate).z, closed.z + 1.5, 0.03);
	run.ticks(120);
	const Vec3 open = run.position(gate);
	ENGINE_CHECK_NEAR(open.x, closed.x, 1e-12);
	ENGINE_CHECK_NEAR(open.y, closed.y, 1e-12);
	ENGINE_CHECK_NEAR(open.z, closed.z + 3, 1e-12);
	ENGINE_CHECK(run.errors().empty());
}

ENGINE_TEST(sample_vault_gate_stops_the_player_until_it_opens)
{
	Run run(read_sample());
	const EntityId gate = run.find("Vault Gate"), player = run.game->player();
	const Vec3 closed = run.position(gate);
	// Toward the vault along the doorway's middle: the gate holds the player in the arena.
	run.teleport(player, {-13, 1.0, closed.z});
	run.tick();
	run.aim({-30, run.position(run.game->camera()).y, closed.z});
	run.ticks(120, held({Action::MoveForward}));
	ENGINE_CHECK(run.position(player).x > closed.x + 0.3);
	ENGINE_CHECK(run.position(player).x < closed.x + 1.0);
	// Open, the player walks in.
	for (const char* name : {"Static Target 3", "Enemy 1"}) run.game->damage(run.find(name), 1000, {});
	run.ticks(150);
	run.ticks(90, held({Action::MoveForward}));
	ENGINE_CHECK(run.position(player).x < -19);
	ENGINE_CHECK(run.errors().empty());
}

ENGINE_TEST(sample_vault_button_gives_its_prize_once_when_pressed_from_within_reach)
{
	Run run(read_sample());
	const EntityId button = run.find("Vault Button"), player = run.game->player();
	ENGINE_REQUIRE(button != 0);
	const Vec3 rest = run.position(button);
	run.ticks(10);
	// E from the start, far from it: nothing.
	run.tick(pressed({Action::Interact}));
	ENGINE_CHECK_EQ(run.game->hud().score, 0);
	// Within reach: the hint, then a press gives the points and the time, and it sinks into the wall.
	run.teleport(player, {rest.x + 1.5, 1.0, rest.z});
	run.ticks(3);
	ENGINE_CHECK(run.game->hud().last_event == "Press E for the vault's prize");
	run.ticks(20);
	ENGINE_CHECK_EQ(run.game->hud().score, 0); // Standing near it is not pressing it.
	const double limit = run.game->hud().time_limit;
	run.tick(pressed({Action::Interact}));
	ENGINE_CHECK_EQ(run.game->hud().score, 300);
	ENGINE_CHECK_NEAR(run.game->hud().time_limit, limit + 20, 1e-9);
	ENGINE_CHECK(run.game->hud().last_event == "+300  Vault, and 20 seconds");
	run.tick();
	ENGINE_CHECK_NEAR(run.position(button).x, rest.x - 0.08, 1e-12);
	// Once only.
	run.ticks(40);
	run.tick(pressed({Action::Interact}));
	run.ticks(5);
	ENGINE_CHECK_EQ(run.game->hud().score, 300);
	ENGINE_CHECK_NEAR(run.game->hud().time_limit, limit + 20, 1e-9);
	ENGINE_CHECK(run.errors().empty());
}

// A button can open a gate itself: it calls press() on its target's script.
ENGINE_TEST(a_button_presses_its_target_script_and_comes_back_out)
{
	Builder b;
	b.floor();
	b.rules();
	const EntityId player = b.player({0, 0.9, 0});
	Components gate;
	gate.body = body(BodyKind::Kinematic);
	gate.colliders = {box({1.5, 1.5, 0.15})};
	gate.script = script("Gate", {{"open_offset", Vec3{0, 3, 0}}, {"speed", 3.0}, {"message", std::string("Open sesame")}});
	const EntityId door = b.add("Door", at(0, 1.5, -8), gate);
	Components press;
	press.script = script("Button", {{"target", EntityLink{door}}, {"once", false}, {"hint", std::string()}, {"travel", Vec3{0, 0, -0.05}}});
	const EntityId button = b.add("Button", at(1, 1.2, -1), press);
	(void)player;
	Run run(b.snapshot());
	run.ticks(30);
	ENGINE_CHECK(!run.flag(door, "opened")); // An empty room: only a button opens it.
	run.tick(pressed({Action::Interact}));
	ENGINE_CHECK(run.flag(door, "opened"));
	ENGINE_CHECK(run.game->hud().last_event == "Open sesame");
	ENGINE_CHECK_NEAR(run.position(button).z, -1.05, 1e-12);
	run.ticks(60);
	ENGINE_CHECK_NEAR(run.position(door).y, 4.5, 1e-12);
	ENGINE_CHECK_NEAR(run.position(button).z, -1.0, 1e-12); // Back out after half a second: it can be pressed again.
	run.tick(pressed({Action::Interact}));
	ENGINE_CHECK_NEAR(run.position(button).z, -1.05, 1e-12);
	const auto presses = run.scripts->runtime().field(button, "presses");
	ENGINE_CHECK(presses && std::holds_alternative<double>(*presses) && std::get<double>(*presses) == 2);
	ENGINE_CHECK(run.errors().empty());
}
