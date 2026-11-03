// Particle effects in the editor, without a window: effect assets, emitters, the edit-mode preview, drafts, undo,
// placing, renaming, Play mode and frames. Real Scene, CommandBus, AssetDatabase, particle system and EditorSession.
#include "engine/test/test.h"

#include <engine/assets/asset_database.h>
#include <engine/assets/importers.h>
#include <engine/editor/EditorSession.hpp>
#include <engine/editor/PropertyMeta.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::editor;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {
fs::path fresh_project(const std::string& name) {
	const auto folder = fs::temp_directory_path() / ("engine-effects-" + name);
	fs::remove_all(folder);
	fs::create_directories(folder);
	return folder;
}
// No gravity and a long life: particles stay where they can be counted.
assets::EffectData fountain() {
	assets::EffectData e;
	e.rate = 20;
	e.lifetime = {10, 10};
	e.acceleration = {0, 0, 0};
	return e;
}
renderer::Frame frame_of(EditorSession& s) {
	const auto f = s.frame(1.0);
	ENGINE_REQUIRE(f);
	return f.value();
}
std::size_t drawn(const renderer::FrameSnapshot& f) {
	std::size_t count = 0;
	for (const auto& batch : f.particles) count += batch.particles.size();
	return count;
}
// One frame to pick the emitters up, then `seconds` of preview in 1/16 s steps, then the frame that draws them.
std::size_t play_for(EditorSession& s, double seconds) {
	(void)frame_of(s);
	for (double t = 0; t < seconds - 1e-9; t += 0.0625) s.update(62'500'000ns);
	return drawn(*frame_of(s));
}
// By value: the check macros keep references, which must not point into a temporary record.
ParticleEmitterComponent emitter_of(const EditorSession& s, EntityId id) {
	const auto record = s.scene().inspect(id);
	ENGINE_REQUIRE(record && record->components.emitter);
	return *record->components.emitter;
}
struct Project {
	EditorSession s;
	assets::AssetId effect;
	EntityId emitter = 0;
	explicit Project(const std::string& name, const assets::EffectData& data = fountain()) {
		ENGINE_REQUIRE(s.open_project(fresh_project(name)));
		const auto created = s.create_effect("fountain", data);
		ENGINE_REQUIRE(created);
		effect = created.value();
		const auto placed = s.place_asset_at(effect, {1, 0, 2});
		ENGINE_REQUIRE(placed);
		emitter = placed.value().created.front();
	}
};
} // namespace

ENGINE_TEST(placing_an_effect_adds_an_emitter_that_plays_while_editing) {
	Project p("place");
	const auto record = p.s.scene().inspect(p.emitter);
	ENGINE_REQUIRE(record && record->components.emitter);
	ENGINE_CHECK_EQ(record->name, std::string("fountain"));
	ENGINE_CHECK_EQ(record->components.emitter->effect.id, p.effect.value);
	ENGINE_CHECK_EQ(record->components.emitter->effect.path, std::string("effects/fountain.effect"));
	ENGINE_CHECK_NEAR(record->transform.position.y, 0.05, 1e-9);
	ENGINE_CHECK(fs::exists(p.s.asset_folder() / "effects" / "fountain.effect"));
	const auto revision = p.s.scene_revision();
	ENGINE_CHECK_EQ(play_for(p.s, 1.0), std::size_t{20});
	ENGINE_CHECK_EQ(p.s.scene_revision(), revision); // The preview never changes the scene.
	const auto f = frame_of(p.s);
	ENGINE_REQUIRE(f->particles.size() == 1);
	ENGINE_CHECK_EQ(f->particles[0].key, p.emitter);
	ENGINE_CHECK(f->particles[0].blend == renderer::ParticleBlendMode::Additive);
	ENGINE_CHECK_EQ(p.s.effect_stats().emitters, std::size_t{1});
	ENGINE_CHECK(p.s.asset_problems().empty());
}

ENGINE_TEST(turning_the_preview_off_or_hiding_the_emitter_stops_it) {
	Project p("preview_off");
	ENGINE_CHECK(play_for(p.s, 0.5) > 0);
	p.s.view_flags.effects = false;
	ENGINE_CHECK_EQ(play_for(p.s, 0.5), std::size_t{0});
	p.s.view_flags.effects = true;
	p.s.set_hidden(p.emitter, true);
	ENGINE_CHECK_EQ(play_for(p.s, 0.5), std::size_t{0});
	p.s.set_hidden(p.emitter, false);
	ENGINE_CHECK(play_for(p.s, 0.5) > 0);
}

ENGINE_TEST(a_draft_shows_at_once_and_saving_is_one_undo_step) {
	Project p("draft");
	ENGINE_CHECK_EQ(play_for(p.s, 0.5), std::size_t{10});
	auto edited = fountain();
	edited.size = {{0, 2.5}};
	ENGINE_REQUIRE(p.s.preview_effect(p.effect, edited));
	const auto f = frame_of(p.s);
	ENGINE_REQUIRE(!f->particles.empty());
	ENGINE_CHECK_EQ(drawn(*f), std::size_t{10}); // The living particles stay ...
	ENGINE_CHECK_NEAR(f->particles[0].particles[0].size, 2.5, 1e-6); // ... and take the new size.
	ENGINE_CHECK(p.s.effect(p.effect)->size == edited.size);
	auto invalid = edited;
	invalid.max_particles = 0;
	ENGINE_CHECK(!p.s.preview_effect(p.effect, invalid));
	const auto before = p.s.history_position();
	ENGINE_REQUIRE(p.s.save_effect(p.effect, edited));
	ENGINE_CHECK_EQ(p.s.history_position(), before + 1);
	ENGINE_CHECK_EQ(play_for(p.s, 0.0), std::size_t{0}); // A saved change starts the emitters again.
	ENGINE_REQUIRE(p.s.undo());
	ENGINE_CHECK(p.s.effect(p.effect)->size == fountain().size);
	ENGINE_REQUIRE(p.s.redo());
	ENGINE_CHECK(p.s.effect(p.effect)->size == edited.size);
	const auto from_disk = assets::parse_effect(
		[&] { std::ifstream in(p.s.asset_folder() / "effects" / "fountain.effect"); return std::string(std::istreambuf_iterator<char>(in), {}); }());
	ENGINE_REQUIRE(from_disk);
	ENGINE_CHECK(from_disk.value() == edited);
	// Discarding a draft goes back to the file.
	auto other = edited;
	other.rate = 1;
	ENGINE_REQUIRE(p.s.preview_effect(p.effect, other));
	p.s.discard_effect_draft(p.effect);
	ENGINE_CHECK_EQ(p.s.effect(p.effect)->rate, edited.rate);
}

ENGINE_TEST(assigning_an_effect_gives_an_entity_an_emitter) {
	Project p("assign");
	const auto created = p.s.execute(commands::CreateEntity{"Torch", {}, 0, 0, Components{MeshComponent{}, {}, {}}});
	ENGINE_REQUIRE(created);
	const auto torch = created.value().created.front();
	ENGINE_REQUIRE(p.s.assign_effect(torch, p.effect));
	ENGINE_CHECK_EQ(emitter_of(p.s, torch).effect.id, p.effect.value);
	ENGINE_CHECK(p.s.scene().inspect(torch)->components.mesh.has_value()); // Nothing else changes.
	ENGINE_REQUIRE(p.s.undo());
	ENGINE_CHECK(!p.s.scene().inspect(torch)->components.emitter);
	const auto material = p.s.create_material("not_an_effect");
	ENGINE_REQUIRE(material);
	ENGINE_CHECK(!p.s.assign_effect(torch, material.value()));
	ENGINE_CHECK(!p.s.assign_effect(torch, assets::AssetId{12345}));
}

ENGINE_TEST(renaming_an_effect_updates_its_emitters) {
	Project p("rename");
	ENGINE_REQUIRE(p.s.rename_asset(p.effect, "fx/water.effect"));
	ENGINE_CHECK_EQ(emitter_of(p.s, p.emitter).effect.path, std::string("fx/water.effect"));
	ENGINE_CHECK(play_for(p.s, 0.5) > 0);
	ENGINE_CHECK(p.s.asset_problems().empty());
}

ENGINE_TEST(a_missing_effect_is_reported_and_draws_nothing) {
	Project p("missing");
	auto emitter = emitter_of(p.s, p.emitter);
	emitter.effect = {0x1234, "effects/gone.effect"};
	ENGINE_REQUIRE(p.s.execute(commands::SetParticleEmitter{p.emitter, emitter}));
	ENGINE_CHECK_EQ(play_for(p.s, 0.5), std::size_t{0});
	ENGINE_REQUIRE(!p.s.asset_problems().empty());
	ENGINE_CHECK(p.s.asset_problems().front().find("gone.effect") != std::string::npos);
}

ENGINE_TEST(play_has_its_own_particles_and_stop_throws_them_away) {
	Project p("play");
	(void)play_for(p.s, 0.5);
	ENGINE_REQUIRE(p.s.play());
	ENGINE_CHECK_EQ(drawn(*frame_of(p.s)), std::size_t{0}); // A fresh system, from the start.
	for (int i = 0; i < 16; ++i) p.s.update(62'500'000ns);
	const auto playing = drawn(*frame_of(p.s));
	ENGINE_CHECK_EQ(playing, std::size_t{20});
	ENGINE_REQUIRE(p.s.pause());
	for (int i = 0; i < 16; ++i) p.s.update(62'500'000ns);
	ENGINE_CHECK_EQ(drawn(*frame_of(p.s)), playing); // Paused holds still.
	ENGINE_REQUIRE(p.s.step());
	ENGINE_REQUIRE(p.s.stop());
	ENGINE_CHECK_EQ(drawn(*frame_of(p.s)), std::size_t{10}); // The editor's preview, where it was.
}

ENGINE_TEST(restart_starts_the_shown_emitters_again) {
	Project p("restart");
	ENGINE_CHECK_EQ(play_for(p.s, 1.0), std::size_t{20});
	p.s.restart_effects();
	ENGINE_CHECK_EQ(drawn(*frame_of(p.s)), std::size_t{0});
}

ENGINE_TEST(an_effect_with_an_image_draws_with_it) {
	Project p("image");
	const fs::path image = p.s.project() / "dot.ppm";
	{
		std::ofstream out(image, std::ios::binary);
		out << "P6\n2 2\n255\n";
		for (int i = 0; i < 4; ++i) out.write("\xFF\xFF\xFF", 3);
	}
	const auto imported = p.s.import_asset(image);
	ENGINE_REQUIRE(imported);
	auto data = fountain();
	data.blend = assets::ParticleBlend::Alpha;
	data.texture = {imported.value().id, imported.value().path};
	ENGINE_REQUIRE(p.s.save_effect(p.effect, data));
	(void)play_for(p.s, 0.5);
	const auto f = frame_of(p.s);
	ENGINE_REQUIRE(f->particles.size() == 1);
	ENGINE_CHECK(f->particles[0].texture.index != 0);
	ENGINE_CHECK(f->particles[0].blend == renderer::ParticleBlendMode::Alpha);
	// Neither the effect nor its image is released while an emitter uses them.
	(void)p.s.release_unused_assets();
	ENGINE_CHECK(frame_of(p.s)->particles[0].texture.index != 0);
}

ENGINE_TEST(the_emitter_has_a_details_table_and_copies_as_text) {
	const auto& table = emitter_properties();
	ENGINE_CHECK_EQ(std::string(table.name), std::string("Particle Emitter"));
	for (const char* id : {"effect", "auto_play", "loop", "rate_scale"}) ENGINE_CHECK(table.find(id) != nullptr);
	ENGINE_CHECK_EQ(std::string(table.find("effect")->asset_type), std::string("effect"));
	ParticleEmitterComponent original{{7, "effects/a.effect"}, false, true, 3};
	ParticleEmitterComponent copy;
	ENGINE_REQUIRE(component_from_text(table, component_to_text(table, original), copy));
	ENGINE_CHECK(copy == original);
}

ENGINE_TEST(frames_drop_particles_that_cannot_be_drawn) {
	std::vector<renderer::RenderParticles> batches(3);
	batches[0].key = 5;
	batches[0].particles = {{{0, 0, 0}, {}, 1, 0, {1, 1, 1, 1}}, {{std::nanf(""), 0, 0}, {}, 1, 0, {1, 1, 1, 1}}, {{0, 0, 0}, {}, -1, 0, {1, 1, 1, 1}}};
	batches[1].key = 2;
	batches[1].particles = {{{0, 0, 0}, {}, 1, 0, {1, 1, 1, 1}}};
	batches[2].key = 5; // A repeated key.
	batches[2].particles = batches[1].particles;
	const auto clean = renderer::clean_particles(batches);
	ENGINE_REQUIRE(clean.size() == 2);
	ENGINE_CHECK_EQ(clean[0].key, std::uint64_t{2});
	ENGINE_CHECK_EQ(clean[1].key, std::uint64_t{5});
	ENGINE_CHECK_EQ(clean[1].particles.size(), std::size_t{1});
	std::vector<renderer::RenderParticles> huge(1);
	huge[0].particles.resize(renderer::max_frame_particles + 10, renderer::RenderParticle{{0, 0, 0}, {}, 1, 0, {1, 1, 1, 1}});
	ENGINE_CHECK_EQ(renderer::clean_particles(huge)[0].particles.size(), renderer::max_frame_particles);
}
