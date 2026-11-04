#include "engine/physics/scene_physics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <utility>

namespace engine::physics {
namespace {

using SceneVec3 = ::engine::Vec3;

// Relative tolerance for treating a world scale as uniform and for detecting a scale change.
constexpr double scale_tolerance = 1.0e-6;
// Maximum |dot| between normalized basis columns before a matrix counts as sheared.
constexpr double shear_tolerance = 1.0e-6;

struct DVec3 {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

DVec3 operator+(DVec3 a, DVec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
DVec3 operator-(DVec3 a, DVec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
DVec3 operator*(DVec3 a, double s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
double dot(DVec3 a, DVec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
DVec3 cross(DVec3 a, DVec3 b) noexcept { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double length(DVec3 a) noexcept { return std::sqrt(dot(a, a)); }

struct DQuat {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	double w = 1.0;
};

DQuat normalize(DQuat q) noexcept
{
	const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	return {q.x / n, q.y / n, q.z / n, q.w / n};
}

DQuat conjugate(DQuat q) noexcept { return {-q.x, -q.y, -q.z, q.w}; }

DQuat operator*(DQuat a, DQuat b) noexcept
{
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

DVec3 rotate(DQuat q, DVec3 v) noexcept
{
	const DVec3 u{q.x, q.y, q.z};
	const DVec3 t = cross(u, v) * 2.0;
	return v + t * q.w + cross(u, t);
}

Vec3 rotate(Quat q, Vec3 v) noexcept
{
	const DVec3 r = rotate(DQuat{q.x, q.y, q.z, q.w}, DVec3{v.x, v.y, v.z});
	return {static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.z)};
}

Vec3 add(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 to_float(const SceneVec3& v) noexcept { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }
Vec3 scale(Vec3 v, float s) noexcept { return {v.x * s, v.y * s, v.z * s}; }

Vec3 transform_point(const Pose& pose, Vec3 local) noexcept
{
	return add(pose.position, rotate(pose.rotation, local));
}

// A world matrix split into translation, rotation and positive per-axis scale.
struct Decomposed {
	DVec3 translation;
	DQuat rotation;
	DVec3 scale;

	[[nodiscard]] bool uniform() const noexcept
	{
		const double hi = std::max({scale.x, scale.y, scale.z});
		const double lo = std::min({scale.x, scale.y, scale.z});
		return hi - lo <= scale_tolerance * hi;
	}
};

// Rejects shear, reflection and degenerate scale: physics poses are rigid.
std::optional<Decomposed> decompose(const WorldTransform& world) noexcept
{
	const auto& m = world.matrix; // Column-major.
	DVec3 c0{m[0], m[1], m[2]};
	DVec3 c1{m[4], m[5], m[6]};
	DVec3 c2{m[8], m[9], m[10]};
	Decomposed out;
	out.translation = {m[12], m[13], m[14]};
	out.scale = {length(c0), length(c1), length(c2)};
	if (!(out.scale.x > 1.0e-12) || !(out.scale.y > 1.0e-12) || !(out.scale.z > 1.0e-12)) {
		return std::nullopt;
	}
	c0 = c0 * (1.0 / out.scale.x);
	c1 = c1 * (1.0 / out.scale.y);
	c2 = c2 * (1.0 / out.scale.z);
	if (std::abs(dot(c0, c1)) > shear_tolerance || std::abs(dot(c1, c2)) > shear_tolerance ||
		std::abs(dot(c0, c2)) > shear_tolerance || dot(c0, cross(c1, c2)) <= 0.0) {
		return std::nullopt;
	}

	// r[row][col], columns are the rotated basis vectors.
	const double r00 = c0.x, r10 = c0.y, r20 = c0.z;
	const double r01 = c1.x, r11 = c1.y, r21 = c1.z;
	const double r02 = c2.x, r12 = c2.y, r22 = c2.z;
	const double trace = r00 + r11 + r22;
	DQuat q;
	if (trace > 0.0) {
		const double s = std::sqrt(trace + 1.0) * 2.0;
		q = {(r21 - r12) / s, (r02 - r20) / s, (r10 - r01) / s, 0.25 * s};
	} else if (r00 > r11 && r00 > r22) {
		const double s = std::sqrt(1.0 + r00 - r11 - r22) * 2.0;
		q = {0.25 * s, (r01 + r10) / s, (r02 + r20) / s, (r21 - r12) / s};
	} else if (r11 > r22) {
		const double s = std::sqrt(1.0 + r11 - r00 - r22) * 2.0;
		q = {(r01 + r10) / s, 0.25 * s, (r12 + r21) / s, (r02 - r20) / s};
	} else {
		const double s = std::sqrt(1.0 + r22 - r00 - r11) * 2.0;
		q = {(r02 + r20) / s, (r12 + r21) / s, 0.25 * s, (r10 - r01) / s};
	}
	out.rotation = normalize(q);
	return out;
}

Pose to_pose(const Decomposed& d) noexcept
{
	return Pose{{static_cast<float>(d.translation.x), static_cast<float>(d.translation.y), static_cast<float>(d.translation.z)},
		{static_cast<float>(d.rotation.x), static_cast<float>(d.rotation.y), static_cast<float>(d.rotation.z),
			static_cast<float>(d.rotation.w)}};
}

bool same_scale(DVec3 a, DVec3 b) noexcept
{
	const auto close = [](double x, double y) { return std::abs(x - y) <= scale_tolerance * std::max(std::abs(x), std::abs(y)); };
	return close(a.x, b.x) && close(a.y, b.y) && close(a.z, b.z);
}

bool is_identity(Quat q) noexcept
{
	return std::abs(q.x) < 1.0e-6f && std::abs(q.y) < 1.0e-6f && std::abs(q.z) < 1.0e-6f;
}

Vec3 mul(Vec3 v, DVec3 s) noexcept
{
	return {static_cast<float>(v.x * s.x), static_cast<float>(v.y * s.y), static_cast<float>(v.z * s.z)};
}

// Bakes world scale into a collider. Returns an error message when the shape cannot represent it exactly.
std::optional<std::string> scale_collider(const Collider& in, const Decomposed& d, Collider& out)
{
	out = in;
	const bool uniform = d.uniform();
	const auto s = static_cast<float>(d.scale.x);
	if (auto* sphere = std::get_if<SphereShape>(&out.shape)) {
		if (!uniform) {
			return "sphere colliders require uniform world scale";
		}
		sphere->radius *= s;
		sphere->center = scale(sphere->center, s);
	} else if (auto* box = std::get_if<BoxShape>(&out.shape)) {
		// is_valid() rejected degenerate quaternions, so this normalization is safe.
		const DQuat q = normalize(DQuat{box->local_pose.rotation.x, box->local_pose.rotation.y, box->local_pose.rotation.z,
			box->local_pose.rotation.w});
		box->local_pose.rotation = {static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w)};
		if (!uniform && !is_identity(box->local_pose.rotation)) {
			return "rotated box colliders require uniform world scale";
		}
		box->half_extents = mul(box->half_extents, d.scale);
		box->local_pose.position = mul(box->local_pose.position, d.scale);
	} else if (auto* capsule = std::get_if<CapsuleShape>(&out.shape)) {
		if (!uniform) {
			return "capsule colliders require uniform world scale";
		}
		capsule->point_a = scale(capsule->point_a, s);
		capsule->point_b = scale(capsule->point_b, s);
		capsule->radius *= s;
	}
	return std::nullopt;
}

bool is_finite(Vec3 v) noexcept
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool valid_body(const RigidBody& b) noexcept
{
	return is_finite(b.linear_velocity) && is_finite(b.angular_velocity) && std::isfinite(b.linear_damping) &&
		b.linear_damping >= 0.0f && std::isfinite(b.angular_damping) && b.angular_damping >= 0.0f &&
		std::isfinite(b.gravity_scale);
}

} // namespace

std::array<float, 4> default_color(DebugCategory category) noexcept
{
	switch (category) {
	case DebugCategory::Static:
		return {0.45f, 0.70f, 0.45f, 1.0f};
	case DebugCategory::Kinematic:
		return {0.40f, 0.60f, 1.00f, 1.0f};
	case DebugCategory::DynamicAwake:
		return {1.00f, 0.60f, 0.20f, 1.0f};
	case DebugCategory::DynamicAsleep:
		return {0.55f, 0.45f, 0.35f, 1.0f};
	case DebugCategory::Sensor:
		return {1.00f, 1.00f, 0.20f, 0.5f};
	case DebugCategory::Ragdoll:
		return {0.85f, 0.45f, 1.00f, 1.0f};
	}
	return {1.0f, 0.0f, 1.0f, 1.0f};
}

struct ScenePhysics::Impl {
	struct Entry {
		// Scene components the body was converted from; a difference at sync marks the entry dirty.
		std::optional<RigidBodyComponent> body_source;
		std::vector<ColliderComponent> collider_source;
		RigidBody body;
		std::vector<Collider> colliders;
		// Authoring changed since the body was built.
		bool dirty = true;
		// Set when converted values are not accepted by the physics layer.
		std::string authoring_issue;
		// The body is withheld because of its place in the hierarchy; rebuilt once that is resolved.
		bool hierarchy_issue = false;
		// A kinematic target was set this tick; its velocity must be cleared once the scene stops moving it.
		bool kinematic_target = false;
		BodyHandle handle;
		// Colliders as built: world scale baked in. Used for debug geometry.
		std::vector<Collider> built;
		DVec3 built_scale{1.0, 1.0, 1.0};
		// Scene world matrix the body currently corresponds to. A different scene matrix is an external edit.
		std::optional<WorldTransform> synced_world;
		std::string issue;
	};

	// Bodies the run owns (D7, D11). Their user key is the owning entity (0 for debris), so contacts name it and the
	// moved-body pass, which matches handles, never mistakes them for the entity's own body.
	struct Part {
		BodyHandle body;
		JointHandle joint;
		int bone = -1;
		std::vector<CapsuleShape> shapes;
	};
	struct Ragdoll {
		EntityId owner = 0;
		std::vector<Part> parts;
		bool owner_disabled = false;
	};
	struct Piece {
		std::uint64_t id = 0;
		EntityId look = 0;
		BodyHandle body;
		Collider built;
		double lifetime = 0.0, age = 0.0, asleep = 0.0;
	};

	PhysicsWorld world;
	std::map<EntityId, Entry> entries;
	bool simulating = false;
	std::uint64_t ticks = 0;
	std::uint64_t simulated_ticks = 0;
	TickReport report;
	std::vector<CollisionEvent> events;
	std::vector<std::pair<EntityId, Pose>> pending;
	std::map<std::uint64_t, Ragdoll> ragdolls;
	std::map<EntityId, std::uint64_t> ragdoll_owners;
	std::vector<Piece> pieces; // Oldest first.
	// By body handle: (ragdoll, part) or debris id.
	std::map<std::uint64_t, std::pair<std::uint64_t, std::size_t>> part_bodies;
	std::map<std::uint64_t, std::uint64_t> piece_bodies;
	std::uint64_t next_runtime_id = 1;
	std::uint64_t debris_spawned = 0, debris_expired = 0, debris_replaced = 0;

	bool owner_disabled(EntityId id) const noexcept
	{
		const auto it = ragdoll_owners.find(id);
		return it != ragdoll_owners.end() && ragdolls.at(it->second).owner_disabled;
	}

	void destroy_ragdoll(std::map<std::uint64_t, Ragdoll>::iterator it) noexcept
	{
		for (auto part = it->second.parts.rbegin(); part != it->second.parts.rend(); ++part) {
			part_bodies.erase(part->body.value);
			if (world.is_valid(part->body)) {
				world.destroy_body(part->body); // Its joints go with it.
			}
		}
		if (it->second.owner_disabled) {
			if (const Entry* e = find(it->second.owner); e && world.is_valid(e->handle)) {
				world.set_body_enabled(e->handle, true);
			}
		}
		ragdoll_owners.erase(it->second.owner);
		ragdolls.erase(it);
	}

	void remove_piece(std::size_t index) noexcept
	{
		piece_bodies.erase(pieces[index].body.value);
		if (world.is_valid(pieces[index].body)) {
			world.destroy_body(pieces[index].body);
		}
		pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(index));
	}

	void clear_runtime_bodies() noexcept
	{
		while (!ragdolls.empty()) {
			destroy_ragdoll(ragdolls.begin());
		}
		while (!pieces.empty()) {
			remove_piece(pieces.size() - 1);
		}
	}

	// Ages debris by one simulated tick and removes what has slept long enough or outlived its lifetime.
	void age_debris(double dt)
	{
		for (std::size_t i = 0; i < pieces.size();) {
			Piece& piece = pieces[i];
			piece.age += dt;
			const auto awake = world.is_awake(piece.body);
			piece.asleep = awake && !awake.value() ? piece.asleep + dt : 0.0;
			if (piece.age >= piece.lifetime || piece.asleep >= debris_sleep_seconds) {
				remove_piece(i);
				++debris_expired;
			} else {
				++i;
			}
		}
	}

	Entry* find(EntityId id) noexcept
	{
		const auto it = entries.find(id);
		return it == entries.end() ? nullptr : &it->second;
	}

	const Entry* find(EntityId id) const noexcept
	{
		const auto it = entries.find(id);
		return it == entries.end() ? nullptr : &it->second;
	}

	void destroy_body(Entry& e) noexcept
	{
		if (world.is_valid(e.handle)) {
			world.destroy_body(e.handle);
		}
		e.handle = {};
		e.built.clear();
		e.synced_world.reset();
		e.kinematic_target = false;
	}

	void fail(Entry& e, std::string message) noexcept
	{
		destroy_body(e);
		e.issue = std::move(message);
	}

	// Dynamic bodies publish world poses as local transforms; that needs a rigid, uniformly scaled
	// parent chain with no other dynamic body above (C-1).
	std::optional<std::string> check_dynamic_hierarchy(const Scene& scene, EntityId parent) const
	{
		if (parent != 0) {
			const auto parent_world = scene.world_transform(parent);
			const auto d = parent_world ? decompose(*parent_world) : std::nullopt;
			if (!d || !d->uniform()) {
				return "dynamic bodies require a rigid, uniformly scaled parent";
			}
		}
		for (std::size_t depth = 0; parent != 0 && depth < Scene::max_entities; ++depth) {
			const Entry* ancestor = find(parent);
			if (ancestor && ancestor->body.type == BodyType::Dynamic) {
				return "dynamic bodies cannot be nested under another dynamic body";
			}
			const auto record = scene.inspect(parent);
			parent = record ? record->parent : 0;
		}
		return std::nullopt;
	}

	void build(EntityId id, Entry& e, const Decomposed& d, bool keep_velocity)
	{
		Vec3 linear = e.body.linear_velocity;
		Vec3 angular = e.body.angular_velocity;
		if (keep_velocity && world.is_valid(e.handle)) {
			linear = world.linear_velocity(e.handle).value();
			angular = world.angular_velocity(e.handle).value();
		}
		destroy_body(e);

		std::vector<Collider> scaled(e.colliders.size());
		for (std::size_t i = 0; i < e.colliders.size(); ++i) {
			if (auto error = scale_collider(e.colliders[i], d, scaled[i])) {
				fail(e, "collider " + std::to_string(i) + ": " + *error);
				return;
			}
		}

		BodyDesc desc;
		desc.type = e.body.type;
		desc.pose = to_pose(d);
		desc.linear_velocity = linear;
		desc.angular_velocity = angular;
		desc.linear_damping = e.body.linear_damping;
		desc.angular_damping = e.body.angular_damping;
		desc.gravity_scale = e.body.gravity_scale;
		desc.enable_sleep = e.body.enable_sleep;
		desc.is_bullet = e.body.is_bullet;
		desc.lock_rotation = e.body.lock_rotation;
		desc.user_key = id;
		auto body = world.create_body(desc);
		if (!body) {
			fail(e, std::string("body rejected: ") + to_string(body.error()));
			return;
		}
		e.handle = body.value();
		for (std::size_t i = 0; i < scaled.size(); ++i) {
			auto collider = world.add_collider(e.handle, scaled[i]);
			if (!collider) {
				fail(e, "collider " + std::to_string(i) + " rejected: " + to_string(collider.error()));
				return;
			}
		}
		e.built = std::move(scaled);
		e.built_scale = d.scale;
		e.issue.clear();
		// A rebuilt body stays out while its entity lies as a ragdoll.
		if (owner_disabled(id)) {
			world.set_body_enabled(e.handle, false);
		}
		++report.rebuilt;
	}

	void sync_entry(const Scene& scene, EntityId id, Entry& e)
	{
		const auto record = scene.inspect(id);
		if (!record) {
			destroy_body(e);
			return;
		}
		if (!e.authoring_issue.empty()) {
			fail(e, e.authoring_issue);
			return;
		}
		const auto scene_world = scene.world_transform(id);
		if (!scene_world) {
			fail(e, "entity has no world transform");
			return;
		}
		// Checked on every sync: a reparent that keeps the world transform, or a parent's body changing,
		// alters this verdict without changing the entity's own world matrix.
		if (e.body.type == BodyType::Dynamic) {
			if (auto error = check_dynamic_hierarchy(scene, record->parent)) {
				fail(e, *error);
				e.synced_world = *scene_world;
				e.hierarchy_issue = true;
				return;
			}
		}
		if (e.hierarchy_issue) {
			e.hierarchy_issue = false;
			e.dirty = true;
		}
		const bool has_body = world.is_valid(e.handle);
		if (!e.dirty && has_body && e.synced_world && e.synced_world->matrix == scene_world->matrix) {
			if (e.kinematic_target) {
				// The scene stopped moving it: stop instead of coasting past the last target.
				world.set_linear_velocity(e.handle, {});
				world.set_angular_velocity(e.handle, {});
				e.kinematic_target = false;
			}
			return;
		}
		if (!has_body && !e.dirty && !e.issue.empty() && e.synced_world && e.synced_world->matrix == scene_world->matrix) {
			return; // Still failing for the same input; keep the recorded issue.
		}

		const auto d = decompose(*scene_world);
		e.synced_world = *scene_world;
		if (!d) {
			fail(e, "world transform has shear, reflection or zero scale");
			e.synced_world = *scene_world;
			return;
		}

		if (e.dirty || !has_body || !same_scale(d->scale, e.built_scale)) {
			// Scale-only rebuilds during simulation keep momentum; authoring edits restart from authored values.
			build(id, e, *d, simulating && !e.dirty && has_body);
			e.dirty = false;
			e.synced_world = *scene_world;
			return;
		}

		const Pose pose = to_pose(*d);
		if (simulating && e.body.type == BodyType::Kinematic) {
			world.move_kinematic(e.handle, pose);
			e.kinematic_target = true;
		} else {
			world.set_body_pose(e.handle, pose);
		}
		++report.scene_updates;
	}

	void collect_events()
	{
		for (const ContactBegin& c : world.contact_begins()) {
			events.push_back({CollisionEvent::Kind::ContactBegin, c.user_key_a, c.user_key_b, {}, {}, 0.0f});
		}
		for (const ContactEnd& c : world.contact_ends()) {
			events.push_back({CollisionEvent::Kind::ContactEnd, c.user_key_a, c.user_key_b, {}, {}, 0.0f});
		}
		for (const ContactHit& c : world.contact_hits()) {
			events.push_back({CollisionEvent::Kind::Hit, c.user_key_a, c.user_key_b, c.point, c.normal, c.approach_speed});
		}
		for (const SensorOverlap& s : world.sensor_begins()) {
			events.push_back({CollisionEvent::Kind::SensorBegin, s.sensor_user_key, s.visitor_user_key, {}, {}, 0.0f});
		}
		for (const SensorOverlap& s : world.sensor_ends()) {
			events.push_back({CollisionEvent::Kind::SensorEnd, s.sensor_user_key, s.visitor_user_key, {}, {}, 0.0f});
		}

		for (const BodyMoved& moved : world.moved_bodies()) {
			const Entry* e = find(moved.user_key);
			if (e && e->handle == moved.body && e->body.type == BodyType::Dynamic) {
				pending.emplace_back(moved.user_key, moved.pose);
			}
		}
		std::sort(pending.begin(), pending.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	}
};

ScenePhysics::ScenePhysics(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ScenePhysics::~ScenePhysics() = default;
ScenePhysics::ScenePhysics(ScenePhysics&& other) noexcept = default;
ScenePhysics& ScenePhysics::operator=(ScenePhysics&& other) noexcept = default;

Result<ScenePhysics> ScenePhysics::create(const WorldConfig& config)
{
	auto world = PhysicsWorld::create(config);
	if (!world) {
		return fail(world.error() == PhysicsError::InvalidConfig ? ErrorCode::InvalidArgument : ErrorCode::InvalidState,
			to_string(world.error()));
	}
	auto impl = std::make_unique<Impl>();
	impl->world = std::move(world).value();
	return ScenePhysics(std::move(impl));
}

RigidBody to_rigid_body(const std::optional<RigidBodyComponent>& body) noexcept
{
	RigidBody out;
	if (!body) {
		out.type = BodyType::Static;
		return out;
	}
	out.type = body->kind == BodyKind::Static ? BodyType::Static : body->kind == BodyKind::Kinematic ? BodyType::Kinematic : BodyType::Dynamic;
	out.linear_velocity = to_float(body->linear_velocity);
	out.angular_velocity = to_float(body->angular_velocity);
	out.linear_damping = static_cast<float>(body->linear_damping);
	out.angular_damping = static_cast<float>(body->angular_damping);
	out.gravity_scale = static_cast<float>(body->gravity_scale);
	out.enable_sleep = body->enable_sleep;
	out.is_bullet = body->is_bullet;
	out.lock_rotation = body->lock_rotation;
	return out;
}

Collider to_collider(const ColliderComponent& collider) noexcept
{
	Collider out;
	const Vec3 center = to_float(collider.center);
	const Quat rotation{static_cast<float>(collider.rotation.x), static_cast<float>(collider.rotation.y),
		static_cast<float>(collider.rotation.z), static_cast<float>(collider.rotation.w)};
	const auto radius = static_cast<float>(collider.radius);
	switch (collider.shape) {
	case ColliderShape::Sphere:
		out.shape = SphereShape{radius, center};
		break;
	case ColliderShape::Capsule: {
		const Vec3 axis = rotate(rotation, Vec3{0.0f, static_cast<float>(collider.half_height), 0.0f});
		out.shape = CapsuleShape{sub(center, axis), add(center, axis), radius};
		break;
	}
	default:
		out.shape = BoxShape{to_float(collider.half_extents), Pose{center, rotation}};
		break;
	}
	out.density = static_cast<float>(collider.density);
	out.friction = static_cast<float>(collider.friction);
	out.restitution = static_cast<float>(collider.restitution);
	out.is_sensor = collider.sensor;
	// Box3D needs the sensor-event flag on both the sensor and its visitors.
	out.enable_sensor_events = collider.events;
	out.enable_contact_events = collider.events;
	out.enable_hit_events = collider.events;
	out.filter.category_bits = collider.category;
	out.filter.mask_bits = collider.mask;
	return out;
}

std::optional<RigidBody> ScenePhysics::rigid_body(EntityId entity) const
{
	const auto* e = impl_->find(entity);
	return e ? std::optional<RigidBody>(e->body) : std::nullopt;
}

std::span<const Collider> ScenePhysics::colliders(EntityId entity) const
{
	const auto* e = impl_->find(entity);
	return e ? std::span<const Collider>(e->colliders) : std::span<const Collider>{};
}

std::vector<EntityId> ScenePhysics::authored_entities() const
{
	std::vector<EntityId> ids;
	ids.reserve(impl_->entries.size());
	for (const auto& [id, e] : impl_->entries) {
		(void)e;
		ids.push_back(id);
	}
	return ids;
}

void ScenePhysics::set_simulation_enabled(bool enabled) noexcept
{
	impl_->simulating = enabled;
	if (!enabled) {
		impl_->pending.clear();
	}
}

bool ScenePhysics::simulation_enabled() const noexcept
{
	return impl_->simulating;
}

Result<void> ScenePhysics::reset_simulation(const Scene& scene)
{
	impl_->clear_runtime_bodies();
	for (auto& [id, e] : impl_->entries) {
		(void)id;
		e.dirty = true;
	}
	impl_->events.clear();
	impl_->pending.clear();
	return sync(scene);
}

Result<void> ScenePhysics::sync(const Scene& scene)
{
	if (!scene.on_owner_thread()) {
		return fail(ErrorCode::WrongThread, "Physics sync requires the scene owner thread");
	}
	// A ragdoll goes with its entity.
	for (auto it = impl_->ragdolls.begin(); it != impl_->ragdolls.end();) {
		const auto next = std::next(it);
		if (!scene.inspect(it->second.owner)) {
			impl_->destroy_ragdoll(it);
		}
		it = next;
	}
	// Mirror the scene's physics components: add or update entries, then drop entities that lost them.
	auto& entries = impl_->entries;
	std::vector<EntityId> authored;
	for (const EntityRecord& record : scene.snapshot().entities) {
		const Components& c = record.components;
		if (!c.body && c.colliders.empty()) {
			continue;
		}
		authored.push_back(record.id);
		auto [it, created] = entries.try_emplace(record.id);
		Impl::Entry& e = it->second;
		if (created || e.body_source != c.body || e.collider_source != c.colliders) {
			e.body_source = c.body;
			e.collider_source = c.colliders;
			e.body = to_rigid_body(c.body);
			e.colliders.clear();
			for (const ColliderComponent& collider : c.colliders) {
				e.colliders.push_back(to_collider(collider));
			}
			e.authoring_issue.clear();
			if (!valid_body(e.body) || !std::all_of(e.colliders.begin(), e.colliders.end(), [](const Collider& k) { return is_valid(k); })) {
				e.authoring_issue = "rigid body or collider values are not accepted by the physics world";
			}
			e.dirty = true;
		}
	}
	for (auto it = entries.begin(); it != entries.end();) {
		if (!std::binary_search(authored.begin(), authored.end(), it->first)) {
			impl_->destroy_body(it->second);
			std::erase_if(impl_->pending, [id = it->first](const auto& p) { return p.first == id; });
			it = entries.erase(it);
		} else {
			++it;
		}
	}
	for (auto& [id, e] : entries) {
		impl_->sync_entry(scene, id, e);
	}
	return {};
}

Result<void> ScenePhysics::step()
{
	impl_->events.clear();
	impl_->pending.clear();
	if (!impl_->simulating) {
		return {};
	}
	if (const PhysicsError error = impl_->world.step(); error != PhysicsError::None) {
		return fail(ErrorCode::InvalidState, to_string(error));
	}
	++impl_->simulated_ticks;
	impl_->report.stepped = true;
	impl_->collect_events();
	impl_->age_debris(std::chrono::duration<double>(impl_->world.config().fixed_timestep).count());
	return {};
}

std::vector<commands::SetTransform> ScenePhysics::pending_transforms(const Scene& scene) const
{
	std::vector<commands::SetTransform> out;
	out.reserve(impl_->pending.size());
	for (const auto& [id, pose] : impl_->pending) {
		const auto record = scene.inspect(id);
		if (!record) {
			continue;
		}
		DVec3 position{pose.position.x, pose.position.y, pose.position.z};
		DQuat rotation = normalize(DQuat{pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w});
		if (record->parent != 0) {
			// sync() guaranteed a rigid, uniformly scaled parent when the body was built.
			const auto parent_world = scene.world_transform(record->parent);
			const auto parent = parent_world ? decompose(*parent_world) : std::nullopt;
			if (!parent || !parent->uniform()) {
				continue;
			}
			const DQuat inverse = conjugate(parent->rotation);
			position = rotate(inverse, position - parent->translation) * (1.0 / parent->scale.x);
			rotation = normalize(inverse * rotation);
		}
		Transform local = record->transform;
		local.position = {position.x, position.y, position.z};
		local.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
		out.push_back({id, local});
	}
	return out;
}

Result<std::size_t> ScenePhysics::publish(Scene& scene, commands::CommandBus& bus, const commands::Context& context)
{
	const auto transforms = pending_transforms(scene);
	std::size_t published = 0;
	std::vector<commands::Command> batch;
	for (std::size_t first = 0; first < transforms.size(); first += commands::CommandBus::max_transaction) {
		const std::size_t last = std::min(transforms.size(), first + commands::CommandBus::max_transaction);
		batch.assign(transforms.begin() + static_cast<std::ptrdiff_t>(first), transforms.begin() + static_cast<std::ptrdiff_t>(last));
		auto receipt = bus.execute(context, bus.revision(), std::span<const commands::Command>(batch));
		if (!receipt) {
			mark_published(scene);
			return receipt.error();
		}
		published += batch.size();
		++impl_->report.transactions;
	}
	impl_->report.published += published;
	mark_published(scene);
	return published;
}

void ScenePhysics::mark_published(const Scene& scene)
{
	for (const auto& [id, pose] : impl_->pending) {
		(void)pose;
		auto* e = impl_->find(id);
		const auto world = scene.world_transform(id);
		if (e && world) {
			e->synced_world = *world;
		}
	}
	impl_->pending.clear();
}

Result<TickReport> ScenePhysics::fixed_update(std::chrono::nanoseconds delta, Scene& scene, commands::CommandBus& bus,
	const commands::Context& context)
{
	if (delta != impl_->world.config().fixed_timestep) {
		return fail(ErrorCode::InvalidArgument, "Fixed delta does not match the physics fixed timestep");
	}
	impl_->report = {};
	++impl_->ticks;
	if (auto synced = sync(scene); !synced) {
		return synced.error();
	}
	if (auto stepped = step(); !stepped) {
		return stepped.error();
	}
	if (auto published = publish(scene, bus, context); !published) {
		return published.error();
	}
	return impl_->report;
}

std::optional<BodyState> ScenePhysics::body_state(EntityId entity) const
{
	const auto* e = impl_->find(entity);
	if (!e || !impl_->world.is_valid(e->handle)) {
		return std::nullopt;
	}
	const PhysicsWorld& w = impl_->world;
	BodyState state;
	state.entity = entity;
	state.type = e->body.type;
	state.pose = w.body_pose(e->handle).value();
	state.linear_velocity = w.linear_velocity(e->handle).value();
	state.angular_velocity = w.angular_velocity(e->handle).value();
	state.mass = w.mass(e->handle).value();
	state.awake = w.is_awake(e->handle).value();
	return state;
}

std::optional<RaycastHit> ScenePhysics::raycast(Vec3 origin, Vec3 direction, float max_distance, const RaycastFilter& filter) const
{
	const float squared = direction.x * direction.x + direction.y * direction.y + direction.z * direction.z;
	if (!(max_distance > 0.0f) || !std::isfinite(max_distance) || !(std::abs(squared - 1.0f) < 1.0e-3f)) {
		return std::nullopt;
	}
	RayFilter ray{filter.category, filter.mask, {}};
	if (const auto* ignored = impl_->find(filter.ignore)) {
		ray.ignore_body = ignored->handle;
	}
	const RayHit hit = impl_->world.cast_ray(origin, scale(direction, max_distance), ray);
	if (!hit.hit) {
		return std::nullopt;
	}
	RaycastHit out{hit.user_key, hit.point, hit.normal, hit.fraction * max_distance};
	if (const auto part = impl_->part_bodies.find(hit.body.value); part != impl_->part_bodies.end()) {
		out.ragdoll = part->second.first;
		out.bone = impl_->ragdolls.at(part->second.first).parts[part->second.second].bone;
	} else if (const auto piece = impl_->piece_bodies.find(hit.body.value); piece != impl_->piece_bodies.end()) {
		out.debris = piece->second;
		out.entity = 0;
	}
	return out;
}

bool ScenePhysics::apply_impulse(EntityId entity, Vec3 impulse, Vec3 world_point)
{
	if (const std::uint64_t ragdoll = ragdoll_of(entity)) {
		return apply_ragdoll_impulse(ragdoll, impulse, world_point);
	}
	const auto* e = impl_->find(entity);
	return e && impl_->world.apply_linear_impulse_at(e->handle, impulse, world_point) == PhysicsError::None;
}

namespace {

bool valid_pose(const Pose& pose) noexcept
{
	const Quat& q = pose.rotation;
	const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
	return is_finite(pose.position) && std::isfinite(n) && n > 1.0e-6f;
}

bool finite_non_negative(float value) noexcept
{
	return std::isfinite(value) && value >= 0.0f;
}

// Distance from a point to a capsule's surface, negative inside.
float capsule_distance(const Pose& pose, const CapsuleShape& capsule, Vec3 point) noexcept
{
	const Vec3 a = transform_point(pose, capsule.point_a), b = transform_point(pose, capsule.point_b);
	const DVec3 ab{b.x - a.x, b.y - a.y, b.z - a.z}, ap{point.x - a.x, point.y - a.y, point.z - a.z};
	const double squared = dot(ab, ab);
	const double t = squared > 1.0e-12 ? std::clamp(dot(ap, ab) / squared, 0.0, 1.0) : 0.0;
	return static_cast<float>(length(ap - ab * t)) - capsule.radius;
}

} // namespace

Result<std::uint64_t> ScenePhysics::create_ragdoll(EntityId owner, const RagdollDesc& desc)
{
	auto& m = *impl_;
	if (!owner) {
		return fail(ErrorCode::InvalidArgument, "A ragdoll needs the entity it belongs to");
	}
	if (m.ragdoll_owners.contains(owner)) {
		return fail(ErrorCode::InvalidState, "The entity is already a ragdoll");
	}
	if (m.ragdolls.size() >= max_ragdolls) {
		return fail(ErrorCode::InvalidState, "There can be at most " + std::to_string(max_ragdolls) + " ragdolls");
	}
	if (desc.parts.empty() || desc.parts.size() > max_ragdoll_parts) {
		return fail(ErrorCode::InvalidArgument, "A ragdoll has 1 to " + std::to_string(max_ragdoll_parts) + " parts");
	}
	if (!finite_non_negative(desc.linear_damping) || !finite_non_negative(desc.angular_damping)) {
		return fail(ErrorCode::InvalidArgument, "Ragdoll damping must be finite and not negative");
	}
	std::vector<int> bones;
	for (std::size_t i = 0; i < desc.parts.size(); ++i) {
		const RagdollPart& p = desc.parts[i];
		const std::string which = "Ragdoll part " + std::to_string(i) + ": ";
		if (p.parent < -1 || (p.parent >= 0 && static_cast<std::size_t>(p.parent) >= i)) {
			return fail(ErrorCode::InvalidArgument, which + "a part's parent comes before it");
		}
		if (p.bone < 0 || std::find(bones.begin(), bones.end(), p.bone) != bones.end()) {
			return fail(ErrorCode::InvalidArgument, which + "each part stands for a different bone");
		}
		bones.push_back(p.bone);
		if (!valid_pose(p.pose) || !is_finite(p.linear_velocity) || !is_finite(p.angular_velocity)) {
			return fail(ErrorCode::InvalidArgument, which + "its pose and velocities must be finite");
		}
		ColliderDesc shape;
		shape.shape = p.capsule;
		shape.density = p.density;
		shape.friction = p.friction;
		bool shapes_valid = is_valid(shape) && p.more.size() < 16;
		for (const CapsuleShape& more : p.more) {
			shape.shape = more;
			shapes_valid = shapes_valid && is_valid(shape);
		}
		if (!shapes_valid || !(p.density > 0.0f)) {
			return fail(ErrorCode::InvalidArgument, which + "its capsules, density or friction are not accepted");
		}
		JointDesc joint;
		joint.frame_a = p.frame_parent;
		joint.frame_b = p.frame_child;
		joint.cone = p.cone;
		joint.twist_lower = p.twist_lower;
		joint.twist_upper = p.twist_upper;
		joint.friction_torque = p.friction_torque;
		if (i > 0 && !is_valid(joint)) {
			return fail(ErrorCode::InvalidArgument, which + "its joint frames or limits are not accepted");
		}
	}

	const std::uint64_t id = m.next_runtime_id++;
	Impl::Ragdoll ragdoll;
	ragdoll.owner = owner;
	// One negative group per ragdoll: its parts never collide with each other.
	const int group = -static_cast<int>(1 + id % 0x3fff'ffffu);
	const auto undo = [&](const std::string& why) -> Result<std::uint64_t> {
		for (auto part = ragdoll.parts.rbegin(); part != ragdoll.parts.rend(); ++part) {
			m.world.destroy_body(part->body);
		}
		return fail(ErrorCode::InvalidState, why);
	};
	for (const RagdollPart& p : desc.parts) {
		BodyDesc body;
		body.type = BodyType::Dynamic;
		body.pose = p.pose;
		body.linear_velocity = p.linear_velocity;
		body.angular_velocity = p.angular_velocity;
		body.linear_damping = desc.linear_damping;
		body.angular_damping = desc.angular_damping;
		body.user_key = owner;
		auto created = m.world.create_body(body);
		if (!created) {
			return undo(std::string("Ragdoll body rejected: ") + to_string(created.error()));
		}
		std::vector<CapsuleShape> shapes{p.capsule};
		shapes.insert(shapes.end(), p.more.begin(), p.more.end());
		ragdoll.parts.push_back({created.value(), {}, p.bone, shapes});
		for (const CapsuleShape& capsule : shapes) {
			ColliderDesc collider;
			collider.shape = capsule;
			collider.density = p.density;
			collider.friction = p.friction;
			collider.filter = {ragdoll_category, desc.mask, group};
			if (auto added = m.world.add_collider(created.value(), collider); !added) {
				return undo(std::string("Ragdoll collider rejected: ") + to_string(added.error()));
			}
		}
		if (p.parent >= 0) {
			JointDesc joint;
			joint.type = JointType::Spherical;
			joint.body_a = ragdoll.parts[static_cast<std::size_t>(p.parent)].body;
			joint.body_b = created.value();
			joint.frame_a = p.frame_parent;
			joint.frame_b = p.frame_child;
			joint.cone = p.cone;
			joint.twist_lower = p.twist_lower;
			joint.twist_upper = p.twist_upper;
			joint.friction_torque = p.friction_torque;
			auto made = m.world.create_joint(joint);
			if (!made) {
				return undo(std::string("Ragdoll joint rejected: ") + to_string(made.error()));
			}
			ragdoll.parts.back().joint = made.value();
		}
	}
	if (desc.disable_owner) {
		ragdoll.owner_disabled = true;
		if (const Impl::Entry* e = m.find(owner); e && m.world.is_valid(e->handle)) {
			m.world.set_body_enabled(e->handle, false);
		}
	}
	for (std::size_t i = 0; i < ragdoll.parts.size(); ++i) {
		m.part_bodies[ragdoll.parts[i].body.value] = {id, i};
	}
	m.ragdoll_owners[owner] = id;
	m.ragdolls.emplace(id, std::move(ragdoll));
	return id;
}

bool ScenePhysics::destroy_ragdoll(std::uint64_t id)
{
	const auto it = impl_->ragdolls.find(id);
	if (it == impl_->ragdolls.end()) {
		return false;
	}
	impl_->destroy_ragdoll(it);
	return true;
}

std::uint64_t ScenePhysics::ragdoll_of(EntityId owner) const
{
	const auto it = impl_->ragdoll_owners.find(owner);
	return it == impl_->ragdoll_owners.end() ? 0 : it->second;
}

std::optional<RagdollState> ScenePhysics::ragdoll(std::uint64_t id) const
{
	const auto it = impl_->ragdolls.find(id);
	if (it == impl_->ragdolls.end()) {
		return std::nullopt;
	}
	const PhysicsWorld& w = impl_->world;
	RagdollState state;
	state.id = id;
	state.owner = it->second.owner;
	for (const Impl::Part& part : it->second.parts) {
		RagdollPartState s;
		s.bone = part.bone;
		s.pose = w.body_pose(part.body).value();
		s.linear_velocity = w.linear_velocity(part.body).value();
		s.angular_velocity = w.angular_velocity(part.body).value();
		state.awake = state.awake || w.is_awake(part.body).value();
		state.parts.push_back(s);
	}
	return state;
}

std::vector<std::uint64_t> ScenePhysics::ragdolls() const
{
	std::vector<std::uint64_t> ids;
	for (const auto& [id, ragdoll] : impl_->ragdolls) {
		(void)ragdoll;
		ids.push_back(id);
	}
	return ids;
}

std::vector<JointState> ScenePhysics::ragdoll_joints(std::uint64_t id) const
{
	std::vector<JointState> joints;
	const auto it = impl_->ragdolls.find(id);
	if (it == impl_->ragdolls.end()) {
		return joints;
	}
	for (const Impl::Part& part : it->second.parts) {
		if (const auto state = impl_->world.joint_state(part.joint)) {
			joints.push_back(state.value());
		}
	}
	return joints;
}

bool ScenePhysics::apply_ragdoll_impulse(std::uint64_t id, Vec3 impulse, Vec3 world_point, int bone)
{
	const auto it = impl_->ragdolls.find(id);
	if (it == impl_->ragdolls.end() || !is_finite(impulse) || !is_finite(world_point)) {
		return false;
	}
	const Impl::Part* chosen = nullptr;
	float nearest = 0.0f;
	for (const Impl::Part& part : it->second.parts) {
		if (bone >= 0) {
			if (part.bone == bone) {
				chosen = &part;
			}
			continue;
		}
		const Pose pose = impl_->world.body_pose(part.body).value();
		float distance = std::numeric_limits<float>::max();
		for (const CapsuleShape& capsule : part.shapes) {
			distance = std::min(distance, capsule_distance(pose, capsule, world_point));
		}
		if (!chosen || distance < nearest) {
			chosen = &part;
			nearest = distance;
		}
	}
	return chosen && impl_->world.apply_linear_impulse_at(chosen->body, impulse, world_point) == PhysicsError::None;
}

Result<std::uint64_t> ScenePhysics::spawn_debris(const Debris& piece)
{
	auto& m = *impl_;
	if (!valid_pose(piece.pose) || !is_finite(piece.linear_velocity) || !is_finite(piece.angular_velocity)) {
		return fail(ErrorCode::InvalidArgument, "Debris pose and velocities must be finite");
	}
	if (!std::isfinite(piece.lifetime) || !(piece.lifetime > 0.0) || piece.lifetime > 600.0) {
		return fail(ErrorCode::InvalidArgument, "Debris lifetime must be within (0, 600] seconds");
	}
	if (!is_valid(piece.collider) || piece.collider.is_sensor || !(piece.collider.density > 0.0f)) {
		return fail(ErrorCode::InvalidArgument, "Debris needs a solid shape with a positive density");
	}
	if (m.pieces.size() >= max_debris) {
		m.remove_piece(0);
		++m.debris_replaced;
	}
	BodyDesc body;
	body.type = BodyType::Dynamic;
	body.pose = piece.pose;
	body.linear_velocity = piece.linear_velocity;
	body.angular_velocity = piece.angular_velocity;
	auto created = m.world.create_body(body);
	if (!created) {
		return fail(ErrorCode::InvalidState, std::string("Debris body rejected: ") + to_string(created.error()));
	}
	Collider collider = piece.collider;
	collider.filter.category_bits = debris_category;
	collider.filter.group = 0;
	collider.enable_sensor_events = false;
	collider.enable_contact_events = false;
	collider.enable_hit_events = false;
	if (auto added = m.world.add_collider(created.value(), collider); !added) {
		m.world.destroy_body(created.value());
		return fail(ErrorCode::InvalidState, std::string("Debris collider rejected: ") + to_string(added.error()));
	}
	const std::uint64_t id = m.next_runtime_id++;
	m.pieces.push_back({id, piece.look, created.value(), collider, piece.lifetime});
	m.piece_bodies[created.value().value] = id;
	++m.debris_spawned;
	return id;
}

bool ScenePhysics::remove_debris(std::uint64_t id)
{
	auto& pieces = impl_->pieces;
	const auto it = std::find_if(pieces.begin(), pieces.end(), [&](const Impl::Piece& p) { return p.id == id; });
	if (it == pieces.end()) {
		return false;
	}
	impl_->remove_piece(static_cast<std::size_t>(it - pieces.begin()));
	return true;
}

std::vector<DebrisState> ScenePhysics::debris() const
{
	std::vector<DebrisState> out;
	for (const Impl::Piece& p : impl_->pieces) {
		out.push_back({p.id, p.look, impl_->world.body_pose(p.body).value(), p.age, p.asleep});
	}
	return out;
}

bool ScenePhysics::set_linear_velocity(EntityId entity, Vec3 velocity)
{
	const auto* e = impl_->find(entity);
	return e && e->body.type != BodyType::Static && impl_->world.set_linear_velocity(e->handle, velocity) == PhysicsError::None;
}

std::span<const CollisionEvent> ScenePhysics::events() const noexcept
{
	return impl_->events;
}

PhysicsDiagnostics ScenePhysics::diagnostics() const
{
	PhysicsDiagnostics d;
	d.simulation_enabled = impl_->simulating;
	d.ticks = impl_->ticks;
	d.simulated_ticks = impl_->simulated_ticks;
	d.world = impl_->world.stats();
	d.authored_entities = impl_->entries.size();
	d.last_tick = impl_->report;
	for (const auto& [id, e] : impl_->entries) {
		if (impl_->world.is_valid(e.handle)) {
			++d.active_bodies;
		}
		if (!e.issue.empty()) {
			d.issues.push_back({id, e.issue});
		}
	}
	d.ragdolls = impl_->ragdolls.size();
	for (const auto& [id, ragdoll] : impl_->ragdolls) {
		(void)id;
		d.ragdoll_parts += ragdoll.parts.size();
	}
	d.debris = impl_->pieces.size();
	d.debris_spawned = impl_->debris_spawned;
	d.debris_expired = impl_->debris_expired;
	d.debris_replaced = impl_->debris_replaced;
	return d;
}

const WorldConfig& ScenePhysics::config() const noexcept
{
	return impl_->world.config();
}

namespace {

struct LineSink {
	std::vector<DebugLine>& lines;
	const Pose& pose;
	EntityId entity;
	DebugCategory category;

	void line(Vec3 a, Vec3 b) const { lines.push_back({transform_point(pose, a), transform_point(pose, b), entity, category}); }

	// Arc of `segments` lines from angle `from`, sweeping `sweep` radians in the (u, v) plane.
	void arc(Vec3 center, Vec3 u, Vec3 v, float radius, int segments, double from, double sweep) const
	{
		const auto point = [&](int k) {
			const double angle = from + sweep * k / segments;
			return add(center, add(scale(u, radius * static_cast<float>(std::cos(angle))), scale(v, radius * static_cast<float>(std::sin(angle)))));
		};
		for (int k = 0; k < segments; ++k) {
			line(point(k), point(k + 1));
		}
	}
};

constexpr double two_pi = 2.0 * std::numbers::pi;

void emit(const LineSink& sink, const SphereShape& sphere, int segments)
{
	const Vec3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
	sink.arc(sphere.center, x, y, sphere.radius, segments, 0.0, two_pi);
	sink.arc(sphere.center, y, z, sphere.radius, segments, 0.0, two_pi);
	sink.arc(sphere.center, z, x, sphere.radius, segments, 0.0, two_pi);
}

void emit(const LineSink& sink, const BoxShape& box, int)
{
	std::array<Vec3, 8> corners;
	for (int i = 0; i < 8; ++i) {
		const Vec3 local{(i & 1) ? box.half_extents.x : -box.half_extents.x, (i & 2) ? box.half_extents.y : -box.half_extents.y,
			(i & 4) ? box.half_extents.z : -box.half_extents.z};
		corners[static_cast<std::size_t>(i)] = transform_point(box.local_pose, local);
	}
	for (int i = 0; i < 8; ++i) {
		for (int bit = 1; bit < 8; bit <<= 1) {
			if ((i & bit) == 0) {
				sink.line(corners[static_cast<std::size_t>(i)], corners[static_cast<std::size_t>(i | bit)]);
			}
		}
	}
}

void emit(const LineSink& sink, const CapsuleShape& capsule, int segments)
{
	const Vec3 axis{capsule.point_b.x - capsule.point_a.x, capsule.point_b.y - capsule.point_a.y, capsule.point_b.z - capsule.point_a.z};
	const float len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
	if (len < 1.0e-6f) {
		emit(sink, SphereShape{capsule.radius, capsule.point_a}, segments);
		return;
	}
	const Vec3 n = scale(axis, 1.0f / len);
	// Any unit vector not parallel to n, then an orthonormal (u, v) basis around the axis.
	const Vec3 helper = std::abs(n.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
	const DVec3 du = cross(DVec3{helper.x, helper.y, helper.z}, DVec3{n.x, n.y, n.z});
	const DVec3 dun = du * (1.0 / length(du));
	const Vec3 u{static_cast<float>(dun.x), static_cast<float>(dun.y), static_cast<float>(dun.z)};
	const DVec3 dv = cross(DVec3{n.x, n.y, n.z}, dun);
	const Vec3 v{static_cast<float>(dv.x), static_cast<float>(dv.y), static_cast<float>(dv.z)};
	const float r = capsule.radius;
	const int half = (segments + 1) / 2;

	sink.arc(capsule.point_a, u, v, r, segments, 0.0, two_pi);
	sink.arc(capsule.point_b, u, v, r, segments, 0.0, two_pi);
	for (const Vec3 side : {u, v, scale(u, -1.0f), scale(v, -1.0f)}) {
		sink.line(add(capsule.point_a, scale(side, r)), add(capsule.point_b, scale(side, r)));
	}
	// Hemisphere caps: half arcs bulging outward along +n at b and -n at a.
	sink.arc(capsule.point_b, u, n, r, half, 0.0, std::numbers::pi);
	sink.arc(capsule.point_b, v, n, r, half, 0.0, std::numbers::pi);
	sink.arc(capsule.point_a, u, scale(n, -1.0f), r, half, 0.0, std::numbers::pi);
	sink.arc(capsule.point_a, v, scale(n, -1.0f), r, half, 0.0, std::numbers::pi);
}

} // namespace

DebugGeometry ScenePhysics::debug_geometry(const DebugGeometryOptions& options) const
{
	DebugGeometry geometry;
	const int segments = std::clamp(options.circle_segments, 4, 256);
	for (const auto& [id, e] : impl_->entries) {
		if (!impl_->world.is_valid(e.handle)) {
			continue;
		}
		if (e.body.type == BodyType::Static && !options.include_static) {
			continue;
		}
		DebugCategory body_category = DebugCategory::Static;
		if (e.body.type == BodyType::Kinematic) {
			body_category = DebugCategory::Kinematic;
		} else if (e.body.type == BodyType::Dynamic) {
			body_category = impl_->world.is_awake(e.handle).value() ? DebugCategory::DynamicAwake : DebugCategory::DynamicAsleep;
		}
		const Pose pose = impl_->world.body_pose(e.handle).value();
		for (const Collider& collider : e.built) {
			if (collider.is_sensor && !options.include_sensors) {
				continue;
			}
			const LineSink sink{geometry.lines, pose, id, collider.is_sensor ? DebugCategory::Sensor : body_category};
			std::visit([&](const auto& shape) { emit(sink, shape, segments); }, collider.shape);
		}
	}
	for (const auto& [ragdoll_id, ragdoll] : impl_->ragdolls) {
		(void)ragdoll_id;
		for (const auto& part : ragdoll.parts) {
			const Pose pose = impl_->world.body_pose(part.body).value();
			for (const CapsuleShape& capsule : part.shapes) {
				append_shape_lines(geometry.lines, capsule, pose, ragdoll.owner, DebugCategory::Ragdoll, segments);
			}
		}
	}
	for (const auto& piece : impl_->pieces) {
		const auto category = impl_->world.is_awake(piece.body).value() ? DebugCategory::DynamicAwake : DebugCategory::DynamicAsleep;
		append_shape_lines(geometry.lines, piece.built.shape, impl_->world.body_pose(piece.body).value(), 0, category, segments);
	}
	return geometry;
}

void append_shape_lines(std::vector<DebugLine>& lines, const ShapeDesc& shape, const Pose& pose, EntityId entity,
	DebugCategory category, int circle_segments)
{
	const LineSink sink{lines, pose, entity, category};
	const int segments = std::clamp(circle_segments, 4, 256);
	std::visit([&](const auto& s) { emit(sink, s, segments); }, shape);
}

} // namespace engine::physics
