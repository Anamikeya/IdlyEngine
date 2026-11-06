#include "engine/physics/ragdoll.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numbers>

namespace engine::physics {
namespace {

constexpr double pi = std::numbers::pi;
constexpr double gravity = 9.81;
// Box3D's bounds for limits.
constexpr double max_twist = 0.99 * pi;
// Room left past a pose that is outside its joint's limits when the ragdoll starts.
constexpr double limit_margin = 0.05;
// The most two joined parts may differ in mass.
constexpr float max_mass_ratio = 4.0f;

struct V {
	double x = 0.0, y = 0.0, z = 0.0;
};
V operator+(V a, V b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator-(V a, V b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V operator*(V a, double s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
double dot(V a, V b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
V cross(V a, V b) noexcept { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double length(V a) noexcept { return std::sqrt(dot(a, a)); }

struct Q {
	double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
};
Q operator*(Q a, Q b) noexcept
{
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Q conjugate(Q q) noexcept { return {-q.x, -q.y, -q.z, q.w}; }
Q normalize(Q q) noexcept
{
	const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	return n > 1.0e-12 ? Q{q.x / n, q.y / n, q.z / n, q.w / n} : Q{};
}
V rotate(Q q, V v) noexcept
{
	const V u{q.x, q.y, q.z};
	const V t = cross(u, v) * 2.0;
	return v + t * q.w + cross(u, t);
}

V to_v(Vec3 v) noexcept { return {v.x, v.y, v.z}; }
Vec3 to_f(V v) noexcept { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }
Q to_q(Quat q) noexcept { return normalize({q.x, q.y, q.z, q.w}); }
Quat to_f(Q q) noexcept { return {static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w)}; }

// The shortest rotation taking unit vector `from` onto unit vector `to`.
Q from_to(V from, V to) noexcept
{
	const double d = dot(from, to);
	if (d > 1.0 - 1.0e-12) {
		return {};
	}
	if (d < -1.0 + 1.0e-12) {
		V axis = cross(from, V{1, 0, 0});
		if (length(axis) < 1.0e-6) {
			axis = cross(from, V{0, 1, 0});
		}
		axis = axis * (1.0 / length(axis));
		return {axis.x, axis.y, axis.z, 0.0};
	}
	const V axis = cross(from, to);
	return normalize({axis.x, axis.y, axis.z, 1.0 + d});
}

// Any unit vector square to a unit vector, and the third of the set.
std::pair<V, V> square_to(V n) noexcept
{
	const V helper = std::abs(n.y) < 0.9 ? V{0, 1, 0} : V{1, 0, 0};
	V u = cross(helper, n);
	u = u * (1.0 / length(u));
	return {u, cross(n, u)};
}

bool finite(Vec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finite(const Pose& p) noexcept
{
	const Quat& q = p.rotation;
	return finite(p.position) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w) &&
		q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w > 1.0e-6f;
}

std::array<V, 8> corners(Vec3 lo, Vec3 hi) noexcept
{
	std::array<V, 8> out;
	for (int i = 0; i < 8; ++i) {
		out[static_cast<std::size_t>(i)] = {(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
	}
	return out;
}

// What the builder works out for each joint before choosing.
struct Bone {
	double length = 0.0;
	V direction{0, 1, 0}; // In the joint's own space.
	double radius = 0.0;  // From the flesh; 0 when unknown.
	bool known = false;   // Length and direction came from a child or the flesh.
};

} // namespace

SwingTwist swing_twist(Quat frame_a, Quat frame_b) noexcept
{
	const Q a = to_q(frame_a);
	Q b = to_q(frame_b);
	if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0) {
		b = {-b.x, -b.y, -b.z, -b.w};
	}
	const Q r = conjugate(a) * b;
	const double twist = 2.0 * (r.w < 0.0 ? std::atan2(-r.z, -r.w) : std::atan2(r.z, r.w));
	const double swing = 2.0 * std::atan2(std::sqrt(r.x * r.x + r.y * r.y), std::sqrt(r.z * r.z + r.w * r.w));
	return {static_cast<float>(swing), static_cast<float>(twist)};
}

RagdollPlan plan_ragdoll(std::span<const RagdollJoint> skeleton, const RagdollSettings& s)
{
	RagdollPlan plan;
	const std::size_t n = skeleton.size();
	plan.body_of_joint.assign(n, -1);
	if (n == 0) {
		plan.problem = "the model has no skeleton";
		return plan;
	}
	const auto in = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; };
	if (!in(s.density, 1.0e-3, 1.0e5) || !in(s.cone, 0.0, pi) || !in(s.twist, 0.0, max_twist) || !in(s.joint_friction, 0.0, 10.0) ||
		!in(s.friction, 0.0, 10.0) || !in(s.min_length, 0.0, 1.0)) {
		plan.problem = "the ragdoll settings are out of range";
		return plan;
	}
	std::map<std::string, const RagdollBoneSettings*> chosen_settings;
	for (const RagdollBoneSettings& b : s.bones) {
		if (!chosen_settings.emplace(b.bone, &b).second) {
			plan.problem = "bone '" + b.bone + "' has settings twice";
			return plan;
		}
		if (b.body < -1 || b.body > 1 || !in(b.radius, 0.0, 1.0e4) || !in(b.density, 0.0, 1.0e5) || !std::isfinite(b.cone) ||
			b.cone > pi || !std::isfinite(b.twist) || b.twist > max_twist) {
			plan.problem = "the settings of bone '" + b.bone + "' are out of range";
			return plan;
		}
	}
	for (std::size_t j = 0; j < n; ++j) {
		const RagdollJoint& joint = skeleton[j];
		if (joint.parent < -1 || joint.parent >= static_cast<int>(j) || !finite(joint.rest) || !finite(joint.flesh_min) ||
			!finite(joint.flesh_max)) {
			plan.problem = "joint " + std::to_string(j) + " of the skeleton is not usable";
			return plan;
		}
	}

	// How big the skeleton is, for what counts as a short bone.
	V lo = to_v(skeleton[0].rest.position), hi = lo;
	for (const RagdollJoint& joint : skeleton) {
		const V p = to_v(joint.rest.position);
		lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
		hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
	}
	const double size = length(hi - lo);

	// How much of the skeleton hangs from each joint.
	std::vector<std::size_t> descendants(n, 1);
	for (std::size_t j = n; j-- > 0;) {
		if (skeleton[j].parent >= 0) {
			descendants[static_cast<std::size_t>(skeleton[j].parent)] += descendants[j];
		}
	}
	// Children that come in pairs (arms from the chest, legs from the hips) branch off; the bone carries on to the
	// unpaired child that carries the most (the neck, the spine). With only pairs, it points between them.
	std::vector<std::vector<std::size_t>> children(n);
	for (std::size_t j = 0; j < n; ++j) {
		if (skeleton[j].parent >= 0) {
			children[static_cast<std::size_t>(skeleton[j].parent)].push_back(j);
		}
	}
	const auto offset_of = [&](std::size_t child) {
		return to_v(skeleton[child].rest.position) - to_v(skeleton[static_cast<std::size_t>(skeleton[child].parent)].rest.position);
	};
	std::vector<int> main_child(n, -1);
	std::vector<V> between(n);
	std::vector<char> has_between(n, 0);
	const double tiny = 1.0e-6 * std::max(size, 1.0e-6);
	for (std::size_t j = 0; j < n; ++j) {
		std::vector<std::size_t> usable;
		// A child at the joint itself (a helper) gives no direction.
		for (const std::size_t c : children[j]) {
			if (length(offset_of(c)) > tiny) {
				usable.push_back(c);
			}
		}
		const auto paired = [&](std::size_t c) {
			return std::any_of(usable.begin(), usable.end(), [&](std::size_t other) {
				const double a = length(offset_of(c)), b = length(offset_of(other));
				return other != c && descendants[other] == descendants[c] && std::abs(a - b) <= 0.05 * std::max(a, b);
			});
		};
		for (const std::size_t c : usable) {
			if (!paired(c) && (main_child[j] < 0 || descendants[c] > descendants[static_cast<std::size_t>(main_child[j])])) {
				main_child[j] = static_cast<int>(c);
			}
		}
		if (main_child[j] < 0 && !usable.empty()) {
			V sum;
			for (const std::size_t c : usable) {
				sum = sum + offset_of(c);
			}
			between[j] = sum * (1.0 / static_cast<double>(usable.size()));
			has_between[j] = length(between[j]) > tiny;
		}
	}

	std::vector<Bone> bones(n);
	for (std::size_t j = 0; j < n; ++j) {
		const RagdollJoint& joint = skeleton[j];
		Bone& bone = bones[j];
		const Q rotation = to_q(joint.rest.rotation);
		if (main_child[j] >= 0 || has_between[j]) {
			const V model = main_child[j] >= 0 ? offset_of(static_cast<std::size_t>(main_child[j])) : between[j];
			const V offset = rotate(conjugate(rotation), model);
			bone.length = length(offset);
			bone.direction = offset * (1.0 / bone.length);
			bone.known = true;
		} else if (joint.skinned) {
			// The end of a limb: as far as its flesh reaches, towards it.
			const V center = (to_v(joint.flesh_min) + to_v(joint.flesh_max)) * 0.5;
			if (length(center) > 1.0e-9) {
				bone.direction = center * (1.0 / length(center));
				for (const V& corner : corners(joint.flesh_min, joint.flesh_max)) {
					bone.length = std::max(bone.length, dot(corner, bone.direction));
				}
				bone.known = true;
			}
		}
		if (!bone.known && joint.parent >= 0) {
			// Carries on the way its parent points.
			const std::size_t parent = static_cast<std::size_t>(joint.parent);
			const Q parent_rotation = to_q(skeleton[parent].rest.rotation);
			bone.direction = rotate(conjugate(rotation), rotate(parent_rotation, bones[parent].direction));
			bone.length = 0.5 * bones[parent].length;
		}
		if (joint.skinned) {
			// Half the flesh's width square to the bone, as the box of every vertex that follows it shows.
			const auto [u, v] = square_to(bone.direction);
			double u_lo = 1e300, u_hi = -1e300, v_lo = 1e300, v_hi = -1e300;
			for (const V& corner : corners(joint.flesh_min, joint.flesh_max)) {
				u_lo = std::min(u_lo, dot(corner, u));
				u_hi = std::max(u_hi, dot(corner, u));
				v_lo = std::min(v_lo, dot(corner, v));
				v_hi = std::max(v_hi, dot(corner, v));
			}
			// A box of every vertex any weight reaches is wider than the limb; this keeps parts from swelling.
			bone.radius = 0.5 * (0.5 * (u_hi - u_lo) + 0.5 * (v_hi - v_lo)) * 0.8;
		}
	}

	// Which bones get bodies.
	std::vector<char> chosen(n, 0), wanted(n, 0);
	std::vector<int> asked(n, -1);
	for (std::size_t j = 0; j < n; ++j) {
		const auto found = chosen_settings.find(skeleton[j].name);
		asked[j] = found == chosen_settings.end() ? -1 : found->second->body;
		wanted[j] = skeleton[j].skinned && bones[j].length > 0.0 && bones[j].length >= s.min_length * size;
		chosen[j] = asked[j] == 1 || (asked[j] == -1 && wanted[j]);
	}
	// The end of a limb shorter than the bone above it rides on that bone's body (D7, after human.c). What ends a limb is
	// the builder's own choice: taking a hand away does not make its forearm an end.
	std::vector<int> rides_on(n, -1);
	std::vector<char> below(n, 0);
	for (std::size_t j = n; j-- > 0;) {
		if (skeleton[j].parent >= 0 && (wanted[j] || chosen[j] || below[j])) {
			below[static_cast<std::size_t>(skeleton[j].parent)] = 1;
		}
	}
	for (std::size_t j = 0; j < n; ++j) {
		if (!chosen[j] || below[j] || asked[j] == 1) {
			continue;
		}
		int above = skeleton[j].parent;
		while (above >= 0 && !chosen[static_cast<std::size_t>(above)]) {
			above = skeleton[static_cast<std::size_t>(above)].parent;
		}
		if (above >= 0 && bones[static_cast<std::size_t>(above)].length > bones[j].length) {
			rides_on[j] = above;
			chosen[j] = 0;
		}
	}
	std::vector<std::size_t> order;
	for (std::size_t j = 0; j < n; ++j) {
		if (chosen[j]) {
			order.push_back(j);
		}
	}
	if (order.size() > max_ragdoll_parts) {
		// The longest bones matter most; the shortest go.
		std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return bones[a].length > bones[b].length; });
		for (std::size_t k = max_ragdoll_parts; k < order.size(); ++k) {
			chosen[order[k]] = 0;
		}
	}

	// A capsule along a bone, in its joint's space: from its own settings, or sized from its length and flesh.
	struct Sized {
		double length = 0.0, radius = 0.0, straight = 0.0;
		CapsuleShape capsule;
	};
	const auto settings_of = [&](std::size_t j) -> const RagdollBoneSettings* {
		const auto found = chosen_settings.find(skeleton[j].name);
		return found == chosen_settings.end() ? nullptr : found->second;
	};
	const auto size_capsule = [&](std::size_t j) {
		const RagdollBoneSettings* own = settings_of(j);
		Sized out;
		out.length = bones[j].length;
		double radius = own && own->radius > 0.0f ? own->radius : bones[j].radius;
		if (!(radius > 0.0)) {
			radius = 0.2 * out.length;
		}
		if (!(own && own->radius > 0.0f) && out.length > 0.0) {
			// Thin limbs stay in proportion to their length; a short, wide bone (hips, chest) may be fatter than it is long.
			radius = std::clamp(radius, 0.08 * out.length, std::max(0.35 * out.length, 0.08 * size));
		}
		if (out.length <= 0.0) {
			// Asked for without any length to go on: a ball.
			if (!(radius > 0.0)) {
				radius = std::max(0.02 * size, 0.01);
			}
			out.length = 2.0 * radius;
		}
		out.radius = radius;
		out.straight = std::max(out.length - 2.0 * radius, 0.0);
		const V a = bones[j].direction * (out.straight > 0.0 ? radius : 0.5 * out.length);
		const V b = bones[j].direction * (out.straight > 0.0 ? out.length - radius : 0.5 * out.length);
		out.capsule = CapsuleShape{to_f(a), to_f(b), static_cast<float>(radius)};
		return out;
	};
	const auto volume = [](const Sized& c) { return pi * c.radius * c.radius * c.straight + 4.0 / 3.0 * pi * c.radius * c.radius * c.radius; };

	for (std::size_t j = 0; j < n; ++j) {
		const int parent_body = skeleton[j].parent >= 0 ? plan.body_of_joint[static_cast<std::size_t>(skeleton[j].parent)] : -1;
		if (!chosen[j]) {
			plan.body_of_joint[j] = parent_body;
			continue;
		}
		const RagdollBoneSettings* own = settings_of(j);
		const Sized sized = size_capsule(j);
		RagdollPlan::Body body;
		body.joint = static_cast<int>(j);
		body.parent = parent_body;
		body.length = static_cast<float>(sized.length);
		body.radius = static_cast<float>(sized.radius);
		body.capsule = sized.capsule;
		body.density = own && own->density > 0.0f ? own->density : s.density;
		body.friction = s.friction;
		double mass = body.density * volume(sized);
		// The bones riding on this one, moved into its space as they rest.
		const Q rotation = to_q(skeleton[j].rest.rotation);
		for (std::size_t m = j + 1; m < n; ++m) {
			if (rides_on[m] != static_cast<int>(j)) {
				continue;
			}
			const Sized end = size_capsule(m);
			const Q end_rotation = to_q(skeleton[m].rest.rotation);
			const auto into = [&](Vec3 p) {
				return to_f(rotate(conjugate(rotation), to_v(skeleton[m].rest.position) + rotate(end_rotation, to_v(p)) - to_v(skeleton[j].rest.position)));
			};
			body.more.push_back(CapsuleShape{into(end.capsule.point_a), into(end.capsule.point_b), end.capsule.radius});
			mass += body.density * volume(end); // A body has one density.

		}
		body.mass = static_cast<float>(mass);
		body.cone = own && own->cone >= 0.0f ? own->cone : s.cone;
		body.twist = own && own->twist >= 0.0f ? own->twist : s.twist;
		if (parent_body >= 0) {
			const RagdollJoint& parent = skeleton[static_cast<std::size_t>(plan.bodies[static_cast<std::size_t>(parent_body)].joint)];
			const Q parent_rotation = to_q(parent.rest.rotation);
			// Both frames' z axes run along this bone, so the cone is about its rest direction and the twist about itself.
			const Q along = from_to(V{0, 0, 1}, bones[j].direction);
			body.frame_child = Pose{{}, to_f(along)};
			body.frame_parent = Pose{to_f(rotate(conjugate(parent_rotation), to_v(skeleton[j].rest.position) - to_v(parent.rest.position))),
				to_f(normalize(conjugate(parent_rotation) * rotation * along))};
		}
		plan.body_of_joint[j] = static_cast<int>(plan.bodies.size());
		plan.bodies.push_back(body);
	}
	if (plan.bodies.empty()) {
		plan.problem = "no bone is long enough and followed by the skin to get a body";
		return plan;
	}
	// Box3D's solver holds a chain well while joined parts weigh within a few times of each other; a light neck between
	// a heavy chest and head stretches. The lighter of two joined parts is made denser to keep within max_mass_ratio,
	// unless its density was set for its bone.
	const auto density_set = [&](const RagdollPlan::Body& body) {
		const auto found = chosen_settings.find(skeleton[static_cast<std::size_t>(body.joint)].name);
		return found != chosen_settings.end() && found->second->density > 0.0f;
	};
	for (int pass = 0; pass < 4; ++pass) {
		for (RagdollPlan::Body& body : plan.bodies) {
			if (body.parent < 0) {
				continue;
			}
			RagdollPlan::Body& parent = plan.bodies[static_cast<std::size_t>(body.parent)];
			RagdollPlan::Body& light = body.mass < parent.mass ? body : parent;
			const float heavy = std::max(body.mass, parent.mass);
			if (light.mass > 0.0f && heavy > max_mass_ratio * light.mass && !density_set(light)) {
				const float scale = heavy / (max_mass_ratio * light.mass);
				light.density *= scale;
				light.mass *= scale;
			}
		}
	}
	plan.mass = 0.0f;
	for (RagdollPlan::Body& body : plan.bodies) {
		plan.mass += body.mass;
		body.friction_torque = static_cast<float>(s.joint_friction * body.mass * gravity * 0.5 * body.length);
	}
	return plan;
}

RagdollDesc place_ragdoll(const RagdollPlan& plan, std::span<const Pose> now, std::span<const Pose> before, float seconds)
{
	RagdollDesc desc;
	if (!plan.ok() || now.size() != plan.body_of_joint.size() || before.size() != now.size() || !(seconds > 0.0f) ||
		!std::isfinite(seconds)) {
		return desc;
	}
	for (std::size_t j = 0; j < now.size(); ++j) {
		if (!finite(now[j]) || !finite(before[j])) {
			return desc;
		}
	}
	const double dt = seconds;
	for (const RagdollPlan::Body& body : plan.bodies) {
		const std::size_t j = static_cast<std::size_t>(body.joint);
		const Q rotation = to_q(now[j].rotation);
		RagdollPart part;
		part.bone = body.joint;
		part.parent = body.parent;
		part.pose = Pose{now[j].position, to_f(rotation)};
		// How the pose was moving: the change over the last step.
		part.linear_velocity = to_f((to_v(now[j].position) - to_v(before[j].position)) * (1.0 / dt));
		Q turn = rotation * conjugate(to_q(before[j].rotation));
		if (turn.w < 0.0) {
			turn = {-turn.x, -turn.y, -turn.z, -turn.w};
		}
		const V axis{turn.x, turn.y, turn.z};
		const double sine = length(axis);
		if (sine > 1.0e-12) {
			part.angular_velocity = to_f(axis * (2.0 * std::atan2(sine, turn.w) / sine / dt));
		}
		part.capsule = body.capsule;
		part.more = body.more;
		part.density = body.density;
		part.friction = body.friction;
		part.friction_torque = body.friction_torque;
		part.cone = body.cone;
		part.twist_lower = -body.twist;
		part.twist_upper = body.twist;
		if (body.parent >= 0) {
			const std::size_t p = static_cast<std::size_t>(plan.bodies[static_cast<std::size_t>(body.parent)].joint);
			const Q parent_rotation = to_q(now[p].rotation);
			part.frame_child = body.frame_child;
			// Anchored where the pose has the joint, so the parts start together.
			part.frame_parent = Pose{to_f(rotate(conjugate(parent_rotation), to_v(now[j].position) - to_v(now[p].position))), body.frame_parent.rotation};
			const SwingTwist st = swing_twist(to_f(parent_rotation * to_q(part.frame_parent.rotation)), to_f(rotation * to_q(part.frame_child.rotation)));
			if (st.swing > part.cone) {
				part.cone = static_cast<float>(std::min(pi, st.swing + limit_margin));
			}
			if (st.twist < part.twist_lower) {
				part.twist_lower = static_cast<float>(std::max(-max_twist, st.twist - limit_margin));
			}
			if (st.twist > part.twist_upper) {
				part.twist_upper = static_cast<float>(std::min(max_twist, st.twist + limit_margin));
			}
		}
		desc.parts.push_back(part);
	}
	return desc;
}

} // namespace engine::physics
