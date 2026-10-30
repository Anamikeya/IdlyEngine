#pragma once

// Bridge between engine::physics and the engine::core scene (stable EntityIds on Flecs).
// Contracts C-1..C-4 in docs/integration/m2-contracts.md define the ownership rules implemented here.
//
// Authoring lives in the scene: RigidBodyComponent and ColliderComponent on each entity, edited through
// commands (undoable, saved with the scene). sync() builds bodies from them; an entity with colliders but
// no body is a static collider. Physics never mutates managed scene components directly: poses reach the
// scene only as commands::SetTransform batches executed on a CommandBus the host supplies.

#include "engine/physics/physics_world.h"

#include <engine/commands/CommandBus.hpp>
#include <engine/scene/Scene.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace engine::physics {

// Body settings as built from the scene. Initial velocities apply whenever the body is (re)built.
struct RigidBody {
	BodyType type = BodyType::Dynamic;
	Vec3 linear_velocity;
	Vec3 angular_velocity;
	float linear_damping = 0.0f;
	float angular_damping = 0.0f;
	float gravity_scale = 1.0f;
	bool enable_sleep = true;
	bool is_bullet = false;
	bool lock_rotation = false;

	friend bool operator==(const RigidBody&, const RigidBody&) = default;
};

// Collider in the entity's unscaled local frame. World scale is baked in when the body is built.
using Collider = ColliderDesc;

// Conversions from the scene's authoring components. No component means a static body.
[[nodiscard]] RigidBody to_rigid_body(const std::optional<RigidBodyComponent>& body) noexcept;
[[nodiscard]] Collider to_collider(const ColliderComponent& collider) noexcept;

struct CollisionEvent {
	enum class Kind : std::uint8_t {
		ContactBegin,
		ContactEnd,
		Hit,
		SensorBegin,
		SensorEnd,
	};

	Kind kind = Kind::ContactBegin;
	// Sensor events: a is the sensor entity, b the visitor. 0 when the collider was destroyed before the event.
	EntityId a = 0;
	EntityId b = 0;
	// Hit events only.
	Vec3 point;
	Vec3 normal;
	float approach_speed = 0.0f;
};

// Which colliders a scene raycast can hit. Sensors never block a ray.
struct RaycastFilter {
	std::uint64_t category = ~std::uint64_t{0};
	std::uint64_t mask = ~std::uint64_t{0};
	// This entity's body is skipped, for example the shooter.
	EntityId ignore = 0;
};

struct RaycastHit {
	EntityId entity = 0;
	Vec3 point;
	Vec3 normal;
	float distance = 0.0f;
};

struct BodyState {
	EntityId entity = 0;
	BodyType type = BodyType::Dynamic;
	// World space, unscaled.
	Pose pose;
	Vec3 linear_velocity;
	Vec3 angular_velocity;
	float mass = 0.0f;
	bool awake = false;
};

// An authored entity whose body could not be built, with the reason. Cleared once the cause is fixed.
struct EntityIssue {
	EntityId entity = 0;
	std::string message;
};

struct TickReport {
	bool stepped = false;
	// Bodies moved to follow a scene edit (teleport, or kinematic target while simulating).
	std::size_t scene_updates = 0;
	// Bodies (re)built this tick.
	std::size_t rebuilt = 0;
	// Scene transforms written from dynamic bodies, and the transactions used to write them.
	std::size_t published = 0;
	std::size_t transactions = 0;
};

struct PhysicsDiagnostics {
	bool simulation_enabled = false;
	std::uint64_t ticks = 0;
	std::uint64_t simulated_ticks = 0;
	WorldStats world;
	std::size_t authored_entities = 0;
	std::size_t active_bodies = 0;
	TickReport last_tick;
	std::vector<EntityIssue> issues;
};

enum class DebugCategory : std::uint8_t {
	Static,
	Kinematic,
	DynamicAwake,
	DynamicAsleep,
	Sensor,
};

struct DebugLine {
	Vec3 a;
	Vec3 b;
	EntityId entity = 0;
	DebugCategory category = DebugCategory::Static;
};

struct DebugGeometryOptions {
	// Segments per full circle, clamped to [4, 256]. Odd values are rounded up for capsule half arcs.
	int circle_segments = 16;
	bool include_static = true;
	bool include_sensors = true;
};

// World-space line list. Renderer-agnostic CPU data; drawing it is the renderer's job (C-4).
struct DebugGeometry {
	std::vector<DebugLine> lines;
};

// Linear RGBA suggestion per category. Renderers are free to use their own palette.
[[nodiscard]] std::array<float, 4> default_color(DebugCategory category) noexcept;

// Owns one PhysicsWorld whose bodies mirror the physics components of one scene, keyed by stable EntityId.
// Move-only. Every call must happen on the scene owner thread.
class ScenePhysics {
public:
	static Result<ScenePhysics> create(const WorldConfig& config = {});

	~ScenePhysics();
	ScenePhysics(ScenePhysics&& other) noexcept;
	ScenePhysics& operator=(ScenePhysics&& other) noexcept;
	ScenePhysics(const ScenePhysics&) = delete;
	ScenePhysics& operator=(const ScenePhysics&) = delete;

	// ---- Authoring as last synced from the scene (edit it through scene commands) ----

	[[nodiscard]] std::optional<RigidBody> rigid_body(EntityId entity) const;
	// Unscaled, as authored. Valid until the next sync.
	[[nodiscard]] std::span<const Collider> colliders(EntityId entity) const;
	// Entities with a body or colliders at the last sync, ascending.
	[[nodiscard]] std::vector<EntityId> authored_entities() const;

	// ---- Simulation ----

	// Disabled by default (edit mode): bodies follow the scene and the world is not stepped.
	// Disabling pauses: velocities are kept. Use reset_simulation to restart from authored state.
	void set_simulation_enabled(bool enabled) noexcept;
	[[nodiscard]] bool simulation_enabled() const noexcept;
	// Rebuilds every body at its current scene pose with authored initial velocities.
	Result<void> reset_simulation(const Scene& scene);

	// One fixed tick (C-3): sync(scene), step() when enabled, publish(scene, bus, context).
	// delta must equal WorldConfig::fixed_timestep.
	Result<TickReport> fixed_update(std::chrono::nanoseconds delta, Scene& scene, commands::CommandBus& bus,
		const commands::Context& context);

	// The pieces of fixed_update, for hosts that need to interleave their own work.
	// Brings bodies in line with the scene: reads body and collider components, then builds, rebuilds,
	// destroys and teleports bodies. Never writes the scene.
	Result<void> sync(const Scene& scene);
	// Steps the world once if simulation is enabled, then collects events and dynamic poses to publish.
	Result<void> step();
	// The SetTransform batch for the poses collected by the last step(), ascending by entity.
	[[nodiscard]] std::vector<commands::SetTransform> pending_transforms(const Scene& scene) const;
	// Executes pending_transforms() on bus in transactions of at most CommandBus::max_transaction,
	// then records the resulting scene matrices so the next sync does not treat them as edits.
	Result<std::size_t> publish(Scene& scene, commands::CommandBus& bus, const commands::Context& context);
	// For hosts that wrote pending_transforms() through another sink: records the scene matrices.
	void mark_published(const Scene& scene);

	// ---- Queries ----

	[[nodiscard]] std::optional<BodyState> body_state(EntityId entity) const;
	// Closest collider along a unit direction. Sees the world as of the last sync and step. Empty on a miss, or for
	// a direction that is not unit length.
	[[nodiscard]] std::optional<RaycastHit> raycast(Vec3 origin, Vec3 direction, float max_distance, const RaycastFilter& filter = {}) const;

	// ---- Runtime forces. These act on the live body and keep no scene state, so they fill no undo history. ----

	// N*s at a world point. False when the entity has no dynamic body (a static wall does not move).
	bool apply_impulse(EntityId entity, Vec3 impulse, Vec3 world_point);
	// False when the entity has no body, or a static one.
	bool set_linear_velocity(EntityId entity, Vec3 velocity);
	// Events of the last step(). Valid until the next step(), fixed_update or reset_simulation.
	[[nodiscard]] std::span<const CollisionEvent> events() const noexcept;
	[[nodiscard]] PhysicsDiagnostics diagnostics() const;
	[[nodiscard]] DebugGeometry debug_geometry(const DebugGeometryOptions& options = {}) const;
	[[nodiscard]] const WorldConfig& config() const noexcept;

private:
	struct Impl;
	explicit ScenePhysics(std::unique_ptr<Impl> impl) noexcept;
	std::unique_ptr<Impl> impl_;
};

} // namespace engine::physics
