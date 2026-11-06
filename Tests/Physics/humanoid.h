#pragma once

// A person about 1.8 m tall for ragdoll tests, as the builder reads a skeleton: joints at rest in a T pose with the
// flesh around each bone. Like most exported rigs, every joint's own x axis runs along its bone, so the builder has
// to work in each joint's space rather than the model's.

#include "engine/physics/ragdoll.h"

#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace humanoid {

using engine::physics::Pose;
using engine::physics::Quat;
using engine::physics::RagdollJoint;
using engine::physics::Vec3;

struct D3 {
	double x = 0, y = 0, z = 0;
};
inline D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

struct DQ {
	double x = 0, y = 0, z = 0, w = 1;
};
inline DQ operator*(DQ a, DQ b)
{
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline DQ conjugate(DQ q) { return {-q.x, -q.y, -q.z, q.w}; }
inline D3 rotate(DQ q, D3 v)
{
	const D3 u{q.x, q.y, q.z};
	const D3 t = cross(u, v) * 2.0;
	return v + t * q.w + cross(u, t);
}
inline DQ axis_angle(D3 axis, double radians)
{
	const double n = std::sqrt(dot(axis, axis));
	const double s = std::sin(radians / 2) / n;
	return {axis.x * s, axis.y * s, axis.z * s, std::cos(radians / 2)};
}
inline DQ from_to(D3 a, D3 b)
{
	a = a * (1 / std::sqrt(dot(a, a)));
	b = b * (1 / std::sqrt(dot(b, b)));
	const double d = dot(a, b);
	if (d > 1 - 1e-12) return {};
	if (d < -1 + 1e-12) return {0, 0, 1, 0};
	const D3 c = cross(a, b);
	const double n = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z + (1 + d) * (1 + d));
	return {c.x / n, c.y / n, c.z / n, (1 + d) / n};
}
inline Quat to_quat(DQ q) { return {float(q.x), float(q.y), float(q.z), float(q.w)}; }
inline DQ to_dq(Quat q) { return {q.x, q.y, q.z, q.w}; }
inline Vec3 to_vec(D3 v) { return {float(v.x), float(v.y), float(v.z)}; }
inline D3 to_d3(Vec3 v) { return {v.x, v.y, v.z}; }

struct Spec {
	const char* name;
	int parent;
	D3 at;          // Model space, at rest.
	D3 along;       // The way its bone runs; for a limb's end, the way its flesh reaches.
	double length;  // Of the flesh along the bone.
	double radius;  // Of the flesh.
	bool skinned = true;
};

inline const std::vector<Spec>& specs()
{
	static const std::vector<Spec> list{
		{"root", -1, {0, 0, 0}, {0, 1, 0}, 1.0, 0.0, false},
		{"pelvis", 0, {0, 1.0, 0}, {0, 1, 0}, 0.1, 0.14},
		{"spine", 1, {0, 1.1, 0}, {0, 1, 0}, 0.25, 0.13},
		{"chest", 2, {0, 1.35, 0}, {0, 1, 0}, 0.2, 0.15},
		{"neck", 3, {0, 1.55, 0}, {0, 1, 0}, 0.1, 0.06},
		{"head", 4, {0, 1.65, 0}, {0, 1, 0}, 0.22, 0.11},
		{"upperarm_l", 3, {0.2, 1.5, 0}, {1, 0, 0}, 0.28, 0.055},
		{"forearm_l", 6, {0.48, 1.5, 0}, {1, 0, 0}, 0.25, 0.045},
		{"hand_l", 7, {0.73, 1.5, 0}, {1, 0, 0}, 0.17, 0.04},
		{"upperarm_r", 3, {-0.2, 1.5, 0}, {-1, 0, 0}, 0.28, 0.055},
		{"forearm_r", 9, {-0.48, 1.5, 0}, {-1, 0, 0}, 0.25, 0.045},
		{"hand_r", 10, {-0.73, 1.5, 0}, {-1, 0, 0}, 0.17, 0.04},
		{"thigh_l", 1, {0.1, 0.95, 0}, {0, -1, 0}, 0.45, 0.08},
		{"calf_l", 12, {0.1, 0.5, 0}, {0, -1, 0}, 0.42, 0.06},
		{"foot_l", 13, {0.1, 0.08, 0}, {0, 0, 1}, 0.2, 0.05},
		{"thigh_r", 1, {-0.1, 0.95, 0}, {0, -1, 0}, 0.45, 0.08},
		{"calf_r", 15, {-0.1, 0.5, 0}, {0, -1, 0}, 0.42, 0.06},
		{"foot_r", 16, {-0.1, 0.08, 0}, {0, 0, 1}, 0.2, 0.05},
	};
	return list;
}

// Each joint's rest rotation turns its own x axis onto its bone.
inline std::vector<RagdollJoint> skeleton()
{
	std::vector<RagdollJoint> joints;
	for (const Spec& s : specs()) {
		RagdollJoint j;
		j.name = s.name;
		j.parent = s.parent;
		j.rest = {to_vec(s.at), to_quat(from_to({1, 0, 0}, s.along))};
		j.skinned = s.skinned;
		j.flesh_min = to_vec({-0.3 * s.radius, -s.radius, -s.radius});
		j.flesh_max = to_vec({s.length + 0.3 * s.radius, s.radius, s.radius});
		joints.push_back(j);
	}
	return joints;
}

inline int index_of(const std::string& name)
{
	const auto& list = specs();
	for (std::size_t i = 0; i < list.size(); ++i) {
		if (name == list[i].name) return int(i);
	}
	return -1;
}

// The skeleton posed: some joints turned about themselves (axes as the skeleton stands at rest), carrying what is below
// them, then the whole turned by `facing` and moved by `offset`. Parents first, as the joints are.
inline std::vector<Pose> posed(const std::map<std::string, DQ>& turns, D3 offset = {}, DQ facing = {})
{
	const auto rest = skeleton();
	std::vector<D3> position(rest.size());
	std::vector<DQ> rotation(rest.size());
	for (std::size_t j = 0; j < rest.size(); ++j) {
		const DQ own = to_dq(rest[j].rest.rotation);
		const D3 at = to_d3(rest[j].rest.position);
		const auto turn = turns.find(rest[j].name);
		const DQ extra = turn == turns.end() ? DQ{} : turn->second;
		if (rest[j].parent < 0) {
			position[j] = at;
			rotation[j] = extra * own;
			continue;
		}
		const std::size_t p = std::size_t(rest[j].parent);
		const DQ parent_rest = to_dq(rest[p].rest.rotation);
		const D3 parent_at = to_d3(rest[p].rest.position);
		// The rest transform relative to the parent, turned by the extra rotation about the joint.
		const D3 local_at = rotate(conjugate(parent_rest), at - parent_at);
		const DQ local_rotation = conjugate(parent_rest) * own;
		const DQ extra_local = conjugate(parent_rest) * extra * parent_rest;
		position[j] = position[p] + rotate(rotation[p], local_at);
		rotation[j] = rotation[p] * extra_local * local_rotation;
	}
	std::vector<Pose> out;
	for (std::size_t j = 0; j < rest.size(); ++j)
		out.push_back({to_vec(rotate(facing, position[j]) + offset), to_quat(facing * rotation[j])});
	return out;
}

} // namespace humanoid
