// Joints in PhysicsWorld and the ragdoll builder (Milestone 5, slice 4). Real Box3D worlds, no mocks.

#include "engine/physics/physics_world.h"
#include "engine/physics/ragdoll.h"
#include "engine/test/test.h"

#include "humanoid.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

using namespace engine::physics;

namespace {

constexpr float pi = std::numbers::pi_v<float>;

PhysicsWorld make_world()
{
	auto result = PhysicsWorld::create({});
	ENGINE_REQUIRE(result.ok());
	return std::move(result).value();
}

BodyHandle body_at(PhysicsWorld& world, Vec3 position, BodyType type = BodyType::Dynamic, ShapeDesc shape = SphereShape{0.1f, {}})
{
	BodyDesc desc;
	desc.type = type;
	desc.pose.position = position;
	auto body = world.create_body(desc);
	ENGINE_REQUIRE(body.ok());
	ColliderDesc collider;
	collider.shape = shape;
	ENGINE_REQUIRE(world.add_collider(body.value(), collider).ok());
	return body.value();
}

float distance(Vec3 a, Vec3 b)
{
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// A pendulum: a static anchor, and a capsule hanging 1 m to its side from a joint at the anchor.
struct Pendulum {
	PhysicsWorld world = make_world();
	BodyHandle anchor, bob;
	JointHandle joint;

	explicit Pendulum(JointDesc desc)
	{
		anchor = body_at(world, {0, 2, 0}, BodyType::Static);
		bob = body_at(world, {0, 2, 0}, BodyType::Dynamic, CapsuleShape{{0.2f, 0, 0}, {1.0f, 0, 0}, 0.1f});
		desc.body_a = anchor;
		desc.body_b = bob;
		// Both frames' z along +x, the way the capsule hangs out at rest.
		const Quat along_x{0, 0.70710678f, 0, 0.70710678f};
		desc.frame_a.rotation = along_x;
		desc.frame_b.rotation = along_x;
		auto made = world.create_joint(desc);
		ENGINE_REQUIRE(made.ok());
		joint = made.value();
	}
};

const RagdollPlan::Body* body_for(const RagdollPlan& plan, const std::string& name)
{
	const int joint = humanoid::index_of(name);
	for (const auto& body : plan.bodies)
		if (body.joint == joint) return &body;
	return nullptr;
}

} // namespace

// ---- Joints ----

ENGINE_TEST(ball_joint_holds_the_bodies_together_within_its_cone)
{
	JointDesc desc;
	desc.cone = 30.0f * pi / 180.0f;
	Pendulum p(desc);
	ENGINE_CHECK_EQ(p.world.stats().joint_count, 1);
	// Gravity pulls the horizontal capsule down; the cone stops it 30 degrees below.
	float worst_cone = 0, worst_gap = 0;
	for (int i = 0; i < 180; ++i) {
		ENGINE_REQUIRE(p.world.step() == PhysicsError::None);
		const auto state = p.world.joint_state(p.joint);
		ENGINE_REQUIRE(state.ok());
		worst_cone = std::max(worst_cone, state.value().cone);
		worst_gap = std::max(worst_gap, state.value().linear_separation);
	}
	ENGINE_CHECK(worst_cone < desc.cone + 0.05f);
	ENGINE_CHECK(worst_cone > desc.cone - 0.05f);
	ENGINE_CHECK(worst_gap < 0.01f);
	// The joint point stays at the anchor.
	ENGINE_CHECK(distance(p.world.body_pose(p.bob).value().position, {0, 2, 0}) < 0.01f);
}

ENGINE_TEST(ball_joint_twist_stays_within_its_limits)
{
	JointDesc desc;
	desc.limit_cone = false;
	desc.twist_lower = -0.2f;
	desc.twist_upper = 0.3f;
	Pendulum p(desc);
	ENGINE_REQUIRE(p.world.set_gravity({}) == PhysicsError::None);
	// Spin the capsule about its own length, both ways.
	ENGINE_REQUIRE(p.world.apply_angular_impulse(p.bob, {0.5f, 0, 0}) == PhysicsError::None);
	float high = 0, low = 0;
	for (int i = 0; i < 120; ++i) {
		p.world.step();
		high = std::max(high, p.world.joint_state(p.joint).value().twist);
	}
	p.world.set_angular_velocity(p.bob, {});
	ENGINE_REQUIRE(p.world.apply_angular_impulse(p.bob, {-1.0f, 0, 0}) == PhysicsError::None);
	for (int i = 0; i < 120; ++i) {
		p.world.step();
		low = std::min(low, p.world.joint_state(p.joint).value().twist);
	}
	ENGINE_CHECK(high > 0.25f && high < 0.35f);
	ENGINE_CHECK(low < -0.15f && low > -0.25f);
}

ENGINE_TEST(hinge_turns_only_about_its_axis_within_its_limits)
{
	JointDesc desc;
	desc.type = JointType::Revolute;
	desc.twist_lower = -0.5f;
	desc.twist_upper = 0.5f;
	Pendulum p(desc);
	// Frames' z is +x, so the hinge axis runs along the capsule: gravity cannot swing it down.
	for (int i = 0; i < 120; ++i) p.world.step();
	const auto state = p.world.joint_state(p.joint).value();
	ENGINE_CHECK(state.type == JointType::Revolute);
	ENGINE_CHECK(state.angular_separation < 0.05f);
	const Pose pose = p.world.body_pose(p.bob).value();
	// Still pointing along +x: the end of the capsule has not dropped.
	const humanoid::D3 tip = humanoid::rotate(humanoid::to_dq(pose.rotation), {1, 0, 0});
	ENGINE_CHECK(tip.x > 0.99);
	// Turning about the axis stops at the limit.
	p.world.apply_angular_impulse(p.bob, {0.5f, 0, 0});
	float high = 0;
	for (int i = 0; i < 120; ++i) {
		p.world.step();
		high = std::max(high, p.world.joint_state(p.joint).value().twist);
	}
	ENGINE_CHECK(high < 0.55f && high > 0.45f);
}

ENGINE_TEST(joint_friction_slows_a_swinging_body)
{
	const auto swing_after = [](float friction) {
		JointDesc desc;
		desc.limit_cone = false;
		desc.limit_twist = false;
		desc.friction_torque = friction;
		Pendulum p(desc);
		for (int i = 0; i < 30; ++i) p.world.step();
		const Vec3 w = p.world.angular_velocity(p.bob).value();
		return std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z);
	};
	const float free = swing_after(0), held = swing_after(120);
	ENGINE_CHECK(free > 1.0f);
	ENGINE_CHECK(held < 0.5f * free);
}

ENGINE_TEST(filter_joint_stops_two_bodies_colliding)
{
	PhysicsWorld world = make_world();
	world.set_gravity({});
	const BodyHandle still = body_at(world, {0, 0, 0}, BodyType::Dynamic, SphereShape{0.5f, {}});
	const BodyHandle moving = body_at(world, {-2, 0, 0}, BodyType::Dynamic, SphereShape{0.5f, {}});
	JointDesc filter;
	filter.type = JointType::Filter;
	filter.body_a = still;
	filter.body_b = moving;
	ENGINE_REQUIRE(world.create_joint(filter).ok());
	world.set_linear_velocity(moving, {4, 0, 0});
	for (int i = 0; i < 60; ++i) world.step();
	// It passed straight through, and the other never moved.
	ENGINE_CHECK(world.body_pose(moving).value().position.x > 1.5f);
	ENGINE_CHECK(distance(world.body_pose(still).value().position, {0, 0, 0}) < 1e-4f);
}

ENGINE_TEST(joint_rejects_bad_descriptions)
{
	PhysicsWorld world = make_world();
	const BodyHandle a = body_at(world, {0, 0, 0}), b = body_at(world, {1, 0, 0});
	JointDesc desc;
	desc.body_a = a;
	desc.body_b = b;
	ENGINE_CHECK(world.create_joint(desc).ok());
	auto same = desc;
	same.body_b = a;
	ENGINE_CHECK(world.create_joint(same).error() == PhysicsError::InvalidArgument);
	auto wide = desc;
	wide.cone = 4.0f;
	ENGINE_CHECK(world.create_joint(wide).error() == PhysicsError::InvalidArgument);
	auto backwards = desc;
	backwards.twist_lower = 0.5f;
	backwards.twist_upper = 0.1f;
	ENGINE_CHECK(world.create_joint(backwards).error() == PhysicsError::InvalidArgument);
	auto nan = desc;
	nan.frame_a.position.x = std::nanf("");
	ENGINE_CHECK(world.create_joint(nan).error() == PhysicsError::InvalidArgument);
	auto degenerate = desc;
	degenerate.frame_b.rotation = {0, 0, 0, 0};
	ENGINE_CHECK(world.create_joint(degenerate).error() == PhysicsError::InvalidArgument);
	auto negative = desc;
	negative.friction_torque = -1;
	ENGINE_CHECK(world.create_joint(negative).error() == PhysicsError::InvalidArgument);
	auto missing = desc;
	missing.body_b = {};
	ENGINE_CHECK(world.create_joint(missing).error() == PhysicsError::InvalidBody);
	ENGINE_CHECK(world.destroy_joint({}) == PhysicsError::InvalidJoint);
	ENGINE_CHECK(!world.joint_state({}).ok());
}

ENGINE_TEST(destroying_a_body_destroys_its_joints)
{
	PhysicsWorld world = make_world();
	const BodyHandle a = body_at(world, {0, 0, 0}), b = body_at(world, {1, 0, 0}), c = body_at(world, {2, 0, 0});
	JointDesc desc;
	desc.body_a = a;
	desc.body_b = b;
	const JointHandle ab = world.create_joint(desc).value();
	desc.body_a = b;
	desc.body_b = c;
	const JointHandle bc = world.create_joint(desc).value();
	ENGINE_CHECK_EQ(world.stats().joint_count, 2);
	ENGINE_REQUIRE(world.destroy_body(b) == PhysicsError::None);
	ENGINE_CHECK(!world.is_valid(ab));
	ENGINE_CHECK(!world.is_valid(bc));
	ENGINE_CHECK_EQ(world.stats().joint_count, 0);
	// A new joint in the freed slot does not bring the old handle back.
	desc.body_a = a;
	desc.body_b = c;
	const JointHandle ac = world.create_joint(desc).value();
	ENGINE_CHECK(world.is_valid(ac));
	ENGINE_CHECK(!world.is_valid(ab));
	ENGINE_CHECK(world.destroy_joint(ac) == PhysicsError::None);
	ENGINE_CHECK(world.destroy_joint(ac) == PhysicsError::InvalidJoint);
}

ENGINE_TEST(disabled_body_neither_falls_nor_blocks_rays)
{
	PhysicsWorld world = make_world();
	const BodyHandle ball = body_at(world, {0, 1, 0}, BodyType::Dynamic, SphereShape{0.5f, {}});
	world.set_linear_velocity(ball, {1, 0, 0});
	ENGINE_REQUIRE(world.set_body_enabled(ball, false) == PhysicsError::None);
	ENGINE_CHECK(!world.is_enabled(ball).value());
	for (int i = 0; i < 30; ++i) world.step();
	ENGINE_CHECK(distance(world.body_pose(ball).value().position, {0, 1, 0}) < 1e-6f);
	ENGINE_CHECK(!world.cast_ray({0, 5, 0}, {0, -10, 0}).hit);
	ENGINE_REQUIRE(world.set_body_enabled(ball, true) == PhysicsError::None);
	ENGINE_REQUIRE(world.set_body_enabled(ball, true) == PhysicsError::None); // Twice is harmless.
	ENGINE_CHECK(world.cast_ray({0, 5, 0}, {0, -10, 0}).hit);
	world.step();
	ENGINE_CHECK(world.body_pose(ball).value().position.y < 1.0f);
	ENGINE_CHECK(world.set_body_enabled({}, false) == PhysicsError::InvalidBody);
}

ENGINE_TEST(angular_impulse_spins_a_body_and_is_refused_by_static_ones)
{
	PhysicsWorld world = make_world();
	world.set_gravity({});
	const BodyHandle ball = body_at(world, {0, 0, 0}, BodyType::Dynamic, SphereShape{0.5f, {}});
	ENGINE_REQUIRE(world.apply_angular_impulse(ball, {0, 100, 0}) == PhysicsError::None);
	const Vec3 w = world.angular_velocity(ball).value();
	ENGINE_CHECK(w.y > 0.1f);
	ENGINE_CHECK(std::abs(w.x) < 1e-6f && std::abs(w.z) < 1e-6f);
	const BodyHandle wall = body_at(world, {3, 0, 0}, BodyType::Static);
	ENGINE_CHECK(world.apply_angular_impulse(wall, {0, 1, 0}) == PhysicsError::InvalidArgument);
	ENGINE_CHECK(world.apply_angular_impulse(ball, {std::nanf(""), 0, 0}) == PhysicsError::InvalidArgument);
}

ENGINE_TEST(swing_and_twist_split_a_rotation_as_box3d_does)
{
	using humanoid::axis_angle;
	using humanoid::to_quat;
	const Quat a = to_quat(axis_angle({0, 1, 0}, 0.7));
	// A turn about z alone is all twist; about x alone, all swing.
	const SwingTwist twist = swing_twist(a, to_quat(humanoid::to_dq(a) * axis_angle({0, 0, 1}, 0.4)));
	ENGINE_CHECK(std::abs(twist.twist - 0.4f) < 1e-5f && std::abs(twist.swing) < 1e-5f);
	const SwingTwist swing = swing_twist(a, to_quat(humanoid::to_dq(a) * axis_angle({1, 0, 0}, -0.9)));
	ENGINE_CHECK(std::abs(swing.swing - 0.9f) < 1e-5f && std::abs(swing.twist) < 1e-5f);
}

// ---- The ragdoll builder ----

ENGINE_TEST(ragdoll_plan_gives_long_skinned_bones_capsules_along_them)
{
	const auto skeleton = humanoid::skeleton();
	const RagdollPlan plan = plan_ragdoll(skeleton, {});
	ENGINE_REQUIRE(plan.ok());
	// Every bone but the unskinned root: hips, spine, chest, neck, head, and two for each limb. Hands and feet are shorter
	// than the bones above them, so they ride on forearms and calves.
	ENGINE_CHECK_EQ(plan.bodies.size(), std::size_t(13));
	ENGINE_CHECK(body_for(plan, "root") == nullptr);
	ENGINE_CHECK_EQ(plan.body_of_joint[0], -1);
	// The hips' bone runs to the spine, which carries most of the skeleton, not to a thigh.
	const auto* pelvis = body_for(plan, "pelvis");
	ENGINE_REQUIRE(pelvis != nullptr);
	ENGINE_CHECK(std::abs(pelvis->length - 0.1f) < 1e-5f);
	ENGINE_CHECK_EQ(pelvis->parent, -1);
	// A forearm: from the elbow to the wrist along its own x, a little thinner than its flesh's box.
	const auto* forearm = body_for(plan, "forearm_l");
	ENGINE_REQUIRE(forearm != nullptr);
	ENGINE_CHECK(std::abs(forearm->length - 0.25f) < 1e-5f);
	ENGINE_CHECK(std::abs(forearm->radius - 0.8f * 0.045f) < 1e-4f);
	ENGINE_CHECK(std::abs(forearm->capsule.point_a.x - forearm->radius) < 1e-5f);
	ENGINE_CHECK(std::abs(forearm->capsule.point_b.x - (0.25f - forearm->radius)) < 1e-5f);
	ENGINE_CHECK(std::abs(forearm->capsule.point_b.y) < 1e-5f && std::abs(forearm->capsule.point_b.z) < 1e-5f);
	ENGINE_CHECK_EQ(plan.bodies[std::size_t(forearm->parent)].joint, humanoid::index_of("upperarm_l"));
	// A foot ends a limb: it reaches as far as its flesh, forward, on the calf's body.
	ENGINE_CHECK(body_for(plan, "foot_l") == nullptr);
	const auto* calf = body_for(plan, "calf_l");
	ENGINE_REQUIRE(calf != nullptr && calf->more.size() == 1);
	const auto& calf_rest = skeleton[std::size_t(humanoid::index_of("calf_l"))].rest;
	const humanoid::D3 toe = humanoid::rotate(humanoid::to_dq(calf_rest.rotation), humanoid::to_d3(calf->more[0].point_b)) + humanoid::to_d3(calf_rest.position);
	const float foot_radius = calf->more[0].radius;
	ENGINE_CHECK(std::abs(foot_radius - 0.8f * 0.05f) < 1e-4f);
	ENGINE_CHECK(std::abs(toe.z - (0.2 + 0.3 * 0.05 - foot_radius)) < 1e-4 && std::abs(toe.y - 0.08) < 1e-4 && std::abs(toe.x - 0.1) < 1e-4);
	ENGINE_CHECK_EQ(plan.body_of_joint[std::size_t(humanoid::index_of("foot_l"))], int(calf - plan.bodies.data()));
	ENGINE_CHECK_EQ(body_for(plan, "forearm_l")->more.size(), std::size_t(1));
	ENGINE_CHECK(body_for(plan, "head") != nullptr); // Longer than the neck: a body of its own.
	ENGINE_CHECK(body_for(plan, "upperarm_l")->more.empty());
	// Masses from the density of water: a 70 kg person is about 70 litres.
	ENGINE_CHECK(plan.mass > 30.0f && plan.mass < 110.0f);
	float total = 0;
	for (const auto& body : plan.bodies) {
		total += body.mass;
		ENGINE_CHECK(body.friction_torque > 0.0f);
	}
	ENGINE_CHECK(std::abs(total - plan.mass) < 1e-3f);
	// Joints the builder gave no body move with their nearest ancestor that has one.
	ENGINE_CHECK_EQ(plan.body_of_joint[std::size_t(humanoid::index_of("calf_r"))], int(body_for(plan, "calf_r") - plan.bodies.data()));
}

ENGINE_TEST(ragdoll_plan_frames_are_at_rest_when_the_skeleton_is)
{
	const auto skeleton = humanoid::skeleton();
	const RagdollPlan plan = plan_ragdoll(skeleton, {});
	ENGINE_REQUIRE(plan.ok());
	for (const auto& body : plan.bodies) {
		if (body.parent < 0) continue;
		const auto& joint = skeleton[std::size_t(body.joint)];
		const auto& parent = skeleton[std::size_t(plan.bodies[std::size_t(body.parent)].joint)];
		const humanoid::DQ a = humanoid::to_dq(parent.rest.rotation) * humanoid::to_dq(body.frame_parent.rotation);
		const humanoid::DQ b = humanoid::to_dq(joint.rest.rotation) * humanoid::to_dq(body.frame_child.rotation);
		const SwingTwist st = swing_twist(humanoid::to_quat(a), humanoid::to_quat(b));
		ENGINE_CHECK(std::abs(st.swing) < 1e-4f && std::abs(st.twist) < 1e-4f);
		// The frame's z runs along the bone, so the cone is about the bone's rest direction.
		const humanoid::D3 z = humanoid::rotate(b, {0, 0, 1});
		const humanoid::D3 along = humanoid::specs()[std::size_t(body.joint)].along;
		ENGINE_CHECK(humanoid::dot(z, along) > 0.9999);
		// The anchor in the parent's space is where the joint rests.
		const humanoid::D3 anchor = humanoid::rotate(humanoid::to_dq(parent.rest.rotation), humanoid::to_d3(body.frame_parent.position)) +
			humanoid::to_d3(parent.rest.position);
		ENGINE_CHECK(distance(humanoid::to_vec(anchor), joint.rest.position) < 1e-5f);
	}
}

ENGINE_TEST(ragdoll_plan_follows_per_bone_settings)
{
	const auto skeleton = humanoid::skeleton();
	RagdollSettings settings;
	settings.cone = 0.5f;
	settings.bones = {{"hand_l", 0}, {"forearm_r", -1, 0.03f, 500.0f, 0.2f, 0.1f}, {"root", 1, 0.05f}, {"foot_r", 1}};
	const RagdollPlan plan = plan_ragdoll(skeleton, settings);
	ENGINE_REQUIRE(plan.ok());
	// Taken away: no body and no capsule; the hand moves with the forearm.
	ENGINE_CHECK(body_for(plan, "hand_l") == nullptr);
	ENGINE_CHECK(body_for(plan, "forearm_l")->more.empty());
	ENGINE_CHECK_EQ(plan.body_of_joint[std::size_t(humanoid::index_of("hand_l"))], int(body_for(plan, "forearm_l") - plan.bodies.data()));
	// Asked for: a foot on a joint of its own.
	ENGINE_CHECK(body_for(plan, "foot_r") != nullptr);
	ENGINE_CHECK(body_for(plan, "calf_r")->more.empty());
	const auto* forearm = body_for(plan, "forearm_r");
	ENGINE_REQUIRE(forearm != nullptr);
	ENGINE_CHECK(std::abs(forearm->radius - 0.03f) < 1e-6f);
	ENGINE_CHECK_EQ(forearm->density, 500.0f);
	ENGINE_CHECK(std::abs(forearm->cone - 0.2f) < 1e-6f && std::abs(forearm->twist - 0.1f) < 1e-6f);
	ENGINE_CHECK(std::abs(body_for(plan, "forearm_l")->cone - 0.5f) < 1e-6f);
	// Asked for, the unskinned root gets a body, and the hips join it.
	const auto* root = body_for(plan, "root");
	ENGINE_REQUIRE(root != nullptr);
	ENGINE_CHECK_EQ(plan.bodies[std::size_t(body_for(plan, "pelvis")->parent)].joint, 0);
	ENGINE_CHECK_EQ(plan.bodies.size(), std::size_t(15));
}

ENGINE_TEST(ragdoll_plan_says_why_it_cannot_be_built)
{
	ENGINE_CHECK(!plan_ragdoll({}, {}).ok());
	ENGINE_CHECK(!plan_ragdoll({}, {}).problem.empty());
	auto skeleton = humanoid::skeleton();
	RagdollSettings settings;
	settings.density = -1;
	ENGINE_CHECK(plan_ragdoll(skeleton, settings).problem.find("range") != std::string::npos);
	settings = {};
	settings.bones = {{"head", 1}, {"head", 0}};
	ENGINE_CHECK(plan_ragdoll(skeleton, settings).problem.find("twice") != std::string::npos);
	settings = {};
	settings.bones = {{"head", 1, -2.0f}};
	ENGINE_CHECK(!plan_ragdoll(skeleton, settings).ok());
	// A parent after its child, and nothing skinned.
	auto broken = skeleton;
	broken[2].parent = 5;
	ENGINE_CHECK(!plan_ragdoll(broken, {}).ok());
	auto bare = skeleton;
	for (auto& joint : bare) joint.skinned = false;
	ENGINE_CHECK(plan_ragdoll(bare, {}).problem.find("long enough") != std::string::npos);
}

ENGINE_TEST(ragdoll_plan_keeps_the_longest_bones_past_the_part_limit)
{
	// A chain of 80 skinned joints, each 1 cm longer than the last.
	std::vector<RagdollJoint> chain;
	float y = 0;
	for (int i = 0; i < 80; ++i) {
		RagdollJoint j;
		j.name = "link" + std::to_string(i);
		j.parent = i - 1;
		j.rest.position = {0, y, 0};
		j.skinned = true;
		j.flesh_max = {0.02f, 0.02f, 0.02f};
		j.flesh_min = {-0.02f, -0.02f, -0.02f};
		chain.push_back(j);
		y += 0.1f + 0.01f * float(i);
	}
	RagdollSettings settings;
	settings.min_length = 0;
	const RagdollPlan plan = plan_ragdoll(chain, settings);
	ENGINE_REQUIRE(plan.ok());
	ENGINE_CHECK_EQ(plan.bodies.size(), max_ragdoll_parts);
	// The shortest links near the start went; the bodies still follow the skeleton's order.
	ENGINE_CHECK(plan.bodies.front().joint >= 15);
	for (std::size_t i = 1; i < plan.bodies.size(); ++i) ENGINE_CHECK(plan.bodies[i].joint > plan.bodies[i - 1].joint);
}

ENGINE_TEST(placing_a_ragdoll_takes_the_velocity_of_the_pose)
{
	const auto skeleton = humanoid::skeleton();
	const RagdollPlan plan = plan_ragdoll(skeleton, {});
	ENGINE_REQUIRE(plan.ok());
	// Walking forward at 3 m/s, the head turning 2 rad/s, sampled at 60 Hz.
	const float dt = 1.0f / 60.0f;
	const auto turn = [](double radians) { return std::map<std::string, humanoid::DQ>{{"neck", humanoid::axis_angle({0, 1, 0}, radians)}}; };
	const auto before = humanoid::posed(turn(0.0), {0, 0, 0});
	const auto now = humanoid::posed(turn(2.0 * dt), {0, 0, 3.0 * dt});
	const RagdollDesc desc = place_ragdoll(plan, now, before, dt);
	ENGINE_REQUIRE(desc.parts.size() == plan.bodies.size());
	for (const auto& part : desc.parts) {
		ENGINE_CHECK(std::abs(part.linear_velocity.z - 3.0f) < 0.02f || part.bone == humanoid::index_of("head"));
		ENGINE_CHECK(distance(part.pose.position, now[std::size_t(part.bone)].position) < 1e-6f);
	}
	const auto& head = *std::find_if(desc.parts.begin(), desc.parts.end(), [](const RagdollPart& p) { return p.bone == humanoid::index_of("head"); });
	const auto& neck = *std::find_if(desc.parts.begin(), desc.parts.end(), [](const RagdollPart& p) { return p.bone == humanoid::index_of("neck"); });
	ENGINE_CHECK(std::abs(neck.angular_velocity.y - 2.0f) < 0.01f);
	ENGINE_CHECK(std::abs(head.angular_velocity.y - 2.0f) < 0.01f);
	// Nothing moved: nothing moves.
	const RagdollDesc still = place_ragdoll(plan, before, before, dt);
	for (const auto& part : still.parts) {
		ENGINE_CHECK(distance(part.linear_velocity, {}) < 1e-6f);
		ENGINE_CHECK(distance(part.angular_velocity, {}) < 1e-6f);
	}
	ENGINE_CHECK(place_ragdoll(plan, now, std::span<const Pose>(before).first(3), dt).parts.empty());
	ENGINE_CHECK(place_ragdoll(plan, now, before, 0).parts.empty());
}

ENGINE_TEST(placing_a_ragdoll_past_its_limits_widens_them_to_fit)
{
	const auto skeleton = humanoid::skeleton();
	RagdollSettings settings;
	settings.cone = 0.5f;
	settings.twist = 0.2f;
	const RagdollPlan plan = plan_ragdoll(skeleton, settings);
	ENGINE_REQUIRE(plan.ok());
	// Arms down at the sides (90 degrees of swing) and the chest twisted 60 degrees.
	const auto pose = humanoid::posed({{"upperarm_l", humanoid::axis_angle({0, 0, 1}, -1.5)},
		{"chest", humanoid::axis_angle({0, 1, 0}, 1.0)}});
	const RagdollDesc desc = place_ragdoll(plan, pose, pose, 1.0f / 60.0f);
	ENGINE_REQUIRE(!desc.parts.empty());
	const auto part = [&](const char* name) {
		return *std::find_if(desc.parts.begin(), desc.parts.end(), [&](const RagdollPart& p) { return p.bone == humanoid::index_of(name); });
	};
	ENGINE_CHECK(part("upperarm_l").cone > 1.5f && part("upperarm_l").cone < 1.6f);
	ENGINE_CHECK(std::abs(part("upperarm_r").cone - 0.5f) < 1e-6f);
	ENGINE_CHECK(part("chest").twist_upper > 1.0f && part("chest").twist_upper < 1.1f);
	ENGINE_CHECK(std::abs(part("chest").twist_lower + 0.2f) < 1e-6f);
	ENGINE_CHECK(std::abs(part("spine").twist_upper - 0.2f) < 1e-6f);
}
