#include "engine/scripting/script_world.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace engine::scripting {

namespace {

const commands::Context script_context{"scripts", 3};

physics::Vec3 to_physics(const Vector3& v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }
Vector3 from_physics(const physics::Vec3& v) { return {v.x, v.y, v.z}; }
Vector3 from_scene(const Vec3& v) { return {v.x, v.y, v.z}; }
Vec3 to_scene(const Vector3& v) { return {v.x, v.y, v.z}; }

using Matrix = std::array<double, 16>; // Column-major, as WorldTransform.

Matrix trs(const Transform& t)
{
	const auto& q = t.rotation;
	const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
	const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
	return {(1 - 2 * (yy + zz)) * t.scale.x, 2 * (xy + wz) * t.scale.x, 2 * (xz - wy) * t.scale.x, 0,
		2 * (xy - wz) * t.scale.y, (1 - 2 * (xx + zz)) * t.scale.y, 2 * (yz + wx) * t.scale.y, 0,
		2 * (xz + wy) * t.scale.z, 2 * (yz - wx) * t.scale.z, (1 - 2 * (xx + yy)) * t.scale.z, 0,
		t.position.x, t.position.y, t.position.z, 1};
}

Matrix multiply(const Matrix& a, const Matrix& b)
{
	Matrix r{};
	for (int column = 0; column < 4; ++column)
		for (int row = 0; row < 4; ++row) {
			double sum = 0;
			for (int k = 0; k < 4; ++k) sum += a[std::size_t(k * 4 + row)] * b[std::size_t(column * 4 + k)];
			r[std::size_t(column * 4 + row)] = sum;
		}
	return r;
}

// A point in the space a matrix maps from, given the point it maps to. Affine matrices only.
std::optional<Vector3> unmap(const Matrix& m, const Vector3& p)
{
	const double a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
	const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	if (std::abs(det) < 1e-12) return std::nullopt;
	const double x = p.x - m[12], y = p.y - m[13], z = p.z - m[14];
	return Vector3{((e * i - f * h) * x - (b * i - c * h) * y + (b * f - c * e) * z) / det,
		(-(d * i - f * g) * x + (a * i - c * g) * y - (a * f - c * d) * z) / det,
		((d * h - e * g) * x - (a * h - b * g) * y + (a * e - b * d) * z) / det};
}

Quaternion compose(const Quaternion& a, const Quaternion& b)
{
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

std::optional<gameplay::Action> action_named(std::string_view name)
{
	static constexpr std::pair<std::string_view, gameplay::Action> actions[] = {
		{"MoveForward", gameplay::Action::MoveForward}, {"MoveBack", gameplay::Action::MoveBack}, {"MoveLeft", gameplay::Action::MoveLeft},
		{"MoveRight", gameplay::Action::MoveRight}, {"Jump", gameplay::Action::Jump}, {"Sprint", gameplay::Action::Sprint},
		{"Fire", gameplay::Action::Fire}, {"Reload", gameplay::Action::Reload}, {"Interact", gameplay::Action::Interact}};
	for (const auto& [known, action] : actions)
		if (known == name) return action;
	return std::nullopt;
}

gameplay::Action action_or_throw(std::string_view name)
{
	const auto action = action_named(name);
	if (!action)
		throw std::runtime_error("there is no action '" + std::string(name) +
			"': MoveForward, MoveBack, MoveLeft, MoveRight, Jump, Sprint, Fire, Reload or Interact");
	return *action;
}

const char* cause_name(gameplay::DamageCause cause)
{
	switch (cause) {
	case gameplay::DamageCause::Weapon: return "weapon";
	case gameplay::DamageCause::Impact: return "impact";
	case gameplay::DamageCause::Script: return "script";
	}
	return "unknown";
}

} // namespace

std::unique_ptr<ScriptWorld> ScriptWorld::create(Scene& scene, commands::CommandBus& bus, physics::ScenePhysics& physics,
	gameplay::Game* game, std::chrono::nanoseconds fixed_step, Setup setup)
{
	const auto snapshot = scene.snapshot();
	const bool scripted = std::any_of(snapshot.entities.begin(), snapshot.entities.end(),
		[](const EntityRecord& e) { return e.components.script && !e.components.script->script.empty(); });
	if (!scripted) return nullptr;
	auto world = std::unique_ptr<ScriptWorld>(new ScriptWorld(scene, bus, physics, game, std::chrono::duration<double>(fixed_step).count(), std::move(setup)));
	world->attach_new();
	return world;
}

ScriptWorld::ScriptWorld(Scene& scene, commands::CommandBus& bus, physics::ScenePhysics& physics, gameplay::Game* game, double dt, Setup setup)
	: scene_(scene), bus_(bus), physics_(physics), game_(game), dt_(dt), setup_(std::move(setup))
{
	runtime_ = std::make_unique<ScriptRuntime>(setup_.loader, setup_.limits, setup_.random_seed, this);
}

ScriptWorld::~ScriptWorld() = default;

void ScriptWorld::attach_new()
{
	for (const auto& e : scene_.snapshot().entities) {
		if (!e.components.script || e.components.script->script.empty() || runtime_->has_instance(e.id)) continue;
		const auto& component = *e.components.script;
		std::string name = setup_.names ? setup_.names(component.script) : std::string();
		if (name.empty()) name = "/Game/" + component.script.path;
		// A script that cannot load is reported by the runtime; the entity simply runs nothing.
		if (runtime_->attach(e.id, name, overrides_of(component, setup_.names))) waiting_.insert(e.id);
	}
}

void ScriptWorld::set_input(const gameplay::InputFrame& input)
{
	for (std::size_t i = 0; i < gameplay::action_count; ++i) input_.pressed[i] = input_.pressed[i] || input.pressed[i];
	input_.held = input.held;
}

void ScriptWorld::pre_physics()
{
	++ticks_;
	clips_.clear();
	effects_.clear();
	ragdolls_.clear();
	const std::vector<EntityId> starting(waiting_.begin(), waiting_.end());
	waiting_.clear();
	for (const EntityId id : starting) runtime_->call(id, "start");
	const Value dt[] = {dt_};
	runtime_->call("tick", dt);
	input_.pressed = {};
	apply();
	forget_gone();
}

void ScriptWorld::post_physics(std::span<const gameplay::AnimationNotice> notices)
{
	// Each side hears the other, unless the game removed one of them after the step: as the game's own sensors do.
	const auto both = [&](EntityId a, EntityId b, const char* callback, std::span<const Value> rest) {
		if ((a && !exists(a)) || (b && !exists(b))) return;
		for (const auto& [self, other] : {std::pair{a, b}, std::pair{b, a}}) {
			if (!self || !runtime_->has_instance(self)) continue;
			std::vector<Value> args{EntityRef{other}};
			args.insert(args.end(), rest.begin(), rest.end());
			runtime_->call(self, callback, args);
		}
	};
	for (const auto& event : physics_.events()) {
		using Kind = physics::CollisionEvent::Kind;
		if (event.kind == Kind::Hit) {
			const Value rest[] = {static_cast<double>(event.approach_speed), from_physics(event.point), from_physics(event.normal)};
			both(event.a, event.b, "on_hit", rest);
		} else if (event.kind == Kind::SensorBegin) {
			both(event.a, event.b, "on_trigger_enter", {});
		} else if (event.kind == Kind::SensorEnd) {
			both(event.a, event.b, "on_trigger_exit", {});
		}
	}
	if (game_) {
		for (const auto& hurt : game_->damage_taken()) {
			if (!runtime_->has_instance(hurt.target)) continue;
			const Value args[] = {hurt.amount, std::string(cause_name(hurt.cause)), from_physics(hurt.point)};
			runtime_->call(hurt.target, "on_damage", args);
		}
		for (const EntityId dead : game_->deaths())
			if (runtime_->has_instance(dead)) runtime_->call(dead, "on_death");
	}
	for (const auto& notice : notices) {
		if (!runtime_->has_instance(notice.entity)) continue;
		const Value args[] = {notice.name, from_physics(notice.point)};
		runtime_->call(notice.entity, "on_clip_event", args);
	}
	const Value dt[] = {dt_};
	runtime_->call("late_tick", dt);
	apply();
	forget_gone();
}

void ScriptWorld::stop()
{
	runtime_->call("on_stop");
	apply();
	forget_gone();
}

std::vector<std::string> ScriptWorld::take_issues() { return std::exchange(issues_, {}); }

void ScriptWorld::apply()
{
	std::vector<commands::Command> batch;
	for (const auto& [id, transform] : moved_)
		if (scene_.native(id) && !destroyed_.contains(id)) batch.emplace_back(commands::SetTransform{id, transform});
	for (const EntityId id : destroyed_)
		if (scene_.native(id)) batch.emplace_back(commands::DeleteEntity{id});
	moved_.clear();
	destroyed_.clear();
	for (std::size_t first = 0; first < batch.size(); first += commands::CommandBus::max_transaction) {
		const std::size_t count = std::min(batch.size() - first, commands::CommandBus::max_transaction);
		if (auto done = bus_.execute(script_context, bus_.revision(), std::span<const commands::Command>(batch.data() + first, count)); !done)
			issues_.push_back("A script's change could not be made: " + done.error().message);
	}
}

void ScriptWorld::forget_gone()
{
	for (const EntityId id : runtime_->instances())
		if (!scene_.native(id)) {
			runtime_->detach(id);
			waiting_.erase(id);
		}
}

std::optional<EntityRecord> ScriptWorld::record(EntityId entity) const
{
	if (destroyed_.contains(entity)) return std::nullopt;
	auto found = scene_.inspect(entity);
	if (found)
		if (const auto moved = moved_.find(entity); moved != moved_.end()) found->transform = moved->second;
	return found;
}

std::array<double, 16> ScriptWorld::world_matrix(EntityId entity) const
{
	// Up the parents, with what scripts set and the scene has not seen yet.
	Matrix world = trs(record(entity)->transform);
	for (EntityId up = record(entity)->parent; up; up = record(up)->parent) world = multiply(trs(record(up)->transform), world);
	return world;
}

gameplay::Game& ScriptWorld::game() const
{
	if (!game_) throw std::runtime_error("this scene runs no game: give an entity a game_mode or player behavior");
	return *game_;
}

// ---- Entities ----

bool ScriptWorld::exists(EntityId entity) const { return entity && !destroyed_.contains(entity) && scene_.native(entity) != 0; }

std::string ScriptWorld::name(EntityId entity) const { return record(entity)->name; }

EntityId ScriptWorld::find(std::string_view name) const
{
	for (const auto& e : scene_.snapshot().entities)
		if (e.name == name && !destroyed_.contains(e.id)) return e.id;
	return 0;
}

EntityId ScriptWorld::parent(EntityId entity) const { return record(entity)->parent; }

std::vector<EntityId> ScriptWorld::children(EntityId entity) const
{
	std::vector<EntityId> kids;
	for (const auto& e : scene_.snapshot().entities)
		if (e.parent == entity && !destroyed_.contains(e.id)) kids.push_back(e.id);
	return kids;
}

LocalTransform ScriptWorld::local(EntityId entity) const
{
	const auto& t = record(entity)->transform;
	return {from_scene(t.position), {t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w}, from_scene(t.scale)};
}

void ScriptWorld::set_local(EntityId entity, const LocalTransform& transform)
{
	const auto& q = transform.rotation;
	const double length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	const bool finite = std::isfinite(transform.position.x) && std::isfinite(transform.position.y) && std::isfinite(transform.position.z) &&
		std::isfinite(transform.scale.x) && std::isfinite(transform.scale.y) && std::isfinite(transform.scale.z) && std::isfinite(length);
	if (!finite || length < 1e-9) throw std::runtime_error("a position, rotation or scale must be finite, and a rotation not zero");
	if (std::abs(transform.scale.x) < 1e-8 || std::abs(transform.scale.y) < 1e-8 || std::abs(transform.scale.z) < 1e-8)
		throw std::runtime_error("a scale of 0 cannot be undone: use a small number");
	Transform t;
	t.position = to_scene(transform.position);
	t.rotation = {q.x / length, q.y / length, q.z / length, q.w / length};
	t.scale = to_scene(transform.scale);
	moved_[entity] = t;
}

Vector3 ScriptWorld::world_position(EntityId entity) const
{
	const auto m = world_matrix(entity);
	return {m[12], m[13], m[14]};
}

Quaternion ScriptWorld::world_rotation(EntityId entity) const
{
	Quaternion q = local(entity).rotation;
	for (EntityId up = parent(entity); up; up = parent(up)) q = compose(local(up).rotation, q);
	return q;
}

void ScriptWorld::set_world_position(EntityId entity, const Vector3& position)
{
	auto t = local(entity);
	const EntityId up = parent(entity);
	if (!up) {
		t.position = position;
	} else {
		const auto there = unmap(world_matrix(up), position);
		if (!there) throw std::runtime_error("its parent is scaled to nothing, so it has no position to take");
		t.position = *there;
	}
	set_local(entity, t);
}

EntityId ScriptWorld::spawn(EntityId original, const Vector3& position, const Quaternion& rotation)
{
	// What scripts changed first, so the copy is of the entity as they see it.
	apply();
	auto copied = bus_.execute(script_context, bus_.revision(), commands::Command(commands::DuplicateEntity{original, ""}));
	if (!copied) throw std::runtime_error("cannot copy entity " + std::to_string(original) + ": " + copied.error().message);
	const EntityId copy = copied.value().created.front();
	auto placed = record(copy)->transform;
	placed.position = to_scene(position);
	const double length = std::sqrt(rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z + rotation.w * rotation.w);
	if (length > 1e-9) placed.rotation = {rotation.x / length, rotation.y / length, rotation.z / length, rotation.w / length};
	const commands::Command place[] = {commands::SetParent{copy, 0}, commands::SetTransform{copy, placed}};
	if (auto done = bus_.execute(script_context, bus_.revision(), std::span<const commands::Command>(place)); !done)
		throw std::runtime_error("cannot place the copy of entity " + std::to_string(original) + ": " + done.error().message);
	attach_new();
	return copy;
}

void ScriptWorld::destroy(EntityId entity)
{
	// With what is under it, as the scene deletes a subtree.
	std::vector<EntityId> going{entity};
	for (std::size_t i = 0; i < going.size(); ++i)
		for (const EntityId child : children(going[i])) going.push_back(child);
	for (const EntityId id : going) {
		moved_.erase(id);
		destroyed_.insert(id);
	}
}

// ---- Physics ----

std::optional<RayHit> ScriptWorld::raycast(const Vector3& origin, const Vector3& direction, double distance, EntityId ignore) const
{
	const double length = std::hypot(direction.x, direction.y, direction.z);
	if (!(length > 0) || !std::isfinite(length)) throw std::runtime_error("a ray needs a direction that is not zero");
	if (!(distance > 0) || !std::isfinite(distance)) throw std::runtime_error("a ray needs a distance above 0");
	physics::RaycastFilter filter;
	filter.ignore = ignore;
	const auto hit = physics_.raycast(to_physics(origin), to_physics({direction.x / length, direction.y / length, direction.z / length}),
		static_cast<float>(distance), filter);
	if (!hit) return std::nullopt;
	return RayHit{hit->entity, from_physics(hit->point), from_physics(hit->normal), hit->distance};
}

bool ScriptWorld::apply_impulse(EntityId entity, const Vector3& impulse, const Vector3& point)
{
	return physics_.apply_impulse(entity, to_physics(impulse), to_physics(point));
}

std::optional<Vector3> ScriptWorld::velocity(EntityId entity) const
{
	const auto state = physics_.body_state(entity);
	if (!state) return std::nullopt;
	return from_physics(state->linear_velocity);
}

bool ScriptWorld::set_velocity(EntityId entity, const Vector3& velocity) { return physics_.set_linear_velocity(entity, to_physics(velocity)); }

bool ScriptWorld::is_dynamic(EntityId entity) const
{
	const auto state = physics_.body_state(entity);
	return state && state->type == physics::BodyType::Dynamic;
}

// ---- Input ----

bool ScriptWorld::held(std::string_view action) const { return input_.is_held(action_or_throw(action)); }
bool ScriptWorld::pressed(std::string_view action) const { return input_.was_pressed(action_or_throw(action)); }

// ---- Animation, effects, ragdolls ----

void ScriptWorld::play_clip(EntityId entity, std::string_view clip, double blend_seconds)
{
	if (!std::isfinite(blend_seconds) || blend_seconds < 0) throw std::runtime_error("a blend time is 0 or more seconds");
	clips_.push_back({entity, std::string(clip), blend_seconds});
}

void ScriptWorld::fire_effect(EntityId entity, const Vector3& point, const Vector3& direction, double strength)
{
	const auto found = record(entity);
	if (!found->components.emitter) throw std::runtime_error(found->name + " has no particle emitter to fire");
	effects_.push_back({entity, to_physics(point), to_physics(direction), std::isfinite(strength) ? std::max(strength, 0.0) : 1.0});
}

void ScriptWorld::ragdoll(EntityId entity)
{
	const auto found = record(entity);
	if (!found->components.ragdoll) throw std::runtime_error(found->name + " has no Ragdoll component");
	ragdolls_.push_back(entity);
}

// ---- The game ----

int ScriptWorld::score() const { return game().hud().score; }
void ScriptWorld::add_score(int points, std::string_view message) { game().add_score(points, message); }
void ScriptWorld::announce(std::string_view message) { game().announce(message); }
void ScriptWorld::damage(EntityId entity, double amount, const Vector3& point) { game().damage(entity, amount, to_physics(point)); }
std::optional<double> ScriptWorld::health(EntityId entity) const { return game_ ? game_->health(entity) : std::nullopt; }
double ScriptWorld::time_left() const { return game().hud().time_remaining; }
void ScriptWorld::add_time(double seconds) { game().add_time(seconds); }
void ScriptWorld::give_ammo(int rounds) { game().give_ammo(rounds); }
int ScriptWorld::targets_left() const { return game().hud().targets_remaining; }
std::vector<EntityId> ScriptWorld::targets() const
{
	auto standing = game().standing_targets();
	std::erase_if(standing, [&](EntityId id) { return destroyed_.contains(id); });
	return standing;
}
EntityId ScriptWorld::player() const { return game_ ? game_->player() : 0; }
std::string ScriptWorld::phase() const { return game_ ? gameplay::to_string(game_->phase()) : "none"; }
void ScriptWorld::win() { game().win(); }
void ScriptWorld::lose() { game().lose(); }

} // namespace engine::scripting
