// Milestone 5 slice 5 in the editor, without a window: the Physics Shooter sample in Play, with the session carrying out
// what the game asks for. Enemies walk and flinch through their animators and fall as ragdolls; footsteps raise dust;
// shots flash at the muzzle; glass breaks into debris drawn like its shard. Real Scene, CommandBus, asset database,
// Box3D, ozz and EditorSession, on the sample's own files. Nothing reaches the editor scene.
#include "engine/test/test.h"

#include <engine/editor/EditorSession.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::editor;

namespace {

const std::filesystem::path sample = std::filesystem::path(GAMEENGINE_TEST_SAMPLES_DIR) / "PhysicsShooter";

// The sample in Play, as the editor runs it: a frame drawn between ticks.
struct Shooter {
	EditorSession s;
	SceneSnapshot authored;
	std::uint64_t revision = 0;
	gameplay::Game* game = nullptr;
	double yaw = 0, pitch = 0;

	explicit Shooter(const char* scene = "physics_shooter.scene")
	{
		ENGINE_REQUIRE(s.open_project(sample));
		ENGINE_REQUIRE(s.load(sample / scene));
		authored = s.editor_scene().snapshot();
		revision = s.commands().revision();
		ENGINE_REQUIRE(s.play());
		(void)frame();
		game = s.game();
		ENGINE_REQUIRE(game != nullptr);
		ENGINE_CHECK(game->setup_issues().empty());
		game->start();
	}
	renderer::Frame frame()
	{
		const auto f = s.frame(1.6);
		ENGINE_REQUIRE(f);
		return f.value();
	}
	void tick(const gameplay::InputFrame& input = {})
	{
		(void)frame();
		game->set_input(input);
		s.update(EditorSession::fixed_step);
	}
	void ticks(int n, const gameplay::InputFrame& input = {})
	{
		for (int i = 0; i < n; ++i) tick(input);
	}
	EntityId find(const std::string& name) const
	{
		for (const auto& e : s.scene().snapshot().entities)
			if (e.name == name) return e.id;
		return 0;
	}
	Vec3 position(EntityId id) const
	{
		const auto w = s.scene().world_transform(id);
		ENGINE_REQUIRE(w.has_value());
		return {w->matrix[12], w->matrix[13], w->matrix[14]};
	}
	void stand(Vec3 at)
	{
		Transform t;
		t.position = at;
		ENGINE_REQUIRE(s.execute(commands::SetTransform{game->player(), t}));
		tick();
	}
	// Turns the view onto a point, as moving the mouse would.
	void aim(Vec3 point)
	{
		const Vec3 eye = position(game->camera());
		const double dx = point.x - eye.x, dy = point.y - eye.y, dz = point.z - eye.z;
		const double want_yaw = std::atan2(-dx, -dz), want_pitch = std::atan2(dy, std::hypot(dx, dz));
		game->look((yaw - want_yaw) / 0.0022, (pitch - want_pitch) / 0.0022);
		yaw = want_yaw;
		pitch = want_pitch;
		game->update_view();
	}
	static gameplay::InputFrame firing()
	{
		gameplay::InputFrame input;
		input.held[std::size_t(gameplay::Action::Fire)] = true;
		return input;
	}
	// The highest point the entity's skeleton reaches as drawn.
	double skeleton_top(EntityId id) const
	{
		double top = -1e9;
		for (const auto& line : s.bone_lines(id)) top = std::max({top, line.from.y, line.to.y});
		return top;
	}
	void stop_and_check_the_editor_scene()
	{
		ENGINE_REQUIRE(s.stop());
		ENGINE_CHECK(s.editor_scene().snapshot() == authored);
		ENGINE_CHECK_EQ(s.commands().revision(), revision);
		ENGINE_CHECK(!s.dirty());
		ENGINE_CHECK(s.ragdolled_entities().empty() && s.debris().empty());
	}
};

} // namespace

ENGINE_TEST(sample_enemies_walk_flinch_and_fall_as_ragdolls_in_play)
{
	Shooter play;
	auto& s = play.s;
	const EntityId enemy = play.find("Enemy 2");
	ENGINE_REQUIRE(enemy != 0);
	play.ticks(40);
	const auto walking = s.animation().status(enemy);
	ENGINE_REQUIRE(walking.has_value());
	ENGINE_CHECK_EQ(std::string(walking->clip), std::string("Walk_Loop"));
	const double standing_top = play.skeleton_top(enemy);
	ENGINE_CHECK(standing_top > 1.5); // A 1.8 m mannequin, upright.
	// Shot in the chest from just past the glass: it flinches on top of its walk, then falls.
	play.stand({0, 0.9, 1.5});
	bool flinched = false;
	for (int i = 0; i < 200 && !s.ragdolled(enemy); ++i) {
		const Vec3 p = play.position(enemy);
		play.aim({p.x, 0.92, p.z});
		play.tick(Shooter::firing());
		if (const auto status = s.animation().status(enemy); status && std::string(status->layered).find("Hit") != std::string::npos) flinched = true;
	}
	ENGINE_CHECK(flinched);
	ENGINE_REQUIRE(s.ragdolled(enemy));
	ENGINE_CHECK(s.game_issues().empty());
	ENGINE_CHECK(s.scene().inspect(enemy).has_value());
	ENGINE_CHECK(s.physics_bridge().ragdoll_of(enemy) != 0);
	// It collapses where it stood, and stays down.
	play.ticks(150);
	const double fallen_top = play.skeleton_top(enemy);
	ENGINE_CHECK(fallen_top < 0.8);
	ENGINE_CHECK(fallen_top > 0.05);
	const auto standing = play.game->standing_targets();
	ENGINE_CHECK(std::find(standing.begin(), standing.end(), enemy) == standing.end());
	// Its capsule sat out: shots now go through to whatever is behind, or push the body.
	ENGINE_CHECK_EQ(play.game->counters().ragdolls, std::uint64_t{1});
	play.stop_and_check_the_editor_scene();
}

ENGINE_TEST(sample_enemies_stand_idle_at_the_ends_of_their_patrols)
{
	Shooter play;
	auto& s = play.s;
	const EntityId enemy = play.find("Enemy 2");
	ENGINE_REQUIRE(enemy != 0);
	// Six metres at a walk, then it stands: in its idle clip, not the crouching one that also has "idle" in its name.
	std::string clip = "Walk_Loop";
	for (int i = 0; i < 600 && clip == "Walk_Loop"; ++i) {
		play.tick();
		if (const auto status = s.animation().status(enemy)) clip = status->clip;
	}
	ENGINE_CHECK_EQ(clip, std::string("Idle_Loop"));
	ENGINE_CHECK_NEAR(play.position(enemy).z, -9.0, 0.05);
	for (int i = 0; i < 200 && clip == "Idle_Loop"; ++i) {
		play.tick();
		if (const auto status = s.animation().status(enemy)) clip = status->clip;
	}
	ENGINE_CHECK_EQ(clip, std::string("Walk_Loop"));
	play.stop_and_check_the_editor_scene();
}

ENGINE_TEST(sample_footsteps_raise_dust_and_shots_flash_at_the_muzzle)
{
	Shooter play;
	auto& s = play.s;
	std::size_t most = 0;
	for (int i = 0; i < 150; ++i) {
		play.tick();
		most = std::max(most, s.effect_stats().particles);
	}
	// Three enemies walking for 2.5 s: about two steps a second each.
	ENGINE_CHECK(play.game->counters().footsteps >= 9);
	ENGINE_CHECK(most > 0);
	// Up into the sky, so only the flash shows.
	const Vec3 eye = play.position(play.game->camera());
	play.aim({eye.x, eye.y + 50, eye.z - 10});
	const auto before = play.game->counters().muzzle_flashes;
	std::uint64_t shots = 0;
	std::size_t flashing = 0;
	for (int i = 0; i < 60; ++i) {
		play.tick(Shooter::firing());
		shots += play.game->shots().size();
		flashing = std::max(flashing, s.effect_stats().one_shots);
	}
	ENGINE_CHECK(shots >= 5);
	ENGINE_CHECK_EQ(play.game->counters().muzzle_flashes - before, shots);
	ENGINE_CHECK(flashing > 0);
	play.stop_and_check_the_editor_scene();
}

ENGINE_TEST(sample_glass_breaks_into_debris_drawn_like_its_shard)
{
	Shooter play;
	auto& s = play.s;
	const EntityId glass = play.find("Glass Barrier"), shard = play.find("Glass Shard Template");
	ENGINE_REQUIRE(glass != 0 && shard != 0);
	play.ticks(10);
	play.aim(play.position(glass));
	for (int i = 0; i < 60 && s.scene().inspect(glass); ++i) play.tick(Shooter::firing());
	play.tick();
	ENGINE_CHECK(!s.scene().inspect(glass).has_value());
	ENGINE_CHECK_EQ(s.debris().size(), std::size_t{14});
	const auto frame = play.frame();
	const auto look = std::find_if(frame->objects.begin(), frame->objects.end(), [&](const auto& o) { return o.entity == shard; });
	ENGINE_REQUIRE(look != frame->objects.end());
	std::size_t drawn = 0;
	for (const auto& o : frame->objects)
		if (o.entity >= renderer::runtime_object_base) {
			++drawn;
			ENGINE_CHECK(o.mesh == look->mesh && o.material == look->material);
		}
	ENGINE_CHECK_EQ(drawn, std::size_t{14});
	// Settled, they go: asleep for two seconds, or past their five.
	play.ticks(400);
	ENGINE_CHECK(s.debris().empty());
	play.stop_and_check_the_editor_scene();
}

// The performance scene (slice 6) holds what its numbers claim: ten enemies walking and 5,000 particles alive.
ENGINE_TEST(stress_scene_has_ten_walking_enemies_and_five_thousand_particles)
{
	Shooter play("stress_test.scene");
	auto& s = play.s;
	std::vector<EntityId> enemies;
	std::vector<Vec3> starts;
	for (int i = 1; i <= 10; ++i) {
		enemies.push_back(play.find("Enemy " + std::to_string(i)));
		ENGINE_REQUIRE(enemies.back() != 0);
		starts.push_back(play.position(enemies.back()));
	}
	ENGINE_CHECK_EQ(play.find("Enemy 11"), EntityId{0});
	const auto targets = play.game->hud().targets_remaining;
	ENGINE_CHECK(targets == 19);
	std::size_t most = 0;
	for (int i = 0; i < 180; ++i) {
		play.tick();
		most = std::max(most, s.effect_stats().particles);
	}
	// Ten fountains of 500 each, full after two seconds, and footstep dust on top.
	ENGINE_CHECK(most >= 5000);
	ENGINE_CHECK(most < 5600);
	ENGINE_CHECK(s.effect_stats().emitters >= 10);
	const std::size_t animators = s.animation().stats().animators;
	ENGINE_CHECK_EQ(animators, std::size_t{10});
	for (std::size_t i = 0; i < enemies.size(); ++i) {
		const auto status = s.animation().status(enemies[i]);
		ENGINE_REQUIRE(status.has_value());
		ENGINE_CHECK_EQ(std::string(status->clip), std::string("Walk_Loop"));
		const Vec3 now = play.position(enemies[i]);
		ENGINE_CHECK(std::hypot(now.x - starts[i].x, now.z - starts[i].z) > 2.0); // About 3 m at a walk.
	}
	ENGINE_CHECK(s.game_issues().empty());
	// The timings slice 6 reports: animation and particles counted apart from gameplay.
	const auto& timing = s.timing();
	ENGINE_CHECK(timing.ticks >= 180);
	ENGINE_CHECK(timing.animation.count() > 0 && timing.animation_max.count() > 0);
	ENGINE_CHECK(timing.effect_updates >= 180 && timing.effects.count() > 0);
	play.stop_and_check_the_editor_scene();
}

// Milestone 6, slice 5: the vault's scripts run in Play through the project's asset database, as in the editor. Its
// gate opens once the west wing's targets have fallen; inside, E at the button gives its prize. Stop leaves nothing.
ENGINE_TEST(sample_vault_scripts_run_in_play_and_stop_leaves_the_scene_as_it_was)
{
	Shooter play;
	auto& s = play.s;
	const EntityId gate = play.find("Vault Gate"), button = play.find("Vault Button");
	ENGINE_REQUIRE(gate != 0 && button != 0);
	ENGINE_CHECK(s.script_instances() == (std::vector<EntityId>{gate, button}));
	const Vec3 closed = play.position(gate);
	play.ticks(30);
	ENGINE_CHECK(play.position(gate) == closed);
	for (const char* name : {"Static Target 3", "Enemy 1"}) play.game->damage(play.find(name), 1000, {});
	play.ticks(150);
	ENGINE_CHECK_NEAR(play.position(gate).z, closed.z + 3, 1e-9);
	ENGINE_CHECK(s.script_field(gate, "opened") == std::optional<scripting::Value>(scripting::Value{true}));
	// In the vault, E at the button.
	const Vec3 at = play.position(button);
	play.stand({at.x + 1.5, 1.0, at.z});
	play.ticks(5);
	gameplay::InputFrame press;
	press.held[std::size_t(gameplay::Action::Interact)] = press.pressed[std::size_t(gameplay::Action::Interact)] = true;
	(void)play.frame();
	s.set_game_input(press);
	s.update(EditorSession::fixed_step);
	const auto hud = play.game->hud();
	ENGINE_CHECK_EQ(hud.score, 200 + 100 + 300); // The two targets, then the prize.
	ENGINE_CHECK(hud.last_event == "+300  Vault, and 20 seconds");
	ENGINE_CHECK(s.script_errors_since_play() == 0);
	ENGINE_CHECK(s.take_script_errors().empty());
	play.stop_and_check_the_editor_scene();
}

// Milestone 6, slice 6: the scripting performance scene holds what its numbers claim. 500 cubes, each moved by the
// sample's Mover.lua at its own speed, run beside the vault's two scripts, and the session times scripts apart.
ENGINE_TEST(script_stress_scene_moves_its_500_cubes_by_script_and_times_the_scripts)
{
	Shooter play("script_stress.scene");
	auto& s = play.s;
	std::vector<EntityId> movers;
	std::vector<Vec3> starts;
	for (int i = 1; i <= 500; ++i) {
		movers.push_back(play.find("Script Mover " + std::to_string(i)));
		ENGINE_REQUIRE(movers.back() != 0);
		starts.push_back(play.position(movers.back()));
	}
	ENGINE_CHECK_EQ(play.find("Script Mover 501"), EntityId{0});
	ENGINE_CHECK_EQ(s.script_instances().size(), std::size_t{502});
	play.ticks(60);
	// One second: the first, at 1 m/s from the bottom, is 1 m up; the second, at 2.4 m/s from the top, went down its
	// 2 m and back up 0.4 m.
	ENGINE_CHECK_NEAR(play.position(movers[0]).y, 6.5, 1e-6);
	ENGINE_CHECK_NEAR(play.position(movers[1]).y, 5.9, 1e-6);
	// At 1 to 2.8 m/s on a 2 m path, every one is at least a meter from where it started, and only up or down.
	for (std::size_t i = 0; i < movers.size(); ++i) {
		const Vec3 now = play.position(movers[i]);
		ENGINE_CHECK(std::abs(now.y - starts[i].y) > 0.99);
		ENGINE_CHECK(now.y >= 5.5 - 1e-9 && now.y <= 7.5 + 1e-9);
		ENGINE_CHECK(now.x == starts[i].x && now.z == starts[i].z);
	}
	ENGINE_CHECK(s.script_errors_since_play() == 0);
	ENGINE_CHECK(s.game_issues().empty());
	const auto& timing = s.timing();
	ENGINE_CHECK(timing.ticks >= 60);
	ENGINE_CHECK(timing.scripts.count() > 0 && timing.scripts_max.count() > 0);
	ENGINE_CHECK(timing.scripts_max <= timing.scripts);
	play.stop_and_check_the_editor_scene();
}
