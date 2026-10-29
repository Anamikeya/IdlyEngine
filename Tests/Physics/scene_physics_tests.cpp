// Integration tests: engine::physics bridged to Codex's real engine::core Scene and CommandBus.
// No mocks: every case runs a Box3D world against a Flecs-backed scene.

#include "engine/physics/scene_physics.h"
#include "engine/test/test.h"

#include <engine/core/Application.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

// physics::Vec3 (float) is the unqualified Vec3 here; the scene's double Vec3 is never named directly.
using namespace engine::physics;
using namespace std::chrono_literals;
namespace commands = engine::commands;
using engine::Application;
using engine::ApplicationClient;
using engine::EntityId;
using engine::ErrorCode;
using engine::FixedUpdate;
using engine::LoopConfig;
using engine::Result;
using engine::Scene;
using engine::SceneSnapshot;
using engine::Transform;

namespace {

constexpr std::uint32_t all_grants =
	static_cast<std::uint32_t>(commands::Capability::Inspect) | static_cast<std::uint32_t>(commands::Capability::Edit);

ScenePhysics make_physics(WorldConfig config = {})
{
	auto result = ScenePhysics::create(config);
	ENGINE_REQUIRE(static_cast<bool>(result));
	return std::move(result.value());
}

Transform at(double x, double y, double z)
{
	Transform t;
	t.position = {x, y, z};
	return t;
}

Collider sphere(float radius, bool contact_events = false)
{
	Collider c;
	c.shape = SphereShape{radius, {}};
	c.enable_contact_events = contact_events;
	return c;
}

Collider box(Vec3 half_extents, bool contact_events = false)
{
	Collider c;
	c.shape = BoxShape{half_extents, {}};
	c.enable_contact_events = contact_events;
	return c;
}

RigidBody body_of(BodyType type)
{
	RigidBody b;
	b.type = type;
	return b;
}

// One scene with an editor bus (undo history) and a history-free runtime bus for physics (contract C-1).
struct Fixture {
	Scene scene;
	commands::CommandBus editor{scene};
	commands::CommandBus runtime{scene, 0};
	commands::Context editor_ctx{"editor", all_grants};
	commands::Context physics_ctx{"physics", all_grants};
	ScenePhysics physics = make_physics();

	EntityId create(const char* name, Transform transform = {}, EntityId parent = 0)
	{
		auto receipt = editor.execute(editor_ctx, editor.revision(), commands::CreateEntity{name, transform, parent, 0});
		ENGINE_REQUIRE(static_cast<bool>(receipt));
		return receipt.value().created.front();
	}

	void move(EntityId id, Transform transform)
	{
		ENGINE_REQUIRE(static_cast<bool>(editor.execute(editor_ctx, editor.revision(), commands::SetTransform{id, transform})));
	}

	EntityId ground()
	{
		const EntityId id = create("ground", at(0, -0.5, 0));
		ENGINE_REQUIRE(static_cast<bool>(physics.set_rigid_body(scene, id, body_of(BodyType::Static))));
		ENGINE_REQUIRE(static_cast<bool>(physics.add_collider(id, box({50.0f, 0.5f, 50.0f}, true))));
		return id;
	}

	EntityId ball(double y, float radius = 0.5f, bool contact_events = false)
	{
		const EntityId id = create("ball", at(0, y, 0));
		ENGINE_REQUIRE(static_cast<bool>(physics.set_rigid_body(scene, id, body_of(BodyType::Dynamic))));
		ENGINE_REQUIRE(static_cast<bool>(physics.add_collider(id, sphere(radius, contact_events))));
		return id;
	}

	TickReport tick()
	{
		auto report = physics.fixed_update(physics.config().fixed_timestep, scene, runtime, physics_ctx);
		ENGINE_REQUIRE(static_cast<bool>(report));
		return report.value();
	}

	void ticks(int count)
	{
		for (int i = 0; i < count; ++i) {
			tick();
		}
	}

	Vec3 world_position(EntityId id) const
	{
		const auto w = scene.world_transform(id);
		ENGINE_REQUIRE(w.has_value());
		return {static_cast<float>(w->matrix[12]), static_cast<float>(w->matrix[13]), static_cast<float>(w->matrix[14])};
	}
};

} // namespace

// ---- Mapping, stepping and publishing ----------------------------------------------------------

ENGINE_TEST(dynamic_body_publishes_pose_to_scene_entity)
{
	Fixture f;
	const EntityId ball = f.ball(10.0);
	f.physics.set_simulation_enabled(true);

	const std::uint64_t editor_revision = f.editor.revision();
	const int steps = 30;
	f.ticks(steps);

	// Free fall under -9.81 m/s^2 for 30 ticks of 1/60 s (semi-implicit Euler, so not exactly analytic).
	const double t = steps / 60.0;
	ENGINE_CHECK_NEAR(f.world_position(ball).y, 10.0 - 0.5 * 9.81 * t * t, 0.1);
	ENGINE_CHECK_NEAR(f.scene.inspect(ball)->transform.position.y, f.world_position(ball).y, 1e-6);

	const auto state = f.physics.body_state(ball);
	ENGINE_REQUIRE(state.has_value());
	ENGINE_CHECK_NEAR(state->pose.position.y, f.world_position(ball).y, 1e-6);
	ENGINE_CHECK(state->linear_velocity.y < -2.0f);
	ENGINE_CHECK(state->mass > 0.0f);

	// Physics writes never reach the editor's revision or undo history (C-1).
	ENGINE_CHECK_EQ(f.editor.revision(), editor_revision);
	ENGINE_CHECK_EQ(f.editor.undo_size(), std::size_t{1});
	ENGINE_CHECK_EQ(f.runtime.undo_size(), std::size_t{0});
	ENGINE_CHECK_EQ(f.runtime.revision(), static_cast<std::uint64_t>(steps));
}

ENGINE_TEST(static_and_edit_mode_bodies_never_publish)
{
	Fixture f;
	f.ground();
	const EntityId ball = f.ball(5.0);

	// Edit mode: bodies exist but the world is not stepped and nothing is written.
	const TickReport report = f.tick();
	ENGINE_CHECK(!report.stepped);
	ENGINE_CHECK_EQ(report.published, std::size_t{0});
	ENGINE_CHECK_EQ(f.physics.diagnostics().active_bodies, std::size_t{2});
	ENGINE_CHECK_EQ(f.physics.diagnostics().simulated_ticks, std::uint64_t{0});
	f.ticks(10);
	ENGINE_CHECK_EQ(f.world_position(ball).y, 5.0f);
	ENGINE_CHECK_EQ(f.runtime.revision(), std::uint64_t{0});
}

ENGINE_TEST(edit_mode_bodies_follow_scene_edits)
{
	Fixture f;
	const EntityId ball = f.ball(1.0);
	f.tick();
	f.move(ball, at(3, 4, 5));
	const TickReport report = f.tick();
	ENGINE_CHECK_EQ(report.scene_updates, std::size_t{1});
	const auto state = f.physics.body_state(ball);
	ENGINE_REQUIRE(state.has_value());
	ENGINE_CHECK(state->pose.position == (Vec3{3.0f, 4.0f, 5.0f}));
}

ENGINE_TEST(fixed_update_rejects_mismatched_delta)
{
	Fixture f;
	auto result = f.physics.fixed_update(10ms, f.scene, f.runtime, f.physics_ctx);
	ENGINE_REQUIRE(!result);
	ENGINE_CHECK(result.error().code == ErrorCode::InvalidArgument);
	ENGINE_CHECK_EQ(f.physics.diagnostics().ticks, std::uint64_t{0});
}

ENGINE_TEST(simulation_is_deterministic_under_fixed_timestep)
{
	const auto run = [] {
		Fixture f;
		f.ground();
		for (int i = 0; i < 6; ++i) {
			const EntityId id = f.create("box", at(0.1 * i, 0.6 + 1.05 * i, 0.05 * i));
			ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, id, body_of(BodyType::Dynamic))));
			ENGINE_REQUIRE(static_cast<bool>(f.physics.add_collider(id, box({0.5f, 0.5f, 0.5f}))));
		}
		f.physics.set_simulation_enabled(true);
		f.ticks(180);
		return f.scene.snapshot();
	};
	const SceneSnapshot first = run();
	const SceneSnapshot second = run();
	ENGINE_CHECK(first == second);
	// The stack actually moved, so the comparison is meaningful.
	ENGINE_CHECK(first.entities.back().transform.position.y < 0.6 + 1.05 * 5);
}

// ---- Feedback-loop guard (C-1) -----------------------------------------------------------------

ENGINE_TEST(published_poses_are_not_treated_as_scene_edits)
{
	Fixture f;
	f.ground();
	const EntityId ball = f.ball(3.0);
	f.physics.set_simulation_enabled(true);
	f.tick();
	for (int i = 0; i < 60; ++i) {
		const TickReport report = f.tick();
		ENGINE_CHECK(report.published <= 1);
		ENGINE_CHECK_EQ(report.scene_updates, std::size_t{0});
	}

	// An editor drag during play is an external edit: exactly one teleport, then physics resumes.
	f.move(ball, at(0, 8, 0));
	const TickReport dragged = f.tick();
	ENGINE_CHECK_EQ(dragged.scene_updates, std::size_t{1});
	ENGINE_CHECK(f.world_position(ball).y > 7.5f);
	ENGINE_CHECK_EQ(f.tick().scene_updates, std::size_t{0});
}

ENGINE_TEST(kinematic_body_follows_scene_without_teleport_and_stops)
{
	Fixture f;
	const EntityId platform = f.create("platform", at(0, 0, 0));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, platform, body_of(BodyType::Kinematic))));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.add_collider(platform, box({1.0f, 0.1f, 1.0f}))));
	f.physics.set_simulation_enabled(true);
	f.tick();

	f.move(platform, at(1, 0, 0));
	f.tick();
	auto state = f.physics.body_state(platform);
	ENGINE_REQUIRE(state.has_value());
	ENGINE_CHECK_NEAR(state->pose.position.x, 1.0, 1e-4);
	ENGINE_CHECK(state->linear_velocity.x > 1.0f); // Moved by velocity, not teleported.

	// No new target: the platform stops at the target instead of coasting.
	f.ticks(10);
	state = f.physics.body_state(platform);
	ENGINE_CHECK_NEAR(state->pose.position.x, 1.0, 1e-3);
	ENGINE_CHECK_EQ(state->linear_velocity.x, 0.0f);
	// Kinematic poses are scene-owned and never published.
	ENGINE_CHECK_EQ(f.runtime.revision(), std::uint64_t{0});
}

// ---- Hierarchy and scale -----------------------------------------------------------------------

ENGINE_TEST(parented_dynamic_body_publishes_local_transform)
{
	Fixture f;
	Transform parent_transform = at(5, 2, 0);
	const double half = std::numbers::pi / 4.0; // 90 degrees about +Y.
	parent_transform.rotation = {0, std::sin(half), 0, std::cos(half)};
	parent_transform.scale = {2, 2, 2};
	const EntityId parent = f.create("pivot", parent_transform);
	Transform child_local = at(1, 3, 0);
	child_local.scale = {0.5, 0.5, 0.5};
	const EntityId child = f.create("ball", child_local, parent);
	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, child, body_of(BodyType::Dynamic))));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.add_collider(child, sphere(0.5f))));

	f.physics.set_simulation_enabled(true);
	f.ticks(20);
	const auto state = f.physics.body_state(child);
	ENGINE_REQUIRE(state.has_value());
	const Vec3 world = f.world_position(child);
	ENGINE_CHECK_NEAR(world.x, state->pose.position.x, 1e-4);
	ENGINE_CHECK_NEAR(world.y, state->pose.position.y, 1e-4);
	ENGINE_CHECK_NEAR(world.z, state->pose.position.z, 1e-4);
	// Falling along world -Y only: x/z stay at the initial world position (parent pivot + rotated offset).
	ENGINE_CHECK_NEAR(world.x, 5.0, 1e-4);
	ENGINE_CHECK_NEAR(world.z, -2.0, 1e-4);
	ENGINE_CHECK(world.y < 8.0f);
	// Physics never changes local scale.
	ENGINE_CHECK(f.scene.inspect(child)->transform.scale == child_local.scale);
	ENGINE_CHECK(f.physics.diagnostics().issues.empty());
}

ENGINE_TEST(unsupported_hierarchies_are_reported_not_simulated)
{
	Fixture f;
	const EntityId outer = f.ball(5.0);
	const EntityId nested = f.create("nested", at(0, 2, 0), outer);
	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, nested, body_of(BodyType::Dynamic))));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.add_collider(nested, sphere(0.25f))));

	Transform stretched = at(10, 0, 0);
	stretched.scale = {1, 3, 1};
	const EntityId stretched_parent = f.create("stretched", stretched);
	const EntityId under_stretch = f.create("child", at(0, 1, 0), stretched_parent);
	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, under_stretch, body_of(BodyType::Dynamic))));

	f.tick();
	const PhysicsDiagnostics d = f.physics.diagnostics();
	ENGINE_CHECK_EQ(d.active_bodies, std::size_t{1});
	ENGINE_REQUIRE(d.issues.size() == 2);
	ENGINE_CHECK_EQ(d.issues[0].entity, nested);
	ENGINE_CHECK_EQ(d.issues[1].entity, under_stretch);
	ENGINE_CHECK(!f.physics.body_state(nested).has_value());
}

ENGINE_TEST(world_scale_is_baked_into_colliders)
{
	Fixture f;
	f.ground();
	Transform scaled = at(0, 4, 0);
	scaled.scale = {2, 2, 2};
	const EntityId ball = f.create("big ball", scaled);
	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, ball, body_of(BodyType::Dynamic))));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.add_collider(ball, sphere(0.5f))));
	f.physics.set_simulation_enabled(true);
	f.ticks(240);
	// Radius 0.5 * scale 2 rests with its centre ~1 m above the ground.
	ENGINE_CHECK_NEAR(f.world_position(ball).y, 1.0, 0.02);

	// A scale edit rebuilds the collider.
	scaled = f.scene.inspect(ball)->transform;
	scaled.scale = {1, 1, 1};
	f.move(ball, scaled);
	ENGINE_CHECK_EQ(f.tick().rebuilt, std::size_t{1});
	f.ticks(240);
	ENGINE_CHECK_NEAR(f.world_position(ball).y, 0.5, 0.02);
}

ENGINE_TEST(non_uniform_scale_accepts_axis_aligned_boxes_only)
{
	Fixture f;
	Transform stretched = at(0, 0, 0);
	stretched.scale = {1, 2, 3};
	const EntityId a = f.create("stretched box", stretched);
	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, a, body_of(BodyType::Static))));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.add_collider(a, box({0.5f, 0.5f, 0.5f}))));
	stretched.position = {5, 0, 0};
	const EntityId b = f.create("stretched sphere", stretched);
	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, b, body_of(BodyType::Static))));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.add_collider(b, sphere(0.5f))));
	f.tick();

	ENGINE_CHECK(f.physics.body_state(a).has_value());
	ENGINE_CHECK(!f.physics.body_state(b).has_value());
	const auto issues = f.physics.diagnostics().issues;
	ENGINE_REQUIRE(issues.size() == 1);
	ENGINE_CHECK_EQ(issues[0].entity, b);

	// The stretched box spans y in [-1, 1].
	const DebugGeometry g = f.physics.debug_geometry();
	float max_y = -100.0f;
	for (const DebugLine& line : g.lines) {
		max_y = std::max({max_y, line.a.y, line.b.y});
	}
	ENGINE_CHECK_NEAR(max_y, 1.0, 1e-5);
}

// ---- Entity lifetime and cleanup (C-2) ---------------------------------------------------------

ENGINE_TEST(entity_delete_destroys_body_and_undo_restores_it)
{
	Fixture f;
	const EntityId ball = f.ball(2.0);
	f.tick();
	ENGINE_CHECK_EQ(f.physics.diagnostics().world.body_count, 1);

	ENGINE_REQUIRE(static_cast<bool>(f.editor.execute(f.editor_ctx, f.editor.revision(), commands::DeleteEntity{ball})));
	f.tick();
	PhysicsDiagnostics d = f.physics.diagnostics();
	ENGINE_CHECK_EQ(d.world.body_count, 0);
	ENGINE_CHECK_EQ(d.detached_entities, std::size_t{1});
	ENGINE_CHECK_EQ(d.authored_entities, std::size_t{1});

	ENGINE_REQUIRE(static_cast<bool>(f.editor.undo(f.editor_ctx, f.editor.revision())));
	f.tick();
	d = f.physics.diagnostics();
	ENGINE_CHECK_EQ(d.world.body_count, 1);
	ENGINE_CHECK_EQ(d.detached_entities, std::size_t{0});
	ENGINE_CHECK_EQ(f.physics.colliders(ball).size(), std::size_t{1});
	ENGINE_CHECK_EQ(f.physics.body_state(ball)->pose.position.y, 2.0f);
}

ENGINE_TEST(purge_detached_drops_authoring_of_missing_entities)
{
	Fixture f;
	const EntityId keep = f.ball(1.0);
	const EntityId gone = f.ball(2.0);
	f.tick();
	ENGINE_REQUIRE(static_cast<bool>(f.editor.execute(f.editor_ctx, f.editor.revision(), commands::DeleteEntity{gone})));
	ENGINE_CHECK_EQ(f.physics.purge_detached(f.scene), std::size_t{1});
	ENGINE_CHECK(f.physics.authored_entities() == (std::vector<EntityId>{keep}));
	ENGINE_CHECK_EQ(f.physics.diagnostics().world.body_count, 1);
}

ENGINE_TEST(physics_resources_are_released)
{
	const int worlds_before = live_world_count();
	const std::int64_t bytes_before = allocated_bytes();
	{
		Fixture f;
		f.ground();
		std::vector<EntityId> balls;
		for (int i = 0; i < 20; ++i) {
			balls.push_back(f.ball(1.0 + i));
		}
		f.physics.set_simulation_enabled(true);
		f.ticks(30);
		ENGINE_CHECK_EQ(live_world_count(), worlds_before + 1);
		for (EntityId id : balls) {
			ENGINE_CHECK(static_cast<bool>(f.physics.remove_rigid_body(id)));
		}
		ENGINE_CHECK_EQ(f.physics.diagnostics().world.body_count, 1);
		ENGINE_CHECK(!f.physics.remove_rigid_body(balls.front()));
	}
	ENGINE_CHECK_EQ(live_world_count(), worlds_before);
	ENGINE_CHECK_EQ(allocated_bytes(), bytes_before);
}

// ---- Collision events --------------------------------------------------------------------------

ENGINE_TEST(collision_events_report_entity_ids)
{
	Fixture f;
	const EntityId ground = f.ground();
	const EntityId ball = f.ball(2.0, 0.5f, true);
	f.physics.set_simulation_enabled(true);

	bool began = false;
	for (int i = 0; i < 120 && !began; ++i) {
		f.tick();
		for (const CollisionEvent& e : f.physics.events()) {
			if (e.kind == CollisionEvent::Kind::ContactBegin) {
				began = true;
				ENGINE_CHECK((e.a == ground && e.b == ball) || (e.a == ball && e.b == ground));
			}
		}
	}
	ENGINE_CHECK(began);
}

ENGINE_TEST(sensor_events_report_entity_ids)
{
	Fixture f;
	const EntityId zone = f.create("trigger", at(0, 0, 0));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, zone, body_of(BodyType::Kinematic))));
	Collider trigger = box({2.0f, 0.5f, 2.0f});
	trigger.is_sensor = true;
	trigger.enable_sensor_events = true;
	ENGINE_REQUIRE(static_cast<bool>(f.physics.add_collider(zone, trigger)));

	const EntityId ball = f.create("ball", at(0, 3, 0));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, ball, body_of(BodyType::Dynamic))));
	Collider visitor = sphere(0.25f);
	visitor.enable_sensor_events = true;
	ENGINE_REQUIRE(static_cast<bool>(f.physics.add_collider(ball, visitor)));

	f.physics.set_simulation_enabled(true);
	bool entered = false;
	bool left = false;
	for (int i = 0; i < 240; ++i) {
		f.tick();
		for (const CollisionEvent& e : f.physics.events()) {
			if (e.kind == CollisionEvent::Kind::SensorBegin) {
				entered = e.a == zone && e.b == ball;
			}
			if (e.kind == CollisionEvent::Kind::SensorEnd) {
				left = e.a == zone && e.b == ball;
			}
		}
	}
	ENGINE_CHECK(entered);
	ENGINE_CHECK(left);
}

// ---- Editor-facing API (C-4) -------------------------------------------------------------------

ENGINE_TEST(authoring_api_validates_input)
{
	Fixture f;
	const EntityId id = f.create("thing");
	ENGINE_CHECK(f.physics.set_rigid_body(f.scene, 999, {}).error().code == ErrorCode::NotFound);
	RigidBody bad;
	bad.linear_damping = -1.0f;
	ENGINE_CHECK(f.physics.set_rigid_body(f.scene, id, bad).error().code == ErrorCode::InvalidArgument);
	ENGINE_CHECK(f.physics.add_collider(id, sphere(0.5f)).error().code == ErrorCode::NotFound);

	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, id, {})));
	ENGINE_CHECK(f.physics.add_collider(id, sphere(-1.0f)).error().code == ErrorCode::InvalidArgument);
	Collider zero_quat = box({1, 1, 1});
	std::get<BoxShape>(zero_quat.shape).local_pose.rotation = {0, 0, 0, 0};
	ENGINE_CHECK(f.physics.add_collider(id, zero_quat).error().code == ErrorCode::InvalidArgument);

	auto index = f.physics.add_collider(id, sphere(0.5f));
	ENGINE_REQUIRE(static_cast<bool>(index));
	ENGINE_CHECK_EQ(index.value(), std::size_t{0});
	ENGINE_CHECK(f.physics.set_collider(id, 1, sphere(1.0f)).error().code == ErrorCode::NotFound);
	ENGINE_CHECK(static_cast<bool>(f.physics.set_collider(id, 0, sphere(1.0f))));
	ENGINE_CHECK_EQ(std::get<SphereShape>(f.physics.colliders(id)[0].shape).radius, 1.0f);
	ENGINE_CHECK(f.physics.remove_collider(id, 3).error().code == ErrorCode::NotFound);
	ENGINE_CHECK(static_cast<bool>(f.physics.remove_collider(id, 0)));
	ENGINE_CHECK(f.physics.colliders(id).empty());
}

ENGINE_TEST(collider_edits_rebuild_the_body)
{
	Fixture f;
	const EntityId ball = f.ball(1.0, 0.5f);
	f.tick();
	const float small_mass = f.physics.body_state(ball)->mass;
	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_collider(ball, 0, sphere(1.0f))));
	ENGINE_CHECK_EQ(f.tick().rebuilt, std::size_t{1});
	ENGINE_CHECK_NEAR(f.physics.body_state(ball)->mass / small_mass, 8.0, 1e-3);
	ENGINE_CHECK_EQ(f.tick().rebuilt, std::size_t{0});
}

ENGINE_TEST(pause_keeps_velocity_and_reset_restores_authored_state)
{
	Fixture f;
	const EntityId ball = f.create("ball", at(0, 50, 0));
	RigidBody thrown = body_of(BodyType::Dynamic);
	thrown.linear_velocity = {3.0f, 0.0f, 0.0f};
	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, ball, thrown)));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.add_collider(ball, sphere(0.5f))));
	f.physics.set_simulation_enabled(true);
	f.ticks(30);
	const BodyState moving = *f.physics.body_state(ball);
	ENGINE_CHECK(moving.linear_velocity.y < -1.0f);

	f.physics.set_simulation_enabled(false);
	f.ticks(5);
	ENGINE_CHECK(f.physics.body_state(ball)->linear_velocity == moving.linear_velocity);
	ENGINE_CHECK(f.physics.body_state(ball)->pose == moving.pose);

	ENGINE_REQUIRE(static_cast<bool>(f.physics.reset_simulation(f.scene)));
	ENGINE_CHECK(f.physics.body_state(ball)->linear_velocity == (Vec3{3.0f, 0.0f, 0.0f}));
	ENGINE_CHECK(f.physics.events().empty());
}

ENGINE_TEST(debug_geometry_outlines_each_shape)
{
	Fixture f;
	const EntityId s = f.create("s", at(0, 0, 0));
	const EntityId b = f.create("b", at(5, 0, 0));
	const EntityId c = f.create("c", at(10, 0, 0));
	const EntityId t = f.create("t", at(15, 0, 0));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, s, body_of(BodyType::Static))));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.add_collider(s, sphere(1.0f))));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, b, body_of(BodyType::Kinematic))));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.add_collider(b, box({1, 2, 3}))));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, c, body_of(BodyType::Dynamic))));
	Collider capsule;
	capsule.shape = CapsuleShape{{0, -1, 0}, {0, 1, 0}, 0.5f};
	ENGINE_REQUIRE(static_cast<bool>(f.physics.add_collider(c, capsule)));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, t, body_of(BodyType::Kinematic))));
	Collider trigger = sphere(1.0f);
	trigger.is_sensor = true;
	ENGINE_REQUIRE(static_cast<bool>(f.physics.add_collider(t, trigger)));
	f.tick();

	DebugGeometryOptions options;
	options.circle_segments = 16;
	const DebugGeometry g = f.physics.debug_geometry(options);
	const auto count = [&](EntityId id) {
		return std::count_if(g.lines.begin(), g.lines.end(), [id](const DebugLine& l) { return l.entity == id; });
	};
	ENGINE_CHECK_EQ(count(s), 3 * 16);
	ENGINE_CHECK_EQ(count(b), 12);
	ENGINE_CHECK_EQ(count(c), 2 * 16 + 4 + 4 * 8);
	ENGINE_CHECK_EQ(count(t), 3 * 16);

	for (const DebugLine& line : g.lines) {
		if (line.entity == s) {
			ENGINE_CHECK(line.category == DebugCategory::Static);
			ENGINE_CHECK_NEAR(std::sqrt(line.a.x * line.a.x + line.a.y * line.a.y + line.a.z * line.a.z), 1.0, 1e-5);
		} else if (line.entity == b) {
			ENGINE_CHECK(line.category == DebugCategory::Kinematic);
			ENGINE_CHECK_NEAR(std::abs(line.a.x - 5.0f), 1.0, 1e-5);
		} else if (line.entity == c) {
			ENGINE_CHECK(line.category == DebugCategory::DynamicAwake);
			ENGINE_CHECK(line.a.y <= 1.5f + 1e-5f && line.a.y >= -1.5f - 1e-5f);
		} else if (line.entity == t) {
			ENGINE_CHECK(line.category == DebugCategory::Sensor);
		}
	}

	options.include_static = false;
	options.include_sensors = false;
	const DebugGeometry filtered = f.physics.debug_geometry(options);
	ENGINE_CHECK_EQ(filtered.lines.size(), static_cast<std::size_t>(12 + 2 * 16 + 4 + 4 * 8));
	ENGINE_CHECK_EQ(default_color(DebugCategory::Sensor)[3], 0.5f);
}

// ---- Play/stop workflow with the editor's undo history (C-1) -----------------------------------

ENGINE_TEST(play_stop_restore_keeps_editor_undo_valid)
{
	Fixture f;
	f.ground();
	const EntityId ball = f.ball(4.0);
	const std::size_t editor_history = f.editor.undo_size();

	const SceneSnapshot before_play = f.scene.snapshot();
	f.physics.set_simulation_enabled(true);
	f.ticks(60);
	ENGINE_CHECK(f.world_position(ball).y < 3.0f);

	// While physics owns the pose, undoing the editor change that created it is refused, not corrupted.
	const auto conflicted = f.editor.undo(f.editor_ctx, f.editor.revision());
	ENGINE_REQUIRE(!conflicted);
	ENGINE_CHECK(conflicted.error().code == ErrorCode::Conflict);

	// Stop: restore the pre-play scene through the runtime bus, then rebuild bodies from it.
	f.physics.set_simulation_enabled(false);
	ENGINE_REQUIRE(static_cast<bool>(f.runtime.execute(f.physics_ctx, f.runtime.revision(), commands::ReplaceScene{before_play})));
	ENGINE_REQUIRE(static_cast<bool>(f.physics.reset_simulation(f.scene)));
	ENGINE_CHECK_EQ(f.world_position(ball).y, 4.0f);
	ENGINE_CHECK_EQ(f.physics.body_state(ball)->pose.position.y, 4.0f);

	ENGINE_CHECK_EQ(f.editor.undo_size(), editor_history);
	ENGINE_CHECK(static_cast<bool>(f.editor.undo(f.editor_ctx, f.editor.revision())));
	ENGINE_CHECK(!f.scene.inspect(ball).has_value());
}

ENGINE_TEST(large_publish_is_split_into_bounded_transactions)
{
	Fixture f;
	const int count = 300;
	std::vector<commands::Command> creates;
	for (int i = 0; i < count; ++i) {
		creates.push_back(commands::CreateEntity{"ball", at(3.0 * (i % 20), 5.0, 3.0 * (i / 20)), 0, 0});
		if (creates.size() == commands::CommandBus::max_transaction || i == count - 1) {
			auto receipt = f.editor.execute(f.editor_ctx, f.editor.revision(), std::span<const commands::Command>(creates));
			ENGINE_REQUIRE(static_cast<bool>(receipt));
			for (EntityId id : receipt.value().created) {
				ENGINE_REQUIRE(static_cast<bool>(f.physics.set_rigid_body(f.scene, id, body_of(BodyType::Dynamic))));
				ENGINE_REQUIRE(static_cast<bool>(f.physics.add_collider(id, sphere(0.5f))));
			}
			creates.clear();
		}
	}
	f.physics.set_simulation_enabled(true);
	const TickReport report = f.tick();
	ENGINE_CHECK_EQ(report.published, static_cast<std::size_t>(count));
	ENGINE_CHECK_EQ(report.transactions, std::size_t{2});
	ENGINE_CHECK_EQ(f.runtime.revision(), std::uint64_t{2});
}

ENGINE_TEST(calls_from_another_thread_are_rejected)
{
	Fixture f;
	f.ball(1.0);
	Result<void> result;
	std::thread worker([&] { result = f.physics.sync(f.scene); });
	worker.join();
	ENGINE_REQUIRE(!result);
	ENGINE_CHECK(result.error().code == ErrorCode::WrongThread);
}

// ---- Driven by engine::core's Application fixed-step scheduler (C-3) ---------------------------

namespace {

struct PhysicsClient final : ApplicationClient {
	std::optional<ScenePhysics> physics;
	std::unique_ptr<commands::CommandBus> runtime;
	EntityId ball = 0;
	std::uint64_t fixed_ticks = 0;
	bool failed = false;

	Result<void> on_start(Application& app) override
	{
		WorldConfig config;
		config.fixed_timestep = 10ms;
		auto created = ScenePhysics::create(config);
		if (!created) {
			return created.error();
		}
		physics.emplace(std::move(created.value()));
		runtime = std::make_unique<commands::CommandBus>(*app.scene(), 0);
		const commands::Context ctx{"host", all_grants};
		auto receipt = app.commands()->execute(ctx, app.commands()->revision(), commands::CreateEntity{"ball", at(0, 20, 0), 0, 0});
		if (!receipt) {
			return receipt.error();
		}
		ball = receipt.value().created.front();
		if (auto r = physics->set_rigid_body(*app.scene(), ball, {}); !r) {
			return r;
		}
		if (auto r = physics->add_collider(ball, sphere(0.5f)); !r) {
			return r.error();
		}
		physics->set_simulation_enabled(true);
		return {};
	}

	void on_fixed_update(Application& app, const FixedUpdate& update) override
	{
		++fixed_ticks;
		const commands::Context ctx{"physics", all_grants};
		if (!physics->fixed_update(update.delta, *app.scene(), *runtime, ctx)) {
			failed = true;
		}
	}

	void on_stop(Application&) noexcept override
	{
		// The runtime bus references the scene, which Application destroys after on_stop (C-3).
		runtime.reset();
		physics.reset();
	}
};

} // namespace

ENGINE_TEST(application_fixed_step_drives_scene_physics)
{
	PhysicsClient client;
	LoopConfig loop;
	loop.fixed_step = 10ms;
	loop.max_catch_up = 200;
	loop.max_frame = 2s;
	Application app(client, loop);
	ENGINE_REQUIRE(static_cast<bool>(app.start()));
	ENGINE_REQUIRE(static_cast<bool>(app.advance(1s)));
	ENGINE_CHECK_EQ(client.fixed_ticks, std::uint64_t{100});
	ENGINE_CHECK(!client.failed);
	const auto record = app.scene()->inspect(client.ball);
	ENGINE_REQUIRE(record.has_value());
	// 1 s of free fall from 20 m.
	ENGINE_CHECK_NEAR(record->transform.position.y, 20.0 - 0.5 * 9.81, 0.1);
	ENGINE_CHECK(static_cast<bool>(app.shutdown()));
	ENGINE_CHECK(!client.physics.has_value());
}
