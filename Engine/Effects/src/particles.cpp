#include "engine/effects/particles.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace engine::effects {

namespace {

using assets::EffectData;
using V3 = std::array<double, 3>;

constexpr double pi = 3.14159265358979323846;
constexpr std::size_t max_one_shots = 256;
constexpr int max_cycles_per_update = 64; // A tiny looping duration cannot spin forever.

V3 add(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 scale(const V3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
V3 normalized(const V3& v, const V3& fallback)
{
	const double length = std::sqrt(dot(v, v));
	return length > 1e-12 && std::isfinite(length) ? scale(v, 1.0 / length) : fallback;
}

// An orthonormal frame without scale: where an emitter is, and which way is its +Y.
struct Frame {
	V3 origin{0, 0, 0};
	V3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};

	V3 to_world(const V3& local) const { return add(origin, add(add(scale(x, local[0]), scale(y, local[1])), scale(z, local[2]))); }
	V3 rotate(const V3& local) const { return add(add(scale(x, local[0]), scale(y, local[1])), scale(z, local[2])); }
	V3 unrotate(const V3& world) const { return {dot(x, world), dot(y, world), dot(z, world)}; }

	static Frame from_matrix(const Matrix& m)
	{
		Frame f;
		f.origin = {m[12], m[13], m[14]};
		f.y = normalized({m[4], m[5], m[6]}, {0, 1, 0});
		const V3 x = normalized({m[0], m[1], m[2]}, {1, 0, 0});
		f.z = normalized(cross(x, f.y), std::abs(f.y[1]) < 0.9 ? normalized(cross({0, 1, 0}, f.y), {0, 0, 1}) : V3{0, 0, 1});
		f.x = cross(f.y, f.z);
		return f;
	}
	static Frame along(const V3& point, const V3& direction)
	{
		Frame f;
		f.origin = point;
		f.y = normalized(direction, {0, 1, 0});
		const V3 helper = std::abs(f.y[1]) < 0.9 ? V3{0, 1, 0} : V3{1, 0, 0};
		f.x = normalized(cross(helper, f.y), {1, 0, 0});
		f.z = cross(f.x, f.y);
		return f;
	}
};

// splitmix64: small, fast and the same on every platform.
struct Random {
	std::uint64_t state = 0;
	std::uint64_t next()
	{
		std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
		return z ^ (z >> 31);
	}
	double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); } // [0, 1)
	double between(double a, double b) { return a + (b - a) * unit(); }
};

std::uint64_t mix(std::uint64_t a, std::uint64_t b)
{
	Random r{a ^ (b * 0xd6e8feb86659fd93ull)};
	return r.next();
}

double srgb_to_linear(double c)
{
	return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

struct Particle {
	V3 position{}; // World space, or the emitter's frame when the effect is not in world space.
	V3 velocity{};
	double age = 0, life = 1, rotation = 0, spin = 0;
};

struct Emitter {
	std::uint64_t key = 0, source = 0, asset = 0;
	std::uint32_t revision = 0;
	std::shared_ptr<const EffectData> effect;
	Frame frame;
	bool one_shot = false, auto_play = true, loop = true;
	double rate_scale = 1;
	bool playing = false, burst_fired = false;
	double cycle_time = 0, carry = 0;
	Random random;
	std::vector<Particle> particles;

	bool local() const { return !one_shot && !effect->world_space; }

	void start()
	{
		playing = true;
		burst_fired = false;
		cycle_time = 0;
		carry = 0;
	}

	void spawn(std::size_t count)
	{
		const EffectData& e = *effect;
		const std::size_t room = e.max_particles > particles.size() ? e.max_particles - particles.size() : 0;
		count = std::min(count, room);
		const double cos_cone = std::cos(std::clamp(e.cone_degrees, 0.0, 180.0) * pi / 180.0);
		for (std::size_t i = 0; i < count; ++i) {
			// A direction uniform over the spherical cap around +Y, then a point uniform in the ball.
			const double cos_theta = random.between(cos_cone, 1.0);
			const double sin_theta = std::sqrt(std::max(0.0, 1.0 - cos_theta * cos_theta));
			const double phi = random.between(0.0, 2.0 * pi);
			const V3 local_direction{sin_theta * std::cos(phi), cos_theta, sin_theta * std::sin(phi)};
			V3 offset{0, 0, 0};
			if (e.spawn_radius > 0) {
				const V3 any{random.between(-1, 1), random.between(-1, 1), random.between(-1, 1)};
				offset = scale(normalized(any, {0, 1, 0}), e.spawn_radius * std::cbrt(random.unit()));
			}
			Particle p;
			const double speed = random.between(e.speed[0], e.speed[1]);
			if (local()) {
				p.position = offset;
				p.velocity = scale(local_direction, speed);
			} else {
				p.position = add(frame.origin, frame.rotate(offset));
				p.velocity = scale(frame.rotate(local_direction), speed);
			}
			p.life = random.between(e.lifetime[0], e.lifetime[1]);
			p.rotation = random.between(0.0, 2.0 * pi);
			p.spin = random.between(e.spin_degrees[0], e.spin_degrees[1]) * pi / 180.0;
			particles.push_back(p);
		}
	}

	void integrate(double dt)
	{
		const EffectData& e = *effect;
		const V3 acceleration = local() ? frame.unrotate(e.acceleration) : V3{e.acceleration[0], e.acceleration[1], e.acceleration[2]};
		const double keep = std::exp(-e.drag * dt);
		for (std::size_t i = 0; i < particles.size();) {
			Particle& p = particles[i];
			p.age += dt;
			if (p.age >= p.life) {
				p = particles.back();
				particles.pop_back();
				continue;
			}
			p.velocity = scale(add(p.velocity, scale(acceleration, dt)), keep);
			p.position = add(p.position, scale(p.velocity, dt));
			p.rotation += p.spin * dt;
			// Floors are world planes, so only particles kept in world space bounce.
			if (e.collide_floor && !local() && p.position[1] < e.floor_height && p.velocity[1] < 0) {
				p.position[1] = e.floor_height;
				p.velocity = scale({p.velocity[0], -p.velocity[1], p.velocity[2]}, e.bounce);
			}
			++i;
		}
	}

	void emit(double dt)
	{
		const EffectData& e = *effect;
		double remaining = dt;
		for (int cycles = 0; playing && cycles < max_cycles_per_update; ++cycles) {
			if (!burst_fired) {
				burst_fired = true;
				spawn(static_cast<std::size_t>(std::llround(e.burst * rate_scale)));
			}
			const double step = std::min(remaining, e.duration - cycle_time);
			carry += e.rate * rate_scale * step;
			const double whole = std::floor(carry);
			carry -= whole;
			spawn(static_cast<std::size_t>(whole));
			cycle_time += step;
			remaining -= step;
			if (cycle_time < e.duration) {
				break;
			}
			if (loop) {
				cycle_time = 0;
				burst_fired = false;
				if (remaining <= 0) {
					break;
				}
			} else {
				playing = false;
			}
		}
	}

	void update(double dt)
	{
		if (!effect) {
			return;
		}
		integrate(dt);
		emit(dt);
	}

	void append_views(std::vector<ParticleView>& out) const
	{
		const EffectData& e = *effect;
		for (const Particle& p : particles) {
			const double life = p.age / p.life;
			const V3 position = local() ? frame.to_world(p.position) : p.position;
			const V3 velocity = local() ? frame.rotate(p.velocity) : p.velocity;
			ParticleView v;
			v.position = {static_cast<float>(position[0]), static_cast<float>(position[1]), static_cast<float>(position[2])};
			v.velocity = {static_cast<float>(velocity[0]), static_cast<float>(velocity[1]), static_cast<float>(velocity[2])};
			v.size = ParticleSystem::size_at(e, life);
			v.rotation = static_cast<float>(std::fmod(p.rotation, 2.0 * pi));
			v.color = ParticleSystem::color_at(e, life);
			out.push_back(v);
		}
	}
};

} // namespace

struct ParticleSystem::Impl {
	std::uint64_t seed = 1;
	std::uint64_t fired = 0;
	std::map<std::uint64_t, Emitter> emitters;
	std::vector<Emitter> one_shots;

	Random random_for(std::uint64_t key, std::uint64_t salt) const { return Random{mix(mix(seed, key), salt)}; }
};

ParticleSystem::ParticleSystem(std::uint64_t seed) : impl_(std::make_unique<Impl>())
{
	impl_->seed = seed;
}

ParticleSystem::~ParticleSystem() = default;
ParticleSystem::ParticleSystem(ParticleSystem&&) noexcept = default;
ParticleSystem& ParticleSystem::operator=(ParticleSystem&&) noexcept = default;

void ParticleSystem::set_emitters(std::span<const EmitterInput> inputs)
{
	std::map<std::uint64_t, Emitter> next;
	for (const EmitterInput& in : inputs) {
		if (in.entity == 0 || next.contains(in.entity)) {
			continue;
		}
		Emitter emitter;
		const auto found = impl_->emitters.find(in.entity);
		const bool same_effect = found != impl_->emitters.end() && found->second.effect && in.effect && found->second.asset == in.asset &&
			found->second.revision == in.revision;
		if (same_effect) {
			emitter = std::move(found->second);
		} else {
			emitter.key = emitter.source = in.entity;
			emitter.random = impl_->random_for(in.entity, 0);
		}
		const bool was_auto = same_effect && emitter.auto_play;
		emitter.effect = in.effect;
		emitter.asset = in.asset;
		emitter.revision = in.revision;
		emitter.frame = Frame::from_matrix(in.world);
		emitter.auto_play = in.auto_play;
		emitter.loop = in.loop;
		emitter.rate_scale = std::isfinite(in.rate_scale) ? std::max(in.rate_scale, 0.0) : 0.0;
		if (!emitter.effect) {
			emitter.particles.clear();
			emitter.playing = false;
		} else if (emitter.auto_play && (!same_effect || !was_auto)) {
			emitter.start();
		} else if (!emitter.auto_play && was_auto) {
			emitter.playing = false; // Turned off: what is alive finishes its life.
		}
		next.emplace(in.entity, std::move(emitter));
	}
	impl_->emitters = std::move(next);
}

bool ParticleSystem::trigger(std::uint64_t entity)
{
	const auto found = impl_->emitters.find(entity);
	if (found == impl_->emitters.end() || !found->second.effect) {
		return false;
	}
	found->second.start();
	return true;
}

void ParticleSystem::fire(std::uint64_t source, std::shared_ptr<const EffectData> effect, const Point& point, const Point& direction,
	double strength)
{
	if (!effect || !std::isfinite(point[0]) || !std::isfinite(point[1]) || !std::isfinite(point[2])) {
		return;
	}
	if (impl_->one_shots.size() >= max_one_shots) {
		impl_->one_shots.erase(impl_->one_shots.begin());
	}
	Emitter shot;
	shot.key = one_shot_key(source);
	shot.source = source;
	shot.one_shot = true;
	shot.loop = false;
	shot.effect = std::move(effect);
	shot.frame = Frame::along({point[0], point[1], point[2]}, {direction[0], direction[1], direction[2]});
	shot.rate_scale = std::isfinite(strength) ? std::max(strength, 0.0) : 0.0;
	shot.random = impl_->random_for(source, ++impl_->fired);
	shot.start();
	impl_->one_shots.push_back(std::move(shot));
}

void ParticleSystem::update(double seconds)
{
	const double dt = std::isfinite(seconds) ? std::clamp(seconds, 0.0, 0.1) : 0.0;
	if (dt <= 0) {
		return;
	}
	for (auto& [key, emitter] : impl_->emitters) {
		(void)key;
		emitter.update(dt);
	}
	for (Emitter& shot : impl_->one_shots) {
		shot.update(dt);
	}
	std::erase_if(impl_->one_shots, [](const Emitter& shot) { return !shot.playing && shot.particles.empty(); });
}

void ParticleSystem::restart()
{
	impl_->one_shots.clear();
	impl_->fired = 0;
	for (auto& [key, emitter] : impl_->emitters) {
		emitter.particles.clear();
		emitter.random = impl_->random_for(key, 0);
		emitter.playing = false;
		if (emitter.effect && emitter.auto_play) {
			emitter.start();
		}
	}
}

std::vector<Batch> ParticleSystem::batches() const
{
	std::map<std::uint64_t, Batch> grouped;
	const auto collect = [&](const Emitter& emitter) {
		if (!emitter.effect || emitter.particles.empty()) {
			return;
		}
		Batch& batch = grouped[emitter.key];
		batch.key = emitter.key;
		batch.effect = emitter.effect;
		emitter.append_views(batch.particles);
	};
	for (const auto& [key, emitter] : impl_->emitters) {
		(void)key;
		collect(emitter);
	}
	for (const Emitter& shot : impl_->one_shots) {
		collect(shot);
	}
	std::vector<Batch> out;
	out.reserve(grouped.size());
	for (auto& [key, batch] : grouped) {
		(void)key;
		out.push_back(std::move(batch));
	}
	return out;
}

Stats ParticleSystem::stats() const
{
	Stats s;
	s.emitters = impl_->emitters.size();
	s.one_shots = impl_->one_shots.size();
	for (const auto& [key, emitter] : impl_->emitters) {
		(void)key;
		s.particles += emitter.particles.size();
	}
	for (const Emitter& shot : impl_->one_shots) {
		s.particles += shot.particles.size();
	}
	return s;
}

float ParticleSystem::size_at(const EffectData& effect, double life) noexcept
{
	const auto& keys = effect.size;
	if (keys.empty()) {
		return 0.0f;
	}
	life = std::clamp(life, 0.0, 1.0);
	if (life <= keys.front().time) {
		return static_cast<float>(keys.front().size);
	}
	for (std::size_t i = 1; i < keys.size(); ++i) {
		if (life <= keys[i].time) {
			const double t = (life - keys[i - 1].time) / (keys[i].time - keys[i - 1].time);
			return static_cast<float>(keys[i - 1].size + (keys[i].size - keys[i - 1].size) * t);
		}
	}
	return static_cast<float>(keys.back().size);
}

std::array<float, 4> ParticleSystem::color_at(const EffectData& effect, double life) noexcept
{
	const auto& keys = effect.color;
	if (keys.empty()) {
		return {0, 0, 0, 0};
	}
	const auto linear = [](const std::array<double, 4>& c) {
		return std::array<double, 4>{srgb_to_linear(c[0]), srgb_to_linear(c[1]), srgb_to_linear(c[2]), c[3]};
	};
	life = std::clamp(life, 0.0, 1.0);
	std::array<double, 4> c = linear(keys.back().color);
	if (life <= keys.front().time) {
		c = linear(keys.front().color);
	} else {
		for (std::size_t i = 1; i < keys.size(); ++i) {
			if (life <= keys[i].time) {
				// Interpolated in linear light, so a fade between two colors does not darken in the middle.
				const double t = (life - keys[i - 1].time) / (keys[i].time - keys[i - 1].time);
				const auto a = linear(keys[i - 1].color), b = linear(keys[i].color);
				for (int k = 0; k < 4; ++k) {
					c[k] = a[k] + (b[k] - a[k]) * t;
				}
				break;
			}
		}
	}
	return {static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2]), static_cast<float>(c[3])};
}

} // namespace engine::effects
