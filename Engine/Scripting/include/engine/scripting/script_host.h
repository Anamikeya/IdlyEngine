#pragma once

// What scripts can do to the world they run in (Milestone 6, D5). The runtime binds it as the `engine` table and the
// methods of entity handles; a host implements it over a scene, its physics and its game (script_world.h), and tests
// can implement it over anything. Every call comes from a script, on the runtime's thread, during a call the host
// made. A call that cannot be carried out throws std::runtime_error with a message for the script's author: the runtime
// turns it into a Lua error at the line that made it.

#include <engine/scripting/script_types.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::scripting {

struct LocalTransform {
	Vector3 position;
	Quaternion rotation;
	Vector3 scale{1, 1, 1};
};

struct RayHit {
	EntityId entity = 0; // 0: something that is no entity, such as a piece of debris.
	Vector3 point;
	Vector3 normal;
	double distance = 0;
};

class ScriptHost {
public:
	virtual ~ScriptHost() = default;

	// ---- Entities. Those that do not exist throw, except exists() ----
	[[nodiscard]] virtual bool exists(EntityId entity) const = 0;
	[[nodiscard]] virtual std::string name(EntityId entity) const = 0;
	// The first entity, by ID, with this name; 0 when there is none.
	[[nodiscard]] virtual EntityId find(std::string_view name) const = 0;
	[[nodiscard]] virtual EntityId parent(EntityId entity) const = 0;
	[[nodiscard]] virtual std::vector<EntityId> children(EntityId entity) const = 0; // Ascending.
	// Local to the parent. A change shows at once to scripts and reaches the scene when the host next applies them.
	[[nodiscard]] virtual LocalTransform local(EntityId entity) const = 0;
	virtual void set_local(EntityId entity, const LocalTransform& transform) = 0;
	[[nodiscard]] virtual Vector3 world_position(EntityId entity) const = 0;
	[[nodiscard]] virtual Quaternion world_rotation(EntityId entity) const = 0;
	virtual void set_world_position(EntityId entity, const Vector3& position) = 0;
	// A copy of the entity and what is under it, at the root, placed there. Its scripts start on the next tick.
	virtual EntityId spawn(EntityId original, const Vector3& position, const Quaternion& rotation) = 0;
	virtual void destroy(EntityId entity) = 0;

	// ---- Physics ----
	[[nodiscard]] virtual std::optional<RayHit> raycast(const Vector3& origin, const Vector3& direction, double distance, EntityId ignore) const = 0;
	// False when the entity has no body that moves.
	virtual bool apply_impulse(EntityId entity, const Vector3& impulse, const Vector3& point) = 0;
	[[nodiscard]] virtual std::optional<Vector3> velocity(EntityId entity) const = 0;
	virtual bool set_velocity(EntityId entity, const Vector3& velocity) = 0;
	// Its body is moved by physics (not static, not kinematic).
	[[nodiscard]] virtual bool is_dynamic(EntityId entity) const = 0;

	// ---- Input: actions by name ("Fire", "Interact"), never devices. Unknown names throw ----
	[[nodiscard]] virtual bool held(std::string_view action) const = 0;
	[[nodiscard]] virtual bool pressed(std::string_view action) const = 0;

	// ---- Animation, effects and ragdolls: visual, carried out by the host after the call ----
	virtual void play_clip(EntityId entity, std::string_view clip, double blend_seconds) = 0;
	// Fires the effect of the entity's particle emitter at a point along a direction.
	virtual void fire_effect(EntityId entity, const Vector3& point, const Vector3& direction, double strength) = 0;
	virtual void ragdoll(EntityId entity) = 0;

	// ---- The game's rules, when the scene runs a game (game_mode or player). Without one they throw ----
	[[nodiscard]] virtual bool has_game() const = 0;
	[[nodiscard]] virtual int score() const = 0;
	virtual void add_score(int points, std::string_view message) = 0;
	virtual void announce(std::string_view message) = 0;
	// Hurts an entity with health as a hit would; its death is reported to scripts like any other.
	virtual void damage(EntityId entity, double amount, const Vector3& point) = 0;
	[[nodiscard]] virtual std::optional<double> health(EntityId entity) const = 0;
	[[nodiscard]] virtual double time_left() const = 0;
	virtual void add_time(double seconds) = 0;
	virtual void give_ammo(int rounds) = 0;
	[[nodiscard]] virtual int targets_left() const = 0;
	[[nodiscard]] virtual EntityId player() const = 0;
	[[nodiscard]] virtual std::string phase() const = 0; // "ready", "playing", "paused", "won", "lost".
	virtual void win() = 0;
	virtual void lose() = 0;

	// ---- Time: fixed ticks since the run began ----
	[[nodiscard]] virtual std::uint64_t ticks() const = 0;
	[[nodiscard]] virtual double step_seconds() const = 0;
};

} // namespace engine::scripting
