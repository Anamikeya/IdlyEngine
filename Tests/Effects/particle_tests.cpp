// Particle simulation without a window: emission, motion, curves, floors, one-shots, limits and determinism.
#include "engine/effects/particles.h"
#include "engine/test/test.h"

#include <cmath>
#include <memory>
#include <vector>

using namespace engine::effects;
using engine::assets::EffectData;
using engine::assets::ParticleBlend;

namespace {

std::shared_ptr<const EffectData> share(const EffectData& e) { return std::make_shared<const EffectData>(e); }

// No gravity, no spread, one speed: particles go straight up at 1 m/s and live 10 s.
EffectData steady()
{
	EffectData e;
	e.duration = 1;
	e.rate = 10;
	e.lifetime = {10, 10};
	e.speed = {1, 1};
	e.cone_degrees = 0;
	e.acceleration = {0, 0, 0};
	e.size = {{0, 0.5}};
	e.color = {{0, {1, 1, 1, 1}}};
	return e;
}

EmitterInput at(std::uint64_t entity, std::shared_ptr<const EffectData> effect, double x = 0, double y = 0, double z = 0)
{
	EmitterInput in;
	in.entity = entity;
	in.effect = std::move(effect);
	in.asset = 1;
	in.revision = 1;
	in.world[12] = x;
	in.world[13] = y;
	in.world[14] = z;
	return in;
}

// Steps of 1/16 s add up exactly in binary, so counts of emitted particles are exact.
constexpr double exact_step = 0.0625;

void run(ParticleSystem& system, double seconds, double step = 1.0 / 60.0)
{
	for (double t = 0; t < seconds - 1e-9; t += step) {
		system.update(step);
	}
}

std::size_t count(const ParticleSystem& system) { return system.stats().particles; }

} // namespace

ENGINE_TEST(emits_at_its_rate_and_bursts_once_a_cycle)
{
	auto e = steady();
	e.burst = 5;
	ParticleSystem system;
	const EmitterInput emitter = at(7, share(e));
	system.set_emitters({&emitter, 1});
	system.update(exact_step);
	ENGINE_CHECK_EQ(count(system), std::size_t{5}); // The burst, and not yet a whole particle from the rate.
	run(system, 1.0 - exact_step, exact_step);
	ENGINE_CHECK_EQ(count(system), std::size_t{15}); // 5 + 10 per second over the first second.
	run(system, 1.0, exact_step);
	ENGINE_CHECK_EQ(count(system), std::size_t{30}); // A second cycle: another burst and 10 more.
	ENGINE_CHECK_EQ(system.stats().emitters, std::size_t{1});
}

ENGINE_TEST(a_cycle_without_loop_stops_emitting)
{
	auto e = steady();
	ParticleSystem system;
	auto emitter = at(1, share(e));
	emitter.loop = false;
	system.set_emitters({&emitter, 1});
	run(system, 3.0, exact_step);
	ENGINE_CHECK_EQ(count(system), std::size_t{10});
}

ENGINE_TEST(particles_move_age_and_die)
{
	auto e = steady();
	e.lifetime = {0.5, 0.5};
	e.burst = 1;
	e.rate = 0;
	ParticleSystem system;
	auto emitter = at(1, share(e), 2, 3, 4);
	emitter.loop = false;
	system.set_emitters({&emitter, 1});
	system.update(0.001); // Born at the emitter.
	run(system, 0.25, 0.05);
	const auto batches = system.batches();
	ENGINE_REQUIRE(batches.size() == 1 && batches[0].particles.size() == 1);
	const auto& p = batches[0].particles[0];
	ENGINE_CHECK_NEAR(p.position[0], 2.0, 1e-5);
	ENGINE_CHECK_NEAR(p.position[1], 3.25, 1e-3); // 1 m/s up for 0.25 s.
	ENGINE_CHECK_NEAR(p.velocity[1], 1.0, 1e-6);
	ENGINE_CHECK_EQ(batches[0].key, std::uint64_t{1});
	run(system, 0.3, 0.05);
	ENGINE_CHECK_EQ(count(system), std::size_t{0});
	ENGINE_CHECK(system.batches().empty());
}

ENGINE_TEST(gravity_and_drag_shape_the_motion)
{
	auto e = steady();
	e.rate = 0;
	e.burst = 1;
	e.speed = {0, 0};
	e.acceleration = {0, -10, 0};
	ParticleSystem system;
	auto emitter = at(1, share(e));
	emitter.loop = false;
	system.set_emitters({&emitter, 1});
	system.update(0.001);
	run(system, 1.0, 0.01);
	auto p = system.batches()[0].particles[0];
	ENGINE_CHECK_NEAR(p.velocity[1], -10.0, 0.05);
	ENGINE_CHECK_NEAR(p.position[1], -5.0, 0.1); // Half g t squared, to the step's precision.

	e.drag = 2;
	ParticleSystem dragged;
	auto slowed = at(1, share(e));
	slowed.loop = false;
	dragged.set_emitters({&slowed, 1});
	dragged.update(0.001);
	run(dragged, 3.0, 0.01);
	// With drag k, velocity tends to g / k.
	ENGINE_CHECK_NEAR(dragged.batches()[0].particles[0].velocity[1], -5.0, 0.1);
}

ENGINE_TEST(spread_stays_inside_the_cone_and_speed_range)
{
	auto e = steady();
	e.rate = 0;
	e.burst = 2000;
	e.max_particles = 2000;
	e.cone_degrees = 30;
	e.speed = {2, 4};
	ParticleSystem system;
	const auto emitter = at(1, share(e));
	system.set_emitters({&emitter, 1});
	system.update(0.001);
	const auto particles = system.batches()[0].particles;
	ENGINE_REQUIRE(particles.size() == 2000);
	const double cos_cone = std::cos(30.0 * 3.14159265358979 / 180.0);
	bool inside = true;
	double sideways = 0;
	for (const auto& p : particles) {
		const double speed = std::sqrt(p.velocity[0] * p.velocity[0] + p.velocity[1] * p.velocity[1] + p.velocity[2] * p.velocity[2]);
		inside = inside && speed >= 2 - 1e-4 && speed <= 4 + 1e-4 && p.velocity[1] / speed >= cos_cone - 1e-5;
		sideways += p.velocity[0];
	}
	ENGINE_CHECK(inside);
	ENGINE_CHECK(std::abs(sideways / 2000) < 0.1); // No side is favoured.
}

ENGINE_TEST(a_rotated_emitter_sprays_along_its_own_up)
{
	auto e = steady();
	e.rate = 0;
	e.burst = 1;
	ParticleSystem system;
	auto emitter = at(1, share(e));
	// Turned so its +Y points along world +X.
	emitter.world = {0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	emitter.loop = false;
	system.set_emitters({&emitter, 1});
	system.update(0.001);
	const auto v = system.batches()[0].particles[0].velocity;
	ENGINE_CHECK_NEAR(v[0], 1.0, 1e-6);
	ENGINE_CHECK_NEAR(v[1], 0.0, 1e-6);
}

ENGINE_TEST(local_space_particles_move_with_the_emitter)
{
	auto e = steady();
	e.rate = 0;
	e.burst = 1;
	e.speed = {0, 0};
	ParticleSystem world_space, local_space;
	auto emitter = at(1, share(e));
	emitter.loop = false;
	world_space.set_emitters({&emitter, 1});
	world_space.update(0.001);
	e.world_space = false;
	auto local = at(1, share(e));
	local.loop = false;
	local_space.set_emitters({&local, 1});
	local_space.update(0.001);
	emitter.world[12] = local.world[12] = 5;
	world_space.set_emitters({&emitter, 1});
	local_space.set_emitters({&local, 1});
	ENGINE_CHECK_NEAR(world_space.batches()[0].particles[0].position[0], 0.0, 1e-6);
	ENGINE_CHECK_NEAR(local_space.batches()[0].particles[0].position[0], 5.0, 1e-6);
}

ENGINE_TEST(particles_bounce_on_the_floor)
{
	auto e = steady();
	e.rate = 0;
	e.burst = 1;
	e.speed = {0, 0};
	e.acceleration = {0, -10, 0};
	e.collide_floor = true;
	e.floor_height = -1;
	e.bounce = 0.5;
	ParticleSystem system;
	auto emitter = at(1, share(e));
	emitter.loop = false;
	system.set_emitters({&emitter, 1});
	system.update(0.001);
	double lowest = 0;
	bool rose = false;
	for (int i = 0; i < 200; ++i) {
		system.update(0.005);
		const auto p = system.batches()[0].particles[0];
		lowest = std::min(lowest, static_cast<double>(p.position[1]));
		rose = rose || (p.position[1] > -1.0 && p.velocity[1] > 0 && lowest <= -1.0 + 1e-6);
	}
	ENGINE_CHECK(lowest >= -1.0 - 1e-6); // Never below the floor.
	ENGINE_CHECK(rose);                   // It came back up.
	e.bounce = 0;
	ParticleSystem resting;
	auto still = at(1, share(e));
	still.loop = false;
	resting.set_emitters({&still, 1});
	resting.update(0.001);
	run(resting, 2.0, 0.01);
	const auto p = resting.batches()[0].particles[0];
	ENGINE_CHECK_NEAR(p.position[1], -1.0, 0.002); // With no bounce it stays on the floor.
}

ENGINE_TEST(size_and_color_follow_their_keys)
{
	EffectData e;
	e.size = {{0, 1}, {0.5, 3}, {1, 0}};
	ENGINE_CHECK_NEAR(ParticleSystem::size_at(e, 0), 1.0, 1e-6);
	ENGINE_CHECK_NEAR(ParticleSystem::size_at(e, 0.25), 2.0, 1e-6);
	ENGINE_CHECK_NEAR(ParticleSystem::size_at(e, 0.75), 1.5, 1e-6);
	ENGINE_CHECK_NEAR(ParticleSystem::size_at(e, 2), 0.0, 1e-6);
	e.color = {{0.5, {1, 0.5, 0, 1}}, {1, {0, 0, 0, 0}}};
	const auto early = ParticleSystem::color_at(e, 0.1); // Before the first key: its value.
	ENGINE_CHECK_NEAR(early[0], 1.0, 1e-6);
	ENGINE_CHECK_NEAR(early[1], 0.214041, 1e-5); // sRGB 0.5 in linear light.
	ENGINE_CHECK_NEAR(early[3], 1.0, 1e-6);
	const auto middle = ParticleSystem::color_at(e, 0.75);
	ENGINE_CHECK_NEAR(middle[0], 0.5, 1e-6); // Halfway between, in linear light.
	ENGINE_CHECK_NEAR(middle[3], 0.5, 1e-6);
}

ENGINE_TEST(max_particles_caps_what_is_alive)
{
	auto e = steady();
	e.rate = 1000;
	e.max_particles = 50;
	ParticleSystem system;
	const auto emitter = at(1, share(e));
	system.set_emitters({&emitter, 1});
	run(system, 1.0);
	ENGINE_CHECK_EQ(count(system), std::size_t{50});
}

ENGINE_TEST(one_shots_fire_along_a_direction_and_go_away)
{
	auto e = steady();
	e.rate = 0;
	e.burst = 8;
	e.duration = 0.1; // A one-shot lives for one cycle, and while its particles do.
	e.lifetime = {0.2, 0.2};
	ParticleSystem system;
	system.fire(42, share(e), {1, 2, 3}, {0, 0, -2}, 1);
	system.fire(42, share(e), {1, 2, 3}, {0, 0, -1}, 0.5);
	system.update(0.01);
	ENGINE_CHECK_EQ(system.stats().one_shots, std::size_t{2});
	const auto batches = system.batches();
	ENGINE_REQUIRE(batches.size() == 1); // Both in the batch of their source.
	ENGINE_CHECK_EQ(batches[0].key, one_shot_key(42));
	ENGINE_CHECK_EQ(batches[0].particles.size(), std::size_t{12}); // 8, then 8 at half strength.
	ENGINE_CHECK_NEAR(batches[0].particles[0].velocity[2], -1.0, 1e-6);
	run(system, 0.3, 0.05);
	ENGINE_CHECK_EQ(system.stats().one_shots, std::size_t{0});
	ENGINE_CHECK(system.batches().empty());
	for (int i = 0; i < 300; ++i) {
		system.fire(1, share(e), {0, 0, 0}, {0, 1, 0});
	}
	ENGINE_CHECK_EQ(system.stats().one_shots, std::size_t{256});
}

ENGINE_TEST(an_emitter_that_waits_plays_when_triggered)
{
	auto e = steady();
	ParticleSystem system;
	auto emitter = at(3, share(e));
	emitter.auto_play = false;
	emitter.loop = false;
	system.set_emitters({&emitter, 1});
	run(system, 0.5, 0.05);
	ENGINE_CHECK_EQ(count(system), std::size_t{0});
	ENGINE_CHECK(system.trigger(3));
	ENGINE_CHECK(!system.trigger(4));
	run(system, 1.5, exact_step);
	ENGINE_CHECK_EQ(count(system), std::size_t{10});
}

ENGINE_TEST(emitters_follow_the_list_and_restart_on_a_new_effect)
{
	auto e = steady();
	ParticleSystem system;
	auto a = at(1, share(e));
	auto b = at(2, share(e));
	const EmitterInput both[] = {a, b};
	system.set_emitters(both);
	run(system, 0.5, exact_step);
	ENGINE_CHECK_EQ(count(system), std::size_t{10});
	system.set_emitters({&a, 1}); // b is gone, with its particles.
	ENGINE_CHECK_EQ(count(system), std::size_t{5});
	a.effect = share(e); // A reloaded copy of the same revision keeps what is alive.
	system.set_emitters({&a, 1});
	ENGINE_CHECK_EQ(count(system), std::size_t{5});
	a.revision = 2; // An edited effect starts again.
	system.set_emitters({&a, 1});
	ENGINE_CHECK_EQ(count(system), std::size_t{0});
	a.effect = nullptr;
	system.set_emitters({&a, 1});
	run(system, 0.5, 0.05);
	ENGINE_CHECK_EQ(count(system), std::size_t{0});
}

ENGINE_TEST(the_same_inputs_give_the_same_particles)
{
	auto e = steady();
	e.cone_degrees = 90;
	e.speed = {1, 5};
	e.lifetime = {0.5, 2};
	e.spin_degrees = {-90, 90};
	e.spawn_radius = 0.3;
	const auto shared = share(e);
	const auto simulate = [&](std::uint64_t seed) {
		ParticleSystem system(seed);
		const auto emitter = at(9, shared);
		system.set_emitters({&emitter, 1});
		system.fire(9, shared, {0, 1, 0}, {1, 0, 0});
		run(system, 1.5);
		return system.batches();
	};
	const auto first = simulate(5), second = simulate(5), other = simulate(6);
	ENGINE_REQUIRE(first.size() == second.size() && first.size() == 2);
	bool same = true, differs = false;
	for (std::size_t b = 0; b < first.size(); ++b) {
		same = same && first[b].particles.size() == second[b].particles.size();
		for (std::size_t i = 0; same && i < first[b].particles.size(); ++i) {
			same = first[b].particles[i].position == second[b].particles[i].position &&
				first[b].particles[i].color == second[b].particles[i].color;
		}
	}
	differs = other[0].particles.empty() || other[0].particles[0].position != first[0].particles[0].position;
	ENGINE_CHECK(same);
	ENGINE_CHECK(differs);
	// Restart puts it back at the beginning, the same way.
	ParticleSystem system(5);
	const auto emitter = at(9, shared);
	system.set_emitters({&emitter, 1});
	run(system, 0.7);
	const auto before = system.batches();
	run(system, 0.4);
	system.restart();
	run(system, 0.7);
	const auto after = system.batches();
	ENGINE_REQUIRE(!before.empty() && !after.empty());
	ENGINE_CHECK(before[0].particles.size() == after[0].particles.size());
	ENGINE_CHECK(before[0].particles[0].position == after[0].particles[0].position);
}

ENGINE_TEST(bad_input_is_ignored)
{
	auto e = steady();
	ParticleSystem system;
	auto zero = at(0, share(e));
	system.set_emitters({&zero, 1});
	ENGINE_CHECK_EQ(system.stats().emitters, std::size_t{0});
	system.fire(1, nullptr, {0, 0, 0}, {0, 1, 0});
	system.fire(1, share(e), {std::nan(""), 0, 0}, {0, 1, 0});
	ENGINE_CHECK_EQ(system.stats().one_shots, std::size_t{0});
	system.update(-1);
	system.update(std::nan(""));
	auto emitter = at(1, share(e));
	system.set_emitters({&emitter, 1});
	system.update(100); // Clamped to one tenth of a second.
	ENGINE_CHECK_EQ(count(system), std::size_t{1});
}
