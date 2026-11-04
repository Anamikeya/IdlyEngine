// Milestone 5 slice 3 in the editor, without a window: animators previewed while editing and run in Play, entities
// attached to joints, clip events kept in a model's .meta with undo, and clips from another model. Real Scene,
// CommandBus, asset database and EditorSession; models are GLBs written in memory. Poses never reach the scene (D1).
#include "engine/test/test.h"
#include "../Assets/glb_writer.hpp"

#include <engine/editor/EditorSession.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>

using namespace engine;
using namespace engine::editor;
using namespace std::chrono_literals;

namespace {

std::filesystem::path fresh_folder(const std::string& name) {
	const auto folder = std::filesystem::temp_directory_path() / ("engine-animation-" + name);
	std::filesystem::remove_all(folder);
	std::filesystem::create_directories(folder);
	return folder;
}

struct Clip {
	std::string name;
	std::array<float, 3> from, to; // The tip joint's translation at 0 s and 1 s.
};

// The bar from the skinning tests: x 0..1, y 0..2, joint "root" at the origin and "tip" a metre up, bound as they rest.
// Its top row follows the tip. Each clip moves the tip over one second.
assets::AssetId animated_bar(EditorSession& s, const std::filesystem::path& folder, const std::string& file_name, const std::vector<Clip>& clips) {
	engine::test::Glb g;
	const int positions = g.floats({0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 2, 0, 1, 2, 0}, 3, true);
	const int indices = g.u16({0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4}, 1);
	const int joints = g.u16({0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, 4);
	const int weights = g.floats({1, 0, 0, 0, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0, 0.5f, 0.5f, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, 4);
	const int binds = g.floats({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1}, 16);
	std::string animations;
	for (const auto& clip : clips) {
		const int times = g.floats({0, 1}, 1, true);
		const int values = g.floats({clip.from[0], clip.from[1], clip.from[2], clip.to[0], clip.to[1], clip.to[2]}, 3);
		animations += std::string(animations.empty() ? "" : ",") + R"({"name":")" + clip.name +
			R"(","channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}],"samplers":[{"input":)" + std::to_string(times) +
			R"(,"output":)" + std::to_string(values) + "}]}";
	}
	const auto bytes = g.build(R"("scene":0,"scenes":[{"nodes":[0,2]}],"nodes":[{"name":"root","children":[1]},{"name":"tip","translation":[0,1,0]},)"
		R"({"name":"bar","mesh":0,"skin":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":)" + std::to_string(positions) +
		R"(,"JOINTS_0":)" + std::to_string(joints) + R"(,"WEIGHTS_0":)" + std::to_string(weights) + R"(},"indices":)" + std::to_string(indices) +
		R"(}]}],"skins":[{"joints":[0,1],"inverseBindMatrices":)" + std::to_string(binds) + "}],\"animations\":[" + animations + "]");
	const auto file = folder / "source" / file_name;
	std::filesystem::create_directories(file.parent_path());
	std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	const auto imported = s.import_asset(file);
	ENGINE_REQUIRE(imported);
	return imported.value().id;
}

const std::vector<Clip> slide_and_hold{{"Slide", {0, 1, 0}, {1, 1, 0}}, {"Hold", {0, 1, 0}, {0, 1, 0}}};

renderer::Frame frame_of(EditorSession& s) {
	const auto f = s.frame(1.5);
	ENGINE_REQUIRE(f);
	return f.value();
}
const renderer::RenderObject& object_of(const renderer::Frame& f, EntityId id) {
	for (const auto& o : f->objects)
		if (o.entity == id) return o;
	ENGINE_REQUIRE(false);
	return f->objects.front();
}
// Where the frame's pose puts the tip joint.
double tip_x(const renderer::Frame& f, EntityId bar) {
	const auto& pose = object_of(f, bar).pose;
	ENGINE_REQUIRE(pose && pose->size() == 2);
	return (*pose)[1][12];
}
Vec3 position(const WorldTransform& w) { return {w.matrix[12], w.matrix[13], w.matrix[14]}; }
void check_near(const Vec3& a, const Vec3& b, double tolerance = 2e-3) {
	ENGINE_CHECK_NEAR(a.x, b.x, tolerance);
	ENGINE_CHECK_NEAR(a.y, b.y, tolerance);
	ENGINE_CHECK_NEAR(a.z, b.z, tolerance);
}
AssetRef ref_of(const EditorSession& s, assets::AssetId id) {
	for (const auto& meta : s.assets())
		if (meta.id == id) return AssetRef{id.value, meta.path};
	ENGINE_REQUIRE(false);
	return {};
}
std::string read_file(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
EntityId place(EditorSession& s, assets::AssetId model) {
	const auto placed = s.place_asset_at(model, {0, 0, 0});
	ENGINE_REQUIRE(placed);
	return placed.value().created.front();
}
} // namespace

ENGINE_TEST(a_placed_animated_model_plays_its_first_clip_while_editing_and_never_edits_the_scene) {
	const auto folder = fresh_folder("preview");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(folder));
	const auto bar = place(s, animated_bar(s, folder, "bar.glb", slide_and_hold));
	const auto animator = s.scene().inspect(bar)->components.animator;
	ENGINE_REQUIRE(animator.has_value());
	ENGINE_CHECK_EQ(animator->clip, std::string("Slide"));
	ENGINE_CHECK(animator->preview);
	s.mark_clean();
	const auto revision = s.scene_revision();
	const auto history = s.history_names().size();
	ENGINE_CHECK_NEAR(tip_x(frame_of(s), bar), 0, 1e-3);
	// Half a second of editing, in uneven frames.
	for (const auto step : {100ms, 150ms, 250ms}) {
		s.update(step);
		(void)frame_of(s);
	}
	ENGINE_CHECK_NEAR(tip_x(frame_of(s), bar), 0.5, 2e-3);
	ENGINE_CHECK_NEAR(s.animation().status(bar)->time, 0.5, 1e-6);
	ENGINE_CHECK_EQ(s.scene_revision(), revision);
	ENGINE_CHECK_EQ(s.history_names().size(), history);
	ENGINE_CHECK(!s.dirty());
	// View > Play Animations off holds every animator still.
	s.view_flags.animations = false;
	(void)frame_of(s);
	s.update(250ms);
	ENGINE_CHECK_NEAR(tip_x(frame_of(s), bar), 0.5, 2e-3);
	// So does the animator's own preview switch, and scrubbing still poses it.
	s.view_flags.animations = true;
	auto still = *animator;
	still.preview = false;
	ENGINE_REQUIRE(s.execute(commands::SetAnimator{bar, still}));
	(void)frame_of(s);
	s.update(250ms);
	ENGINE_CHECK_NEAR(tip_x(frame_of(s), bar), 0.5, 2e-3);
	ENGINE_REQUIRE(s.animation().set_time(bar, 0.25));
	ENGINE_CHECK_NEAR(tip_x(frame_of(s), bar), 0.25, 2e-3);
	// Its box follows the pose: the top right corner moved from x = 1 to 1.25.
	ENGINE_CHECK_NEAR(s.local_bounds(bar)->second.x, 1.25, 2e-3);
	// The scene file holds the animator and nothing of the pose.
	std::ostringstream out;
	ENGINE_REQUIRE(write_scene(out, s.editor_scene().snapshot()));
	ENGINE_CHECK(out.str().find("ANIMATOR " + std::to_string(bar) + " - \"Slide\" 1 1 0 0") != std::string::npos);
	// Choosing another clip is one undo step and starts it.
	ENGINE_REQUIRE(s.assign_clip(bar, "Hold"));
	ENGINE_CHECK_EQ(std::string(s.history_names().back()), std::string("Play Hold"));
	(void)frame_of(s);
	ENGINE_CHECK_EQ(std::string(s.animation().status(bar)->clip), std::string("Hold"));
	ENGINE_REQUIRE(s.undo());
	(void)frame_of(s);
	ENGINE_CHECK_EQ(std::string(s.animation().status(bar)->clip), std::string("Slide"));
}

ENGINE_TEST(play_mode_animates_with_the_fixed_ticks_and_stop_returns_to_the_preview) {
	const auto folder = fresh_folder("play");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(folder));
	const auto bar = place(s, animated_bar(s, folder, "bar.glb", slide_and_hold));
	s.view_flags.animations = false; // The editor's own preview stays at the start throughout.
	(void)frame_of(s);
	const auto editor_revision = s.editor_scene().snapshot();
	ENGINE_REQUIRE(s.play());
	(void)frame_of(s);
	// 30 ticks of 1/60 s.
	for (int i = 0; i < 6; ++i) s.update(std::chrono::nanoseconds(5 * EditorSession::fixed_step));
	ENGINE_CHECK_EQ(s.runtime_ticks(), std::uint64_t{30});
	ENGINE_CHECK_NEAR(s.animation().status(bar)->time, 0.5, 1e-6);
	ENGINE_CHECK_NEAR(tip_x(frame_of(s), bar), 0.5, 2e-3);
	// Gameplay's controls work in Play and touch no scene: a quarter-second crossfade to Hold, 6 ticks in.
	ENGINE_REQUIRE(s.animation().play(bar, "Hold", 0.25));
	s.update(std::chrono::nanoseconds(5 * EditorSession::fixed_step));
	s.update(std::chrono::nanoseconds(EditorSession::fixed_step));
	const auto fading = *s.animation().status(bar);
	ENGINE_CHECK_EQ(fading.clip, std::string("Hold"));
	ENGINE_CHECK_EQ(fading.fading_from, std::string("Slide"));
	ENGINE_CHECK_NEAR(fading.fade, 0.4, 1e-6);
	for (int i = 0; i < 4; ++i) s.update(std::chrono::nanoseconds(5 * EditorSession::fixed_step));
	ENGINE_CHECK(s.animation().status(bar)->fading_from.empty());
	ENGINE_CHECK_NEAR(tip_x(frame_of(s), bar), 0, 2e-3);
	// Paused, nothing moves; a step advances one tick.
	ENGINE_REQUIRE(s.pause());
	ENGINE_REQUIRE(s.animation().play(bar, "Slide", 0));
	s.update(250ms);
	ENGINE_CHECK_NEAR(s.animation().status(bar)->time, 0, 1e-9);
	ENGINE_REQUIRE(s.step());
	ENGINE_CHECK_NEAR(s.animation().status(bar)->time, 1.0 / 60, 1e-6);
	ENGINE_REQUIRE(s.stop());
	ENGINE_CHECK(s.editor_scene().snapshot() == editor_revision);
	ENGINE_CHECK_NEAR(tip_x(frame_of(s), bar), 0, 1e-3);
	// A new run starts the clip from the beginning again.
	ENGINE_REQUIRE(s.play());
	(void)frame_of(s);
	ENGINE_CHECK_NEAR(s.animation().status(bar)->time, 0, 1e-9);
	ENGINE_CHECK_EQ(std::string(s.animation().status(bar)->clip), std::string("Slide"));
}

ENGINE_TEST(an_entity_attached_to_a_joint_is_drawn_picked_and_moved_on_it) {
	const auto folder = fresh_folder("attach");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(folder));
	const auto bar = place(s, animated_bar(s, folder, "bar.glb", slide_and_hold));
	Transform above; above.position = {0, 0.5, 0}; above.scale = {0.2, 0.2, 0.2};
	Components held{MeshComponent{}, {}, {}};
	held.attach = AttachToBoneComponent{"tip"};
	const auto cube = s.execute(commands::CreateEntity{"Held", above, bar, 0, held});
	ENGINE_REQUIRE(cube);
	const auto held_id = cube.value().created.front();
	// Something under the attached entity follows it.
	Transform further; further.position = {0, 1, 0};
	const auto under = s.execute(commands::CreateEntity{"Under", further, held_id, 0, Components{MeshComponent{}, {}, {}}});
	ENGINE_REQUIRE(under);
	const auto under_id = under.value().created.front();
	auto f = frame_of(s);
	// The tip rests a metre up, so the cube is drawn 1.5 m up, and the one under it 0.2 m higher still.
	check_near(position(object_of(f, held_id).world), {0, 1.5, 0});
	check_near(position(object_of(f, under_id).world), {0, 1.7, 0});
	check_near(position(*s.drawn_world(held_id)), {0, 1.5, 0});
	check_near(position(*s.scene().world_transform(held_id)), {0, 0.5, 0}); // The scene is unchanged.
	s.update(500ms);
	f = frame_of(s);
	check_near(position(object_of(f, held_id).world), {0.5, 1.5, 0});
	check_near(position(*s.joint_world(bar, "tip")), {0.5, 1, 0});
	// A click where it is drawn picks it.
	const auto& view = f->camera.view.matrix;
	const auto& projection = f->camera.projection.matrix;
	const auto ndc = [&](Vec3 p) {
		const double vx = view[0] * p.x + view[4] * p.y + view[8] * p.z + view[12], vy = view[1] * p.x + view[5] * p.y + view[9] * p.z + view[13],
					 vz = view[2] * p.x + view[6] * p.y + view[10] * p.z + view[14];
		const double w = projection[3] * vx + projection[7] * vy + projection[11] * vz + projection[15];
		return std::pair<double, double>{(projection[0] * vx + projection[4] * vy + projection[8] * vz + projection[12]) / w,
										 (projection[1] * vx + projection[5] * vy + projection[9] * vz + projection[13]) / w};
	};
	const auto [x, y] = ndc({0.5, 1.5, 0});
	ENGINE_CHECK_EQ(s.pick(*f, x, y), held_id);
	// Moving it where it is drawn keeps it on the joint: its transform is relative to the tip.
	ENGINE_REQUIRE(s.animation().set_paused(bar, true));
	auto target = *s.drawn_world(held_id);
	target.matrix[12] += 1;
	ENGINE_REQUIRE(s.set_world(held_id, target));
	check_near(s.scene().inspect(held_id)->transform.position, {1, 0.5, 0});
	check_near(position(*s.drawn_world(held_id)), {1.5, 1.5, 0});
	// The same through a gizmo drag.
	s.select(held_id);
	ENGINE_REQUIRE(s.begin_drag("Move"));
	WorldTransform up;
	up.matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0.25, 0, 1};
	ENGINE_REQUIRE(s.update_drag(up));
	ENGINE_REQUIRE(s.end_drag());
	check_near(position(*s.drawn_world(held_id)), {1.5, 1.75, 0});
	check_near(s.scene().inspect(held_id)->transform.position, {1, 0.75, 0});
	// A joint the model does not have leaves it on its parent.
	ENGINE_REQUIRE(s.execute(commands::SetAttachToBone{held_id, AttachToBoneComponent{"nope"}}));
	check_near(position(object_of(frame_of(s), held_id).world), {1, 0.75, 0});
	ENGINE_CHECK(s.joint_world(bar, "nope") == std::nullopt);
}

ENGINE_TEST(clip_events_live_in_the_models_meta_with_undo_and_fire_while_it_plays) {
	const auto folder = fresh_folder("events");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(folder));
	const auto model = animated_bar(s, folder, "bar.glb", slide_and_hold);
	const auto bar = place(s, model);
	ENGINE_CHECK_EQ(s.clip_source(bar).value, model.value);
	ENGINE_REQUIRE(s.set_clip_events(model, {{"Slide", 0.25f, "step"}, {"Slide", 0.75f, "step"}}));
	ENGINE_CHECK_EQ(std::string(s.history_names().back()), std::string("Edit Clip Events of bar"));
	ENGINE_CHECK_EQ(s.clip_events(model).size(), std::size_t{2});
	const auto meta_file = folder / "Assets" / (ref_of(s, model).path + ".meta");
	ENGINE_CHECK(read_file(meta_file).find("event = 0.25 step \"Slide\"\n") != std::string::npos);
	(void)frame_of(s);
	s.update(500ms);
	ENGINE_REQUIRE(s.animation_events().size() == 1);
	ENGINE_CHECK_EQ(s.animation_events().back().name, std::string("step"));
	ENGINE_CHECK_EQ(s.animation_events().back().entity, bar);
	s.update(400ms);
	ENGINE_CHECK_EQ(s.animation_events().size(), std::size_t{2});
	// Undo and redo put the file back as it was.
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK(s.clip_events(model).empty());
	ENGINE_REQUIRE(s.redo());
	ENGINE_CHECK_EQ(s.clip_events(model).size(), std::size_t{2});
	// Bad events and other assets are refused, and nothing is recorded for them.
	const auto steps = s.history_names().size();
	ENGINE_CHECK(!s.set_clip_events(model, {{"Slide", 0.5f, "Bad Name"}}));
	ENGINE_CHECK(!s.set_clip_events(assets::AssetId{12345}, {}));
	ENGINE_REQUIRE(s.set_clip_events(model, s.clip_events(model))); // No change, no step.
	ENGINE_CHECK_EQ(s.history_names().size(), steps);
	ENGINE_REQUIRE(s.play());
	ENGINE_CHECK(!s.set_clip_events(model, {}));
	ENGINE_REQUIRE(s.stop());
	// A fresh session reads them back from the .meta.
	EditorSession other;
	ENGINE_REQUIRE(other.open_project(folder));
	ENGINE_CHECK_EQ(other.clip_events(model).size(), std::size_t{2});
}

ENGINE_TEST(clips_from_another_model_play_by_joint_name_and_problems_are_reported) {
	const auto folder = fresh_folder("sources");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(folder));
	const auto model = animated_bar(s, folder, "bar.glb", slide_and_hold);
	const auto moves = animated_bar(s, folder, "moves.glb", {{"Wave", {0, 1, 0}, {-1, 1, 0}}});
	const auto bar = place(s, model);
	auto animator = *s.scene().inspect(bar)->components.animator;
	animator.clips = ref_of(s, moves);
	animator.clip = "Wave";
	ENGINE_REQUIRE(s.execute(commands::SetAnimator{bar, animator}));
	(void)frame_of(s);
	ENGINE_CHECK_EQ(s.clip_source(bar).value, moves.value);
	ENGINE_REQUIRE(s.clip_set(bar) != nullptr);
	ENGINE_CHECK_EQ(s.clip_set(bar)->size(), std::size_t{1});
	s.update(500ms);
	ENGINE_CHECK_NEAR(tip_x(frame_of(s), bar), -0.5, 2e-3);
	ENGINE_CHECK(s.asset_problems().empty());
	// An unknown clip rests and says why.
	animator.clip = "Nope";
	ENGINE_REQUIRE(s.execute(commands::SetAnimator{bar, animator}));
	ENGINE_CHECK_NEAR(tip_x(frame_of(s), bar), 0, 1e-3);
	ENGINE_REQUIRE(s.asset_problems().size() == 1);
	ENGINE_CHECK(s.asset_problems()[0].find("no clip 'Nope'") != std::string::npos);
	// An animator on an entity that draws no model says so too.
	Components lonely;
	lonely.animator = AnimatorComponent{};
	ENGINE_REQUIRE(s.execute(commands::CreateEntity{"Lonely", {}, 0, 0, lonely}));
	(void)frame_of(s);
	bool reported = false;
	for (const auto& p : s.asset_problems()) reported = reported || p.find("Animator on 'Lonely'") != std::string::npos;
	ENGINE_CHECK(reported);
}

ENGINE_TEST(bone_lines_show_the_skeleton_as_it_is_drawn) {
	const auto folder = fresh_folder("bones");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(folder));
	const auto bar = place(s, animated_bar(s, folder, "bar.glb", slide_and_hold));
	(void)frame_of(s);
	auto lines = s.bone_lines(bar);
	ENGINE_REQUIRE(lines.size() == 1);
	check_near(lines[0].from, {0, 0, 0});
	check_near(lines[0].to, {0, 1, 0});
	s.update(500ms);
	(void)frame_of(s);
	lines = s.bone_lines();
	ENGINE_REQUIRE(lines.size() == 1);
	check_near(lines[0].to, {0.5, 1, 0});
	ENGINE_CHECK(s.bone_lines(bar + 1000).empty());
}

ENGINE_TEST(skeletons_and_clips_of_models_nothing_uses_are_freed) {
	const auto folder = fresh_folder("release");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(folder));
	const auto bar = place(s, animated_bar(s, folder, "bar.glb", slide_and_hold));
	(void)frame_of(s);
	const std::weak_ptr<const animation::ClipSet> clips = s.clip_set(bar);
	const std::weak_ptr<const animation::Rig> rig = s.rig(bar);
	ENGINE_REQUIRE(!clips.expired() && !rig.expired());
	// While the model is in the scene, releasing keeps them: the animator would start again otherwise.
	(void)s.release_unused_assets();
	ENGINE_CHECK(s.clip_set(bar) == clips.lock());
	ENGINE_REQUIRE(s.execute(commands::DeleteEntity{bar}));
	(void)frame_of(s);
	(void)s.release_unused_assets();
	ENGINE_CHECK(clips.expired());
	ENGINE_CHECK(rig.expired());
}
