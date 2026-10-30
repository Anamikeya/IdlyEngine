#pragma once

// A game configured by scene data. Entities carry BehaviorComponents; Game::create parses them into typed Flecs
// components on the runtime scene and runs the rules on them, one fixed tick at a time:
//
//   pre_physics()    input, player movement, firing (raycasts, impulses), moving platforms, one command batch
//   ScenePhysics::fixed_update()   the host calls this: sync, step, publish
//   post_physics()   collision and sensor events, damage, score, win and loss
//
// Box3D events are read after the step, so nothing touches the physics world during a callback. Entities are removed
// through the runtime command bus, which takes effect at the next sync. Everything here is driven by ticks and input,
// so a run is reproducible; rendering and the wall clock play no part.
//
// Behaviors, with their keys and defaults:
//   game_mode      time_limit 90
//   player         walk_speed 5, sprint_speed 8.5, jump_speed 5.2, acceleration 60, air_acceleration 12,
//                  look_sensitivity 0.0022, spawn_floor -20 (falling below this returns the player to the start)
//   weapon         damage 25, fire_interval 0.12, magazine 15, reserve 120 (-1: unlimited), reload_time 1.4,
//                  range 200, impulse 60. On the player entity.
//   weapon_view    recoil 0.04, recoil_recovery 12. On a child of the camera: the visible gun.
//   health         max 50
//   target         points 100. Destroying every target wins the game.
//   mover          ax ay az bx by bz (world positions), speed 2. Moves a kinematic body back and forth.
//   impact_damage  min_speed 3, scale 6. Hit events above min_speed hurt: (speed - min_speed) * scale.
//   bonus_zone     points 50. A sensor awarding points once for each dynamic body that enters.
//   pickup         ammo 0, time 0. A sensor the player collects; it is then removed.
//   impact_effect  lifetime 0.25, scale 0.12. Template entity: copies of its mesh mark where shots land.
//
// Physics collision layers used by convention: a ray from the player's weapon skips the player's own body.

#include "engine/gameplay/input.h"

#include <engine/commands/CommandBus.hpp>
#include <engine/physics/scene_physics.h>
#include <engine/scene/Scene.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace engine::gameplay {

// Every behavior type this module understands, with its keys at their default values, for an editor to offer.
[[nodiscard]] const std::vector<BehaviorComponent>& behavior_templates();

enum class GamePhase : std::uint8_t {
	Ready,   // Waiting for start(). The world simulates so stacks settle; the timer does not run.
	Playing,
	Paused,
	Won,
	Lost,
};

[[nodiscard]] const char* to_string(GamePhase phase) noexcept;

// Everything a HUD needs, as plain data. A standalone runtime can draw this without any of the editor.
struct HudState {
	GamePhase phase = GamePhase::Ready;
	int score = 0;
	int targets_remaining = 0;
	int targets_total = 0;
	double time_remaining = 0.0;
	double time_limit = 0.0;
	int ammo = 0;
	int magazine = 0;
	int reserve = 0; // -1: unlimited.
	bool reloading = false;
	double reload_progress = 0.0; // [0, 1]
	double hit_marker = 0.0;      // 1 just after a shot hurt something, fading to 0.
	std::string last_event;       // The latest scoring event, for a short message.
	double last_event_age = 0.0;
	std::string fall_notice;      // Set when the player was returned to the start after falling out of the arena.
};

enum class DamageCause : std::uint8_t { Weapon, Impact };

struct DamageEvent {
	EntityId target = 0;
	double amount = 0.0;
	DamageCause cause = DamageCause::Weapon;
};

// One shot's result this tick.
struct ShotResult {
	EntityId entity = 0; // 0: a miss.
	physics::Vec3 point;
	physics::Vec3 normal;
	bool damaged = false; // The entity has health.
	bool pushed = false;  // The entity has a dynamic body, which took the impulse.
};

class Game {
public:
	// Parses the scene's behaviors. Empty when it has none of `game_mode`, `player`: that scene runs no game.
	// Reports what it could not use in setup_issues().
	static std::unique_ptr<Game> create(Scene& scene, commands::CommandBus& bus, physics::ScenePhysics& physics, std::chrono::nanoseconds fixed_step);
	~Game();
	Game(const Game&) = delete;
	Game& operator=(const Game&) = delete;

	// Input for the next tick. Actions are ignored outside the Playing phase.
	void set_input(const InputFrame& input);
	// Mouse movement in device units, applied at once so the view responds between ticks.
	void look(double dx, double dy);
	// Writes the camera's aim into the scene; the host calls it once per rendered frame, and it is a no-op unless aim changed.
	void update_view();

	void start();  // Ready to Playing.
	void pause();  // Playing to Paused.
	void resume(); // Paused to Playing.

	void pre_physics();
	void post_physics();

	[[nodiscard]] GamePhase phase() const noexcept;
	[[nodiscard]] HudState hud() const;
	[[nodiscard]] EntityId player() const noexcept;
	[[nodiscard]] EntityId camera() const noexcept;
	[[nodiscard]] bool grounded() const noexcept;
	[[nodiscard]] std::uint64_t ticks() const noexcept;
	// Damage dealt and shots fired in the last tick.
	[[nodiscard]] const std::vector<DamageEvent>& damage_events() const noexcept;
	[[nodiscard]] const std::vector<ShotResult>& shots() const noexcept;
	// Behaviors that could not be used, and failed runtime commands.
	[[nodiscard]] const std::vector<std::string>& setup_issues() const noexcept;

private:
	struct Impl;
	explicit Game(std::unique_ptr<Impl> impl) noexcept;
	std::unique_ptr<Impl> impl_;
};

} // namespace engine::gameplay
