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

	Shooter()
	{
		ENGINE_REQUIRE(s.open_project(sample));
		ENGINE_REQUIRE(s.load(sample / "physics_shooter.scene"));
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
