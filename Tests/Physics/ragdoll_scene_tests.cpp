// Ragdolls and debris in ScenePhysics (Milestone 5, slice 4): bodies the run owns, which mirror no entity. Real Box3D
// against the real Scene and CommandBus.

#include "engine/physics/scene_physics.h"
#include "engine/test/test.h"

#include "humanoid.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <numbers>
#include <string>
#include <vector>

using namespace engine::physics;
namespace commands = engine::commands;
using engine::BodyKind;
using engine::ColliderComponent;
using engine::ColliderShape;
using engine::Components;
using engine::EntityId;
using engine::RigidBodyComponent;
using engine::Scene;
using engine::Transform;
using humanoid::axis_angle;
using humanoid::DQ;

namespace {

constexpr std::uint32_t all_grants =
	static_cast<std::uint32_t>(commands::Capability::Inspect) | static_cast<std::uint32_t>(commands::Capability::Edit);
constexpr float tick_seconds = 1.0f / 60.0f;

float length(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// A floor whose top is at y = 0, and a person standing on it: a kinematic capsule, as a character has.
struct World {
	Scene scene;
	commands::CommandBus bus{scene, 0};
	commands::Context ctx{"test", all_grants};
	ScenePhysics physics = [] {
		auto created = ScenePhysics::create({});
		ENGINE_REQUIRE(static_cast<bool>(created));
		return std::move(created.value());
	}();
	EntityId floor = 0, person = 0;

	explicit World(bool capsule = true)
	{
		physics.set_simulation_enabled(true);
		Components ground;
		ColliderComponent slab;
		slab.shape = ColliderShape::Box;
		slab.half_extents = {50, 0.5, 50};
		slab.events = true;
		ground.colliders.push_back(slab);
		floor = create("Floor", {0, -0.5, 0}, ground);
		Components body;
		if (capsule) {
			RigidBodyComponent kinematic;
			kinematic.kind = BodyKind::Kinematic;
			body.body = kinematic;
			ColliderComponent c;
			c.shape = ColliderShape::Capsule;
			c.radius = 0.3;
			c.half_height = 0.6;
			c.center = {0, 0.9, 0};
			body.colliders.push_back(c);
		}
		person = create("Person", {}, body);
		ENGINE_REQUIRE(static_cast<bool>(physics.sync(scene)));
	}

	EntityId create(const char* name, engine::Vec3 at, Components components)
	{
		Transform t;
		t.position = at;
		auto receipt = bus.execute(ctx, bus.revision(), commands::CreateEntity{name, t, 0, 0, std::move(components)});
		ENGINE_REQUIRE(static_cast<bool>(receipt));
		return receipt.value().created.front();
	}

	void tick() { ENGINE_REQUIRE(static_cast<bool>(physics.fixed_update(physics.config().fixed_timestep, scene, bus, ctx))); }

	std::uint64_t fall(const std::vector<Pose>& now, const std::vector<Pose>& before, RagdollDesc* placed = nullptr)
	{
		const RagdollPlan plan = plan_ragdoll(humanoid::skeleton(), {});
		ENGINE_REQUIRE(plan.ok());
		const RagdollDesc desc = place_ragdoll(plan, now, before, tick_seconds);
		ENGINE_REQUIRE(desc.parts.size() == plan.bodies.size());
		if (placed) *placed = desc;
		auto made = physics.create_ragdoll(person, desc);
		ENGINE_REQUIRE(static_cast<bool>(made));
		return made.value();
	}
};

const RagdollPartState& part(const RagdollState& state, const char* bone)
{
	const int index = humanoid::index_of(bone);
	const auto it = std::find_if(state.parts.begin(), state.parts.end(), [&](const RagdollPartState& p) { return p.bone == index; });
	ENGINE_REQUIRE(it != state.parts.end());
	return *it;
}

struct Fall {
	std::string name;
	std::vector<Pose> now, before;
};

// Poses to fall from: standing, slumped, sitting in the air, twisted past the joints' limits, face down, and thrown.
std::vector<Fall> falls()
{
	const DQ none{};
	const auto up = [](double y) { return humanoid::D3{0, y, 0}; };
	std::vector<Fall> list;
	const auto add = [&](std::string name, const std::map<std::string, DQ>& turns, humanoid::D3 at, DQ facing = {},
						 humanoid::D3 moving = {}, const std::map<std::string, DQ>& turns_before = {}) {
		const auto now = humanoid::posed(turns, at, facing);
		const auto before = humanoid::posed(turns_before.empty() ? turns : turns_before, at - moving * tick_seconds, facing);
		list.push_back({std::move(name), now, before});
	};
	add("standing", {}, up(0.05));
	add("slumped", {{"upperarm_l", axis_angle({0, 0, 1}, -1.4)}, {"upperarm_r", axis_angle({0, 0, 1}, 1.4)},
					   {"calf_l", axis_angle({1, 0, 0}, 0.6)}, {"calf_r", axis_angle({1, 0, 0}, 0.6)}, {"spine", axis_angle({1, 0, 0}, 0.4)}},
		up(0.3));
	add("sitting", {{"thigh_l", axis_angle({1, 0, 0}, -1.5)}, {"thigh_r", axis_angle({1, 0, 0}, -1.5)},
					   {"calf_l", axis_angle({1, 0, 0}, 1.5)}, {"calf_r", axis_angle({1, 0, 0}, 1.5)}},
		up(0.8));
	add("twisted", {{"chest", axis_angle({0, 1, 0}, 1.1)}, {"neck", axis_angle({0, 1, 0}, -0.9)},
					   {"forearm_l", axis_angle({0, 1, 0}, 2.0)}, {"thigh_r", axis_angle({0, 0, 1}, -1.2)}},
		up(0.2));
	add("face down", {}, up(0.6), axis_angle({1, 0, 0}, 1.5707963));
	// Knocked backwards and spinning, as a shot would leave it.
	add("thrown", {{"chest", axis_angle({1, 0, 0}, 0.1)}}, up(0.5), axis_angle({0, 1, 0}, 0.7), {0, 2.0, -4.0}, {{"chest", none}});
	return list;
}

struct Record {
	float speed = 0, spin = 0, gap = 0, end_gap = 0, lowest = 1e9f, highest = -1e9f;
	double start_energy = 0, peak_energy = 0;
	bool asleep = false;
	std::vector<RagdollPartState> end;
};

// A part's mass and where its center of mass sits in its own space, from its capsules: what Box3D works out.
struct Mass {
	double mass = 0;
	humanoid::D3 center;
	double reach = 0; // From the center to the farthest point of any capsule: no moment of inertia exceeds mass * reach^2.
};

Mass mass_of(const RagdollPart& part)
{
	std::vector<CapsuleShape> shapes{part.capsule};
	shapes.insert(shapes.end(), part.more.begin(), part.more.end());
	Mass m;
	std::vector<std::pair<double, humanoid::D3>> pieces;
	for (const auto& c : shapes) {
		const humanoid::D3 a = humanoid::to_d3(c.point_a), b = humanoid::to_d3(c.point_b);
		const double r = c.radius, h = std::sqrt(humanoid::dot(b - a, b - a));
		const double mass = part.density * (std::numbers::pi * r * r * h + 4.0 / 3.0 * std::numbers::pi * r * r * r);
		pieces.emplace_back(mass, (a + b) * 0.5);
		m.mass += mass;
	}
	for (const auto& [mass, center] : pieces) m.center = m.center + center * (mass / m.mass);
	for (const auto& c : shapes)
		for (const auto& p : {c.point_a, c.point_b}) {
			const humanoid::D3 d = humanoid::to_d3(p) - m.center;
			m.reach = std::max(m.reach, std::sqrt(humanoid::dot(d, d)) + c.radius);
		}
	return m;
}

// Linear kinetic and potential energy of the parts. Joints and contacts only take energy away, so this never rises above
// what the ragdoll started with, its spin included; an explosion is energy from nowhere.
double energy(const std::vector<Mass>& masses, const RagdollState& state)
{
	double e = 0;
	for (std::size_t i = 0; i < masses.size(); ++i) {
		const auto& p = state.parts[i];
		const humanoid::D3 center = humanoid::rotate(humanoid::to_dq(p.pose.rotation), masses[i].center) + humanoid::to_d3(p.pose.position);
		const double v = length(p.linear_velocity);
		e += 0.5 * masses[i].mass * v * v + masses[i].mass * 9.81 * center.y;
	}
	return e;
}

Record drop(const Fall& fall, int ticks)
{
	World world;
	RagdollDesc desc;
	const std::uint64_t id = world.fall(fall.now, fall.before, &desc);
	std::vector<Mass> masses;
	for (const auto& part : desc.parts) masses.push_back(mass_of(part));
	Record r;
	const auto start = *world.physics.ragdoll(id);
	r.start_energy = energy(masses, start);
	for (std::size_t i = 0; i < masses.size(); ++i) {
		const double w = length(start.parts[i].angular_velocity);
		r.start_energy += 0.5 * masses[i].mass * masses[i].reach * masses[i].reach * w * w;
	}
	r.peak_energy = energy(masses, start);
	for (int t = 0; t < ticks; ++t) {
		world.tick();
		const auto state = world.physics.ragdoll(id);
		ENGINE_REQUIRE(state.has_value());
		r.peak_energy = std::max(r.peak_energy, energy(masses, *state));
		for (const auto& p : state->parts) {
			r.speed = std::max(r.speed, length(p.linear_velocity));
			r.spin = std::max(r.spin, length(p.angular_velocity));
		}
		const auto joints = world.physics.ragdoll_joints(id);
		for (const auto& joint : joints) {
			if (t >= 2) r.gap = std::max(r.gap, joint.linear_separation);
			if (t == ticks - 1) r.end_gap = std::max(r.end_gap, joint.linear_separation);
		}
		if (t == ticks - 1) {
			r.asleep = !state->awake;
			r.end = state->parts;
		}
	}
	for (const auto& p : r.end) {
		r.lowest = std::min(r.lowest, p.pose.position.y);
		r.highest = std::max(r.highest, p.pose.position.y);
	}
	return r;
}

} // namespace

ENGINE_TEST(ragdoll_settles_from_many_poses_without_exploding)
{
	for (const Fall& fall : falls()) {
		const Record r = drop(fall, 600);
		std::printf("%-10s energy %.0f J at most of %.0f J, speed %.2f m/s, spin %.1f rad/s, gap %.3f m (%.4f m at rest), end y %.3f..%.3f, %s\n",
			fall.name.c_str(), r.peak_energy, r.start_energy, r.speed, r.spin, r.gap, r.end_gap, r.lowest, r.highest, r.asleep ? "asleep" : "awake");
		// No energy from nowhere: what an explosion is.
		ENGINE_CHECK(r.peak_energy <= r.start_energy * 1.01 + 0.5);
		ENGINE_CHECK(r.speed < 15.0f);
		// The joints hold: they stretch a little as a limb slaps the floor, and close up again.
		ENGINE_CHECK(r.gap < 0.08f);
		ENGINE_CHECK(r.end_gap < 0.005f);
		// It lies on the floor, not through it, and has stopped.
		ENGINE_CHECK(r.lowest > -0.02f);
		ENGINE_CHECK(r.highest < 0.5f);
		ENGINE_CHECK(r.asleep);
	}
}

ENGINE_TEST(ragdoll_falls_the_same_way_every_time)
{
	for (const Fall& fall : falls()) {
		const Record a = drop(fall, 240), b = drop(fall, 240);
		ENGINE_REQUIRE(a.end.size() == b.end.size());
		for (std::size_t i = 0; i < a.end.size(); ++i) {
			ENGINE_CHECK(std::memcmp(&a.end[i].pose, &b.end[i].pose, sizeof(Pose)) == 0);
			ENGINE_CHECK(std::memcmp(&a.end[i].linear_velocity, &b.end[i].linear_velocity, sizeof(Vec3)) == 0);
		}
	}
}

ENGINE_TEST(ragdoll_mirrors_no_entity_and_never_writes_the_scene)
{
	World world;
	const auto snapshot = world.scene.snapshot();
	const auto revision = world.bus.revision();
	const auto bodies = world.physics.diagnostics().world.body_count;
	const auto pose = humanoid::posed({}, {0, 0.3, 0});
	const std::uint64_t id = world.fall(pose, pose);
	const auto d = world.physics.diagnostics();
	ENGINE_CHECK_EQ(d.ragdolls, std::size_t(1));
	ENGINE_CHECK_EQ(d.ragdoll_parts, std::size_t(13));
	ENGINE_CHECK_EQ(d.world.body_count, bodies + 13);
	ENGINE_CHECK_EQ(d.world.joint_count, 12); // Every part but the hips hangs from another.
	ENGINE_CHECK_EQ(d.authored_entities, std::size_t(2));
	ENGINE_CHECK_EQ(world.physics.ragdoll_of(world.person), id);
	ENGINE_CHECK(world.physics.ragdolls() == std::vector<std::uint64_t>{id});
	const float head = part(*world.physics.ragdoll(id), "head").pose.position.y;
	for (int t = 0; t < 60; ++t) world.tick();
	ENGINE_CHECK(part(*world.physics.ragdoll(id), "head").pose.position.y < head - 0.5f);
	ENGINE_CHECK_EQ(world.bus.revision(), revision);
	ENGINE_CHECK(world.scene.snapshot() == snapshot);
	ENGINE_CHECK(world.physics.pending_transforms(world.scene).empty());
	// The ragdoll's parts show in the debug outlines, under the person.
	const auto lines = world.physics.debug_geometry().lines;
	ENGINE_CHECK(std::any_of(lines.begin(), lines.end(), [&](const DebugLine& l) { return l.category == DebugCategory::Ragdoll && l.entity == world.person; }));
}

ENGINE_TEST(ragdoll_starts_with_the_velocity_of_the_pose)
{
	World world(false);
	const auto now = humanoid::posed({}, {0, 1.0, 0});
	const auto before = humanoid::posed({}, {0, 1.0, -3.0 * tick_seconds});
	const std::uint64_t id = world.fall(now, before);
	const auto start = world.physics.ragdoll(id);
	for (const auto& p : start->parts) ENGINE_CHECK(std::abs(p.linear_velocity.z - 3.0f) < 1e-3f);
	world.tick();
	const auto moved = world.physics.ragdoll(id);
	for (std::size_t i = 0; i < moved->parts.size(); ++i) {
		const float dz = moved->parts[i].pose.position.z - start->parts[i].pose.position.z;
		ENGINE_CHECK(std::abs(dz - 3.0f * tick_seconds) < 0.01f);
	}
}

ENGINE_TEST(ray_hits_on_a_ragdoll_name_its_owner_and_the_bone)
{
	World world(false);
	const auto pose = humanoid::posed({}, {0, 0.6, 0}, axis_angle({1, 0, 0}, 1.5707963));
	const std::uint64_t id = world.fall(pose, pose);
	for (int t = 0; t < 120; ++t) world.tick();
	const auto state = *world.physics.ragdoll(id);
	const Vec3 chest = part(state, "chest").pose.position;
	const auto hit = world.physics.raycast({chest.x, 2.0f, chest.z}, {0, -1, 0}, 5.0f);
	ENGINE_REQUIRE(hit.has_value());
	ENGINE_CHECK_EQ(hit->entity, world.person);
	ENGINE_CHECK_EQ(hit->ragdoll, id);
	ENGINE_CHECK(hit->bone == humanoid::index_of("chest") || hit->bone == humanoid::index_of("spine"));
	ENGINE_CHECK_EQ(hit->debris, std::uint64_t(0));
	// Rays can leave ragdolls out.
	RaycastFilter through;
	through.mask = ~ragdoll_category;
	const auto floor = world.physics.raycast({chest.x, 2.0f, chest.z}, {0, -1, 0}, 5.0f, through);
	ENGINE_REQUIRE(floor.has_value());
	ENGINE_CHECK_EQ(floor->entity, world.floor);
	ENGINE_CHECK_EQ(floor->bone, -1);
}

ENGINE_TEST(a_shot_at_a_ragdoll_pushes_the_part_it_hits)
{
	World world(false);
	world.physics.set_simulation_enabled(false); // Hanging still in the air, so only the shot moves it.
	const auto pose = humanoid::posed({}, {0, 1.0, 0});
	const std::uint64_t id = world.fall(pose, pose);
	// The hand rides on the forearm's body.
	const Vec3 hand = pose[std::size_t(humanoid::index_of("hand_l"))].position;
	// Through the entity, as gameplay's shots go: the part nearest the point takes it.
	ENGINE_CHECK(world.physics.apply_impulse(world.person, {0, 0, 4}, {hand.x + 0.08f, hand.y, hand.z}));
	const auto after = *world.physics.ragdoll(id);
	ENGINE_CHECK(part(after, "forearm_l").linear_velocity.z > 1.0f);
	ENGINE_CHECK(length(part(after, "chest").linear_velocity) < 1e-6f);
	// By bone.
	ENGINE_CHECK(world.physics.apply_ragdoll_impulse(id, {0, 0, 50}, part(after, "chest").pose.position, humanoid::index_of("chest")));
	ENGINE_CHECK(part(*world.physics.ragdoll(id), "chest").linear_velocity.z > 1.0f);
	ENGINE_CHECK(!world.physics.apply_ragdoll_impulse(id, {0, 0, 1}, {}, 999));
	ENGINE_CHECK(!world.physics.apply_ragdoll_impulse(id + 100, {0, 0, 1}, {}));
}

ENGINE_TEST(the_owners_own_body_sits_out_while_it_is_a_ragdoll)
{
	World world;
	const auto through_capsule = [&] {
		const auto hit = world.physics.raycast({-3, 1.4f, 0}, {1, 0, 0}, 6.0f);
		return hit && hit->entity == world.person && hit->ragdoll == 0;
	};
	ENGINE_CHECK(through_capsule());
	const auto pose = humanoid::posed({}, {0, 0.05f, 0});
	const std::uint64_t id = world.fall(pose, pose);
	for (int t = 0; t < 120; ++t) world.tick();
	ENGINE_CHECK(!through_capsule());
	// It stays out when its colliders change and the body is rebuilt.
	auto record = *world.scene.inspect(world.person);
	record.components.colliders.front().radius = 0.35;
	ENGINE_REQUIRE(static_cast<bool>(world.bus.execute(world.ctx, world.bus.revision(), commands::SetColliders{world.person, record.components.colliders})));
	world.tick();
	ENGINE_CHECK(!through_capsule());
	ENGINE_CHECK(world.physics.destroy_ragdoll(id));
	ENGINE_CHECK(!world.physics.destroy_ragdoll(id));
	ENGINE_CHECK(through_capsule());
	ENGINE_CHECK_EQ(world.physics.ragdoll_of(world.person), std::uint64_t(0));
	ENGINE_CHECK_EQ(world.physics.diagnostics().world.joint_count, 0);
}

ENGINE_TEST(a_ragdoll_goes_with_its_entity_and_with_a_reset)
{
	World world;
	const auto pose = humanoid::posed({}, {0, 0.05f, 0});
	world.fall(pose, pose);
	ENGINE_REQUIRE(static_cast<bool>(world.bus.execute(world.ctx, world.bus.revision(), commands::DeleteEntity{world.person})));
	world.tick();
	ENGINE_CHECK(world.physics.ragdolls().empty());
	ENGINE_CHECK_EQ(world.physics.diagnostics().world.body_count, 1);

	World again;
	again.fall(pose, pose);
	Debris piece;
	piece.pose.position = {2, 1, 0};
	piece.collider.shape = BoxShape{{0.05f, 0.05f, 0.05f}, {}};
	ENGINE_REQUIRE(static_cast<bool>(again.physics.spawn_debris(piece)));
	ENGINE_REQUIRE(static_cast<bool>(again.physics.reset_simulation(again.scene)));
	ENGINE_CHECK(again.physics.ragdolls().empty());
	ENGINE_CHECK(again.physics.debris().empty());
	ENGINE_CHECK_EQ(again.physics.diagnostics().world.body_count, 2);
}

ENGINE_TEST(ragdoll_contacts_name_its_owner)
{
	World world(false);
	const auto pose = humanoid::posed({}, {0, 0.3, 0});
	world.fall(pose, pose);
	bool touched = false;
	for (int t = 0; t < 90; ++t) {
		world.tick();
		for (const auto& e : world.physics.events()) {
			ENGINE_CHECK((e.a == world.floor || e.a == world.person) && (e.b == world.floor || e.b == world.person));
			touched = touched || (e.kind == CollisionEvent::Kind::ContactBegin && (e.a == world.person || e.b == world.person));
		}
	}
	ENGINE_CHECK(touched);
}

ENGINE_TEST(ragdoll_refuses_bad_descriptions)
{
	World world(false);
	const RagdollPlan plan = plan_ragdoll(humanoid::skeleton(), {});
	const auto pose = humanoid::posed({}, {0, 1, 0});
	const RagdollDesc good = place_ragdoll(plan, pose, pose, tick_seconds);
	ENGINE_CHECK(!world.physics.create_ragdoll(0, good));
	ENGINE_CHECK(!world.physics.create_ragdoll(world.person, {}));
	auto later = good;
	later.parts[1].parent = 5;
	ENGINE_CHECK(!world.physics.create_ragdoll(world.person, later));
	auto twice = good;
	twice.parts[2].bone = twice.parts[1].bone;
	ENGINE_CHECK(!world.physics.create_ragdoll(world.person, twice));
	auto nan = good;
	nan.parts[3].pose.position.y = std::nanf("");
	ENGINE_CHECK(!world.physics.create_ragdoll(world.person, nan));
	auto weightless = good;
	weightless.parts[0].density = 0;
	ENGINE_CHECK(!world.physics.create_ragdoll(world.person, weightless));
	auto loose = good;
	loose.parts[4].twist_lower = 1;
	ENGINE_CHECK(!world.physics.create_ragdoll(world.person, loose));
	ENGINE_CHECK_EQ(world.physics.diagnostics().world.body_count, 1); // Nothing half built was left behind.
	ENGINE_REQUIRE(static_cast<bool>(world.physics.create_ragdoll(world.person, good)));
	ENGINE_CHECK(!world.physics.create_ragdoll(world.person, good));
	// At most max_ragdolls.
	RagdollDesc one;
	one.parts.push_back(good.parts.front());
	for (std::size_t i = 1; i < max_ragdolls; ++i) ENGINE_CHECK(static_cast<bool>(world.physics.create_ragdoll(1000 + i, one)));
	ENGINE_CHECK(!world.physics.create_ragdoll(5000, one));
}

ENGINE_TEST(debris_is_capped_and_the_oldest_piece_makes_room)
{
	World world(false);
	std::vector<std::uint64_t> ids;
	for (int i = 0; i < 70; ++i) {
		Debris piece;
		piece.look = world.floor;
		piece.pose.position = {float(i % 10) * 0.3f, 1.0f + float(i / 10) * 0.3f, 0};
		piece.collider.shape = BoxShape{{0.05f, 0.05f, 0.05f}, {}};
		auto spawned = world.physics.spawn_debris(piece);
		ENGINE_REQUIRE(static_cast<bool>(spawned));
		ids.push_back(spawned.value());
	}
	const auto pieces = world.physics.debris();
	ENGINE_CHECK_EQ(pieces.size(), max_debris);
	ENGINE_CHECK_EQ(pieces.front().id, ids[6]);
	ENGINE_CHECK_EQ(pieces.back().id, ids.back());
	ENGINE_CHECK_EQ(pieces.front().look, world.floor);
	const auto d = world.physics.diagnostics();
	ENGINE_CHECK_EQ(d.debris, max_debris);
	ENGINE_CHECK_EQ(d.debris_spawned, std::uint64_t(70));
	ENGINE_CHECK_EQ(d.debris_replaced, std::uint64_t(6));
	ENGINE_CHECK_EQ(d.world.body_count, 1 + int(max_debris));
	ENGINE_CHECK(world.physics.remove_debris(ids.back()));
	ENGINE_CHECK(!world.physics.remove_debris(ids.back()));
	ENGINE_CHECK(!world.physics.remove_debris(ids.front()));
}

ENGINE_TEST(debris_goes_once_asleep_for_two_seconds_or_past_its_lifetime)
{
	World world(false);
	Debris resting;
	resting.pose.position = {0, 0.3f, 0};
	resting.collider.shape = BoxShape{{0.1f, 0.1f, 0.1f}, {}};
	resting.lifetime = 100;
	Debris brief = resting;
	brief.pose.position = {3, 5, 0};
	brief.lifetime = 0.5;
	const std::uint64_t stays = world.physics.spawn_debris(resting).value();
	const std::uint64_t short_lived = world.physics.spawn_debris(brief).value();
	const auto has = [&](std::uint64_t id) {
		const auto list = world.physics.debris();
		return std::any_of(list.begin(), list.end(), [&](const DebrisState& d) { return d.id == id; });
	};
	int gone_at = -1;
	double longest_sleep = 0;
	for (int t = 1; t <= 600 && gone_at < 0; ++t) {
		world.tick();
		if (t == 29) ENGINE_CHECK(has(short_lived));
		if (t == 30) ENGINE_CHECK(!has(short_lived));
		for (const auto& d : world.physics.debris()) longest_sleep = std::max(longest_sleep, d.asleep);
		if (!has(stays)) gone_at = t;
	}
	// It landed, fell asleep, and went after two seconds asleep: well before its lifetime.
	ENGINE_CHECK(gone_at > 120 && gone_at < 360);
	ENGINE_CHECK(longest_sleep >= debris_sleep_seconds - 2 * tick_seconds);
	ENGINE_CHECK_EQ(world.physics.diagnostics().debris_expired, std::uint64_t(2));
	ENGINE_CHECK_EQ(world.physics.diagnostics().world.body_count, 1);
}

ENGINE_TEST(debris_mirrors_no_entity_and_rays_can_leave_it_out)
{
	World world(false);
	const auto revision = world.bus.revision();
	Debris piece;
	piece.pose.position = {0, 0.5f, 0};
	piece.collider.shape = BoxShape{{0.2f, 0.2f, 0.2f}, {}};
	const std::uint64_t id = world.physics.spawn_debris(piece).value();
	for (int t = 0; t < 60; ++t) {
		world.tick();
		for (const auto& e : world.physics.events()) ENGINE_CHECK((e.a == world.floor || e.a == 0) && (e.b == world.floor || e.b == 0));
	}
	const auto hit = world.physics.raycast({0, 2, 0}, {0, -1, 0}, 5);
	ENGINE_REQUIRE(hit.has_value());
	ENGINE_CHECK_EQ(hit->debris, id);
	ENGINE_CHECK_EQ(hit->entity, EntityId(0));
	RaycastFilter through;
	through.mask = ~debris_category;
	ENGINE_CHECK_EQ(world.physics.raycast({0, 2, 0}, {0, -1, 0}, 5, through)->entity, world.floor);
	ENGINE_CHECK_EQ(world.bus.revision(), revision);
	// An impulse through an entity never reaches debris.
	ENGINE_CHECK(!world.physics.apply_impulse(0, {0, 1, 0}, {0, 0.2f, 0}));
}

ENGINE_TEST(debris_refuses_bad_pieces)
{
	World world(false);
	Debris good;
	good.collider.shape = SphereShape{0.1f, {}};
	ENGINE_CHECK(static_cast<bool>(world.physics.spawn_debris(good)));
	auto never = good;
	never.lifetime = 0;
	ENGINE_CHECK(!world.physics.spawn_debris(never));
	auto forever = good;
	forever.lifetime = 601;
	ENGINE_CHECK(!world.physics.spawn_debris(forever));
	auto ghost = good;
	ghost.collider.is_sensor = true;
	ENGINE_CHECK(!world.physics.spawn_debris(ghost));
	auto weightless = good;
	weightless.collider.density = 0;
	ENGINE_CHECK(!world.physics.spawn_debris(weightless));
	auto nan = good;
	nan.linear_velocity.x = std::nanf("");
	ENGINE_CHECK(!world.physics.spawn_debris(nan));
	ENGINE_CHECK_EQ(world.physics.debris().size(), std::size_t(1));
}
