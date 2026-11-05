#pragma once

// Particle simulation on the CPU, visual only (Milestone 5, D8). It reads effect assets and emitter placements and
// produces particles for a renderer to draw. It never touches the scene, physics or gameplay, so a run plays the same
// with or without it. No Filament, Box3D or editor dependency. One thread: the caller's.

#include <engine/assets/asset_types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace engine::effects {

using Matrix = std::array<double, 16>; // Column-major, like engine::WorldTransform.
using Point = std::array<double, 3>;

// An emitter that follows an entity, as the host reads it from the scene each frame.
struct EmitterInput {
	std::uint64_t entity = 0;
	std::shared_ptr<const assets::EffectData> effect; // Null: the emitter stops and its particles are dropped.
	std::uint64_t asset = 0;                          // Another asset, or a new revision of it, starts the emitter again.
	std::uint32_t revision = 0;
	Matrix world{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	bool auto_play = true;
	bool loop = true;
	double rate_scale = 1;
};

// One particle, ready to draw. Color is linear and not premultiplied.
struct ParticleView {
	std::array<float, 3> position{};
	std::array<float, 3> velocity{}; // World space, for stretched particles.
	float size = 0;                  // Quad width in metres.
	float rotation = 0;              // Radians around the view direction.
	std::array<float, 4> color{};
};

// The particles of one emitter, or of every one-shot fired from one source.
struct Batch {
	std::uint64_t key = 0; // Stable while the batch lives: the entity, or one_shot_key(source).
	std::shared_ptr<const assets::EffectData> effect;
	std::vector<ParticleView> particles;
};

// Batch key for one-shots fired from `source`, distinct from every entity's own key.
constexpr std::uint64_t one_shot_key(std::uint64_t source) noexcept { return source | (std::uint64_t{1} << 63); }

struct Stats {
	std::size_t emitters = 0;  // Following entities.
	std::size_t one_shots = 0; // Fired and still alive.
	std::size_t particles = 0;
};

class ParticleSystem {
public:
	// The seed makes every run with the same inputs produce the same particles.
	explicit ParticleSystem(std::uint64_t seed = 1);
	~ParticleSystem();
	ParticleSystem(ParticleSystem&&) noexcept;
	ParticleSystem& operator=(ParticleSystem&&) noexcept;

	// The emitters that exist now. One missing from the list is removed with its particles; a new one starts at once
	// if it plays by itself. Entities must be unique and nonzero.
	void set_emitters(std::span<const EmitterInput> emitters);
	// Starts one cycle of an emitter that does not play by itself, or restarts one that does. False for an unknown entity.
	bool trigger(std::uint64_t entity);
	// Fires one cycle of an effect at a world point, spreading around `direction` (+Y when zero). `strength` multiplies
	// the number of particles; >= 0. Its particles are drawn in the batch of one_shot_key(source). At most 256 one-shots
	// live at once; the oldest goes first.
	void fire(std::uint64_t source, std::shared_ptr<const assets::EffectData> effect, const Point& point, const Point& direction,
		double strength = 1);
	// Advances by `seconds`, clamped to [0, 0.1] so a hitch cannot throw particles across the scene.
	void update(double seconds);
	// Removes every particle and one-shot; emitters start again from their beginning.
	void restart();

	// Ascending by key. Empty batches are left out.
	[[nodiscard]] std::vector<Batch> batches() const;
	[[nodiscard]] Stats stats() const;

	// Evaluated curves, exposed for tests and previews. `life` is clamped to [0, 1]; colors come back linear.
	[[nodiscard]] static float size_at(const assets::EffectData& effect, double life) noexcept;
	[[nodiscard]] static std::array<float, 4> color_at(const assets::EffectData& effect, double life) noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace engine::effects
