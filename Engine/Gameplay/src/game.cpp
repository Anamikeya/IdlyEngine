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
	std::vector<ShotResult> shots;
	std::vector<std::string> issues;
	std::vector<commands::Command> batch;
	std::set<EntityId> removing;

	Impl(Scene& s, commands::CommandBus& b, ScenePhysics& p, double step)
		: scene(s), bus(b), physics(p), world(s.ecs().native()), dt(step)
	{
	}

	flecs::entity native(EntityId id) const { return world.entity(scene.native(id)); }
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
				damage.push_back({hit->entity, weapon.damage, DamageCause::Weapon});
			}
			spawn_effect(hit->point, hit->normal);
		}
		shots.push_back(shot);
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
			if (!health) {
				continue;
			}
			health->current -= event.amount;
			if (event.cause == DamageCause::Weapon) {
				hit_marker = 1;
			}
			if (health->current <= 0) {
				destroyed(event.target);
				continue;
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

	void destroyed(EntityId id)
	{
		const auto e = native(id);
		auto& rules = state();
		if (const auto* target = e.try_get<TargetComponent>()) {
			rules.score += target->points;
			--rules.targets_remaining;
			const auto record = scene.inspect(id);
			announce("+" + std::to_string(target->points) + "  " + (record ? record->name : std::string("Target")) + " destroyed");
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
						damage.push_back({id, (event.approach_speed - impact->min_speed) * impact->scale, DamageCause::Impact});
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
	g.shots.clear();
	g.removing.clear();
	g.fall_notice.clear();
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
	g.update_effects();
	g.submit();
	g.input.pressed = {};
}

void Game::post_physics()
{
	auto& g = *impl_;
	g.handle_events();
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
const std::vector<std::string>& Game::setup_issues() const noexcept { return impl_->issues; }

} // namespace engine::gameplay
