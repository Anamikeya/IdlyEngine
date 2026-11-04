// Engine/Animation: rigs and clips built from the plain asset types, then played, blended, layered and solved by the
// animation system. Skeletons and clips are written in code, as the importer produces them from a GLB, and Fox comes
// from the dependency tree. Poses are compared at 1 mm per metre: ozz stores rotations in 16 bits (see
// ozz_smoke_tests.cpp).
#include "engine/test/test.h"

#include <engine/animation/animation.h>
#include <engine/assets/importers.h>
#include <engine/assets/skeleton.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using namespace engine;
using namespace engine::animation;
namespace fs = std::filesystem;

constexpr double precision = 1e-3;
constexpr float quarter_turn = 1.57079632679f;

std::array<float, 4> about_z(float angle) { return {0, 0, std::sin(angle / 2), std::cos(angle / 2)}; }

assets::Joint joint(std::string name, int parent, std::array<float, 3> translation)
{
	assets::Joint j;
	j.name = std::move(name);
	j.parent = parent;
	j.translation = translation;
	return j;
}

// shoulder at the origin, elbow 1 m up, hand 1 m above that.
assets::MeshSkin arm()
{
	assets::MeshSkin skin;
	skin.joints = {joint("shoulder", -1, {0, 0, 0}), joint("elbow", 0, {0, 1, 0}), joint("hand", 1, {0, 1, 0})};
	return skin;
}

// A clip turning the shoulder about +Z from `from` to `to` radians over `duration` seconds (one key when they match).
assets::AnimationClip turn(std::string name, std::size_t joints, float from, float to, float duration = 1)
{
	assets::AnimationClip clip;
	clip.name = std::move(name);
	clip.duration = duration;
	clip.tracks.resize(joints);
	clip.tracks[0].rotation.push_back({0, about_z(from)});
	if (from != to || duration > 0) {
		clip.tracks[0].rotation.push_back({duration, about_z(to)});
	}
	return clip;
}

std::shared_ptr<const Rig> rig_of(const assets::MeshSkin& skin)
{
	auto built = Rig::build(skin);
	ENGINE_REQUIRE(static_cast<bool>(built));
	return built.value();
}

std::shared_ptr<const ClipSet> clips_of(const std::shared_ptr<const Rig>& rig, const assets::MeshSkin& skin,
	const std::vector<assets::AnimationClip>& clips)
{
	auto built = ClipSet::build(rig, skin, clips);
	ENGINE_REQUIRE(static_cast<bool>(built));
	return built.value();
}

// rest, up (a quarter turn held), raise (rest to a quarter turn over 1 s).
std::shared_ptr<const ClipSet> arm_clips()
{
	const auto skin = arm();
	return clips_of(rig_of(skin), skin, {turn("rest", 3, 0, 0), turn("up", 3, quarter_turn, quarter_turn), turn("raise", 3, 0, quarter_turn)});
}

AnimatorInput animator(std::uint64_t entity, std::shared_ptr<const ClipSet> clips, std::string clip)
{
	AnimatorInput in;
	in.entity = entity;
	in.clips = std::move(clips);
	in.clip = std::move(clip);
	return in;
}

std::array<double, 3> position(const Pose& pose, std::size_t joint)
{
	const auto& m = pose->at(joint);
	return {m[12], m[13], m[14]};
}

void check_at(const Pose& pose, std::size_t joint, double x, double y, double z = 0, double tolerance = precision)
{
	ENGINE_REQUIRE(pose != nullptr);
	const auto p = position(pose, joint);
	ENGINE_CHECK_NEAR(p[0], x, tolerance);
	ENGINE_CHECK_NEAR(p[1], y, tolerance);
	ENGINE_CHECK_NEAR(p[2], z, tolerance);
}

double distance(const std::array<double, 3>& a, const std::array<double, 3>& b)
{
	return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

const double half = std::sqrt(0.5);

} // namespace

ENGINE_TEST(a_rig_keeps_its_joint_order_whatever_order_ozz_uses)
{
	// ozz numbers joints depth first (root, a, a_tip, b); the model's order is root, a, b, a_tip.
	assets::MeshSkin skin;
	skin.joints = {joint("root", -1, {0, 1, 0}), joint("a", 0, {1, 0, 0}), joint("b", 0, {-1, 0, 0}), joint("a_tip", 1, {0, 0, 2})};
	skin.joints[2].rotation = about_z(quarter_turn);
	const auto rig = rig_of(skin);
	ENGINE_CHECK_EQ(rig->joint_count(), std::size_t{4});
	ENGINE_CHECK_EQ(rig->joint_name(2), std::string("b"));
	ENGINE_CHECK_EQ(rig->joint_parent(3), 1);
	ENGINE_CHECK_EQ(*rig->find_joint("a_tip"), std::size_t{3});
	ENGINE_CHECK(!rig->find_joint("nope"));
	const auto expected = assets::rest_pose(skin);
	const auto& rest = rig->rest_pose();
	ENGINE_REQUIRE(rest && rest->size() == 4);
	for (std::size_t j = 0; j < 4; ++j) {
		for (std::size_t k = 0; k < 16; ++k) {
			ENGINE_CHECK_NEAR((*rest)[j][k], expected[j][k], precision);
		}
	}
	check_at(rest, 3, 1, 1, 2);
	check_at(rest, 2, -1, 1, 0);
}

ENGINE_TEST(a_rig_refuses_a_broken_skeleton)
{
	ENGINE_CHECK(!Rig::build({}));
	assets::MeshSkin late;
	late.joints = {joint("child", 1, {}), joint("parent", -1, {})};
	ENGINE_CHECK(!Rig::build(late));
	assets::MeshSkin twins;
	twins.joints = {joint("same", -1, {}), joint("same", 0, {})};
	ENGINE_CHECK(!Rig::build(twins));
	assets::MeshSkin broken = arm();
	broken.joints[1].translation[0] = std::nanf("");
	ENGINE_CHECK(!Rig::build(broken));
}

ENGINE_TEST(an_animator_plays_a_clip_at_known_times)
{
	const auto clips = arm_clips();
	AnimationSystem system;
	const AnimatorInput in[] = {animator(7, clips, "raise")};
	system.set_animators(in);
	// The pose is there before any update: the clip's start.
	check_at(system.pose(7), 1, 0, 1);
	check_at(system.pose(7), 2, 0, 2);
	system.update(0.25);
	system.update(0.25);
	ENGINE_CHECK_NEAR(system.status(7)->time, 0.5, 1e-9);
	check_at(system.pose(7), 1, -half, half);
	check_at(system.pose(7), 2, -2 * half, 2 * half);
	// It loops: 1.25 s in is a quarter of the way again.
	system.update(0.25);
	system.update(0.25);
	system.update(0.25);
	ENGINE_CHECK_NEAR(system.status(7)->time, 0.25, 1e-9);
	ENGINE_CHECK_EQ(system.stats().animators, std::size_t{1});
	ENGINE_CHECK_EQ(system.stats().posed_joints, std::size_t{3});
}

ENGINE_TEST(a_clip_that_does_not_loop_stops_at_its_end_and_speed_scales_time)
{
	const auto clips = arm_clips();
	AnimationSystem system;
	auto in = animator(1, clips, "raise");
	in.loop = false;
	in.speed = 2;
	system.set_animators({&in, 1});
	system.update(0.25);
	ENGINE_CHECK_NEAR(system.status(1)->time, 0.5, 1e-9);
	for (int i = 0; i < 4; ++i) {
		system.update(0.25);
	}
	ENGINE_CHECK_NEAR(system.status(1)->time, 1.0, 1e-9);
	check_at(system.pose(1), 1, -1, 0);
	// Backwards from the end.
	in.speed = -1;
	system.set_animators({&in, 1});
	system.update(0.25);
	ENGINE_CHECK_NEAR(system.status(1)->time, 0.75, 1e-9);
	// Not playing (the editor's preview off) holds still; a start time is where it holds.
	in.playing = false;
	in.start_time = 0.5;
	system.set_animators({&in, 1});
	system.update(0.25);
	ENGINE_CHECK_NEAR(system.status(1)->time, 0.5, 1e-9);
	check_at(system.pose(1), 1, -half, half);
}

ENGINE_TEST(a_crossfade_blends_the_old_clip_into_the_new_one)
{
	const auto clips = arm_clips();
	AnimationSystem system;
	const AnimatorInput in[] = {animator(3, clips, "rest")};
	system.set_animators(in);
	ENGINE_REQUIRE(static_cast<bool>(system.play(3, "up", 1.0)));
	system.update(0.5);
	const auto halfway = system.status(3);
	ENGINE_CHECK_EQ(halfway->clip, std::string("up"));
	ENGINE_CHECK_EQ(halfway->fading_from, std::string("rest"));
	ENGINE_CHECK_NEAR(halfway->fade, 0.5, 1e-9);
	// Half of a quarter turn.
	check_at(system.pose(3), 1, -half, half);
	system.update(0.6);
	ENGINE_CHECK(system.status(3)->fading_from.empty());
	check_at(system.pose(3), 1, -1, 0);
	// No blend cuts at once.
	ENGINE_REQUIRE(static_cast<bool>(system.play(3, "rest", 0)));
	system.update(0.01);
	check_at(system.pose(3), 1, 0, 1);
	ENGINE_CHECK(!system.play(3, "missing"));
	ENGINE_CHECK(!system.play(3, "up", -1));
	ENGINE_CHECK(!system.play(99, "up"));
}

ENGINE_TEST(a_layered_clip_adds_its_motion_on_top_and_ends_by_itself)
{
	const auto clips = arm_clips();
	AnimationSystem system;
	const AnimatorInput in[] = {animator(4, clips, "up")};
	system.set_animators(in);
	// "raise" as a difference from its first frame: up to a further quarter turn over 1 s, on top of "up".
	ENGINE_REQUIRE(static_cast<bool>(system.play_layered(4, "raise", 1, 0)));
	system.update(0.5);
	ENGINE_CHECK_EQ(std::string(system.status(4)->layered), std::string("raise"));
	// 90 + 45 degrees.
	check_at(system.pose(4), 1, -half, -half);
	system.update(0.25);
	system.update(0.3);
	ENGINE_CHECK(system.status(4)->layered.empty());
	check_at(system.pose(4), 1, -1, 0);
	// Half the weight, half the motion.
	ENGINE_REQUIRE(static_cast<bool>(system.play_layered(4, "raise", 0.5, 0)));
	system.update(0.5);
	const double angle = 1.5707963 + 0.5 * 0.78539816;
	check_at(system.pose(4), 1, -std::sin(angle), std::cos(angle));
	ENGINE_CHECK(!system.play_layered(4, "raise", 2));
	ENGINE_CHECK(!system.play_layered(4, "missing"));
}

ENGINE_TEST(crossed_events_wrap_with_a_loop_and_stop_without_one)
{
	const std::vector<double> times{0.25, 0.75};
	using List = std::vector<std::size_t>;
	ENGINE_CHECK(crossed_events(times, 0.5, 0.6, 1, true, false) == List({1}));
	ENGINE_CHECK(crossed_events(times, 0.5, 0.8, 1, true, false) == List({1, 0}));
	ENGINE_CHECK(crossed_events(times, 0.5, 2.0, 1, true, false) == List({1, 0, 1, 0}));
	ENGINE_CHECK(crossed_events(times, 0.5, 2.0, 1, false, false) == List({1}));
	ENGINE_CHECK(crossed_events(times, 0.5, -0.6, 1, true, false) == List({0}));
	ENGINE_CHECK(crossed_events(times, 0.5, -0.8, 1, true, false) == List({0, 1}));
	ENGINE_CHECK(crossed_events(times, 0.5, -0.8, 1, false, false) == List({0}));
	// An event exactly where a clip starts fires only when it starts.
	ENGINE_CHECK(crossed_events(times, 0.25, 0.1, 1, true, true) == List({0}));
	ENGINE_CHECK(crossed_events(times, 0.25, 0.1, 1, true, false).empty());
	ENGINE_CHECK(crossed_events(times, 0.1, 0, 1, true, false).empty());
	// Very short laps cannot flood the queue.
	ENGINE_CHECK_EQ(crossed_events(times, 0, 100, 0.01, true, false).size(), std::size_t{64});
	// A single pose has only its start.
	ENGINE_CHECK(crossed_events(std::vector<double>{0}, 0, 0.5, 0, true, true) == List({0}));
}

ENGINE_TEST(clip_events_fire_as_playback_crosses_them_across_loops)
{
	const auto clips = arm_clips();
	AnimationSystem system;
	auto in = animator(5, clips, "raise");
	in.events = {{"raise", 0, "start"}, {"raise", 0.25f, "left"}, {"raise", 0.75f, "right"}, {"up", 0.5f, "other"},
		{"raise", 5, "never"}};
	system.set_animators({&in, 1});
	for (int i = 0; i < 4; ++i) {
		system.update(0.2);
	}
	auto events = system.take_events();
	ENGINE_REQUIRE(events.size() == 3);
	ENGINE_CHECK_EQ(events[0].name, std::string("start"));
	ENGINE_CHECK_EQ(events[1].name, std::string("left"));
	ENGINE_CHECK_EQ(events[2].name, std::string("right"));
	ENGINE_CHECK_EQ(events[2].entity, std::uint64_t{5});
	ENGINE_CHECK_EQ(events[2].clip, std::string("raise"));
	ENGINE_CHECK_NEAR(events[2].time, 0.75, 1e-6);
	ENGINE_CHECK(system.take_events().empty());
	// Into the second lap: the start again (at 1.0), then the left foot (at 1.25).
	system.update(0.2);
	system.update(0.2);
	events = system.take_events();
	ENGINE_REQUIRE(events.size() == 1);
	ENGINE_CHECK_EQ(events[0].name, std::string("start"));
	system.update(0.1);
	events = system.take_events();
	ENGINE_REQUIRE(events.size() == 1);
	ENGINE_CHECK_EQ(events[0].name, std::string("left"));
	// Scrubbing fires nothing.
	ENGINE_REQUIRE(static_cast<bool>(system.set_time(5, 0.9)));
	ENGINE_CHECK(system.take_events().empty());
	// A layered clip's events fire too.
	ENGINE_REQUIRE(static_cast<bool>(system.play_layered(5, "up")));
	system.update(0.1);
	system.update(0.5);
	bool other = false;
	for (const auto& e : system.take_events()) {
		other = other || e.name == "other";
	}
	ENGINE_CHECK(other);
}

ENGINE_TEST(joint_world_places_a_joint_where_the_entity_is)
{
	const auto clips = arm_clips();
	AnimationSystem system;
	auto in = animator(2, clips, "up");
	in.world[12] = 5;
	in.world[13] = 1;
	system.set_animators({&in, 1});
	const auto hand = system.joint_world(2, "hand");
	ENGINE_REQUIRE(hand.has_value());
	ENGINE_CHECK_NEAR((*hand)[12], 5 - 2, precision);
	ENGINE_CHECK_NEAR((*hand)[13], 1, precision);
	const auto model = system.joint_model(2, "elbow");
	ENGINE_REQUIRE(model.has_value());
	ENGINE_CHECK_NEAR((*model)[12], -1, precision);
	ENGINE_CHECK(!system.joint_world(2, "missing"));
	ENGINE_CHECK(!system.joint_world(3, "hand"));
}

ENGINE_TEST(clips_from_another_model_play_on_the_joints_whose_names_match)
{
	const auto target = arm();
	const auto rig = rig_of(target);
	// Another model: an extra root, the shoulder below it, and no hand. Its elbow channel has no keys.
	assets::MeshSkin source;
	source.joints = {joint("pelvis", -1, {0, 3, 0}), joint("shoulder", 0, {0, 0, 0}), joint("elbow", 1, {0, 9, 0})};
	assets::AnimationClip clip;
	clip.name = "wave";
	clip.duration = 1;
	clip.tracks.resize(3);
	clip.tracks[1].rotation.push_back({0, about_z(quarter_turn)});
	auto bound = ClipSet::build(rig, source, std::span<const assets::AnimationClip>(&clip, 1));
	ENGINE_REQUIRE(static_cast<bool>(bound));
	ENGINE_CHECK_EQ(bound.value()->matched_joints(), std::size_t{2});
	AnimationSystem system;
	const AnimatorInput in[] = {animator(1, bound.value(), "wave")};
	system.set_animators(in);
	// The arm's own lengths, turned by the other model's shoulder.
	check_at(system.pose(1), 1, -1, 0);
	check_at(system.pose(1), 2, -2, 0);
	assets::MeshSkin stranger;
	stranger.joints = {joint("tail", -1, {})};
	ENGINE_CHECK(!ClipSet::build(rig, stranger, std::span<const assets::AnimationClip>(&clip, 1)));
	ENGINE_CHECK(!ClipSet::build(nullptr, target, {}));
}

ENGINE_TEST(two_bone_ik_puts_the_end_of_a_leg_on_its_target)
{
	// hip 2 m up, knee bent slightly forward (+Z), ankle at the ground.
	assets::MeshSkin leg;
	leg.joints = {joint("hip", -1, {0, 2, 0}), joint("knee", 0, {0, -1, 0.05f}), joint("ankle", 1, {0, -1, -0.05f})};
	const auto rig = rig_of(leg);
	const auto clips = clips_of(rig, leg, {turn("stand", 3, 0, 0)});
	AnimationSystem system;
	auto in = animator(9, clips, "stand");
	in.world[12] = 10; // The IK target is in world space.
	system.set_animators({&in, 1});
	TwoBoneIK chain;
	chain.start = "hip";
	chain.middle = "knee";
	chain.end = "ankle";
	chain.target = {10, 0.5, 0.3};
	chain.pole = {0, 0, 1};
	chain.bend_axis = {-1, 0, 0};
	ENGINE_REQUIRE(static_cast<bool>(system.set_ik(9, {chain})));
	system.update(0.016);
	const auto pose = system.pose(9);
	check_at(pose, 2, 0, 0.5, 0.3, 2e-3);
	ENGINE_CHECK(system.status(9)->ik_reached);
	// The bones keep their lengths, and the knee bends towards the pole.
	const double thigh = std::hypot(1.0, 0.05);
	ENGINE_CHECK_NEAR(distance(position(pose, 0), position(pose, 1)), thigh, 2e-3);
	ENGINE_CHECK_NEAR(distance(position(pose, 1), position(pose, 2)), thigh, 2e-3);
	ENGINE_CHECK(position(pose, 1)[2] > 0.3);
	// Out of reach: the leg points at the target, straight, and says so.
	chain.target = {10, -5, 0};
	ENGINE_REQUIRE(static_cast<bool>(system.set_ik(9, {chain})));
	system.update(0.016);
	ENGINE_CHECK(!system.status(9)->ik_reached);
	ENGINE_CHECK(position(system.pose(9), 2)[1] < 2 - 1.9);
	// Weight 0 leaves the animation alone.
	chain.weight = 0;
	ENGINE_REQUIRE(static_cast<bool>(system.set_ik(9, {chain})));
	system.update(0.016);
	check_at(system.pose(9), 2, 0, 0, 0);
	ENGINE_CHECK(!system.set_ik(9, {TwoBoneIK{"hip", "ankle", "knee"}}));
	ENGINE_CHECK(!system.set_ik(9, {TwoBoneIK{"hip", "knee", "toe"}}));
	ENGINE_REQUIRE(static_cast<bool>(system.set_ik(9, {})));
}

ENGINE_TEST(the_scene_side_restarts_an_animator_only_when_its_authored_clip_changes)
{
	const auto clips = arm_clips();
	AnimationSystem system;
	auto in = animator(6, clips, "rest");
	system.set_animators({&in, 1});
	// Gameplay plays something else; the scene still says "rest", which changes nothing.
	ENGINE_REQUIRE(static_cast<bool>(system.play(6, "raise", 0)));
	system.update(0.5);
	system.set_animators({&in, 1});
	ENGINE_CHECK_EQ(std::string(system.status(6)->clip), std::string("raise"));
	in.speed = 0.5;
	system.set_animators({&in, 1});
	ENGINE_CHECK_EQ(std::string(system.status(6)->clip), std::string("raise"));
	ENGINE_CHECK_NEAR(system.status(6)->speed, 0.5, 1e-12);
	// A new authored clip starts again from the start time.
	in.clip = "up";
	in.start_time = 0.25;
	system.set_animators({&in, 1});
	ENGINE_CHECK_EQ(std::string(system.status(6)->clip), std::string("up"));
	ENGINE_CHECK_NEAR(system.status(6)->time, 0.25, 1e-9);
	// An unknown clip rests; restart() goes back to the authored start; a missing entity is dropped.
	in.clip = "nope";
	system.set_animators({&in, 1});
	ENGINE_CHECK(system.status(6)->clip.empty());
	ENGINE_CHECK(!system.set_time(6, 0.5));
	for (std::size_t k = 0; k < 16; ++k) {
		ENGINE_CHECK_NEAR((*system.pose(6))[2][k], (*clips->rig()->rest_pose())[2][k], 1e-6);
	}
	in.clip = "raise";
	system.set_animators({&in, 1});
	system.update(0.5);
	ENGINE_REQUIRE(static_cast<bool>(system.set_paused(6, true)));
	system.update(0.5);
	ENGINE_CHECK_NEAR(system.status(6)->time, 0.5, 1e-9);
	ENGINE_CHECK(system.status(6)->paused);
	system.restart();
	ENGINE_CHECK_NEAR(system.status(6)->time, 0.25, 1e-9);
	ENGINE_CHECK(!system.status(6)->paused);
	system.set_animators({});
	ENGINE_CHECK(system.pose(6) == nullptr);
	ENGINE_CHECK_EQ(system.stats().animators, std::size_t{0});
}

ENGINE_TEST(the_same_inputs_give_the_same_poses_bit_for_bit)
{
	const auto clips = arm_clips();
	const auto run = [&] {
		AnimationSystem system;
		auto in = animator(1, clips, "raise");
		in.events = {{"raise", 0.5f, "mid"}};
		system.set_animators({&in, 1});
		std::vector<JointMatrix> trace;
		for (int i = 0; i < 120; ++i) {
			if (i == 30) {
				(void)system.play(1, "up", 0.4);
			}
			if (i == 60) {
				(void)system.play_layered(1, "raise", 0.7, 0.2);
			}
			system.update(1.0 / 60);
			trace.insert(trace.end(), system.pose(1)->begin(), system.pose(1)->end());
		}
		return std::make_pair(trace, system.take_events().size());
	};
	const auto a = run(), b = run();
	ENGINE_CHECK(a.first == b.first);
	ENGINE_CHECK_EQ(a.second, b.second);
}

ENGINE_TEST(fox_plays_every_clip_from_its_own_file)
{
	if (!assets::gltf_import_available()) {
		return;
	}
	const fs::path fox = fs::path(GAMEENGINE_TEST_RESOURCES_DIR) / "filament/third_party/models/Fox/Fox.glb";
	if (!fs::exists(fox)) {
		return;
	}
	std::ifstream file(fox, std::ios::binary);
	const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	const auto mesh = assets::import_glb(bytes);
	ENGINE_REQUIRE(static_cast<bool>(mesh));
	const auto& data = mesh.value();
	const auto rig = rig_of(data.skin);
	const auto clips = clips_of(rig, data.skin, data.clips);
	ENGINE_REQUIRE(clips->size() == 3);
	ENGINE_CHECK_EQ(clips->name(1), std::string("Walk"));
	ENGINE_CHECK_NEAR(clips->duration(1), 0.7083333, 1e-5);
	// The rest pose agrees with the asset library's skeleton math (Fox is in centimetres).
	const auto expected = assets::rest_pose(data.skin);
	for (std::size_t j = 0; j < expected.size(); ++j) {
		for (std::size_t k = 12; k < 15; ++k) {
			ENGINE_CHECK_NEAR((*rig->rest_pose())[j][k], expected[j][k], 0.05);
		}
	}
	for (std::size_t c = 0; c < clips->size(); ++c) {
		AnimationSystem system;
		const AnimatorInput in[] = {animator(1, clips, clips->name(c))};
		system.set_animators(in);
		const auto start = system.pose(1);
		for (int i = 0; i < 20; ++i) {
			system.update(1.0 / 60);
		}
		const auto later = system.pose(1);
		ENGINE_REQUIRE(start && later && later->size() == data.skin.joints.size());
		double moved = 0;
		for (std::size_t j = 0; j < later->size(); ++j) {
			for (std::size_t k = 0; k < 16; ++k) {
				ENGINE_CHECK(std::isfinite((*later)[j][k]));
			}
			moved = std::max(moved, distance(position(start, j), position(later, j)));
		}
		// A third of a second into any of its clips, some joint has moved over a centimetre.
		ENGINE_CHECK(moved > 1.0);
	}
}
