#pragma once

// Ragdolls from a skeleton (Milestone 5, D7). The builder chooses which bones get a body, sizes a capsule for each from
// the bone's length and the flesh around it, and joins them with ball joints whose cone and twist limits start from
// Box3D's human.c. Placing a plan puts its parts at a pose, moving as fast as the pose was. Plain data: no scene, and
// Box3D stays behind PhysicsWorld.

#include "engine/physics/types.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace engine::physics {

inline constexpr std::size_t max_ragdoll_parts = 64;

// One body of a ragdoll: a capsule standing for a bone of the skeleton, jointed to its parent's body at its own origin.
struct RagdollPart {
	int bone = -1;   // The skeleton joint it stands for, reported by hits. Unique within the ragdoll.
	int parent = -1; // An earlier part, or -1 for a part with no joint (the root).
	Pose pose;       // In the world, where it starts.
	Vec3 linear_velocity;  // Of its origin.
	Vec3 angular_velocity;
	CapsuleShape capsule; // In the part's own space.
	// More capsules on the same body, in its space: the ends of limbs that ride on it (a hand on a forearm).
	std::vector<CapsuleShape> more;
	float density = 1000.0f;
	float friction = 0.6f;
	// The joint to the parent part. Ignored without one.
	Pose frame_parent; // In the parent part's space.
	Pose frame_child;  // In this part's space; its origin is where the joint is.
	float cone = 0.7f;
	float twist_lower = -0.35f;
	float twist_upper = 0.35f;
	float friction_torque = 0.0f;
};

struct RagdollDesc {
	std::vector<RagdollPart> parts; // Parents first; 1 to max_ragdoll_parts.
	float linear_damping = 0.0f;
	float angular_damping = 0.0f;
	// What the parts collide with. Parts of one ragdoll never collide with each other.
	std::uint64_t mask = ~std::uint64_t{0};
	// The entity's own body leaves the simulation while the ragdoll lives, so a character's capsule does not hold it up.
	bool disable_owner = true;
};

// A joint of the skeleton as the builder reads it, parents first. Lengths in meters as the model stands in the world:
// any scale is already applied.
struct RagdollJoint {
	std::string name;
	int parent = -1;
	Pose rest; // Where it rests, in the model's space.
	// The flesh that follows it: a box in the joint's own space, from the skin's vertices. Without any, the bone gets no
	// body unless it is asked for.
	bool skinned = false;
	Vec3 flesh_min;
	Vec3 flesh_max;
};

// Settings for one bone, by name. Values left out come from the ragdoll's.
struct RagdollBoneSettings {
	std::string bone;
	// 1 gives the bone a body of its own; 0 gives it neither a body nor a shape, so it only follows its parent; -1 leaves
	// it to the builder, which gives the end of a limb shorter than the bone above it (a hand, a foot) no body of its own
	// but a capsule on that bone's, as Box3D's human.c does. Light ends on joints of their own whip about and stretch.
	int body = -1;
	float radius = 0.0f;    // Meters; 0 sizes it from the flesh.
	float density = 0.0f;   // kg/m^3; 0 is the ragdoll's.
	float cone = -1.0f;     // Radians; below 0 is the ragdoll's.
	float twist = -1.0f;    // Radians either way; below 0 is the ragdoll's.
};

struct RagdollSettings {
	float density = 1000.0f;
	float cone = 0.7f;   // How far a bone swings from its rest direction relative to its parent.
	float twist = 0.35f; // How far it turns about itself, either way.
	// Resistance to turning in every joint, as a fraction of the torque the part's own weight puts on it.
	float joint_friction = 0.5f;
	float friction = 0.6f; // Of the parts' surfaces.
	// Bones shorter than this fraction of the skeleton's size get no body; they move with their parent.
	float min_length = 0.04f;
	std::vector<RagdollBoneSettings> bones; // Each bone at most once.
};

struct RagdollPlan {
	struct Body {
		int joint = -1;  // Of the skeleton.
		int parent = -1; // An earlier body, or -1.
		float length = 0.0f;
		float radius = 0.0f;
		CapsuleShape capsule; // In the joint's own space.
		std::vector<CapsuleShape> more; // The bones that ride on this one, in its space.
		float density = 0.0f;
		float friction = 0.6f;
		float mass = 0.0f;
		// The joint to the parent body, at rest when the skeleton is.
		Pose frame_parent;
		Pose frame_child;
		float cone = 0.0f;
		float twist = 0.0f; // Either way.
		float friction_torque = 0.0f;
	};
	std::vector<Body> bodies; // In the skeleton's order.
	// For each joint of the skeleton: the body it moves with, its own or its nearest ancestor's; -1 for none.
	std::vector<int> body_of_joint;
	float mass = 0.0f;
	std::string problem; // Why there are no bodies; empty when there are.

	[[nodiscard]] bool ok() const noexcept { return problem.empty() && !bodies.empty(); }
};

[[nodiscard]] RagdollPlan plan_ragdoll(std::span<const RagdollJoint> skeleton, const RagdollSettings& settings);

// The plan's parts at a pose: every joint's pose in the world now, and `seconds` before, for the velocities. A joint the
// pose has turned past its limits gets them widened to fit, so starting never fights the joints. Empty when the
// spans do not match the skeleton or `seconds` is not positive.
[[nodiscard]] RagdollDesc place_ragdoll(const RagdollPlan& plan, std::span<const Pose> now, std::span<const Pose> before,
	float seconds);

// The swing and twist of frame b relative to frame a, both in the world, as Box3D's ball joint measures them.
struct SwingTwist {
	float swing = 0.0f;
	float twist = 0.0f;
};
[[nodiscard]] SwingTwist swing_twist(Quat frame_a, Quat frame_b) noexcept;

} // namespace engine::physics
