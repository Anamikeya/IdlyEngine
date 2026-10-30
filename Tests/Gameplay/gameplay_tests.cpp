// Gameplay against the real scene, command bus and Box3D world, with no window: input goes in, ticks run, and the
// results are read back. The last group plays the real sample scene from Samples/PhysicsShooter.

#include "engine/gameplay/game.h"
#include "engine/test/test.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::gameplay;
namespace fs = std::filesystem;

#define ENGINE_FAIL(text)                                               \
	do {                                                                \
		::engine::test::report_failure(__FILE__, __LINE__, (text));     \
		throw ::engine::test::RequireFailed{};                          \
	} while (false)

namespace {

constexpr std::chrono::nanoseconds fixed_step{16'666'667};
constexpr double tick_seconds = 0.016666667;

Transform at(double x, double y, double z)
{
	Transform t;
	t.position = {x, y, z};
	return t;
}

BehaviorComponent behavior(std::string type, std::vector<std::pair<std::string, double>> values = {})
{
	return {std::move(type), std::move(values)};
}

ColliderComponent box(Vec3 half, double density = 1000, double friction = 0.7, bool events = true)
{
	ColliderComponent c;
	c.half_extents = half;
	c.density = density;
	c.friction = friction;
	c.events = events;
	return c;
}

RigidBodyComponent body(BodyKind kind)
{
	RigidBodyComponent b;
	b.kind = kind;
	return b;
}

InputFrame held(std::initializer_list<Action> actions)
{
	InputFrame frame;
	for (const Action action : actions) {
		frame.held[static_cast<std::size_t>(action)] = true;
	}
	return frame;
}

InputFrame pressed(std::initializer_list<Action> actions)
{
	InputFrame frame = held(actions);
	frame.pressed = frame.held;
	return frame;
}

// A runtime scene, as a play session has one.
struct Arena {
	Scene scene;
	commands::CommandBus bus{scene, 0};
	commands::Context context{"test", 3};
	std::unique_ptr<physics::ScenePhysics> world;
	std::unique_ptr<Game> game;

	Arena()
	{
		auto created = physics::ScenePhysics::create();
		ENGINE_REQUIRE(static_cast<bool>(created));
		world = std::make_unique<physics::ScenePhysics>(std::move(created.value()));
		world->set_simulation_enabled(true);
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

	// A capsule on the floor at the origin looking down -Z, with a camera and a gun with the given settings.
	EntityId player(std::vector<std::pair<std::string, double>> weapon = {}, double z = 0)
	{
		Components c;
		c.body = body(BodyKind::Dynamic);
		c.body->lock_rotation = true;
		c.body->enable_sleep = false;
		ColliderComponent capsule;
		capsule.shape = ColliderShape::Capsule;
		capsule.radius = 0.35;
		capsule.half_height = 0.55;
		capsule.density = 130;
		capsule.friction = 0;
		capsule.events = true;
		c.colliders = {capsule};
		c.behaviors = {behavior("player"), behavior("weapon", std::move(weapon))};
		const auto id = add("Player", at(0, 0.9, z), c);
		Components view;
		view.camera = CameraComponent{};
		add("Camera", at(0, 0.6, 0), view, id);
		return id;
	}

	void rules(double time_limit = 90)
	{
		Components c;
		c.behaviors = {behavior("game_mode", {{"time_limit", time_limit}})};
		add("Rules", {}, c);
	}

	// Static target with health, straight ahead of the player at z.
	EntityId target(double z, double health = 50, int points = 100)
	{
		Components c;
		c.colliders = {box({0.5, 0.5, 0.1})};
		c.behaviors = {behavior("health", {{"max", health}}), behavior("target", {{"points", double(points)}})};
		return add("Target", at(0, 1.5, z), c);
	}

	void begin()
	{
		ENGINE_REQUIRE(static_cast<bool>(world->sync(scene)));
		game = Game::create(scene, bus, *world, fixed_step);
		ENGINE_REQUIRE(game != nullptr);
		ENGINE_CHECK(game->setup_issues().empty());
		game->start();
		settle();
	}

	void tick(const InputFrame& input = {})
	{
		game->set_input(input);
		game->pre_physics();
		const auto report = world->fixed_update(fixed_step, scene, bus, context);
		ENGINE_REQUIRE(static_cast<bool>(report));
		game->post_physics();
	}

	void ticks(int count, const InputFrame& input = {})
	{
		for (int i = 0; i < count; ++i) {
			tick(input);
		}
	}

	void settle() { ticks(20); }

	Vec3 position(EntityId id) const
	{
		const auto w = scene.world_transform(id);
		ENGINE_REQUIRE(w.has_value());
		return {w->matrix[12], w->matrix[13], w->matrix[14]};
	}
};

// ---- The sample scene, loaded from disk -------------------------------------------------------

const fs::path sample_scene = fs::path(GAMEENGINE_SAMPLES_DIR) / "PhysicsShooter/physics_shooter.scene";

SceneSnapshot read_sample()
{
	std::ifstream file(sample_scene, std::ios::binary);
	ENGINE_REQUIRE(static_cast<bool>(file));
	auto snapshot = read_scene(file);
	if (!snapshot) {
		ENGINE_FAIL(snapshot.error().message.c_str());
	}
	return snapshot.value();
}

struct SamplePlay : Arena {
	double yaw = 0, pitch = 0;
	EntityId player_id = 0;

	explicit SamplePlay(const SceneSnapshot& snapshot)
	{
		ENGINE_REQUIRE(static_cast<bool>(bus.execute(context, 0, commands::Command(commands::ReplaceScene{snapshot}))));
		ENGINE_REQUIRE(static_cast<bool>(world->sync(scene)));
		game = Game::create(scene, bus, *world, fixed_step);
		ENGINE_REQUIRE(game != nullptr);
		ENGINE_CHECK(game->setup_issues().empty());
		player_id = game->player();
		game->start();
	}

	EntityId find(const std::string& name) const
	{
		for (const auto& e : scene.snapshot().entities) {
			if (e.name == name) {
				return e.id;
			}
		}
		return 0;
	}

	std::vector<EntityId> targets() const
	{
		std::vector<EntityId> found;
		for (const auto& e : scene.snapshot().entities) {
			if (std::any_of(e.components.behaviors.begin(), e.components.behaviors.end(), [](const BehaviorComponent& b) { return b.type == "target"; })) {
				found.push_back(e.id);
			}
		}
		return found;
	}

	// Stands in front of a point, on its +Z side, and turns to face it, as moving the mouse would.
	void aim_from(Vec3 stand, Vec3 point)
	{
		ENGINE_REQUIRE(static_cast<bool>(bus.execute(context, bus.revision(), commands::Command(commands::SetTransform{player_id, at(stand.x, stand.y, stand.z)}))));
		tick();
		aim(point);
	}

	void aim(Vec3 point)
	{
		const Vec3 eye = position(game->camera());
		const double dx = point.x - eye.x, dy = point.y - eye.y, dz = point.z - eye.z;
		const double want_yaw = std::atan2(-dx, -dz), want_pitch = std::atan2(dy, std::hypot(dx, dz));
		const double sensitivity = 0.0022;
		game->look((yaw - want_yaw) / sensitivity, (pitch - want_pitch) / sensitivity);
		yaw = want_yaw;
		pitch = want_pitch;
		game->update_view();
	}
};

} // namespace

// ---- Input actions ---------------------------------------------------------------------------

ENGINE_TEST(input_map_turns_device_codes_into_actions)
{
	InputMap map = InputMap::shooter_defaults();
	map.set(code::w, true);
	map.set(12345, true); // Nothing bound: ignored.
	InputFrame frame = map.take();
	ENGINE_CHECK(frame.is_held(Action::MoveForward));
	ENGINE_CHECK(frame.was_pressed(Action::MoveForward));
	ENGINE_CHECK(!frame.is_held(Action::Fire));
	frame = map.take();
	ENGINE_CHECK(frame.is_held(Action::MoveForward));
	ENGINE_CHECK(!frame.was_pressed(Action::MoveForward)); // Pressed once, not every frame.
	map.set(code::w, false);
	ENGINE_CHECK(!map.take().is_held(Action::MoveForward));
	// A press and release between takes is not lost.
	map.set(code::space, true);
	map.set(code::space, false);
	frame = map.take();
	ENGINE_CHECK(frame.was_pressed(Action::Jump) && !frame.is_held(Action::Jump));
	map.set(code::mouse_left, true);
	map.release_all();
	ENGINE_CHECK(!map.take().is_held(Action::Fire));
}

ENGINE_TEST(a_scene_without_game_behaviors_runs_no_game)
{
	Arena a;
	a.floor();
	ENGINE_REQUIRE(static_cast<bool>(a.world->sync(a.scene)));
	ENGINE_CHECK(Game::create(a.scene, a.bus, *a.world, fixed_step) == nullptr);
}

ENGINE_TEST(unusable_behaviors_are_reported)
{
	Arena a;
	Components odd;
	odd.behaviors = {behavior("player"), behavior("no_such_thing")}; // A player with no body, and an unknown type.
	a.add("Odd", {}, odd);
	Components mover;
	mover.behaviors = {behavior("mover")}; // A mover with no kinematic body.
	a.add("Mover", {}, mover);
	ENGINE_REQUIRE(static_cast<bool>(a.world->sync(a.scene)));
	const auto game = Game::create(a.scene, a.bus, *a.world, fixed_step);
	ENGINE_REQUIRE(game != nullptr);
	ENGINE_CHECK_EQ(game->setup_issues().size(), std::size_t{3});
}

// ---- Player ----------------------------------------------------------------------------------

ENGINE_TEST(player_walks_where_it_faces_and_sprints_faster)
{
	Arena a;
	a.floor();
	const auto player = a.player();
	a.rules();
	a.begin();
	const Vec3 start = a.position(player);
	a.ticks(60, held({Action::MoveForward}));
	const Vec3 walked = a.position(player);
	ENGINE_CHECK(walked.z < start.z - 4.0); // About 5 m/s for a second, with a short acceleration.
	ENGINE_CHECK(std::abs(walked.x - start.x) < 0.01);
	a.ticks(60);
	const Vec3 stopped = a.position(player);
	ENGINE_CHECK(std::abs(stopped.z - walked.z) < 1.0); // Stops quickly once released.
	a.ticks(60, held({Action::MoveBack, Action::Sprint}));
	ENGINE_CHECK(a.position(player).z > stopped.z + 7.0); // About 8.5 m/s.
	const Vec3 side = a.position(player);
	a.ticks(30, held({Action::MoveRight}));
	ENGINE_CHECK(a.position(player).x > side.x + 1.5);
}

ENGINE_TEST(looking_turns_the_camera_and_the_walking_direction)
{
	Arena a;
	a.floor();
	const auto player = a.player();
	a.rules();
	a.begin();
	const auto eye = [&] { return a.scene.inspect(a.game->camera())->transform.rotation; };
	const auto before = eye();
	// A quarter turn right: yaw goes negative, so forward becomes +X.
	a.game->look(std::numbers::pi / 2 / 0.0022, 0);
	a.game->update_view();
	ENGINE_CHECK(eye() != before);
	const Vec3 start = a.position(player);
	a.ticks(60, held({Action::MoveForward}));
	ENGINE_CHECK(a.position(player).x > start.x + 4.0);
	ENGINE_CHECK(std::abs(a.position(player).z - start.z) < 0.05);
	// Pitch stops short of straight up, so the view never flips.
	a.game->look(0, -100000);
	a.game->update_view();
	const auto q = eye();
	ENGINE_CHECK(std::isfinite(q.w));
	const auto forward_y = 2 * (q.w * q.x - q.y * q.z);
	ENGINE_CHECK(forward_y > 0.99 && forward_y < 1.0);
}

ENGINE_TEST(player_stands_on_the_floor_and_is_stopped_by_walls)
{
	Arena a;
	a.floor();
	const auto player = a.player();
	a.rules();
	Components wall;
	wall.colliders = {box({5, 3, 0.5})};
	const auto wall_id = a.add("Wall", at(0, 3, -3), wall);
	a.begin();
	a.ticks(240, held({Action::MoveForward, Action::Sprint}));
	const Vec3 p = a.position(player);
	ENGINE_CHECK(p.z > -2.5 + 0.35 - 0.05); // The wall's face, plus the capsule's radius.
	ENGINE_CHECK(p.z < -1.5); // It did reach the wall.
	ENGINE_CHECK(std::abs(p.y - 0.9) < 0.05); // Still standing at capsule height.
	ENGINE_CHECK(a.game->grounded());
	ENGINE_CHECK_NEAR(a.position(wall_id).z, -3.0, 1e-3); // The wall did not move.
}

ENGINE_TEST(player_jumps_only_from_the_ground)
{
	Arena a;
	a.floor();
	const auto player = a.player();
	a.rules();
	a.begin();
	ENGINE_CHECK(a.game->grounded());
	const double rest = a.position(player).y;
	a.tick(pressed({Action::Jump}));
	double peak = rest;
	for (int i = 0; i < 20; ++i) {
		a.tick();
		peak = std::max(peak, a.position(player).y);
	}
	ENGINE_CHECK(!a.game->grounded());
	// Pressing again in the air adds nothing: v^2 / 2g is about 1.38 m for one jump.
	a.tick(pressed({Action::Jump}));
	for (int i = 0; i < 60; ++i) {
		a.tick();
		peak = std::max(peak, a.position(player).y);
	}
	ENGINE_CHECK(peak > rest + 1.1);
	ENGINE_CHECK(peak < rest + 1.6);
	ENGINE_CHECK(a.game->grounded());
	ENGINE_CHECK(std::abs(a.position(player).y - rest) < 0.05);
}

ENGINE_TEST(falling_out_of_the_arena_returns_the_player_to_the_start)
{
	Arena a;
	const auto player = a.player(); // No floor at all.
	a.rules();
	a.begin();
	for (int i = 0; i < 300 && a.position(player).y > -5; ++i) {
		a.tick();
	}
	ENGINE_CHECK(a.position(player).y <= -5);
	for (int i = 0; i < 300 && a.game->hud().fall_notice.empty(); ++i) {
		a.tick();
	}
	ENGINE_CHECK(!a.game->hud().fall_notice.empty());
	a.tick();
	ENGINE_CHECK(a.position(player).y > 0);
}

// ---- Weapon ----------------------------------------------------------------------------------

ENGINE_TEST(firing_rate_limits_shots_and_consumes_ammunition)
{
	Arena a;
	a.floor();
	a.player({{"fire_interval", 0.5}, {"magazine", 10}});
	a.rules();
	a.begin();
	ENGINE_CHECK_EQ(a.game->hud().ammo, 10);
	int shots = 0;
	for (int i = 0; i < 90; ++i) {
		a.tick(held({Action::Fire}));
		shots += static_cast<int>(a.game->shots().size());
	}
	ENGINE_CHECK_EQ(shots, 3); // 1.5 s at one shot per 0.5 s, starting at once.
	ENGINE_CHECK_EQ(a.game->hud().ammo, 7);
}

ENGINE_TEST(reload_takes_time_refills_from_reserve_and_blocks_firing)
{
	Arena a;
	a.floor();
	a.player({{"fire_interval", 0}, {"magazine", 4}, {"reserve", 5}, {"reload_time", 1}});
	a.rules();
	a.begin();
	a.ticks(4, held({Action::Fire}));
	ENGINE_CHECK_EQ(a.game->hud().ammo, 0);
	// Empty: the trigger does nothing further while held, and a fresh pull starts a reload.
	a.ticks(3, held({Action::Fire}));
	ENGINE_CHECK(!a.game->hud().reloading);
	a.tick(pressed({Action::Fire}));
	ENGINE_CHECK(a.game->hud().reloading);
	a.ticks(30, held({Action::Fire}));
	ENGINE_CHECK(a.game->hud().reloading); // Half way: no shots.
	ENGINE_CHECK_EQ(a.game->hud().ammo, 0);
	ENGINE_CHECK(a.game->hud().reload_progress > 0.4 && a.game->hud().reload_progress < 0.6);
	a.ticks(35);
	ENGINE_CHECK(!a.game->hud().reloading);
	ENGINE_CHECK_EQ(a.game->hud().ammo, 4);
	ENGINE_CHECK_EQ(a.game->hud().reserve, 1);
	// R with a partly empty magazine takes what is left in reserve.
	a.ticks(2, held({Action::Fire}));
	a.tick(pressed({Action::Reload}));
	a.ticks(65);
	ENGINE_CHECK_EQ(a.game->hud().ammo, 3);
	ENGINE_CHECK_EQ(a.game->hud().reserve, 0);
	// Nothing left: no reload starts.
	a.tick(pressed({Action::Reload}));
	ENGINE_CHECK(!a.game->hud().reloading);
}

ENGINE_TEST(a_full_magazine_cannot_be_reloaded)
{
	Arena a;
	a.floor();
	a.player({{"magazine", 4}});
	a.rules();
	a.begin();
	a.tick(pressed({Action::Reload}));
	ENGINE_CHECK(!a.game->hud().reloading);
}

ENGINE_TEST(shots_register_hits_damage_and_score)
{
	Arena a;
	a.floor();
	a.player({{"damage", 25}, {"fire_interval", 0.1}});
	a.rules();
	const auto target = a.target(-6, 50, 120);
	a.begin();
	ENGINE_CHECK_EQ(a.game->hud().targets_total, 1);
	a.tick(pressed({Action::Fire}));
	ENGINE_REQUIRE(a.game->shots().size() == 1);
	ENGINE_CHECK_EQ(a.game->shots()[0].entity, target);
	ENGINE_CHECK(a.game->shots()[0].damaged);
	ENGINE_CHECK(!a.game->shots()[0].pushed); // A static body takes no impulse.
	ENGINE_CHECK_EQ(a.game->damage_events().size(), std::size_t{1});
	ENGINE_CHECK(a.game->hud().hit_marker > 0.5);
	ENGINE_CHECK_EQ(a.game->hud().score, 0); // One hit of two.
	ENGINE_CHECK_NEAR(a.position(target).z, -6.0, 1e-9);
	a.ticks(10);
	a.tick(pressed({Action::Fire}));
	ENGINE_CHECK(!a.scene.inspect(target).has_value());
	ENGINE_CHECK_EQ(a.game->hud().score, 120);
	ENGINE_CHECK_EQ(a.game->hud().targets_remaining, 0);
	ENGINE_CHECK(!a.game->hud().last_event.empty());
	a.ticks(5);
	ENGINE_CHECK_EQ(a.game->hud().score, 120); // Destroyed once.
}

ENGINE_TEST(a_miss_hits_nothing_and_still_uses_a_round)
{
	Arena a;
	a.floor();
	a.player({{"magazine", 5}});
	a.rules();
	a.begin();
	a.game->look(0, -std::numbers::pi / 2 / 0.0022); // Straight up, at empty sky.
	a.game->update_view();
	a.tick(pressed({Action::Fire}));
	ENGINE_REQUIRE(a.game->shots().size() == 1);
	ENGINE_CHECK_EQ(a.game->shots()[0].entity, EntityId{0});
	ENGINE_CHECK_EQ(a.game->hud().ammo, 4);
}

ENGINE_TEST(a_shot_pushes_a_crate_in_its_direction_less_if_heavier)
{
	const auto pushed_by_shot = [](double density) {
		Arena a;
		a.floor();
		a.player({{"impulse", 100}});
		a.rules();
		Components crate;
		crate.body = body(BodyKind::Dynamic);
		// Tall enough to be in the line of fire at eye height. No friction, so the speed read below is the impulse's alone.
		crate.colliders = {box({1, 1, 1}, density, 0)};
		const auto id = a.add("Crate", at(0, 1, -6), crate);
		a.begin();
		const Vec3 start = a.position(id);
		a.tick(pressed({Action::Fire}));
		ENGINE_REQUIRE(a.game->shots().size() == 1);
		ENGINE_CHECK_EQ(a.game->shots()[0].entity, id);
		ENGINE_CHECK(a.game->shots()[0].pushed);
		ENGINE_CHECK(!a.game->shots()[0].damaged); // It has no health.
		const auto state = a.world->body_state(id);
		ENGINE_REQUIRE(state.has_value());
		const double speed = -state->linear_velocity.z;
		a.ticks(20);
		ENGINE_CHECK(a.position(id).z < start.z); // Away from the shooter.
		return speed;
	};
	const double light = pushed_by_shot(10), heavy = pushed_by_shot(40);
	ENGINE_CHECK(light > 1.0); // 100 N*s on 80 kg.
	ENGINE_CHECK_NEAR(light / heavy, 4.0, 0.4);
}

ENGINE_TEST(a_shot_at_a_wall_leaves_a_mark_and_does_not_move_it)
{
	Arena a;
	a.floor();
	a.player();
	a.rules();
	Components wall;
	wall.colliders = {box({5, 3, 0.5})};
	const auto id = a.add("Wall", at(0, 3, -6), wall);
	a.begin();
	const auto entities = a.scene.size();
	a.tick(pressed({Action::Fire}));
	ENGINE_REQUIRE(a.game->shots().size() == 1);
	ENGINE_CHECK_EQ(a.game->shots()[0].entity, id);
	ENGINE_CHECK(!a.game->shots()[0].pushed);
	ENGINE_CHECK_EQ(a.scene.size(), entities + 1); // The impact mark.
	ENGINE_CHECK_NEAR(a.game->shots()[0].point.z, -5.5f, 1e-3f);
	ENGINE_CHECK_NEAR(a.position(id).z, -6.0, 1e-9);
	a.ticks(60);
	ENGINE_CHECK_EQ(a.scene.size(), entities); // The mark has faded.
}

// ---- Targets ---------------------------------------------------------------------------------

ENGINE_TEST(a_dynamic_target_takes_damage_and_moves)
{
	Arena a;
	a.floor();
	a.player({{"impulse", 100}, {"fire_interval", 0.01}});
	a.rules();
	Components c;
	c.body = body(BodyKind::Dynamic);
	c.colliders = {box({1, 1, 1}, 20, 0)};
	c.behaviors = {behavior("health", {{"max", 60}}), behavior("target")};
	const auto id = a.add("Target", at(0, 1, -6), c);
	a.begin();
	const Vec3 start = a.position(id);
	a.tick(pressed({Action::Fire}));
	ENGINE_CHECK(a.game->shots().at(0).pushed && a.game->shots().at(0).damaged);
	a.ticks(10);
	ENGINE_CHECK(a.position(id).z < start.z - 0.05);
	ENGINE_CHECK(a.scene.inspect(id).has_value());
}

ENGINE_TEST(a_moving_target_follows_its_path_and_can_be_shot)
{
	Arena a;
	a.floor();
	a.player({{"damage", 100}});
	a.rules();
	Components c;
	c.body = body(BodyKind::Kinematic);
	ColliderComponent ball;
	ball.shape = ColliderShape::Sphere;
	c.colliders = {ball};
	c.behaviors = {behavior("health", {{"max", 50}}), behavior("target"), behavior("mover", {{"ax", -4}, {"ay", 1.5}, {"az", -6}, {"bx", 4}, {"by", 1.5}, {"bz", -6}, {"speed", 4}})};
	const auto id = a.add("Mover", at(-4, 1.5, -6), c);
	a.begin();
	// Back and forth, at the path's speed, without leaving it.
	double smallest = 1e9, largest = -1e9;
	for (int i = 0; i < 300; ++i) {
		a.tick();
		const Vec3 p = a.position(id);
		smallest = std::min(smallest, p.x);
		largest = std::max(largest, p.x);
		ENGINE_CHECK_NEAR(p.y, 1.5, 1e-6);
	}
	ENGINE_CHECK(smallest < -3.8 && smallest >= -4.01);
	ENGINE_CHECK(largest > 3.8 && largest <= 4.01);
	const double moved_from = a.position(id).x;
	a.tick();
	ENGINE_CHECK_NEAR(std::abs(a.position(id).x - moved_from), 4 * tick_seconds, 0.01);
	// The body really is there: wait for it to cross the line of fire, then fire.
	for (int i = 0; i < 300 && std::abs(a.position(id).x) > 0.2; ++i) {
		a.tick();
	}
	a.tick(pressed({Action::Fire}));
	ENGINE_CHECK_EQ(a.game->shots().at(0).entity, id);
	ENGINE_CHECK(!a.scene.inspect(id).has_value());
}

ENGINE_TEST(a_hard_collision_hurts_a_target_with_impact_damage)
{
	Arena a;
	a.floor();
	a.player();
	a.rules();
	Components c;
	c.body = body(BodyKind::Dynamic);
	c.colliders = {box({0.4, 0.4, 0.4}, 100)};
	c.behaviors = {behavior("health", {{"max", 10}}), behavior("target"), behavior("impact_damage", {{"min_speed", 3}, {"scale", 6}})};
	const auto id = a.add("Target", at(4, 6, 4), c);
	a.begin(); // Not long enough to land.
	ENGINE_CHECK(a.scene.inspect(id).has_value());
	for (int i = 0; i < 120 && a.scene.inspect(id); ++i) {
		a.tick();
	}
	ENGINE_CHECK(!a.scene.inspect(id).has_value());
	ENGINE_CHECK_EQ(a.game->hud().targets_remaining, 0);
}

ENGINE_TEST(a_gentle_landing_does_no_impact_damage)
{
	Arena a;
	a.floor();
	a.player();
	a.rules();
	Components c;
	c.body = body(BodyKind::Dynamic);
	c.colliders = {box({0.4, 0.4, 0.4}, 100)};
	c.behaviors = {behavior("health", {{"max", 10}}), behavior("target"), behavior("impact_damage", {{"min_speed", 3}, {"scale", 6}})};
	const auto id = a.add("Target", at(4, 0.5, 4), c);
	a.begin();
	a.ticks(60);
	ENGINE_CHECK(a.scene.inspect(id).has_value());
}

ENGINE_TEST(a_bonus_zone_scores_each_dynamic_body_once)
{
	Arena a;
	a.floor();
	a.player();
	a.rules();
	Components zone;
	ColliderComponent sensor = box({2, 1, 2});
	sensor.sensor = true;
	zone.colliders = {sensor};
	zone.behaviors = {behavior("bonus_zone", {{"points", 50}})};
	a.add("Zone", at(5, 1, -5), zone);
	Components crate;
	crate.body = body(BodyKind::Dynamic);
	crate.colliders = {box({0.3, 0.3, 0.3}, 100, 0.2)};
	crate.body->linear_velocity = {0, 0, 0};
	a.add("Crate", at(5, 4, -5), crate); // Falls through the sensor.
	auto other = crate;
	a.add("Other", at(5.5, 5, -5.5), other);
	a.begin();
	a.ticks(120);
	ENGINE_CHECK_EQ(a.game->hud().score, 100);
	a.ticks(60);
	ENGINE_CHECK_EQ(a.game->hud().score, 100);
}

ENGINE_TEST(walking_through_a_pickup_collects_it)
{
	Arena a;
	a.floor();
	const auto player = a.player({{"reserve", 10}});
	(void)player;
	a.rules(50);
	Components time;
	ColliderComponent sensor;
	sensor.shape = ColliderShape::Sphere;
	sensor.radius = 0.6;
	sensor.sensor = true;
	sensor.events = true;
	time.colliders = {sensor};
	time.behaviors = {behavior("pickup", {{"ammo", 20}, {"time", 15}})};
	const auto id = a.add("Pickup", at(0, 1, -3), time);
	a.begin();
	const double before = a.game->hud().time_remaining;
	a.ticks(90, held({Action::MoveForward}));
	ENGINE_CHECK(!a.scene.inspect(id).has_value());
	ENGINE_CHECK_EQ(a.game->hud().reserve, 30);
	ENGINE_CHECK(a.game->hud().time_remaining > before + 12);
}

// ---- Game rules ------------------------------------------------------------------------------

ENGINE_TEST(destroying_every_target_wins)
{
	Arena a;
	a.floor();
	a.player({{"damage", 100}, {"fire_interval", 0}});
	a.rules();
	a.target(-6);
	a.begin();
	ENGINE_CHECK(a.game->phase() == GamePhase::Playing);
	a.tick(pressed({Action::Fire}));
	ENGINE_CHECK(a.game->phase() == GamePhase::Won);
	// The game is over: input does nothing.
	const int ammo = a.game->hud().ammo;
	a.ticks(10, held({Action::Fire}));
	ENGINE_CHECK_EQ(a.game->hud().ammo, ammo);
	ENGINE_CHECK(a.game->phase() == GamePhase::Won);
}

ENGINE_TEST(running_out_of_time_loses)
{
	Arena a;
	a.floor();
	a.player();
	a.rules(1);
	a.target(-6);
	a.begin();
	ENGINE_CHECK(a.game->phase() == GamePhase::Playing);
	a.ticks(60);
	ENGINE_CHECK(a.game->phase() == GamePhase::Lost);
	ENGINE_CHECK_NEAR(a.game->hud().time_remaining, 0.0, 1e-9);
}

ENGINE_TEST(the_clock_runs_only_while_playing)
{
	Arena a;
	a.floor();
	a.player();
	a.rules(10);
	a.target(-6);
	ENGINE_REQUIRE(static_cast<bool>(a.world->sync(a.scene)));
	a.game = Game::create(a.scene, a.bus, *a.world, fixed_step);
	ENGINE_CHECK(a.game->phase() == GamePhase::Ready);
	a.ticks(30, held({Action::Fire, Action::MoveForward}));
	ENGINE_CHECK_NEAR(a.game->hud().time_remaining, 10.0, 1e-9);
	ENGINE_CHECK_EQ(a.game->hud().ammo, a.game->hud().magazine); // No input before the start.
	a.game->start();
	a.ticks(60);
	ENGINE_CHECK_NEAR(a.game->hud().time_remaining, 9.0, 0.01);
	a.game->pause();
	ENGINE_CHECK(a.game->phase() == GamePhase::Paused);
	a.ticks(60, held({Action::Fire}));
	ENGINE_CHECK_NEAR(a.game->hud().time_remaining, 9.0, 0.01);
	ENGINE_CHECK_EQ(a.game->hud().ammo, a.game->hud().magazine);
	a.game->resume();
	a.ticks(60);
	ENGINE_CHECK_NEAR(a.game->hud().time_remaining, 8.0, 0.01);
}

ENGINE_TEST(the_same_input_gives_the_same_game)
{
	const auto run = [] {
		Arena a;
		a.floor();
		a.player({{"impulse", 80}, {"fire_interval", 0.1}});
		a.rules();
		a.target(-20);
		for (int i = 0; i < 4; ++i) {
			Components crate;
			crate.body = body(BodyKind::Dynamic);
			crate.colliders = {box({0.5, 0.5, 0.5}, 80)};
			a.add("Crate", at(0.2 * i, 0.5 + i * 1.02, -6), crate);
		}
		a.begin();
		for (int i = 0; i < 300; ++i) {
			InputFrame input = i % 40 < 20 ? held({Action::Fire, Action::MoveForward}) : held({Action::MoveRight});
			if (i % 90 == 0) {
				input.pressed[static_cast<std::size_t>(Action::Jump)] = true;
			}
			a.game->look(i % 7, i % 5 - 2);
			a.game->update_view();
			a.tick(input);
		}
		return a.scene.snapshot();
	};
	ENGINE_CHECK(run() == run());
}

// ---- The sample ------------------------------------------------------------------------------

ENGINE_TEST(sample_scene_loads_with_every_behavior_understood)
{
	SamplePlay play(read_sample());
	ENGINE_CHECK_EQ(play.game->hud().targets_total, 9);
	ENGINE_CHECK_NEAR(play.game->hud().time_limit, 90.0, 1e-9);
	ENGINE_CHECK(play.game->camera() != 0);
	ENGINE_CHECK(play.world->diagnostics().issues.empty());
	ENGINE_CHECK(play.world->diagnostics().active_bodies > 40);
}

ENGINE_TEST(sample_scene_settles_without_exploding)
{
	const SceneSnapshot authored = read_sample();
	SamplePlay play(authored);
	play.ticks(300);
	for (const char* name : {"Crate 0-0", "Crate 2-1", "Domino 1", "Domino 10", "Stack Crate 6", "Shelf Crate 3", "Dynamic Target 1", "Dynamic Target 2", "Chain Target", "Water Bottle Target"}) {
		const EntityId id = play.find(name);
		if (!id) {
			ENGINE_FAIL(name); // Nothing is destroyed by standing still.
		}
		const auto before = std::find_if(authored.entities.begin(), authored.entities.end(), [&](const EntityRecord& e) { return e.id == id; });
		const Vec3 now = play.position(id), then = before->transform.position;
		if (std::hypot(now.x - then.x, now.y - then.y, now.z - then.z) > 0.15) {
			ENGINE_FAIL(name); // It should rest where the scene put it.
		}
	}
	ENGINE_CHECK_EQ(play.game->hud().targets_remaining, 9);
	ENGINE_CHECK_EQ(play.game->hud().score, 0);
	// Everything is on the floor or above it, and the player has not moved.
	for (const auto& e : play.scene.snapshot().entities) {
		if (e.components.body && e.components.body->kind == BodyKind::Dynamic) {
			ENGINE_CHECK(e.transform.position.y > 0);
		}
	}
	ENGINE_CHECK_NEAR(play.position(play.player_id).z, 12.0, 0.01);
	ENGINE_CHECK(play.game->grounded());
}

ENGINE_TEST(sample_can_be_won_by_shooting_every_target)
{
	SamplePlay play(read_sample());
	play.ticks(30);
	int guard = 0;
	while (play.game->phase() == GamePhase::Playing && guard++ < 20) {
		const auto remaining = play.targets();
		if (remaining.empty()) {
			break;
		}
		const EntityId id = remaining.front();
		const auto name = play.scene.inspect(id)->name;
		// Stand 3 m away and fire until it is gone, moving to another side every so often in case something is in the way.
		constexpr std::array<std::pair<double, double>, 4> sides{{{0, 3.2}, {-3.2, 0}, {3.2, 0}, {0, -3.2}}};
		for (int tries = 0; tries < 400 && play.scene.inspect(id); ++tries) {
			const Vec3 p = play.position(id);
			if (tries % 25 == 0) {
				const auto [dx, dz] = sides[static_cast<std::size_t>(tries / 25) % sides.size()];
				play.aim_from({std::clamp(p.x + dx, -15.0, 15.0), std::max(p.y - 0.4, 1.0), std::clamp(p.z + dz, -15.0, 15.0)}, p);
			} else {
				play.aim(p);
			}
			play.tick(held({Action::Fire}));
			if (play.game->hud().ammo == 0) {
				play.tick(pressed({Action::Reload}));
				play.ticks(80);
			}
		}
		if (play.scene.inspect(id)) {
			ENGINE_FAIL(name.c_str());
		}
	}
	ENGINE_CHECK(play.game->phase() == GamePhase::Won);
	ENGINE_CHECK(play.game->hud().score >= 1250);
	ENGINE_CHECK_EQ(play.game->hud().targets_remaining, 0);
}

ENGINE_TEST(sample_glass_breaks_without_counting_as_a_target)
{
	SamplePlay play(read_sample());
	play.ticks(30);
	const EntityId glass = play.find("Glass Barrier");
	ENGINE_REQUIRE(glass != 0);
	play.aim(play.position(glass));
	play.ticks(40, held({Action::Fire}));
	ENGINE_CHECK(!play.scene.inspect(glass).has_value());
	ENGINE_CHECK_EQ(play.game->hud().targets_remaining, 9);
	ENGINE_CHECK_EQ(play.game->hud().score, 0);
}

ENGINE_TEST(sample_is_lost_when_the_time_runs_out)
{
	SamplePlay play(read_sample());
	play.ticks(static_cast<int>(90 / tick_seconds) + 5);
	ENGINE_CHECK(play.game->phase() == GamePhase::Lost);
	ENGINE_CHECK_EQ(play.game->hud().targets_remaining, 9);
}

ENGINE_TEST(sample_restart_starts_over_from_the_scene)
{
	const SceneSnapshot authored = read_sample();
	int ammo_after = 0;
	{
		SamplePlay first(authored);
		first.ticks(20);
		first.ticks(20, held({Action::Fire}));
		ammo_after = first.game->hud().ammo;
	}
	ENGINE_CHECK(ammo_after < 15);
	SamplePlay second(authored);
	ENGINE_CHECK_EQ(second.game->hud().ammo, 15);
	ENGINE_CHECK_EQ(second.game->hud().score, 0);
	ENGINE_CHECK_EQ(second.targets().size(), std::size_t{9});
	ENGINE_CHECK(second.scene.snapshot().entities == authored.entities);
}

ENGINE_TEST(sample_domino_chain_destroys_its_target_with_one_push)
{
	SamplePlay play(read_sample());
	play.ticks(60);
	const EntityId chain = play.find("Chain Target"), first = play.find("Domino 1");
	ENGINE_REQUIRE(chain != 0 && first != 0);
	const Vec3 p = play.position(first);
	// From straight in front of the first slab, high up, so it tips over away from the shooter.
	play.aim_from({p.x, 1.0, p.z + 5}, {p.x, 1.75, p.z});
	play.ticks(3);
	play.tick(pressed({Action::Fire}));
	ENGINE_REQUIRE(play.game->shots().size() == 1);
	ENGINE_CHECK_EQ(play.game->shots()[0].entity, first);
	for (int i = 0; i < 480 && play.scene.inspect(chain); ++i) {
		play.tick();
	}
	ENGINE_CHECK(!play.scene.inspect(chain).has_value());
	ENGINE_CHECK_EQ(play.game->hud().targets_remaining, 8);
}

ENGINE_TEST(sample_crate_pushed_off_the_shelf_scores_in_the_bonus_zone)
{
	SamplePlay play(read_sample());
	play.ticks(60);
	const EntityId crate = play.find("Shelf Crate 3");
	ENGINE_REQUIRE(crate != 0);
	const Vec3 p = play.position(crate);
	play.aim_from({p.x, 1.0, p.z + 6}, p);
	int score = 0;
	for (int i = 0; i < 300 && !score; ++i) {
		play.aim(play.position(crate));
		play.tick(held({Action::Fire}));
		score = play.game->hud().score;
	}
	ENGINE_CHECK_EQ(score, 50);
}
