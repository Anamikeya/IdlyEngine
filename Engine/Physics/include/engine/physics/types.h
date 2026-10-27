#pragma once

// Engine-facing physics value types. Box3D types never appear in public headers.
// Conventions match engine::core: right-handed, meters, +Y up, quaternion (x, y, z, w).

#include <cstdint>
#include <utility>
#include <variant>

namespace engine::physics {

struct Vec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;

	friend bool operator==(const Vec3&, const Vec3&) = default;
};

struct Quat {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	float w = 1.0f;

	friend bool operator==(const Quat&, const Quat&) = default;
};

struct Pose {
	Vec3 position;
	Quat rotation;

	friend bool operator==(const Pose&, const Pose&) = default;
};

enum class PhysicsError : std::uint8_t {
	None = 0,
	InvalidConfig,
	WorldLimitReached,
	InvalidWorld,
	InvalidBody,
	InvalidCollider,
	InvalidArgument,
};

const char* to_string(PhysicsError error) noexcept;

// Value-or-error. Kept local to physics until engine::core publishes a shared Result type.
template <typename T>
class PhysicsResult {
public:
	PhysicsResult(T value) : value_(std::move(value)) {}
	PhysicsResult(PhysicsError error) : value_(), error_(error) {}

	[[nodiscard]] bool ok() const noexcept { return error_ == PhysicsError::None; }
	explicit operator bool() const noexcept { return ok(); }
	[[nodiscard]] PhysicsError error() const noexcept { return error_; }
	[[nodiscard]] T& value() & noexcept { return value_; }
	[[nodiscard]] const T& value() const& noexcept { return value_; }
	// By value: a reference into a temporary result would dangle at the end of the statement.
	[[nodiscard]] T value() && noexcept { return std::move(value_); }

private:
	T value_;
	PhysicsError error_ = PhysicsError::None;
};

// Opaque generational handles. A handle stays invalid forever once its object is destroyed,
// even if the underlying slot is reused. Value 0 is the null handle.
struct BodyHandle {
	std::uint64_t value = 0;

	[[nodiscard]] bool is_null() const noexcept { return value == 0; }
	friend bool operator==(const BodyHandle&, const BodyHandle&) = default;
};

struct ColliderHandle {
	std::uint64_t value = 0;

	[[nodiscard]] bool is_null() const noexcept { return value == 0; }
	friend bool operator==(const ColliderHandle&, const ColliderHandle&) = default;
};

enum class BodyType : std::uint8_t {
	Static,
	Kinematic,
	Dynamic,
};

struct BodyDesc {
	BodyType type = BodyType::Dynamic;
	Pose pose;
	Vec3 linear_velocity;
	Vec3 angular_velocity;
	float linear_damping = 0.0f;
	float angular_damping = 0.0f;
	float gravity_scale = 1.0f;
	bool enable_sleep = true;
	bool is_bullet = false;
	// Opaque engine key echoed in events (e.g. a scene stable id). Physics never interprets it.
	std::uint64_t user_key = 0;
};

struct SphereShape {
	float radius = 0.5f;
	Vec3 center;
};

struct BoxShape {
	Vec3 half_extents{0.5f, 0.5f, 0.5f};
	Pose local_pose;
};

// Capsule between two local points.
struct CapsuleShape {
	Vec3 point_a{0.0f, -0.5f, 0.0f};
	Vec3 point_b{0.0f, 0.5f, 0.0f};
	float radius = 0.25f;
};

using ShapeDesc = std::variant<SphereShape, BoxShape, CapsuleShape>;

struct CollisionFilter {
	std::uint64_t category_bits = ~std::uint64_t{0};
	std::uint64_t mask_bits = ~std::uint64_t{0};
	// Same positive group always collides, same negative group never collides.
	int group = 0;
};

struct ColliderDesc {
	ShapeDesc shape = SphereShape{};
	float density = 1000.0f; // kg/m^3
	float friction = 0.6f;
	float restitution = 0.0f;
	// Sensors detect overlap but never collide. Sensors cannot be added to static bodies' compounds.
	bool is_sensor = false;
	// Required on both the sensor and any collider that should be detected by sensors.
	bool enable_sensor_events = false;
	bool enable_contact_events = false;
	bool enable_hit_events = false;
	CollisionFilter filter;
};

// Events below are produced by PhysicsWorld::step()/advance() and are valid until the next call.

struct BodyMoved {
	BodyHandle body;
	std::uint64_t user_key = 0;
	Pose pose;
	bool fell_asleep = false;
};

struct ContactBegin {
	ColliderHandle collider_a;
	ColliderHandle collider_b;
	BodyHandle body_a;
	BodyHandle body_b;
	std::uint64_t user_key_a = 0;
	std::uint64_t user_key_b = 0;
};

// Either collider may already be destroyed: handles are kept, bodies/keys are null/0 in that case.
struct ContactEnd {
	ColliderHandle collider_a;
	ColliderHandle collider_b;
	BodyHandle body_a;
	BodyHandle body_b;
	std::uint64_t user_key_a = 0;
	std::uint64_t user_key_b = 0;
};

struct ContactHit {
	ColliderHandle collider_a;
	ColliderHandle collider_b;
	BodyHandle body_a;
	BodyHandle body_b;
	std::uint64_t user_key_a = 0;
	std::uint64_t user_key_b = 0;
	Vec3 point;
	Vec3 normal;
	float approach_speed = 0.0f;
};

// Same destruction rules as ContactEnd for the end event.
struct SensorOverlap {
	ColliderHandle sensor;
	ColliderHandle visitor;
	BodyHandle sensor_body;
	BodyHandle visitor_body;
	std::uint64_t sensor_user_key = 0;
	std::uint64_t visitor_user_key = 0;
};

} // namespace engine::physics
