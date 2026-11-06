#include "engine/gameplay/game.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <utility>

namespace engine::gameplay {
namespace {

using physics::ScenePhysics;
using PVec3 = physics::Vec3;

const commands::Context game_context{"gameplay", 3};

double value_of(const BehaviorComponent& behavior, const char* key, double fallback)
{
	for (const auto& [name, value] : behavior.values) {
		if (name == key) {
			return value;
		}
	}
	return fallback;
}

const BehaviorComponent* find_behavior(const EntityRecord& record, const char* type)
{
	for (const auto& behavior : record.components.behaviors) {
		if (behavior.type == type) {
			return &behavior;
		}
	}
	return nullptr;
}

Quaternion yaw_pitch(double yaw, double pitch)
{
	// Yaw around +Y, then pitch around the turned +X.
	const double cy = std::cos(yaw / 2), sy = std::sin(yaw / 2), cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
	return {cy * sp, sy * cp, -sy * sp, cy * cp};
}

PVec3 to_float(const Vec3& v)
{
	return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

// ---- Flecs components, on the runtime scene's entities. The scene never saves these. ----

struct PlayerController {
	double walk_speed, sprint_speed, jump_speed, acceleration, air_acceleration, look_sensitivity, spawn_floor;
};
struct WeaponComponent {
	double damage, fire_interval, reload_time, range, impulse;
	int magazine_size, ammo, reserve;
	double cooldown = 0, reloading = 0; // Seconds left.
};
struct WeaponView {
	Transform rest;
	double recoil, recovery;
	double kick = 0;
};
struct HealthComponent {
	double max, current;
};
struct TargetComponent {
	int points;
};
struct MoverComponent {
	Vec3 a, b;
	double speed;
	double travelled = 0; // Along the back-and-forth path, in meters.
};
struct ImpactDamage {
	double min_speed, scale;
};
struct BonusZone {
	int points;
	std::vector<EntityId> scored;
};
struct Pickup {
	int ammo;
	double time;
};
struct Flash {
	Vec3 original;
	double remaining;
};
struct Effect {
	double remaining;
};
// Patrols between two points (D: slice 5). The pose is the host's; this moves the entity and asks for animations.
struct EnemyComponent {
	double walk_speed, pause, turn_rate, flinch_time;
	Vec3 a, b;
	double yaw = 0;
	bool to_b = true;
	double waiting = 0, flinching = 0;
	AnimationCue playing = AnimationCue::Walk;
};
struct BreakableComponent {
	int shards;
	double speed;
};
// Killed into a ragdoll: still in the scene, no longer a living target.
struct Fallen {};
// The game's rules, as a Flecs singleton.
struct GameState {
	GamePhase phase = GamePhase::Ready;
	double time_limit = 90, elapsed = 0;
	int score = 0, targets_total = 0, targets_remaining = 0;
};

} // namespace

const std::vector<BehaviorComponent>& behavior_templates()
{
	static const std::vector<BehaviorComponent> templates{
		{"game_mode", {{"time_limit", 90}}},
		{"player", {{"walk_speed", 5}, {"sprint_speed", 8.5}, {"jump_speed", 5.2}, {"acceleration", 60}, {"air_acceleration", 12}, {"look_sensitivity", 0.0022}, {"spawn_floor", -20}}},
		{"weapon", {{"damage", 25}, {"fire_interval", 0.12}, {"magazine", 15}, {"reserve", 120}, {"reload_time", 1.4}, {"range", 200}, {"impulse", 60}}},
		{"weapon_view", {{"recoil", 0.04}, {"recoil_recovery", 12}}},
		{"health", {{"max", 50}}},
		{"target", {{"points", 100}}},
		{"mover", {{"ax", 0}, {"ay", 0}, {"az", 0}, {"bx", 0}, {"by", 0}, {"bz", 0}, {"speed", 2}}},
		{"impact_damage", {{"min_speed", 3}, {"scale", 6}}},
		{"bonus_zone", {{"points", 50}}},
		{"pickup", {{"ammo", 0}, {"time", 0}}},
		{"impact_effect", {{"lifetime", 0.25}, {"scale", 0.12}}},
		{"enemy", {{"walk_speed", 1.05}, {"ax", 0}, {"az", 0}, {"bx", 0}, {"bz", 0}, {"pause", 1.5}, {"turn_rate", 4}, {"flinch_time", 0.45}}},
		{"muzzle_effect", {}},
		{"footstep_effect", {}},
		{"dust_effect", {{"min_speed", 3}, {"cooldown", 0.25}}},
		{"breakable", {{"shards", 10}, {"speed", 2.5}}},
		{"shard", {{"lifetime", 6}, {"density", 2500}}},
	};
	return templates;
}

const char* to_string(GamePhase phase) noexcept
{
	switch (phase) {
	case GamePhase::Ready:
		return "ready";
	case GamePhase::Playing:
		return "playing";
	case GamePhase::Paused:
		return "paused";
	case GamePhase::Won:
		return "won";
	case GamePhase::Lost:
		return "lost";
	}
	return "unknown";
}

InputMap InputMap::shooter_defaults()
{
	InputMap map;
	map.bind(code::w, Action::MoveForward);
	map.bind(code::s, Action::MoveBack);
	map.bind(code::a, Action::MoveLeft);
	map.bind(code::d, Action::MoveRight);
	map.bind(code::space, Action::Jump);
	map.bind(code::left_shift, Action::Sprint);
	map.bind(code::mouse_left, Action::Fire);
	map.bind(code::r, Action::Reload);
	map.bind(code::e, Action::Interact);
	return map;
}

void InputMap::bind(int device_code, Action action)
{
	bindings_[device_code] = action;
}

void InputMap::set(int device_code, bool down)
{
	const auto binding = bindings_.find(device_code);
	if (binding == bindings_.end()) {
		return;
	}
	bool& state = down_[device_code];
	if (down && !state) {
		pressed_[static_cast<std::size_t>(binding->second)] = true;
	}
	state = down;
}

void InputMap::release_all()
{
	down_.clear();
	pressed_ = {};
}

InputFrame InputMap::take()
{
	InputFrame frame;
	frame.pressed = std::exchange(pressed_, {});
	for (const auto& [device_code, down] : down_) {
		if (down) {
			frame.held[static_cast<std::size_t>(bindings_.at(device_code))] = true;
		}
	}
	return frame;
}

struct Game::Impl {
	Scene& scene;
	commands::CommandBus& bus;
	ScenePhysics& physics;
	flecs::world& world;
	double dt;
	EntityId player = 0, camera = 0, weapon_view = 0, effect_template = 0;
	EntityId muzzle_template = 0, footstep_template = 0, dust_template = 0, shard_template = 0;
	double dust_min_speed = 3, dust_cooldown = 0.25, shard_lifetime = 6, shard_density = 2500;
	std::map<EntityId, double> dust_wait; // Seconds until a body may raise dust again.
	std::vector<AnimationRequest> animations;
	std::vector<RagdollRequest> ragdolls;
	GameCounters counters;
	std::uint64_t random_state = 0x5eed'1234'abcd'0001;
	Transform player_start;
	Transform camera_rest;
	double yaw = 0, pitch = 0;
	bool view_dirty = true;
	bool grounded = false;
	double jump_cooldown = 0;
	std::uint64_t ticks = 0;
	InputFrame input;
	double effect_lifetime = 0.25, effect_scale = 0.12;
	double hit_marker = 0;
	std::string last_event;
	double last_event_age = 1e9;
	std::string fall_notice;
	std::vector<DamageEvent> damage;
	std::vector<DamageEvent> queued; // From scripts, for the next apply_damage().
	std::vector<DamageEvent> hurt;   // What reached health in the last post_physics().
	std::vector<EntityId> deaths;
	std::vector<ShotResult> shots;
	std::vector<EffectEvent> effects;
	std::vector<std::string> issues;
	std::vector<commands::Command> batch;
	std::set<EntityId> removing;

	Impl(Scene& s, commands::CommandBus& b, ScenePhysics& p, double step)
		: scene(s), bus(b), physics(p), world(s.ecs().native()), dt(step)
	{
	}

	flecs::entity native(EntityId id) const { return world.entity(scene.native(id)); }
	// splitmix64, the same stream every run: what breaks scatters the same way given the same shots.
	double random01()
	{
		std::uint64_t z = (random_state += 0x9e37'79b9'7f4a'7c15ull);
		z = (z ^ (z >> 30)) * 0xbf58'476d'1ce4'e5b9ull;
		z = (z ^ (z >> 27)) * 0x94d0'49bb'1331'11ebull;
		z ^= z >> 31;
		return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
	}
	double random(double low, double high) { return low + (high - low) * random01(); }
	bool alive(EntityId id) const { return scene.native(id) != 0 && !removing.contains(id); }
	GameState& state() { return *world.try_get_mut<GameState>(); }
	const GameState& state() const { return *world.try_get_mut<GameState>(); }

	void submit()
	{
		for (std::size_t first = 0; first < batch.size(); first += commands::CommandBus::max_transaction) {
			const std::size_t count = std::min(batch.size() - first, commands::CommandBus::max_transaction);
			auto result = bus.execute(game_context, bus.revision(), std::span<const commands::Command>(batch.data() + first, count));
			if (!result) {
				issues.push_back("Gameplay command failed: " + result.error().message);
			}
		}
		batch.clear();
	}

	void parse(const SceneSnapshot& snapshot)
	{
		world.component<PlayerController>();
		world.component<WeaponComponent>();
		world.component<WeaponView>();
		world.component<HealthComponent>();
		world.component<TargetComponent>();
		world.component<MoverComponent>();
		world.component<ImpactDamage>();
		world.component<BonusZone>();
		world.component<Pickup>();
		world.component<Flash>();
		world.component<Effect>();
		world.component<EnemyComponent>();
		world.component<BreakableComponent>();
		world.component<Fallen>();
		world.component<GameState>();
		GameState rules;
		for (const EntityRecord& record : snapshot.entities) {
			const auto e = native(record.id);
			for (const BehaviorComponent& b : record.components.behaviors) {
				const auto number = [&](const char* key, double fallback) { return value_of(b, key, fallback); };
				if (b.type == "game_mode") {
					rules.time_limit = std::max(number("time_limit", 90), 1.0);
				} else if (b.type == "player") {
					if (player) {
						issues.push_back("More than one entity has a player behavior; '" + record.name + "' is ignored");
						continue;
					}
					if (!record.components.body || record.components.body->kind != BodyKind::Dynamic) {
						issues.push_back("Player '" + record.name + "' needs a dynamic body");
						continue;
					}
					player = record.id;
					player_start = record.transform;
					e.set<PlayerController>({number("walk_speed", 5), number("sprint_speed", 8.5), number("jump_speed", 5.2), number("acceleration", 60),
						number("air_acceleration", 12), number("look_sensitivity", 0.0022), number("spawn_floor", -20)});
				} else if (b.type == "weapon") {
					const int magazine = std::max(static_cast<int>(number("magazine", 15)), 1);
					e.set<WeaponComponent>({number("damage", 25), std::max(number("fire_interval", 0.12), 0.0), std::max(number("reload_time", 1.4), 0.0),
						std::max(number("range", 200), 0.1), std::max(number("impulse", 60), 0.0), magazine, magazine, static_cast<int>(number("reserve", 120))});
				} else if (b.type == "weapon_view") {
					weapon_view = record.id;
					e.set<WeaponView>({record.transform, number("recoil", 0.04), number("recoil_recovery", 12)});
				} else if (b.type == "health") {
					const double max = std::max(number("max", 50), 0.001);
					e.set<HealthComponent>({max, max});
				} else if (b.type == "target") {
					e.set<TargetComponent>({static_cast<int>(number("points", 100))});
					++rules.targets_total;
				} else if (b.type == "mover") {
					if (!record.components.body || record.components.body->kind != BodyKind::Kinematic) {
						issues.push_back("Mover '" + record.name + "' needs a kinematic body");
						continue;
					}
					e.set<MoverComponent>({Vec3{number("ax", record.transform.position.x), number("ay", record.transform.position.y), number("az", record.transform.position.z)},
						Vec3{number("bx", record.transform.position.x), number("by", record.transform.position.y), number("bz", record.transform.position.z)},
						std::max(number("speed", 2), 0.0)});
				} else if (b.type == "impact_damage") {
					e.set<ImpactDamage>({number("min_speed", 3), number("scale", 6)});
				} else if (b.type == "bonus_zone") {
					e.set<BonusZone>({static_cast<int>(number("points", 50)), {}});
				} else if (b.type == "pickup") {
					e.set<Pickup>({static_cast<int>(number("ammo", 0)), number("time", 0)});
				} else if (b.type == "impact_effect") {
					effect_template = record.id;
					effect_lifetime = std::max(number("lifetime", 0.25), dt);
					effect_scale = std::max(number("scale", 0.12), 0.001);
				} else if (b.type == "enemy") {
					if (!record.components.body || record.components.body->kind != BodyKind::Kinematic || record.parent != 0) {
						issues.push_back("Enemy '" + record.name + "' needs a kinematic body and no parent");
						continue;
					}
					const auto& p = record.transform.position;
					const auto& q = record.transform.rotation;
					EnemyComponent enemy{std::max(number("walk_speed", 1.05), 0.0), std::max(number("pause", 1.5), 0.0), std::max(number("turn_rate", 4), 0.01),
						std::max(number("flinch_time", 0.45), 0.0), Vec3{number("ax", p.x), p.y, number("az", p.z)}, Vec3{number("bx", p.x), p.y, number("bz", p.z)}};
					enemy.yaw = std::atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.x * q.x + q.y * q.y));
					e.set<EnemyComponent>(enemy);
				} else if (b.type == "muzzle_effect") {
					muzzle_template = record.id;
				} else if (b.type == "footstep_effect") {
					footstep_template = record.id;
				} else if (b.type == "dust_effect") {
					dust_template = record.id;
					dust_min_speed = std::max(number("min_speed", 3), 0.0);
					dust_cooldown = std::max(number("cooldown", 0.25), 0.0);
				} else if (b.type == "breakable") {
					e.set<BreakableComponent>({std::clamp(static_cast<int>(number("shards", 10)), 0, 64), std::max(number("speed", 2.5), 0.0)});
				} else if (b.type == "shard") {
					shard_template = record.id;
					shard_lifetime = std::clamp(number("lifetime", 6), 0.1, 600.0);
					shard_density = std::max(number("density", 2500), 1.0);
				} else {
					issues.push_back("Unknown behavior '" + b.type + "' on '" + record.name + "'");
				}
			}
		}
		rules.targets_remaining = rules.targets_total;
		world.set<GameState>(rules);
		if (!player) {
			return;
		}
		// The view is the first primary camera below the player; its parent keeps it at eye height.
		for (const EntityRecord& record : snapshot.entities) {
			if (record.parent == player && record.components.camera) {
				camera = record.id;
				camera_rest = record.transform;
				break;
			}
		}
		if (!camera) {
			issues.push_back("Player has no child entity with a camera; aiming has no effect");
		}
		if (camera) {
			// Start looking the way the authored camera does.
			const auto& q = camera_rest.rotation;
			yaw = std::atan2(2 * (q.w * q.y + q.x * q.z), 1 - 2 * (q.x * q.x + q.y * q.y));
			pitch = std::asin(std::clamp(2 * (q.w * q.x - q.y * q.z), -1.0, 1.0));
		}
	}

	std::optional<std::pair<PVec3, PVec3>> aim() const
	{
		const auto w = scene.world_transform(camera ? camera : player);
		if (!w) {
			return std::nullopt;
		}
		const auto& m = w->matrix;
		const double length = std::hypot(m[8], m[9], m[10]);
		if (!(length > 0)) {
			return std::nullopt;
		}
		return std::make_pair(PVec3{float(m[12]), float(m[13]), float(m[14])},
			PVec3{float(-m[8] / length), float(-m[9] / length), float(-m[10] / length)});
	}

	float standing_distance = 0.9f; // Center to the bottom of the capsule.

	void move_player()
	{
		const auto* controller = native(player).try_get<PlayerController>();
		const auto body = physics.body_state(player);
		if (!controller || !body) {
			return;
		}
		// Falling out of the arena is a bug in the level, not a way to lose: put the player back.
		if (body->pose.position.y < controller->spawn_floor) {
			batch.push_back(commands::SetTransform{player, player_start});
			physics.set_linear_velocity(player, {});
			fall_notice = "You fell out of the arena and were returned to the start";
			return;
		}
		jump_cooldown = std::max(jump_cooldown - dt, 0.0);
		// Rays down from the center and four points around it, so standing on an edge still counts.
		grounded = false;
		const physics::RaycastFilter feet{~std::uint64_t{0}, ~std::uint64_t{0}, player};
		const auto& p = body->pose.position;
		for (const auto& [ox, oz] : {std::pair{0.0f, 0.0f}, {0.25f, 0.0f}, {-0.25f, 0.0f}, {0.0f, 0.25f}, {0.0f, -0.25f}}) {
			const auto hit = physics.raycast({p.x + ox, p.y, p.z + oz}, {0, -1, 0}, standing_distance + 0.12f, feet);
			if (hit && hit->normal.y > 0.6f) {
				grounded = true;
				break;
			}
		}
		const bool playing = state().phase == GamePhase::Playing;
		double forward = 0, right = 0;
		if (playing) {
			forward = (input.is_held(Action::MoveForward) ? 1.0 : 0.0) - (input.is_held(Action::MoveBack) ? 1.0 : 0.0);
			right = (input.is_held(Action::MoveRight) ? 1.0 : 0.0) - (input.is_held(Action::MoveLeft) ? 1.0 : 0.0);
		}
		if (const double length = std::hypot(forward, right); length > 1) {
			forward /= length;
			right /= length;
		}
		const double speed = playing && input.is_held(Action::Sprint) ? controller->sprint_speed : controller->walk_speed;
		const double wanted_x = (-std::sin(yaw) * forward + std::cos(yaw) * right) * speed;
		const double wanted_z = (-std::cos(yaw) * forward - std::sin(yaw) * right) * speed;
		const double rate = (grounded ? controller->acceleration : controller->air_acceleration) * dt;
		const auto approach = [rate](double current, double wanted) { return current + std::clamp(wanted - current, -rate, rate); };
		PVec3 velocity = body->linear_velocity;
		velocity.x = float(approach(velocity.x, wanted_x));
		velocity.z = float(approach(velocity.z, wanted_z));
		if (playing && input.was_pressed(Action::Jump) && grounded && jump_cooldown <= 0) {
			velocity.y = float(controller->jump_speed);
			jump_cooldown = 0.25;
			grounded = false;
		}
		physics.set_linear_velocity(player, velocity);
	}

	void spawn_effect(const PVec3& at, const PVec3& normal)
	{
		Transform t;
		t.position = {at.x + normal.x * 0.02, at.y + normal.y * 0.02, at.z + normal.z * 0.02};
		t.scale = {effect_scale, effect_scale, effect_scale};
		Components components;
		if (const auto source = effect_template ? scene.inspect(effect_template) : std::nullopt) {
			if (source->components.emitter) {
				// Particles instead of an entity: nothing enters the scene, so the bus is not touched.
				effects.push_back({effect_template, {static_cast<float>(t.position.x), static_cast<float>(t.position.y), static_cast<float>(t.position.z)},
					normal, 1.0});
				return;
			}
			components.mesh = source->components.mesh;
		}
		if (!components.mesh) {
			components.mesh = MeshComponent{BuiltinMesh::Sphere, {1, 0.75, 0.3}, 0.6, 0};
		}
		auto created = bus.execute(game_context, bus.revision(), commands::Command(commands::CreateEntity{"Impact", t, 0, 0, std::move(components)}));
		if (!created) {
			issues.push_back("Impact effect failed: " + created.error().message);
			return;
		}
		native(created.value().created.front()).set<Effect>({effect_lifetime});
	}

	void fire(WeaponComponent& weapon)
	{
		const auto ray = aim();
		if (!ray) {
			return;
		}
		--weapon.ammo;
		weapon.cooldown = weapon.fire_interval;
		if (auto* view = weapon_view && alive(weapon_view) ? native(weapon_view).try_get_mut<WeaponView>() : nullptr) {
			view->kick = view->recoil;
		}
		ShotResult shot;
		const auto& [origin, direction] = *ray;
		physics::RaycastFilter filter;
		filter.ignore = player;
		const auto hit = physics.raycast(origin, direction, float(weapon.range), filter);
		if (hit) {
			shot.entity = hit->entity;
			shot.point = hit->point;
			shot.normal = hit->normal;
			const PVec3 impulse{direction.x * float(weapon.impulse), direction.y * float(weapon.impulse), direction.z * float(weapon.impulse)};
			shot.pushed = physics.apply_impulse(hit->entity, impulse, hit->point);
			if (hit->entity && scene.native(hit->entity) && native(hit->entity).has<HealthComponent>()) {
				shot.damaged = true;
				damage.push_back({hit->entity, weapon.damage, DamageCause::Weapon, impulse, hit->point});
			}
			spawn_effect(hit->point, hit->normal);
		}
		shots.push_back(shot);
		// The flash at the barrel, along the aim, as the gun is this tick.
		if (muzzle_template && alive(muzzle_template)) {
			if (const auto w = scene.world_transform(muzzle_template)) {
				effects.push_back({muzzle_template, {float(w->matrix[12]), float(w->matrix[13]), float(w->matrix[14])}, direction, 1.0});
				++counters.muzzle_flashes;
			}
		}
	}

	void cue(EntityId id, AnimationCue wanted)
	{
		animations.push_back({id, wanted});
	}

	void update_enemies()
	{
		std::vector<std::pair<EntityId, EnemyComponent*>> enemies;
		world.each([&](flecs::entity e, const StableId& id, EnemyComponent& enemy) {
			if (!e.has<Fallen>()) {
				enemies.emplace_back(id.value, &enemy);
			}
		});
		std::sort(enemies.begin(), enemies.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		for (const auto& [id, enemy] : enemies) {
			if (!alive(id)) {
				continue;
			}
			auto record = scene.inspect(id);
			Vec3 p = record->transform.position;
			AnimationCue wanted = enemy->playing;
			if (enemy->flinching > 0) {
				// Staggered: it stands where it was hit.
				enemy->flinching = std::max(enemy->flinching - dt, 0.0);
			} else if (enemy->waiting > 0) {
				enemy->waiting = std::max(enemy->waiting - dt, 0.0);
				wanted = enemy->waiting > 0 ? AnimationCue::Idle : AnimationCue::Walk;
			} else {
				const Vec3& goal = enemy->to_b ? enemy->b : enemy->a;
				const double dx = goal.x - p.x, dz = goal.z - p.z, far = std::hypot(dx, dz);
				if (far < 0.02) {
					// An end of the patrol: stand a while, then go back.
					enemy->to_b = !enemy->to_b;
					enemy->waiting = enemy->pause;
					wanted = enemy->pause > 0 ? AnimationCue::Idle : AnimationCue::Walk;
				} else {
					const double turn = std::remainder(std::atan2(dx, dz) - enemy->yaw, 2 * std::numbers::pi);
					const double step = enemy->turn_rate * dt;
					enemy->yaw = std::remainder(enemy->yaw + std::clamp(turn, -step, step), 2 * std::numbers::pi);
					// It walks once it roughly faces the way, and stays on its line.
					if (std::abs(turn) < 0.8) {
						const double go = std::min(enemy->walk_speed * dt, far);
						p.x += dx / far * go;
						p.z += dz / far * go;
					}
					wanted = AnimationCue::Walk;
				}
			}
			if (wanted != enemy->playing) {
				enemy->playing = wanted;
				cue(id, wanted);
			}
			// Standing or staggered, nothing changes: no command, so the scene is not copied for it.
			const Quaternion facing{0, std::sin(enemy->yaw / 2), 0, std::cos(enemy->yaw / 2)};
			if (p != record->transform.position || facing != record->transform.rotation) {
				record->transform.position = p;
				record->transform.rotation = facing;
				batch.push_back(commands::SetTransform{id, record->transform});
			}
		}
	}

	// Pieces of debris like the shard template, through the entity's box and along the shot, and the template's particles.
	void break_apart(EntityId id, const BreakableComponent& breakable, const DamageEvent& cause)
	{
		const auto w = scene.world_transform(id);
		const auto look = shard_template ? scene.inspect(shard_template) : std::nullopt;
		const auto look_world = shard_template ? scene.world_transform(shard_template) : std::nullopt;
		if (!w || !look || !look->components.mesh || !look_world) {
			return;
		}
		const auto& m = w->matrix;
		const auto column = [](const std::array<double, 16>& matrix, int c) { return Vec3{matrix[c * 4], matrix[c * 4 + 1], matrix[c * 4 + 2]}; };
		const Vec3 x = column(m, 0), y = column(m, 1), z = column(m, 2);
		const auto length = [](const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); };
		const Vec3 piece{length(column(look_world->matrix, 0)), length(column(look_world->matrix, 1)), length(column(look_world->matrix, 2))};
		const double push = length(Vec3{cause.impulse.x, cause.impulse.y, cause.impulse.z});
		const Vec3 along = push > 0 ? Vec3{cause.impulse.x / push, cause.impulse.y / push, cause.impulse.z / push} : Vec3{};
		for (int i = 0; i < breakable.shards; ++i) {
			const double u = random(-0.5, 0.5), v = random(-0.5, 0.5), s = random(-0.5, 0.5);
			physics::Debris debris;
			debris.look = shard_template;
			debris.pose.position = {float(m[12] + x.x * u + y.x * v + z.x * s), float(m[13] + x.y * u + y.y * v + z.y * s),
				float(m[14] + x.z * u + y.z * v + z.z * s)};
			const double angle = random(0, 2 * std::numbers::pi), ax = random(-1, 1), ay = random(-1, 1), az = random(-1, 1);
			const double axis = std::max(std::sqrt(ax * ax + ay * ay + az * az), 1e-6), half = std::sin(angle / 2) / axis;
			debris.pose.rotation = {float(ax * half), float(ay * half), float(az * half), float(std::cos(angle / 2))};
			const double speed = breakable.speed * random(0.6, 1.4);
			debris.linear_velocity = {float(along.x * speed + random(-0.8, 0.8)), float(along.y * speed + random(0, 1.2)),
				float(along.z * speed + random(-0.8, 0.8))};
			debris.angular_velocity = {float(random(-8, 8)), float(random(-8, 8)), float(random(-8, 8))};
			debris.collider.shape = physics::BoxShape{{float(piece.x / 2), float(piece.y / 2), float(piece.z / 2)}, {}};
			debris.collider.density = float(shard_density);
			debris.collider.friction = 0.5f;
			debris.collider.restitution = 0.2f;
			debris.lifetime = shard_lifetime;
			if (physics.spawn_debris(debris)) {
				++counters.shards;
			}
		}
		if (look->components.emitter) {
			const bool shot = cause.cause == DamageCause::Weapon;
			effects.push_back({shard_template, shot ? cause.point : PVec3{float(m[12]), float(m[13]), float(m[14])},
				push > 0 ? PVec3{float(along.x), float(along.y), float(along.z)} : PVec3{0, 1, 0}, 1.0});
		}
	}

	void update_weapon()
	{
		auto* weapon = native(player).try_get_mut<WeaponComponent>();
		if (!weapon) {
			return;
		}
		weapon->cooldown = std::max(weapon->cooldown - dt, 0.0);
		if (weapon->reloading > 0) {
			weapon->reloading -= dt;
			if (weapon->reloading <= 0) {
				weapon->reloading = 0;
				const int wanted = weapon->magazine_size - weapon->ammo;
				const int taken = weapon->reserve < 0 ? wanted : std::min(wanted, weapon->reserve);
				weapon->ammo += taken;
				if (weapon->reserve >= 0) {
					weapon->reserve -= taken;
				}
			}
			return;
		}
		if (state().phase != GamePhase::Playing) {
			return;
		}
		const bool can_reload = weapon->ammo < weapon->magazine_size && weapon->reserve != 0;
		if (input.was_pressed(Action::Reload) && can_reload) {
			weapon->reloading = weapon->reload_time;
			return;
		}
		if (!input.is_held(Action::Fire) || weapon->cooldown > 0) {
			return;
		}
		if (weapon->ammo > 0) {
			fire(*weapon);
		} else if (can_reload && input.was_pressed(Action::Fire)) {
			weapon->reloading = weapon->reload_time; // Pulling the trigger on empty starts a reload.
		}
	}

	void update_movers()
	{
		std::vector<std::pair<EntityId, MoverComponent*>> movers;
		world.each([&](flecs::entity, const StableId& id, MoverComponent& mover) { movers.emplace_back(id.value, &mover); });
		std::sort(movers.begin(), movers.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		for (const auto& [id, mover] : movers) {
			if (!alive(id)) {
				continue;
			}
			const double length = std::hypot(mover->b.x - mover->a.x, mover->b.y - mover->a.y, mover->b.z - mover->a.z);
			if (length <= 0) {
				continue;
			}
			mover->travelled = std::fmod(mover->travelled + mover->speed * dt, 2 * length);
			const double along = (mover->travelled <= length ? mover->travelled : 2 * length - mover->travelled) / length;
			auto record = scene.inspect(id);
			record->transform.position = {mover->a.x + (mover->b.x - mover->a.x) * along, mover->a.y + (mover->b.y - mover->a.y) * along,
				mover->a.z + (mover->b.z - mover->a.z) * along};
			batch.push_back(commands::SetTransform{id, record->transform});
		}
	}

	void update_effects()
	{
		std::vector<std::pair<EntityId, double*>> items;
		world.each([&](flecs::entity, const StableId& id, Effect& effect) { items.emplace_back(id.value, &effect.remaining); });
		for (const auto& [id, remaining] : items) {
			*remaining -= dt;
			if (*remaining <= 0) {
				remove(id);
			}
		}
		std::vector<std::pair<EntityId, Flash*>> flashes;
		world.each([&](flecs::entity, const StableId& id, Flash& flash) { flashes.emplace_back(id.value, &flash); });
		for (const auto& [id, flash] : flashes) {
			flash->remaining -= dt;
			if (flash->remaining <= 0 && alive(id)) {
				auto mesh = scene.inspect(id)->components.mesh;
				if (mesh) {
					mesh->color = flash->original;
					batch.push_back(commands::SetMesh{id, mesh});
				}
				native(id).remove<Flash>();
			}
		}
		if (weapon_view && alive(weapon_view)) {
			auto* view = native(weapon_view).try_get_mut<WeaponView>();
			if (view && view->kick > 0) {
				view->kick = std::max(view->kick - view->kick * view->recovery * dt - 1e-4, 0.0);
				Transform t = view->rest;
				t.position.z += view->kick;
				batch.push_back(commands::SetTransform{weapon_view, t});
			}
		}
	}

	void remove(EntityId id)
	{
		if (alive(id)) {
			removing.insert(id);
			batch.push_back(commands::DeleteEntity{id});
		}
	}

	void announce(std::string text)
	{
		last_event = std::move(text);
		last_event_age = 0;
	}

	void apply_damage()
	{
		for (const DamageEvent& event : damage) {
			if (!alive(event.target)) {
				continue;
			}
			const auto e = native(event.target);
			auto* health = e.try_get_mut<HealthComponent>();
			if (!health || e.has<Fallen>()) {
				continue;
			}
			health->current -= event.amount;
			hurt.push_back(event);
			if (event.cause == DamageCause::Weapon) {
				hit_marker = 1;
			}
			if (health->current <= 0) {
				destroyed(event.target, event);
				continue;
			}
			// A shot staggers an enemy, which flinches on top of what it was doing.
			if (auto* enemy = e.try_get_mut<EnemyComponent>(); enemy && event.cause == DamageCause::Weapon) {
				enemy->flinching = enemy->flinch_time;
				cue(event.target, AnimationCue::Flinch);
				++counters.flinches;
			}
			// A short red tint on the runtime copy.
			auto mesh = scene.inspect(event.target)->components.mesh;
			if (mesh && !e.has<Flash>()) {
				e.set<Flash>({mesh->color, 0.1});
				mesh->color = {1, 0.15, 0.1};
				batch.push_back(commands::SetMesh{event.target, mesh});
			}
		}
	}

	void destroyed(EntityId id, const DamageEvent& cause)
	{
		const auto e = native(id);
		auto& rules = state();
		const auto record = scene.inspect(id);
		deaths.push_back(id);
		if (const auto* target = e.try_get<TargetComponent>()) {
			rules.score += target->points;
			--rules.targets_remaining;
			announce("+" + std::to_string(target->points) + "  " + (record ? record->name : std::string("Target")) + " destroyed");
		}
		// A model whose Ragdoll falls on death stays, and falls from its pose with the killing shot's push.
		if (record && record->components.ragdoll && record->components.ragdoll->trigger == RagdollTrigger::OnDeath && record->components.mesh &&
			!record->components.mesh->mesh_asset.empty()) {
			e.add<Fallen>();
			e.remove<HealthComponent>();
			ragdolls.push_back({id, cause.cause == DamageCause::Weapon, cause.impulse, cause.point});
			++counters.ragdolls;
			return;
		}
		if (const auto* breakable = e.try_get<BreakableComponent>()) {
			break_apart(id, *breakable, cause);
		}
		remove(id);
	}

	void handle_events()
	{
		auto& rules = state();
		for (const physics::CollisionEvent& event : physics.events()) {
			if (event.kind == physics::CollisionEvent::Kind::Hit) {
				for (const EntityId id : {event.a, event.b}) {
					if (!id || !scene.native(id)) {
						continue;
					}
					const auto* impact = native(id).try_get<ImpactDamage>();
					if (impact && event.approach_speed > impact->min_speed) {
						damage.push_back({id, (event.approach_speed - impact->min_speed) * impact->scale, DamageCause::Impact, {}, event.point});
					}
				}
				// Dust where something hit hard: once a cooldown for each moving body, never for the player.
				if (dust_template && event.approach_speed > dust_min_speed) {
					for (const EntityId id : {event.a, event.b}) {
						if (!id || id == player || !scene.native(id) || dust_wait.contains(id)) {
							continue;
						}
						const auto moving = physics.body_state(id);
						if (!moving || moving->type != physics::BodyType::Dynamic) {
							continue;
						}
						dust_wait[id] = dust_cooldown;
						effects.push_back({dust_template, event.point, event.normal, std::clamp(event.approach_speed / std::max(dust_min_speed, 0.1), 1.0, 3.0)});
						++counters.dust;
						break;
					}
				}
			} else if (event.kind == physics::CollisionEvent::Kind::SensorBegin && event.a && event.b && scene.native(event.a) && scene.native(event.b)) {
				const auto sensor = native(event.a);
				if (auto* zone = sensor.try_get_mut<BonusZone>()) {
					const auto visitor = physics.body_state(event.b);
					const bool fresh = std::find(zone->scored.begin(), zone->scored.end(), event.b) == zone->scored.end();
					if (visitor && visitor->type == physics::BodyType::Dynamic && event.b != player && fresh) {
						zone->scored.push_back(event.b);
						rules.score += zone->points;
						announce("+" + std::to_string(zone->points) + "  Bonus");
					}
				}
				if (const auto* pickup = sensor.try_get<Pickup>(); pickup && event.b == player && alive(event.a)) {
					if (auto* weapon = native(player).try_get_mut<WeaponComponent>(); weapon && weapon->reserve >= 0) {
						weapon->reserve += pickup->ammo;
					}
					rules.time_limit += pickup->time;
					announce(pickup->time > 0 ? "+" + std::to_string(static_cast<int>(pickup->time)) + " seconds" : "Ammo collected");
					remove(event.a);
				}
			}
		}
	}

};

Game::Game(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Game::~Game() = default;

std::unique_ptr<Game> Game::create(Scene& scene, commands::CommandBus& bus, physics::ScenePhysics& physics, std::chrono::nanoseconds fixed_step)
{
	const SceneSnapshot snapshot = scene.snapshot();
	const auto has = [&](const char* type) {
		return std::any_of(snapshot.entities.begin(), snapshot.entities.end(), [&](const EntityRecord& e) { return find_behavior(e, type) != nullptr; });
	};
	if (!has("game_mode") && !has("player")) {
		return nullptr;
	}
	auto impl = std::make_unique<Impl>(scene, bus, physics, std::chrono::duration<double>(fixed_step).count());
	impl->parse(snapshot);
	// Center to the bottom of the capsule, for ground checks.
	if (const auto record = impl->player ? scene.inspect(impl->player) : std::nullopt) {
		for (const ColliderComponent& k : record->components.colliders) {
			if (k.shape == ColliderShape::Capsule) {
				impl->standing_distance = static_cast<float>(k.half_height + k.radius - k.center.y);
			}
		}
	}
	return std::unique_ptr<Game>(new Game(std::move(impl)));
}

void Game::set_input(const InputFrame& input)
{
	// Accumulates presses until a tick consumes them, so a frame with no tick loses nothing.
	for (std::size_t i = 0; i < action_count; ++i) {
		impl_->input.pressed[i] = impl_->input.pressed[i] || input.pressed[i];
	}
	impl_->input.held = input.held;
}

void Game::look(double dx, double dy)
{
	auto& g = *impl_;
	if (g.state().phase != GamePhase::Playing || !g.player || (dx == 0 && dy == 0)) {
		return;
	}
	const auto* controller = g.native(g.player).try_get<PlayerController>();
	if (!controller) {
		return;
	}
	const double limit = std::numbers::pi / 2 - 0.02;
	g.yaw = std::remainder(g.yaw - dx * controller->look_sensitivity, 2 * std::numbers::pi);
	g.pitch = std::clamp(g.pitch - dy * controller->look_sensitivity, -limit, limit);
	g.view_dirty = true;
}

void Game::update_view()
{
	auto& g = *impl_;
	if (!g.view_dirty || !g.camera || !g.alive(g.camera)) {
		return;
	}
	g.view_dirty = false;
	Transform t = g.camera_rest;
	t.rotation = yaw_pitch(g.yaw, g.pitch);
	g.batch.push_back(commands::SetTransform{g.camera, t});
	g.submit();
}

void Game::start()
{
	if (impl_->state().phase == GamePhase::Ready) {
		impl_->state().phase = GamePhase::Playing;
	}
}

void Game::pause()
{
	if (impl_->state().phase == GamePhase::Playing) {
		impl_->state().phase = GamePhase::Paused;
	}
}

void Game::resume()
{
	if (impl_->state().phase == GamePhase::Paused) {
		impl_->state().phase = GamePhase::Playing;
	}
}

void Game::pre_physics()
{
	auto& g = *impl_;
	++g.ticks;
	g.damage.clear();
	g.hurt.clear();
	g.deaths.clear();
	g.shots.clear();
	g.effects.clear();
	g.animations.clear();
	g.ragdolls.clear();
	g.removing.clear();
	g.fall_notice.clear();
	for (auto it = g.dust_wait.begin(); it != g.dust_wait.end();) {
		it->second -= g.dt;
		it = it->second <= 0 ? g.dust_wait.erase(it) : std::next(it);
	}
	auto& rules = g.state();
	if (rules.phase == GamePhase::Playing) {
		rules.elapsed += g.dt;
	}
	g.hit_marker = std::max(g.hit_marker - g.dt * 4, 0.0);
	g.last_event_age += g.dt;
	if (g.player && g.alive(g.player)) {
		g.move_player();
		g.update_weapon();
	}
	g.update_movers();
	g.update_enemies();
	g.update_effects();
	g.submit();
	g.input.pressed = {};
}

void Game::hear(std::span<const AnimationNotice> notices)
{
	auto& g = *impl_;
	for (const AnimationNotice& notice : notices) {
		if (!g.alive(notice.entity) || g.native(notice.entity).has<Fallen>() || notice.name.find("footstep") == std::string::npos) {
			continue;
		}
		++g.counters.footsteps;
		if (g.footstep_template && g.alive(g.footstep_template)) {
			g.effects.push_back({g.footstep_template, notice.point, {0, 1, 0}, 1.0});
		}
	}
}

void Game::post_physics()
{
	auto& g = *impl_;
	g.handle_events();
	g.damage.insert(g.damage.end(), g.queued.begin(), g.queued.end());
	g.queued.clear();
	g.apply_damage();
	g.submit();
	auto& rules = g.state();
	if (rules.phase != GamePhase::Playing) {
		return;
	}
	if (rules.targets_total > 0 && rules.targets_remaining <= 0) {
		rules.phase = GamePhase::Won;
	} else if (rules.elapsed >= rules.time_limit) {
		rules.phase = GamePhase::Lost;
	}
}

GamePhase Game::phase() const noexcept
{
	return impl_->state().phase;
}

HudState Game::hud() const
{
	const auto& g = *impl_;
	const auto& rules = g.state();
	HudState hud;
	hud.phase = rules.phase;
	hud.score = rules.score;
	hud.targets_remaining = rules.targets_remaining;
	hud.targets_total = rules.targets_total;
	hud.time_limit = rules.time_limit;
	hud.time_remaining = std::max(rules.time_limit - rules.elapsed, 0.0);
	hud.hit_marker = g.hit_marker;
	hud.last_event = g.last_event;
	hud.last_event_age = g.last_event_age;
	hud.fall_notice = g.fall_notice;
	if (g.player && g.scene.native(g.player)) {
		if (const auto* weapon = g.native(g.player).try_get<WeaponComponent>()) {
			hud.ammo = weapon->ammo;
			hud.magazine = weapon->magazine_size;
			hud.reserve = weapon->reserve;
			hud.reloading = weapon->reloading > 0;
			hud.reload_progress = weapon->reload_time > 0 ? 1 - weapon->reloading / weapon->reload_time : 1;
		}
	}
	return hud;
}

EntityId Game::player() const noexcept { return impl_->player; }
EntityId Game::camera() const noexcept { return impl_->camera; }
bool Game::grounded() const noexcept { return impl_->grounded; }
std::uint64_t Game::ticks() const noexcept { return impl_->ticks; }
const std::vector<DamageEvent>& Game::damage_events() const noexcept { return impl_->damage; }
const std::vector<ShotResult>& Game::shots() const noexcept { return impl_->shots; }
const std::vector<EffectEvent>& Game::effect_events() const noexcept { return impl_->effects; }
const std::vector<AnimationRequest>& Game::animation_requests() const noexcept { return impl_->animations; }
const std::vector<RagdollRequest>& Game::ragdoll_requests() const noexcept { return impl_->ragdolls; }
GameCounters Game::counters() const noexcept { return impl_->counters; }

std::vector<EntityId> Game::standing_targets() const
{
	const auto& g = *impl_;
	std::vector<EntityId> ids;
	g.world.each([&](flecs::entity e, const StableId& id, const TargetComponent&) {
		if (!e.has<Fallen>() && g.alive(id.value)) {
			ids.push_back(id.value);
		}
	});
	std::sort(ids.begin(), ids.end());
	return ids;
}
const std::vector<std::string>& Game::setup_issues() const noexcept { return impl_->issues; }

void Game::add_score(int points, std::string_view message)
{
	impl_->state().score += points;
	if (!message.empty()) impl_->announce(std::string(message));
}

void Game::announce(std::string_view message) { impl_->announce(std::string(message)); }

void Game::damage(EntityId target, double amount, physics::Vec3 point)
{
	if (!std::isfinite(amount) || amount <= 0) return;
	impl_->queued.push_back({target, amount, DamageCause::Script, {}, point});
}

std::optional<double> Game::health(EntityId entity) const
{
	if (!impl_->alive(entity)) return std::nullopt;
	const auto* health = impl_->native(entity).try_get<HealthComponent>();
	if (!health) return std::nullopt;
	return health->current;
}

void Game::add_time(double seconds)
{
	if (std::isfinite(seconds)) impl_->state().time_limit += seconds;
}

void Game::give_ammo(int rounds)
{
	auto& g = *impl_;
	if (!g.player || !g.alive(g.player)) return;
	if (auto* weapon = g.native(g.player).try_get_mut<WeaponComponent>(); weapon && weapon->reserve >= 0) weapon->reserve += rounds;
}

void Game::win()
{
	if (impl_->state().phase == GamePhase::Playing) impl_->state().phase = GamePhase::Won;
}

void Game::lose()
{
	if (impl_->state().phase == GamePhase::Playing) impl_->state().phase = GamePhase::Lost;
}

const std::vector<DamageEvent>& Game::damage_taken() const noexcept { return impl_->hurt; }
const std::vector<EntityId>& Game::deaths() const noexcept { return impl_->deaths; }

} // namespace engine::gameplay
