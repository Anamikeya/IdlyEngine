#pragma once

// Scripts running in a game (Milestone 6, D2): one runtime for a play session's scene, and an instance for each entity
// whose Script component names a script. The host calls it at two fixed points of each tick, after the game's own:
//
//   pre_physics()    start() for instances that have not started, then tick(dt)
//   physics, animation, Game::post_physics()
//   post_physics()   on_hit, on_trigger_enter and _exit, on_damage, on_death, on_clip_event, then late_tick(dt)
//
// What scripts change in the scene is gathered and applied at the end of each of the two, through the same command
// bus as gameplay, so physics and the next frame see it, and Stop throws it away with the play session's copy of the
// scene. Reads see the changes at once. Nothing here reads a clock or a frame: a run repeats exactly.

#include <engine/commands/CommandBus.hpp>
#include <engine/gameplay/game.h>
#include <engine/physics/scene_physics.h>
#include <engine/scripting/scene_values.h>
#include <engine/scripting/script_host.h>
#include <engine/scripting/script_runtime.h>

#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace engine::scripting {

// A clip a script asked an entity to play, for the host's animation system.
struct ClipRequest {
	EntityId entity = 0;
	std::string clip;
	double blend_seconds = 0.2;
};

class ScriptWorld final : public ScriptHost {
public:
	struct Setup {
		ScriptLoader loader;
		AssetNamer names;          // A scene's script reference as the package name to load it by.
		RuntimeLimits limits;
		std::uint64_t random_seed = 1;
	};
	// Null when no entity of the scene has a script to run. `game` may be null: a scene without one runs scripts too.
	static std::unique_ptr<ScriptWorld> create(Scene& scene, commands::CommandBus& bus, physics::ScenePhysics& physics,
		gameplay::Game* game, std::chrono::nanoseconds fixed_step, Setup setup);
	~ScriptWorld() override;
	ScriptWorld(const ScriptWorld&) = delete;
	ScriptWorld& operator=(const ScriptWorld&) = delete;

	// Input for the next tick; presses are kept until a tick has seen them. Simulate gives none.
	void set_input(const gameplay::InputFrame& input);
	void pre_physics();
	void post_physics(std::span<const gameplay::AnimationNotice> notices);
	// on_stop() on every running instance, before the session ends.
	void stop();

	// What scripts asked of animation, effects and ragdolls since the last pre_physics(), for the host to carry out.
	[[nodiscard]] const std::vector<ClipRequest>& clip_requests() const noexcept { return clips_; }
	[[nodiscard]] const std::vector<gameplay::EffectEvent>& effect_requests() const noexcept { return effects_; }
	[[nodiscard]] const std::vector<EntityId>& ragdoll_requests() const noexcept { return ragdolls_; }
	// Scene commands that failed, such as a destroy of what the game had removed already.
	std::vector<std::string> take_issues();
	ScriptRuntime& runtime() noexcept { return *runtime_; }

	// ---- ScriptHost ----
	bool exists(EntityId entity) const override;
	std::string name(EntityId entity) const override;
	EntityId find(std::string_view name) const override;
	EntityId parent(EntityId entity) const override;
	std::vector<EntityId> children(EntityId entity) const override;
	LocalTransform local(EntityId entity) const override;
	void set_local(EntityId entity, const LocalTransform& transform) override;
	Vector3 world_position(EntityId entity) const override;
	Quaternion world_rotation(EntityId entity) const override;
	void set_world_position(EntityId entity, const Vector3& position) override;
	EntityId spawn(EntityId original, const Vector3& position, const Quaternion& rotation) override;
	void destroy(EntityId entity) override;
	std::optional<RayHit> raycast(const Vector3& origin, const Vector3& direction, double distance, EntityId ignore) const override;
	bool apply_impulse(EntityId entity, const Vector3& impulse, const Vector3& point) override;
	std::optional<Vector3> velocity(EntityId entity) const override;
	bool set_velocity(EntityId entity, const Vector3& velocity) override;
	bool is_dynamic(EntityId entity) const override;
	bool held(std::string_view action) const override;
	bool pressed(std::string_view action) const override;
	void play_clip(EntityId entity, std::string_view clip, double blend_seconds) override;
	void fire_effect(EntityId entity, const Vector3& point, const Vector3& direction, double strength) override;
	void ragdoll(EntityId entity) override;
	bool has_game() const override { return game_ != nullptr; }
	int score() const override;
	void add_score(int points, std::string_view message) override;
	void announce(std::string_view message) override;
	void damage(EntityId entity, double amount, const Vector3& point) override;
	std::optional<double> health(EntityId entity) const override;
	double time_left() const override;
	void add_time(double seconds) override;
	void give_ammo(int rounds) override;
	int targets_left() const override;
	std::vector<EntityId> targets() const override;
	EntityId player() const override;
	std::string phase() const override;
	void win() override;
	void lose() override;
	std::uint64_t ticks() const override { return ticks_; }
	double step_seconds() const override { return dt_; }

private:
	ScriptWorld(Scene& scene, commands::CommandBus& bus, physics::ScenePhysics& physics, gameplay::Game* game, double dt, Setup setup);
	gameplay::Game& game() const;
	// Instances for the scripted entities that have none yet, which start on the next pre_physics().
	void attach_new();
	// Applies what scripts changed. Safe during a call (spawn does it).
	void apply();
	// Forgets the instances whose entities are gone. Only between calls: never while one of them runs.
	void forget_gone();
	std::optional<EntityRecord> record(EntityId entity) const;
	// Its local transform as scripts see it, without copying the rest of the entity.
	Transform transform_of(EntityId entity) const;
	std::array<double, 16> world_matrix(EntityId entity) const;

	Scene& scene_;
	commands::CommandBus& bus_;
	physics::ScenePhysics& physics_;
	gameplay::Game* game_;
	double dt_;
	Setup setup_;
	std::unique_ptr<ScriptRuntime> runtime_;
	std::uint64_t ticks_ = 0;
	gameplay::InputFrame input_;
	std::map<EntityId, Transform> moved_;  // Local transforms scripts set, not applied yet.
	std::set<EntityId> destroyed_;         // Not applied yet.
	std::set<EntityId> waiting_;           // Attached, not started.
	std::vector<ClipRequest> clips_;
	std::vector<gameplay::EffectEvent> effects_;
	std::vector<EntityId> ragdolls_;
	std::vector<std::string> issues_;
};

} // namespace engine::scripting
