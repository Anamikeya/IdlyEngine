#include "engine/physics/physics_world.h"

#include <box3d/box3d.h>
#include <box3d/collision.h>
#include <box3d/constants.h>

#include <cmath>
#include <unordered_map>
#include <vector>

namespace engine::physics {
namespace {

constexpr int max_sub_steps = 64;

b3Vec3 to_b3(Vec3 v) noexcept
{
	return b3Vec3{v.x, v.y, v.z};
}

b3Quat to_b3(Quat q) noexcept
{
	return b3Quat{b3Vec3{q.x, q.y, q.z}, q.w};
}

Vec3 from_b3(b3Vec3 v) noexcept
{
	return Vec3{v.x, v.y, v.z};
}

Quat from_b3(b3Quat q) noexcept
{
	return Quat{q.v.x, q.v.y, q.v.z, q.s};
}

Pose from_b3(b3WorldTransform t) noexcept
{
	return Pose{from_b3(b3ToVec3(t.p)), from_b3(t.q)};
}

b3WorldTransform to_b3_world(const Pose& pose) noexcept
{
	return b3WorldTransform{b3ToPos(to_b3(pose.position)), to_b3(pose.rotation)};
}

bool is_finite(float f) noexcept
{
	return std::isfinite(f);
}

bool is_finite(Vec3 v) noexcept
{
	return is_finite(v.x) && is_finite(v.y) && is_finite(v.z);
}

bool is_finite_non_negative(float f) noexcept
{
	return is_finite(f) && f >= 0.0f;
}

bool is_finite_positive(float f) noexcept
{
	return is_finite(f) && f > 0.0f;
}

// Box3D asserts on non-unit quaternions; accept any non-degenerate input and normalize it.
bool normalized(Quat q, Quat& out) noexcept
{
	if (!is_finite(q.x) || !is_finite(q.y) || !is_finite(q.z) || !is_finite(q.w)) {
		return false;
	}
	const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	if (length < 1.0e-6f) {
		return false;
	}
	out = Quat{q.x / length, q.y / length, q.z / length, q.w / length};
	return true;
}

std::uint64_t encode(std::int32_t index1, std::uint16_t world0, std::uint16_t generation) noexcept
{
	return static_cast<std::uint64_t>(static_cast<std::uint32_t>(index1)) |
		(static_cast<std::uint64_t>(world0) << 32) | (static_cast<std::uint64_t>(generation) << 48);
}

BodyHandle encode(b3BodyId id) noexcept
{
	return BodyHandle{B3_IS_NULL(id) ? 0 : encode(id.index1, id.world0, id.generation)};
}

ColliderHandle encode(b3ShapeId id) noexcept
{
	return ColliderHandle{B3_IS_NULL(id) ? 0 : encode(id.index1, id.world0, id.generation)};
}

b3BodyId decode(BodyHandle handle) noexcept
{
	return b3BodyId{static_cast<std::int32_t>(handle.value & 0xffff'ffffu), static_cast<std::uint16_t>(handle.value >> 32),
		static_cast<std::uint16_t>(handle.value >> 48)};
}

b3ShapeId decode(ColliderHandle handle) noexcept
{
	return b3ShapeId{static_cast<std::int32_t>(handle.value & 0xffff'ffffu), static_cast<std::uint16_t>(handle.value >> 32),
		static_cast<std::uint16_t>(handle.value >> 48)};
}

b3BodyType to_b3(BodyType type) noexcept
{
	switch (type) {
	case BodyType::Static:
		return b3_staticBody;
	case BodyType::Kinematic:
		return b3_kinematicBody;
	case BodyType::Dynamic:
		break;
	}
	return b3_dynamicBody;
}

bool valid_config(const WorldConfig& config) noexcept
{
	return is_finite(config.gravity) && config.fixed_timestep.count() > 0 && config.sub_steps >= 1 &&
		config.sub_steps <= max_sub_steps && config.worker_count >= 1 && config.max_steps_per_advance >= 1;
}

struct ShapeValidator {
	bool operator()(const SphereShape& s) const noexcept { return is_finite_positive(s.radius) && is_finite(s.center); }

	bool operator()(const BoxShape& b) const noexcept
	{
		Quat q;
		return is_finite_positive(b.half_extents.x) && is_finite_positive(b.half_extents.y) &&
			is_finite_positive(b.half_extents.z) && is_finite(b.local_pose.position) && normalized(b.local_pose.rotation, q);
	}

	bool operator()(const CapsuleShape& c) const noexcept
	{
		return is_finite_positive(c.radius) && is_finite(c.point_a) && is_finite(c.point_b);
	}
};

} // namespace

const char* to_string(PhysicsError error) noexcept
{
	switch (error) {
	case PhysicsError::None:
		return "none";
	case PhysicsError::InvalidConfig:
		return "invalid world config";
	case PhysicsError::WorldLimitReached:
		return "physics world limit reached";
	case PhysicsError::InvalidWorld:
		return "invalid physics world";
	case PhysicsError::InvalidBody:
		return "invalid body";
	case PhysicsError::InvalidCollider:
		return "invalid collider";
	case PhysicsError::InvalidArgument:
		return "invalid argument";
	}
	return "unknown physics error";
}

struct PhysicsWorld::Impl {
	b3WorldId world = b3_nullWorldId;
	WorldConfig config;
	float fixed_dt = 0.0f;
	std::chrono::nanoseconds accumulator{0};
	std::uint64_t step_count = 0;

	// Indexed by b3BodyId::index1; Box3D reuses slots, create_body overwrites.
	std::vector<std::uint64_t> body_keys;

	std::vector<BodyMoved> moved;
	std::unordered_map<std::uint64_t, std::size_t> moved_slots;
	std::vector<ContactBegin> contact_begins;
	std::vector<ContactEnd> contact_ends;
	std::vector<ContactHit> contact_hits;
	std::vector<SensorOverlap> sensor_begins;
	std::vector<SensorOverlap> sensor_ends;

	~Impl()
	{
		if (b3World_IsValid(world)) {
			b3DestroyWorld(world);
		}
	}

	bool owns(b3BodyId id) const noexcept
	{
		return id.index1 != 0 && id.world0 + 1 == world.index1 && b3Body_IsValid(id);
	}

	bool owns(b3ShapeId id) const noexcept
	{
		return id.index1 != 0 && id.world0 + 1 == world.index1 && b3Shape_IsValid(id);
	}

	std::uint64_t key_of(b3BodyId id) const noexcept
	{
		const auto index = static_cast<std::size_t>(id.index1);
		return index < body_keys.size() ? body_keys[index] : 0;
	}

	// Resolves the owning body of a shape that may already be destroyed.
	void resolve(b3ShapeId shape, BodyHandle& body, std::uint64_t& key) const noexcept
	{
		if (owns(shape)) {
			const b3BodyId body_id = b3Shape_GetBody(shape);
			body = encode(body_id);
			key = key_of(body_id);
		}
	}

	void clear_events() noexcept
	{
		moved.clear();
		moved_slots.clear();
		contact_begins.clear();
		contact_ends.clear();
		contact_hits.clear();
		sensor_begins.clear();
		sensor_ends.clear();
	}

	void step_once()
	{
		b3World_Step(world, fixed_dt, config.sub_steps);
		++step_count;
		collect_events();
	}

	void collect_events()
	{
		const b3BodyEvents body_events = b3World_GetBodyEvents(world);
		for (int i = 0; i < body_events.moveCount; ++i) {
			const b3BodyMoveEvent& e = body_events.moveEvents[i];
			const BodyHandle handle = encode(e.bodyId);
			const BodyMoved record{handle, key_of(e.bodyId), from_b3(e.transform), e.fellAsleep};
			const auto [it, inserted] = moved_slots.try_emplace(handle.value, moved.size());
			if (inserted) {
				moved.push_back(record);
			} else {
				moved[it->second] = record;
			}
		}

		const b3ContactEvents contacts = b3World_GetContactEvents(world);
		for (int i = 0; i < contacts.beginCount; ++i) {
			const b3ContactBeginTouchEvent& e = contacts.beginEvents[i];
			ContactBegin record{encode(e.shapeIdA), encode(e.shapeIdB), {}, {}, 0, 0};
			resolve(e.shapeIdA, record.body_a, record.user_key_a);
			resolve(e.shapeIdB, record.body_b, record.user_key_b);
			contact_begins.push_back(record);
		}
		for (int i = 0; i < contacts.endCount; ++i) {
			const b3ContactEndTouchEvent& e = contacts.endEvents[i];
			ContactEnd record{encode(e.shapeIdA), encode(e.shapeIdB), {}, {}, 0, 0};
			resolve(e.shapeIdA, record.body_a, record.user_key_a);
			resolve(e.shapeIdB, record.body_b, record.user_key_b);
			contact_ends.push_back(record);
		}
		for (int i = 0; i < contacts.hitCount; ++i) {
			const b3ContactHitEvent& e = contacts.hitEvents[i];
			ContactHit record{encode(e.shapeIdA), encode(e.shapeIdB), {}, {}, 0, 0, from_b3(b3ToVec3(e.point)),
				from_b3(e.normal), e.approachSpeed};
			resolve(e.shapeIdA, record.body_a, record.user_key_a);
			resolve(e.shapeIdB, record.body_b, record.user_key_b);
			contact_hits.push_back(record);
		}

		const b3SensorEvents sensors = b3World_GetSensorEvents(world);
		for (int i = 0; i < sensors.beginCount; ++i) {
			const b3SensorBeginTouchEvent& e = sensors.beginEvents[i];
			SensorOverlap record{encode(e.sensorShapeId), encode(e.visitorShapeId), {}, {}, 0, 0};
			resolve(e.sensorShapeId, record.sensor_body, record.sensor_user_key);
			resolve(e.visitorShapeId, record.visitor_body, record.visitor_user_key);
			sensor_begins.push_back(record);
		}
		for (int i = 0; i < sensors.endCount; ++i) {
			const b3SensorEndTouchEvent& e = sensors.endEvents[i];
			SensorOverlap record{encode(e.sensorShapeId), encode(e.visitorShapeId), {}, {}, 0, 0};
			resolve(e.sensorShapeId, record.sensor_body, record.sensor_user_key);
			resolve(e.visitorShapeId, record.visitor_body, record.visitor_user_key);
			sensor_ends.push_back(record);
		}
	}
};

PhysicsResult<PhysicsWorld> PhysicsWorld::create(const WorldConfig& config)
{
	if (!valid_config(config)) {
		return PhysicsError::InvalidConfig;
	}
	// b3GetMaxWorldCount() is a high-water mark, not a capacity; Box3D asserts past B3_MAX_WORLDS.
	if (b3GetWorldCount() >= B3_MAX_WORLDS) {
		return PhysicsError::WorldLimitReached;
	}

	b3WorldDef def = b3DefaultWorldDef();
	def.gravity = to_b3(config.gravity);
	def.enableSleep = config.enable_sleep;
	def.enableContinuous = config.enable_continuous;
	def.workerCount = static_cast<std::uint32_t>(config.worker_count);

	auto impl = std::make_unique<Impl>();
	impl->world = b3CreateWorld(&def);
	if (!b3World_IsValid(impl->world)) {
		return PhysicsError::WorldLimitReached;
	}
	impl->config = config;
	impl->fixed_dt = static_cast<float>(std::chrono::duration<double>(config.fixed_timestep).count());
	return PhysicsWorld(std::move(impl));
}

PhysicsWorld::PhysicsWorld() noexcept = default;
PhysicsWorld::PhysicsWorld(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PhysicsWorld::~PhysicsWorld() = default;
PhysicsWorld::PhysicsWorld(PhysicsWorld&& other) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&& other) noexcept = default;

bool PhysicsWorld::valid() const noexcept
{
	return impl_ != nullptr && b3World_IsValid(impl_->world);
}

const WorldConfig& PhysicsWorld::config() const noexcept
{
	static const WorldConfig empty{};
	return impl_ ? impl_->config : empty;
}

void PhysicsWorld::reset() noexcept
{
	impl_.reset();
}

PhysicsError PhysicsWorld::step()
{
	if (!valid()) {
		return PhysicsError::InvalidWorld;
	}
	impl_->clear_events();
	impl_->step_once();
	return PhysicsError::None;
}

AdvanceReport PhysicsWorld::advance(std::chrono::nanoseconds elapsed)
{
	AdvanceReport report;
	if (!valid()) {
		return report;
	}
	impl_->clear_events();

	const std::chrono::nanoseconds tick = impl_->config.fixed_timestep;
	if (elapsed.count() > 0) {
		impl_->accumulator += elapsed;
	}

	std::int64_t due = impl_->accumulator / tick;
	if (due > impl_->config.max_steps_per_advance) {
		const std::int64_t excess = due - impl_->config.max_steps_per_advance;
		report.dropped = tick * excess;
		impl_->accumulator -= report.dropped;
		due = impl_->config.max_steps_per_advance;
	}

	for (std::int64_t i = 0; i < due; ++i) {
		impl_->step_once();
		impl_->accumulator -= tick;
	}
	report.steps = static_cast<int>(due);
	report.interpolation_alpha = static_cast<float>(
		static_cast<double>(impl_->accumulator.count()) / static_cast<double>(tick.count()));
	return report;
}

PhysicsResult<BodyHandle> PhysicsWorld::create_body(const BodyDesc& desc)
{
	if (!valid()) {
		return PhysicsError::InvalidWorld;
	}
	Quat rotation;
	if (!is_finite(desc.pose.position) || !normalized(desc.pose.rotation, rotation) || !is_finite(desc.linear_velocity) ||
		!is_finite(desc.angular_velocity) || !is_finite_non_negative(desc.linear_damping) ||
		!is_finite_non_negative(desc.angular_damping) || !is_finite(desc.gravity_scale)) {
		return PhysicsError::InvalidArgument;
	}

	b3BodyDef def = b3DefaultBodyDef();
	def.type = to_b3(desc.type);
	def.position = b3ToPos(to_b3(desc.pose.position));
	def.rotation = to_b3(rotation);
	def.linearVelocity = to_b3(desc.linear_velocity);
	def.angularVelocity = to_b3(desc.angular_velocity);
	def.linearDamping = desc.linear_damping;
	def.angularDamping = desc.angular_damping;
	def.gravityScale = desc.gravity_scale;
	def.enableSleep = desc.enable_sleep;
	def.isBullet = desc.is_bullet;

	const b3BodyId id = b3CreateBody(impl_->world, &def);
	if (B3_IS_NULL(id)) {
		return PhysicsError::InvalidWorld;
	}
	const auto index = static_cast<std::size_t>(id.index1);
	if (index >= impl_->body_keys.size()) {
		impl_->body_keys.resize(index + 1, 0);
	}
	impl_->body_keys[index] = desc.user_key;
	return encode(id);
}

PhysicsError PhysicsWorld::destroy_body(BodyHandle body)
{
	if (!valid()) {
		return PhysicsError::InvalidWorld;
	}
	const b3BodyId id = decode(body);
	if (!impl_->owns(id)) {
		return PhysicsError::InvalidBody;
	}
	impl_->body_keys[static_cast<std::size_t>(id.index1)] = 0;
	b3DestroyBody(id);
	return PhysicsError::None;
}

bool PhysicsWorld::is_valid(BodyHandle body) const noexcept
{
	return valid() && impl_->owns(decode(body));
}

PhysicsResult<ColliderHandle> PhysicsWorld::add_collider(BodyHandle body, const ColliderDesc& desc)
{
	if (!valid()) {
		return PhysicsError::InvalidWorld;
	}
	const b3BodyId body_id = decode(body);
	if (!impl_->owns(body_id)) {
		return PhysicsError::InvalidBody;
	}
	if (!physics::is_valid(desc)) {
		return PhysicsError::InvalidArgument;
	}

	b3ShapeDef def = b3DefaultShapeDef();
	def.density = desc.density;
	def.baseMaterial.friction = desc.friction;
	def.baseMaterial.restitution = desc.restitution;
	def.isSensor = desc.is_sensor;
	def.enableSensorEvents = desc.enable_sensor_events;
	def.enableContactEvents = desc.enable_contact_events;
	def.enableHitEvents = desc.enable_hit_events;
	def.filter.categoryBits = desc.filter.category_bits;
	def.filter.maskBits = desc.filter.mask_bits;
	def.filter.groupIndex = desc.filter.group;

	b3ShapeId shape = b3_nullShapeId;
	if (const auto* sphere = std::get_if<SphereShape>(&desc.shape)) {
		const b3Sphere geometry{to_b3(sphere->center), sphere->radius};
		shape = b3CreateSphereShape(body_id, &def, &geometry);
	} else if (const auto* box = std::get_if<BoxShape>(&desc.shape)) {
		Quat rotation;
		normalized(box->local_pose.rotation, rotation);
		const b3Transform local{to_b3(box->local_pose.position), to_b3(rotation)};
		// b3BoxHull is self-contained; Box3D copies it into the shape, so a stack value is fine.
		const b3BoxHull hull = b3MakeTransformedBoxHull(box->half_extents.x, box->half_extents.y, box->half_extents.z, local);
		shape = b3CreateHullShape(body_id, &def, &hull.base);
	} else if (const auto* capsule = std::get_if<CapsuleShape>(&desc.shape)) {
		// Box3D degrades a capsule with coincident points into a sphere.
		const b3Capsule geometry{to_b3(capsule->point_a), to_b3(capsule->point_b), capsule->radius};
		shape = b3CreateCapsuleShape(body_id, &def, &geometry);
	}

	if (B3_IS_NULL(shape)) {
		return PhysicsError::InvalidArgument;
	}
	return encode(shape);
}

PhysicsError PhysicsWorld::destroy_collider(ColliderHandle collider)
{
	if (!valid()) {
		return PhysicsError::InvalidWorld;
	}
	const b3ShapeId id = decode(collider);
	if (!impl_->owns(id)) {
		return PhysicsError::InvalidCollider;
	}
	b3DestroyShape(id, true);
	return PhysicsError::None;
}

bool PhysicsWorld::is_valid(ColliderHandle collider) const noexcept
{
	return valid() && impl_->owns(decode(collider));
}

BodyHandle PhysicsWorld::collider_body(ColliderHandle collider) const noexcept
{
	const b3ShapeId id = decode(collider);
	if (!valid() || !impl_->owns(id)) {
		return {};
	}
	return encode(b3Shape_GetBody(id));
}

PhysicsResult<Pose> PhysicsWorld::body_pose(BodyHandle body) const
{
	const b3BodyId id = decode(body);
	if (!valid() || !impl_->owns(id)) {
		return PhysicsError::InvalidBody;
	}
	return from_b3(b3Body_GetTransform(id));
}

PhysicsError PhysicsWorld::set_body_pose(BodyHandle body, const Pose& pose)
{
	const b3BodyId id = decode(body);
	if (!valid() || !impl_->owns(id)) {
		return PhysicsError::InvalidBody;
	}
	Quat rotation;
	if (!is_finite(pose.position) || !normalized(pose.rotation, rotation)) {
		return PhysicsError::InvalidArgument;
	}
	b3Body_SetTransform(id, b3ToPos(to_b3(pose.position)), to_b3(rotation));
	return PhysicsError::None;
}

PhysicsError PhysicsWorld::move_kinematic(BodyHandle body, const Pose& target)
{
	const b3BodyId id = decode(body);
	if (!valid() || !impl_->owns(id)) {
		return PhysicsError::InvalidBody;
	}
	if (b3Body_GetType(id) != b3_kinematicBody) {
		return PhysicsError::InvalidArgument;
	}
	Pose normalized_target{target.position, {}};
	if (!is_finite(target.position) || !normalized(target.rotation, normalized_target.rotation)) {
		return PhysicsError::InvalidArgument;
	}
	b3Body_SetTargetTransform(id, to_b3_world(normalized_target), impl_->fixed_dt, true);
	return PhysicsError::None;
}

PhysicsResult<Vec3> PhysicsWorld::linear_velocity(BodyHandle body) const
{
	const b3BodyId id = decode(body);
	if (!valid() || !impl_->owns(id)) {
		return PhysicsError::InvalidBody;
	}
	return from_b3(b3Body_GetLinearVelocity(id));
}

PhysicsError PhysicsWorld::set_linear_velocity(BodyHandle body, Vec3 velocity)
{
	const b3BodyId id = decode(body);
	if (!valid() || !impl_->owns(id)) {
		return PhysicsError::InvalidBody;
	}
	if (!is_finite(velocity)) {
		return PhysicsError::InvalidArgument;
	}
	b3Body_SetLinearVelocity(id, to_b3(velocity));
	return PhysicsError::None;
}

PhysicsResult<Vec3> PhysicsWorld::angular_velocity(BodyHandle body) const
{
	const b3BodyId id = decode(body);
	if (!valid() || !impl_->owns(id)) {
		return PhysicsError::InvalidBody;
	}
	return from_b3(b3Body_GetAngularVelocity(id));
}

PhysicsError PhysicsWorld::set_angular_velocity(BodyHandle body, Vec3 velocity)
{
	const b3BodyId id = decode(body);
	if (!valid() || !impl_->owns(id)) {
		return PhysicsError::InvalidBody;
	}
	if (!is_finite(velocity)) {
		return PhysicsError::InvalidArgument;
	}
	b3Body_SetAngularVelocity(id, to_b3(velocity));
	return PhysicsError::None;
}

PhysicsError PhysicsWorld::apply_force(BodyHandle body, Vec3 force)
{
	const b3BodyId id = decode(body);
	if (!valid() || !impl_->owns(id)) {
		return PhysicsError::InvalidBody;
	}
	if (!is_finite(force)) {
		return PhysicsError::InvalidArgument;
	}
	b3Body_ApplyForceToCenter(id, to_b3(force), true);
	return PhysicsError::None;
}

PhysicsError PhysicsWorld::apply_linear_impulse(BodyHandle body, Vec3 impulse)
{
	const b3BodyId id = decode(body);
	if (!valid() || !impl_->owns(id)) {
		return PhysicsError::InvalidBody;
	}
	if (!is_finite(impulse)) {
		return PhysicsError::InvalidArgument;
	}
	b3Body_ApplyLinearImpulseToCenter(id, to_b3(impulse), true);
	return PhysicsError::None;
}

PhysicsResult<float> PhysicsWorld::mass(BodyHandle body) const
{
	const b3BodyId id = decode(body);
	if (!valid() || !impl_->owns(id)) {
		return PhysicsError::InvalidBody;
	}
	return b3Body_GetMass(id);
}

PhysicsResult<bool> PhysicsWorld::is_awake(BodyHandle body) const
{
	const b3BodyId id = decode(body);
	if (!valid() || !impl_->owns(id)) {
		return PhysicsError::InvalidBody;
	}
	return b3Body_IsAwake(id);
}

PhysicsResult<std::uint64_t> PhysicsWorld::user_key(BodyHandle body) const
{
	const b3BodyId id = decode(body);
	if (!valid() || !impl_->owns(id)) {
		return PhysicsError::InvalidBody;
	}
	return impl_->key_of(id);
}

PhysicsError PhysicsWorld::set_gravity(Vec3 gravity)
{
	if (!valid()) {
		return PhysicsError::InvalidWorld;
	}
	if (!is_finite(gravity)) {
		return PhysicsError::InvalidArgument;
	}
	b3World_SetGravity(impl_->world, to_b3(gravity));
	impl_->config.gravity = gravity;
	return PhysicsError::None;
}

std::span<const BodyMoved> PhysicsWorld::moved_bodies() const noexcept
{
	return impl_ ? std::span<const BodyMoved>(impl_->moved) : std::span<const BodyMoved>{};
}

std::span<const ContactBegin> PhysicsWorld::contact_begins() const noexcept
{
	return impl_ ? std::span<const ContactBegin>(impl_->contact_begins) : std::span<const ContactBegin>{};
}

std::span<const ContactEnd> PhysicsWorld::contact_ends() const noexcept
{
	return impl_ ? std::span<const ContactEnd>(impl_->contact_ends) : std::span<const ContactEnd>{};
}

std::span<const ContactHit> PhysicsWorld::contact_hits() const noexcept
{
	return impl_ ? std::span<const ContactHit>(impl_->contact_hits) : std::span<const ContactHit>{};
}

std::span<const SensorOverlap> PhysicsWorld::sensor_begins() const noexcept
{
	return impl_ ? std::span<const SensorOverlap>(impl_->sensor_begins) : std::span<const SensorOverlap>{};
}

std::span<const SensorOverlap> PhysicsWorld::sensor_ends() const noexcept
{
	return impl_ ? std::span<const SensorOverlap>(impl_->sensor_ends) : std::span<const SensorOverlap>{};
}

WorldStats PhysicsWorld::stats() const noexcept
{
	WorldStats stats;
	if (!valid()) {
		return stats;
	}
	const b3Counters counters = b3World_GetCounters(impl_->world);
	stats.body_count = counters.bodyCount;
	stats.collider_count = counters.shapeCount;
	stats.contact_count = counters.contactCount;
	stats.awake_body_count = b3World_GetAwakeBodyCount(impl_->world);
	stats.step_count = impl_->step_count;
	return stats;
}

bool is_valid(const ColliderDesc& desc) noexcept
{
	return std::visit(ShapeValidator{}, desc.shape) && is_finite_non_negative(desc.density) &&
		is_finite_non_negative(desc.friction) && is_finite_non_negative(desc.restitution);
}

int live_world_count() noexcept
{
	return b3GetWorldCount();
}

std::int64_t allocated_bytes() noexcept
{
	return b3GetByteCount();
}

} // namespace engine::physics
