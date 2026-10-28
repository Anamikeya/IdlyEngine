#pragma once

#include "engine/physics/types.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>

namespace engine::physics {

struct WorldConfig {
	Vec3 gravity{0.0f, -9.81f, 0.0f};
	// Simulation tick. Integer nanoseconds to match the engine::core fixed-step scheduler.
	std::chrono::nanoseconds fixed_timestep{16'666'667};
	int sub_steps = 4;
	// 1 keeps the simulation single threaded. >1 lets Box3D start its own worker threads.
	int worker_count = 1;
	// Upper bound on ticks per advance() call; excess time is dropped and reported.
	int max_steps_per_advance = 8;
	bool enable_sleep = true;
	bool enable_continuous = true;
};

struct AdvanceReport {
	int steps = 0;
	std::chrono::nanoseconds dropped{0};
	// Fraction of a tick left in the accumulator, for render interpolation. [0, 1)
	float interpolation_alpha = 0.0f;
};

struct WorldStats {
	int body_count = 0;
	int collider_count = 0;
	int contact_count = 0;
	int awake_body_count = 0;
	std::uint64_t step_count = 0;
};

// Owns one Box3D world. Move-only; destroying it destroys every body and collider it owns.
// Not thread safe: call from the thread that owns the simulation tick.
class PhysicsWorld {
public:
	static PhysicsResult<PhysicsWorld> create(const WorldConfig& config = {});

	PhysicsWorld() noexcept;
	~PhysicsWorld();
	PhysicsWorld(PhysicsWorld&& other) noexcept;
	PhysicsWorld& operator=(PhysicsWorld&& other) noexcept;
	PhysicsWorld(const PhysicsWorld&) = delete;
	PhysicsWorld& operator=(const PhysicsWorld&) = delete;

	[[nodiscard]] bool valid() const noexcept;
	[[nodiscard]] const WorldConfig& config() const noexcept;
	// Destroys the world early. Idempotent.
	void reset() noexcept;

	// Exactly one fixed tick. Use this when engine::core's Application drives on_fixed_update.
	PhysicsError step();
	// Accumulates wall time and runs as many fixed ticks as fit, for standalone hosts.
	AdvanceReport advance(std::chrono::nanoseconds elapsed);

	PhysicsResult<BodyHandle> create_body(const BodyDesc& desc);
	// Destroys the body and all of its colliders.
	PhysicsError destroy_body(BodyHandle body);
	[[nodiscard]] bool is_valid(BodyHandle body) const noexcept;

	PhysicsResult<ColliderHandle> add_collider(BodyHandle body, const ColliderDesc& desc);
	PhysicsError destroy_collider(ColliderHandle collider);
	[[nodiscard]] bool is_valid(ColliderHandle collider) const noexcept;
	[[nodiscard]] BodyHandle collider_body(ColliderHandle collider) const noexcept;

	[[nodiscard]] PhysicsResult<Pose> body_pose(BodyHandle body) const;
	// Teleport. Wakes the body and breaks existing contacts; prefer move_kinematic for animated bodies.
	PhysicsError set_body_pose(BodyHandle body, const Pose& pose);
	// Sets velocity on a kinematic body so it reaches target by the end of the next tick.
	PhysicsError move_kinematic(BodyHandle body, const Pose& target);
	[[nodiscard]] PhysicsResult<Vec3> linear_velocity(BodyHandle body) const;
	PhysicsError set_linear_velocity(BodyHandle body, Vec3 velocity);
	[[nodiscard]] PhysicsResult<Vec3> angular_velocity(BodyHandle body) const;
	PhysicsError set_angular_velocity(BodyHandle body, Vec3 velocity);
	PhysicsError apply_force(BodyHandle body, Vec3 force);
	PhysicsError apply_linear_impulse(BodyHandle body, Vec3 impulse);
	[[nodiscard]] PhysicsResult<float> mass(BodyHandle body) const;
	[[nodiscard]] PhysicsResult<bool> is_awake(BodyHandle body) const;
	[[nodiscard]] PhysicsResult<std::uint64_t> user_key(BodyHandle body) const;
	PhysicsError set_gravity(Vec3 gravity);

	// Results of the most recent step()/advance(). Spans stay valid until the next step/advance/reset.
	// Moved bodies are de-duplicated across an advance(); each entry carries the latest pose.
	[[nodiscard]] std::span<const BodyMoved> moved_bodies() const noexcept;
	[[nodiscard]] std::span<const ContactBegin> contact_begins() const noexcept;
	[[nodiscard]] std::span<const ContactEnd> contact_ends() const noexcept;
	[[nodiscard]] std::span<const ContactHit> contact_hits() const noexcept;
	[[nodiscard]] std::span<const SensorOverlap> sensor_begins() const noexcept;
	[[nodiscard]] std::span<const SensorOverlap> sensor_ends() const noexcept;

	[[nodiscard]] WorldStats stats() const noexcept;

private:
	struct Impl;
	explicit PhysicsWorld(std::unique_ptr<Impl> impl) noexcept;
	std::unique_ptr<Impl> impl_;
};

// Number of live Box3D worlds in the process, for leak checks.
int live_world_count() noexcept;
// Bytes currently allocated by Box3D across all worlds, for leak checks.
std::int64_t allocated_bytes() noexcept;

} // namespace engine::physics
