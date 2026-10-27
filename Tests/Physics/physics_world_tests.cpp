#include "engine/physics/physics_world.h"
#include "engine/test/test.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

using namespace engine::physics;
using namespace std::chrono_literals;

namespace {

PhysicsWorld make_world(WorldConfig config = {})
{
	auto result = PhysicsWorld::create(config);
	ENGINE_REQUIRE(result.ok());
	return std::move(result).value();
}

BodyHandle make_ground(PhysicsWorld& world)
{
	BodyDesc desc;
	desc.type = BodyType::Static;
	desc.pose.position = {0.0f, -0.5f, 0.0f};
	auto body = world.create_body(desc);
	ENGINE_REQUIRE(body.ok());
	ColliderDesc collider;
	collider.shape = BoxShape{{50.0f, 0.5f, 50.0f}, {}};
	collider.enable_contact_events = true;
	ENGINE_REQUIRE(world.add_collider(body.value(), collider).ok());
	return body.value();
}

BodyHandle make_ball(PhysicsWorld& world, Vec3 position, std::uint64_t key = 0, bool contact_events = false)
{
	BodyDesc desc;
	desc.pose.position = position;
	desc.user_key = key;
	auto body = world.create_body(desc);
	ENGINE_REQUIRE(body.ok());
	ColliderDesc collider;
	collider.shape = SphereShape{0.5f, {}};
	collider.enable_contact_events = contact_events;
	ENGINE_REQUIRE(world.add_collider(body.value(), collider).ok());
	return body.value();
}

} // namespace

// ---- World lifecycle -----------------------------------------------------------------------

ENGINE_TEST(world_create_and_destroy)
{
	const int before = live_world_count();
	{
		PhysicsWorld world = make_world();
		ENGINE_CHECK(world.valid());
		ENGINE_CHECK_EQ(live_world_count(), before + 1);
		ENGINE_CHECK_EQ(world.stats().body_count, 0);
	}
	ENGINE_CHECK_EQ(live_world_count(), before);
}

ENGINE_TEST(world_default_constructed_is_invalid)
{
	PhysicsWorld world;
	ENGINE_CHECK(!world.valid());
	ENGINE_CHECK(world.step() == PhysicsError::InvalidWorld);
	ENGINE_CHECK(world.create_body({}).error() == PhysicsError::InvalidWorld);
	ENGINE_CHECK_EQ(world.advance(1s).steps, 0);
	ENGINE_CHECK(world.moved_bodies().empty());
}

ENGINE_TEST(world_rejects_invalid_config)
{
	WorldConfig zero_step;
	zero_step.fixed_timestep = 0ns;
	ENGINE_CHECK(PhysicsWorld::create(zero_step).error() == PhysicsError::InvalidConfig);

	WorldConfig no_substeps;
	no_substeps.sub_steps = 0;
	ENGINE_CHECK(PhysicsWorld::create(no_substeps).error() == PhysicsError::InvalidConfig);

	WorldConfig bad_gravity;
	bad_gravity.gravity.y = std::numeric_limits<float>::quiet_NaN();
	ENGINE_CHECK(PhysicsWorld::create(bad_gravity).error() == PhysicsError::InvalidConfig);

	WorldConfig no_workers;
	no_workers.worker_count = 0;
	ENGINE_CHECK(PhysicsWorld::create(no_workers).error() == PhysicsError::InvalidConfig);
}

ENGINE_TEST(world_move_transfers_ownership)
{
	const int before = live_world_count();
	PhysicsWorld a = make_world();
	const BodyHandle ball = make_ball(a, {0, 5, 0});
	PhysicsWorld b = std::move(a);
	ENGINE_CHECK(!a.valid()); // NOLINT(bugprone-use-after-move): moved-from state is specified
	ENGINE_CHECK(b.valid());
	ENGINE_CHECK(b.is_valid(ball));
	ENGINE_CHECK_EQ(live_world_count(), before + 1);
	b.reset();
	b.reset(); // idempotent
	ENGINE_CHECK(!b.valid());
	ENGINE_CHECK_EQ(live_world_count(), before);
}

ENGINE_TEST(world_reports_limit_instead_of_asserting)
{
	std::vector<PhysicsWorld> worlds;
	PhysicsError last = PhysicsError::None;
	for (int i = 0; i < 1024; ++i) {
		auto result = PhysicsWorld::create();
		if (!result.ok()) {
			last = result.error();
			break;
		}
		worlds.push_back(std::move(result).value());
	}
	ENGINE_CHECK(last == PhysicsError::WorldLimitReached);
	ENGINE_CHECK(!worlds.empty());
	worlds.clear();
	ENGINE_CHECK_EQ(live_world_count(), 0);
}

ENGINE_TEST(world_destruction_releases_box3d_memory)
{
	const std::int64_t before = allocated_bytes();
	{
		PhysicsWorld world = make_world();
		make_ground(world);
		for (int i = 0; i < 100; ++i) {
			make_ball(world, {0.0f, 1.0f + static_cast<float>(i) * 1.1f, 0.0f});
		}
		for (int i = 0; i < 30; ++i) {
			world.step();
		}
		ENGINE_CHECK(allocated_bytes() > before);
	}
	ENGINE_CHECK_EQ(allocated_bytes(), before);
}

// ---- Fixed timestep ------------------------------------------------------------------------

ENGINE_TEST(step_advances_exactly_one_tick)
{
	PhysicsWorld world = make_world();
	ENGINE_CHECK(world.step() == PhysicsError::None);
	ENGINE_CHECK(world.step() == PhysicsError::None);
	ENGINE_CHECK_EQ(world.stats().step_count, 2u);
}

ENGINE_TEST(advance_accumulates_partial_ticks)
{
	WorldConfig config;
	config.fixed_timestep = 10ms;
	PhysicsWorld world = make_world(config);

	AdvanceReport r = world.advance(4ms);
	ENGINE_CHECK_EQ(r.steps, 0);
	ENGINE_CHECK_NEAR(r.interpolation_alpha, 0.4, 1e-6);

	r = world.advance(7ms); // 11ms accumulated
	ENGINE_CHECK_EQ(r.steps, 1);
	ENGINE_CHECK_NEAR(r.interpolation_alpha, 0.1, 1e-6);

	r = world.advance(29ms); // 30ms accumulated
	ENGINE_CHECK_EQ(r.steps, 3);
	ENGINE_CHECK_NEAR(r.interpolation_alpha, 0.0, 1e-6);
	ENGINE_CHECK_EQ(world.stats().step_count, 4u);
}

ENGINE_TEST(advance_caps_catch_up_and_reports_dropped_time)
{
	WorldConfig config;
	config.fixed_timestep = 10ms;
	config.max_steps_per_advance = 3;
	PhysicsWorld world = make_world(config);

	const AdvanceReport r = world.advance(105ms);
	ENGINE_CHECK_EQ(r.steps, 3);
	ENGINE_CHECK_EQ(r.dropped.count(), std::chrono::nanoseconds(70ms).count());
	ENGINE_CHECK_NEAR(r.interpolation_alpha, 0.5, 1e-6);
}

ENGINE_TEST(advance_ignores_negative_elapsed)
{
	PhysicsWorld world = make_world();
	const AdvanceReport r = world.advance(-1s);
	ENGINE_CHECK_EQ(r.steps, 0);
	ENGINE_CHECK_EQ(world.stats().step_count, 0u);
}

ENGINE_TEST(free_fall_matches_gravity)
{
	PhysicsWorld world = make_world();
	const BodyHandle ball = make_ball(world, {0, 100, 0});

	// One second of simulated time at the default 60 Hz tick.
	for (int i = 0; i < 60; ++i) {
		ENGINE_REQUIRE(world.step() == PhysicsError::None);
	}
	const double t = 60.0 * std::chrono::duration<double>(world.config().fixed_timestep).count();
	const Vec3 v = world.linear_velocity(ball).value();
	ENGINE_CHECK_NEAR(v.y, -9.81 * t, 0.01);
	// Semi-implicit Euler drops slightly further than the analytic 0.5*g*t^2 (~4.905 m).
	const float fallen = 100.0f - world.body_pose(ball).value().position.y;
	ENGINE_CHECK_NEAR(fallen, 0.5 * 9.81 * t * t, 0.1);
}

ENGINE_TEST(simulation_is_deterministic_for_equal_inputs)
{
	auto run = [] {
		PhysicsWorld world = make_world();
		make_ground(world);
		std::vector<BodyHandle> bodies;
		for (int i = 0; i < 20; ++i) {
			const float f = static_cast<float>(i);
			bodies.push_back(make_ball(world, {0.3f * std::sin(f), 1.0f + f * 1.05f, 0.3f * std::cos(f)}));
		}
		for (int i = 0; i < 240; ++i) {
			world.step();
		}
		std::vector<Pose> poses;
		for (BodyHandle b : bodies) {
			poses.push_back(world.body_pose(b).value());
		}
		return poses;
	};
	const auto first = run();
	const auto second = run();
	ENGINE_REQUIRE(first.size() == second.size());
	for (std::size_t i = 0; i < first.size(); ++i) {
		ENGINE_CHECK(first[i] == second[i]); // bitwise-equal floats
	}
}

// ---- Rigid body lifecycle ------------------------------------------------------------------

ENGINE_TEST(body_create_and_destroy)
{
	PhysicsWorld world = make_world();
	const BodyHandle ball = make_ball(world, {1, 2, 3}, 42);
	ENGINE_CHECK(!ball.is_null());
	ENGINE_CHECK(world.is_valid(ball));
	ENGINE_CHECK_EQ(world.stats().body_count, 1);
	ENGINE_CHECK_EQ(world.stats().collider_count, 1);
	ENGINE_CHECK_EQ(world.user_key(ball).value(), 42u);
	ENGINE_CHECK(world.body_pose(ball).value().position == (Vec3{1, 2, 3}));

	ENGINE_CHECK(world.destroy_body(ball) == PhysicsError::None);
	ENGINE_CHECK(!world.is_valid(ball));
	ENGINE_CHECK_EQ(world.stats().body_count, 0);
	ENGINE_CHECK_EQ(world.stats().collider_count, 0);
	ENGINE_CHECK(world.destroy_body(ball) == PhysicsError::InvalidBody);
	ENGINE_CHECK(world.body_pose(ball).error() == PhysicsError::InvalidBody);
}

ENGINE_TEST(stale_handle_stays_invalid_after_slot_reuse)
{
	PhysicsWorld world = make_world();
	const BodyHandle first = make_ball(world, {0, 0, 0}, 1);
	world.destroy_body(first);
	const BodyHandle second = make_ball(world, {0, 0, 0}, 2);
	ENGINE_CHECK(!(first == second));
	ENGINE_CHECK(!world.is_valid(first));
	ENGINE_CHECK(world.is_valid(second));
	ENGINE_CHECK_EQ(world.user_key(second).value(), 2u);
}

ENGINE_TEST(handle_from_other_world_is_rejected)
{
	PhysicsWorld a = make_world();
	PhysicsWorld b = make_world();
	const BodyHandle in_a = make_ball(a, {0, 0, 0});
	ENGINE_CHECK(a.is_valid(in_a));
	ENGINE_CHECK(!b.is_valid(in_a));
	ENGINE_CHECK(b.destroy_body(in_a) == PhysicsError::InvalidBody);
	ENGINE_CHECK(a.is_valid(in_a));
}

ENGINE_TEST(body_rejects_invalid_desc)
{
	PhysicsWorld world = make_world();
	BodyDesc nan_position;
	nan_position.pose.position.x = std::numeric_limits<float>::quiet_NaN();
	ENGINE_CHECK(world.create_body(nan_position).error() == PhysicsError::InvalidArgument);

	BodyDesc zero_rotation;
	zero_rotation.pose.rotation = {0, 0, 0, 0};
	ENGINE_CHECK(world.create_body(zero_rotation).error() == PhysicsError::InvalidArgument);

	BodyDesc negative_damping;
	negative_damping.linear_damping = -1.0f;
	ENGINE_CHECK(world.create_body(negative_damping).error() == PhysicsError::InvalidArgument);
	ENGINE_CHECK_EQ(world.stats().body_count, 0);
}

ENGINE_TEST(body_rotation_is_normalized)
{
	PhysicsWorld world = make_world();
	BodyDesc desc;
	desc.type = BodyType::Static;
	desc.pose.rotation = {0, 2, 0, 2}; // 90 degrees about Y, unnormalized
	const BodyHandle body = world.create_body(desc).value();
	const Quat q = world.body_pose(body).value().rotation;
	ENGINE_CHECK_NEAR(q.y, std::sqrt(0.5), 1e-5);
	ENGINE_CHECK_NEAR(q.w, std::sqrt(0.5), 1e-5);
}

ENGINE_TEST(static_body_does_not_move)
{
	PhysicsWorld world = make_world();
	const BodyHandle ground = make_ground(world);
	for (int i = 0; i < 30; ++i) {
		world.step();
	}
	ENGINE_CHECK(world.body_pose(ground).value().position == (Vec3{0, -0.5f, 0}));
	ENGINE_CHECK_EQ(world.mass(ground).value(), 0.0f);
}

ENGINE_TEST(dynamic_body_mass_follows_density)
{
	PhysicsWorld world = make_world();
	BodyDesc desc;
	const BodyHandle body = world.create_body(desc).value();
	ColliderDesc collider;
	collider.shape = BoxShape{{0.5f, 0.5f, 0.5f}, {}};
	collider.density = 2.0f;
	ENGINE_REQUIRE(world.add_collider(body, collider).ok());
	ENGINE_CHECK_NEAR(world.mass(body).value(), 2.0, 1e-4); // 1 m^3 * 2 kg/m^3
}

ENGINE_TEST(ball_comes_to_rest_on_ground)
{
	PhysicsWorld world = make_world();
	make_ground(world);
	const BodyHandle ball = make_ball(world, {0, 3, 0});
	for (int i = 0; i < 300; ++i) {
		world.step();
	}
	const Pose pose = world.body_pose(ball).value();
	ENGINE_CHECK_NEAR(pose.position.y, 0.5, 0.02); // radius above the ground plane at y = 0
	ENGINE_CHECK(!world.is_awake(ball).value());
}

ENGINE_TEST(impulse_changes_velocity_by_impulse_over_mass)
{
	WorldConfig config;
	config.gravity = {0, 0, 0};
	PhysicsWorld world = make_world(config);
	const BodyHandle ball = make_ball(world, {0, 0, 0});
	const float m = world.mass(ball).value();
	ENGINE_REQUIRE(m > 0.0f);
	ENGINE_CHECK(world.apply_linear_impulse(ball, {m * 3.0f, 0, 0}) == PhysicsError::None);
	ENGINE_CHECK_NEAR(world.linear_velocity(ball).value().x, 3.0, 1e-4);
}

ENGINE_TEST(set_velocity_and_teleport)
{
	WorldConfig config;
	config.gravity = {0, 0, 0};
	PhysicsWorld world = make_world(config);
	const BodyHandle ball = make_ball(world, {0, 0, 0});
	ENGINE_CHECK(world.set_linear_velocity(ball, {0, 0, 6}) == PhysicsError::None);
	ENGINE_CHECK(world.set_angular_velocity(ball, {0, 1, 0}) == PhysicsError::None);
	ENGINE_CHECK_NEAR(world.angular_velocity(ball).value().y, 1.0, 1e-6);
	for (int i = 0; i < 60; ++i) {
		world.step();
	}
	ENGINE_CHECK_NEAR(world.body_pose(ball).value().position.z, 6.0, 0.01);

	ENGINE_CHECK(world.set_body_pose(ball, {{10, 0, 0}, {}}) == PhysicsError::None);
	ENGINE_CHECK(world.body_pose(ball).value().position == (Vec3{10, 0, 0}));
	ENGINE_CHECK(world.set_linear_velocity(ball, {std::numeric_limits<float>::infinity(), 0, 0}) ==
		PhysicsError::InvalidArgument);
}

ENGINE_TEST(kinematic_body_reaches_target)
{
	PhysicsWorld world = make_world();
	BodyDesc desc;
	desc.type = BodyType::Kinematic;
	const BodyHandle platform = world.create_body(desc).value();
	ENGINE_CHECK(world.move_kinematic(platform, {{1, 0, 0}, {}}) == PhysicsError::None);
	world.step();
	ENGINE_CHECK_NEAR(world.body_pose(platform).value().position.x, 1.0, 1e-4);

	const BodyHandle ball = make_ball(world, {0, 0, 0});
	ENGINE_CHECK(world.move_kinematic(ball, {{1, 0, 0}, {}}) == PhysicsError::InvalidArgument);
}

// ---- Colliders -----------------------------------------------------------------------------

ENGINE_TEST(collider_shapes_and_destruction)
{
	PhysicsWorld world = make_world();
	const BodyHandle body = world.create_body({}).value();

	ColliderDesc sphere;
	sphere.shape = SphereShape{0.5f, {}};
	ColliderDesc box;
	box.shape = BoxShape{{0.5f, 1.0f, 0.25f}, {{0, 1, 0}, {}}};
	ColliderDesc capsule;
	capsule.shape = CapsuleShape{{0, -1, 0}, {0, 1, 0}, 0.3f};

	const ColliderHandle a = world.add_collider(body, sphere).value();
	const ColliderHandle b = world.add_collider(body, box).value();
	const ColliderHandle c = world.add_collider(body, capsule).value();
	ENGINE_CHECK_EQ(world.stats().collider_count, 3);
	ENGINE_CHECK(world.collider_body(b) == body);

	const float mass_all = world.mass(body).value();
	ENGINE_CHECK(world.destroy_collider(b) == PhysicsError::None);
	ENGINE_CHECK(!world.is_valid(b));
	ENGINE_CHECK(world.is_valid(a) && world.is_valid(c));
	ENGINE_CHECK(world.mass(body).value() < mass_all); // mass is recomputed
	ENGINE_CHECK(world.destroy_collider(b) == PhysicsError::InvalidCollider);

	world.destroy_body(body);
	ENGINE_CHECK(!world.is_valid(a));
	ENGINE_CHECK(!world.is_valid(c));
	ENGINE_CHECK_EQ(world.stats().collider_count, 0);
}

ENGINE_TEST(collider_rejects_invalid_desc)
{
	PhysicsWorld world = make_world();
	const BodyHandle body = world.create_body({}).value();

	ColliderDesc zero_radius;
	zero_radius.shape = SphereShape{0.0f, {}};
	ENGINE_CHECK(world.add_collider(body, zero_radius).error() == PhysicsError::InvalidArgument);

	ColliderDesc flat_box;
	flat_box.shape = BoxShape{{1.0f, 0.0f, 1.0f}, {}};
	ENGINE_CHECK(world.add_collider(body, flat_box).error() == PhysicsError::InvalidArgument);

	ColliderDesc negative_density;
	negative_density.density = -1.0f;
	ENGINE_CHECK(world.add_collider(body, negative_density).error() == PhysicsError::InvalidArgument);

	ENGINE_CHECK(world.add_collider(BodyHandle{}, ColliderDesc{}).error() == PhysicsError::InvalidBody);
	ENGINE_CHECK_EQ(world.stats().collider_count, 0);
}

ENGINE_TEST(collision_filter_prevents_contact)
{
	PhysicsWorld world = make_world();
	BodyDesc ground_desc;
	ground_desc.type = BodyType::Static;
	ground_desc.pose.position = {0, -0.5f, 0};
	const BodyHandle ground = world.create_body(ground_desc).value();
	ColliderDesc ground_collider;
	ground_collider.shape = BoxShape{{10, 0.5f, 10}, {}};
	ground_collider.filter.category_bits = 0x1;
	world.add_collider(ground, ground_collider);

	BodyDesc ghost_desc;
	ghost_desc.pose.position = {0, 1, 0};
	const BodyHandle ghost = world.create_body(ghost_desc).value();
	ColliderDesc ghost_collider;
	ghost_collider.filter.mask_bits = ~std::uint64_t{0x1};
	world.add_collider(ghost, ghost_collider);

	for (int i = 0; i < 120; ++i) {
		world.step();
	}
	ENGINE_CHECK(world.body_pose(ghost).value().position.y < -5.0f); // fell through
}

// ---- Events / transform synchronization ----------------------------------------------------

ENGINE_TEST(moved_bodies_report_user_keys_and_latest_pose)
{
	PhysicsWorld world = make_world();
	const BodyHandle a = make_ball(world, {0, 10, 0}, 100);
	const BodyHandle b = make_ball(world, {5, 10, 0}, 200);
	BodyDesc static_desc;
	static_desc.type = BodyType::Static;
	static_desc.user_key = 300;
	world.create_body(static_desc);

	world.advance(std::chrono::nanoseconds(world.config().fixed_timestep * 3));
	const auto moved = world.moved_bodies();
	ENGINE_CHECK_EQ(moved.size(), 2u); // de-duplicated across 3 ticks; static body never moves
	for (const BodyMoved& m : moved) {
		ENGINE_CHECK(m.body == a || m.body == b);
		ENGINE_CHECK_EQ(m.user_key, m.body == a ? 100u : 200u);
		ENGINE_CHECK(m.pose == world.body_pose(m.body).value());
	}
}

ENGINE_TEST(contact_begin_and_end_events)
{
	PhysicsWorld world = make_world();
	const BodyHandle ground = make_ground(world);
	const BodyHandle ball = make_ball(world, {0, 1.0f, 0}, 7, true);

	bool began = false;
	for (int i = 0; i < 120 && !began; ++i) {
		world.step();
		for (const ContactBegin& c : world.contact_begins()) {
			const bool pair = (c.body_a == ball && c.body_b == ground) || (c.body_a == ground && c.body_b == ball);
			began = began || pair;
			if (pair) {
				ENGINE_CHECK_EQ(c.body_a == ball ? c.user_key_a : c.user_key_b, 7u);
			}
		}
	}
	ENGINE_REQUIRE(began);

	// Destroying a touching body produces an end event whose collider is already gone.
	world.destroy_body(ball);
	world.step();
	bool ended = false;
	for (const ContactEnd& c : world.contact_ends()) {
		if (c.body_a == ground || c.body_b == ground) {
			ended = true;
			ENGINE_CHECK(c.body_a == ground ? c.body_b.is_null() : c.body_a.is_null());
		}
	}
	ENGINE_CHECK(ended);
}

ENGINE_TEST(hit_event_reports_approach_speed)
{
	PhysicsWorld world = make_world();
	make_ground(world);
	BodyDesc desc;
	desc.pose.position = {0, 2, 0};
	desc.linear_velocity = {0, -10, 0};
	const BodyHandle ball = world.create_body(desc).value();
	ColliderDesc collider;
	collider.enable_hit_events = true;
	collider.enable_contact_events = true;
	world.add_collider(ball, collider);

	float speed = 0.0f;
	for (int i = 0; i < 60 && speed == 0.0f; ++i) {
		world.step();
		for (const ContactHit& h : world.contact_hits()) {
			speed = std::max(speed, h.approach_speed);
			ENGINE_CHECK_NEAR(std::fabs(h.normal.y), 1.0, 1e-3);
		}
	}
	ENGINE_CHECK(speed > 5.0f);
}

ENGINE_TEST(sensor_reports_overlap)
{
	PhysicsWorld world = make_world();
	BodyDesc sensor_desc;
	sensor_desc.type = BodyType::Static;
	sensor_desc.user_key = 1;
	const BodyHandle trigger = world.create_body(sensor_desc).value();
	ColliderDesc sensor;
	sensor.shape = BoxShape{{2, 2, 2}, {}};
	sensor.is_sensor = true;
	sensor.enable_sensor_events = true;
	world.add_collider(trigger, sensor);

	BodyDesc visitor_desc;
	visitor_desc.pose.position = {0, 5, 0};
	visitor_desc.user_key = 2;
	const BodyHandle visitor = world.create_body(visitor_desc).value();
	ColliderDesc visitor_collider;
	visitor_collider.enable_sensor_events = true;
	world.add_collider(visitor, visitor_collider);

	bool entered = false;
	bool exited = false;
	for (int i = 0; i < 240 && !exited; ++i) {
		world.step();
		for (const SensorOverlap& s : world.sensor_begins()) {
			entered = entered || (s.sensor_body == trigger && s.visitor_body == visitor && s.visitor_user_key == 2);
		}
		for (const SensorOverlap& s : world.sensor_ends()) {
			exited = exited || (entered && s.sensor_body == trigger);
		}
	}
	ENGINE_CHECK(entered);
	ENGINE_CHECK(exited);
}

ENGINE_TEST(events_cleared_on_next_step)
{
	PhysicsWorld world = make_world();
	make_ball(world, {0, 10, 0});
	world.step();
	ENGINE_CHECK_EQ(world.moved_bodies().size(), 1u);
	world.set_gravity({0, 0, 0});
	world.reset();
	ENGINE_CHECK(world.moved_bodies().empty());
}
