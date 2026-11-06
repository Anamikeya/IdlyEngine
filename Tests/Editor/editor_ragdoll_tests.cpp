// Milestone 5 slice 4 in the editor, without a window: ragdolls planned from a model's skeleton, falling in Play from
// the pose the animator left, and handed back; debris drawn like its template and gone once it settles. Real Scene,
// CommandBus, asset database, Box3D and EditorSession, with Fox from the dependency tree. Nothing reaches the scene
// (D1, D7).
#include "engine/test/test.h"

#include <engine/editor/EditorSession.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

using namespace engine;
using namespace engine::editor;
using namespace std::chrono_literals;

namespace {

std::filesystem::path fresh_folder(const std::string& name) {
	const auto folder = std::filesystem::temp_directory_path() / ("engine-ragdoll-" + name);
	std::filesystem::remove_all(folder);
	std::filesystem::create_directories(folder);
	return folder;
}

const std::filesystem::path fox_file = std::filesystem::path(GAMEENGINE_TEST_RESOURCES_DIR) / "filament/third_party/models/Fox/Fox.glb";

// A floor whose top is at y = 0, and Fox standing on it at 1:100 (about 0.8 m tall), walking, with a Ragdoll.
struct Stage {
	EditorSession s;
	EntityId floor = 0, fox = 0;
	explicit Stage(const std::string& name, bool walking = true) {
		ENGINE_REQUIRE(s.open_project(fresh_folder(name)));
		const auto imported = s.import_asset(fox_file);
		ENGINE_REQUIRE(imported);
		const auto placed = s.place_asset_at(imported.value().id, {0, 0, 0});
		ENGINE_REQUIRE(placed);
		fox = placed.value().created.front();
		auto record = *s.scene().inspect(fox);
		record.transform.scale = {0.01, 0.01, 0.01};
		ENGINE_REQUIRE(s.execute(commands::SetTransform{fox, record.transform}));
		if (walking) {
			auto animator = *record.components.animator;
			animator.clip = "Walk";
			ENGINE_REQUIRE(s.execute(commands::SetAnimator{fox, animator}));
		}
		ENGINE_REQUIRE(s.execute(commands::SetRagdoll{fox, RagdollComponent{}}));
		Components ground;
		ColliderComponent slab;
		slab.shape = ColliderShape::Box;
		slab.half_extents = {20, 0.5, 20};
		ground.colliders.push_back(slab);
		Transform below;
		below.position = {0, -0.5, 0};
		const auto made = s.execute(commands::CreateEntity{"Floor", below, 0, 0, ground});
		ENGINE_REQUIRE(made);
		floor = made.value().created.front();
	}
	// As the editor runs: a frame drawn between ticks hands the animators what the scene says.
	void ticks(int n) {
		for (int i = 0; i < n; ++i) {
			(void)frame();
			s.update(EditorSession::fixed_step);
		}
	}
	renderer::Frame frame() {
		const auto f = s.frame(1.5);
		ENGINE_REQUIRE(f);
		return f.value();
	}
	// A joint as drawn now, in the world.
	glm::dvec3 joint(const char* name) {
		const auto w = s.joint_world(fox, name);
		ENGINE_REQUIRE(w.has_value());
		return {w->matrix[12], w->matrix[13], w->matrix[14]};
	}
};

std::string joint_name(EditorSession& s, EntityId id, int joint) {
	return s.rig(id)->joint_name(std::size_t(joint));
}

bool has_body(EditorSession& s, EntityId id, const physics::RagdollPlan& plan, const std::string& name) {
	return std::any_of(plan.bodies.begin(), plan.bodies.end(), [&](const auto& b) { return joint_name(s, id, b.joint) == name; });
}

} // namespace

ENGINE_TEST(fox_gets_a_ragdoll_of_its_long_bones_sized_as_it_stands) {
	Stage stage("plan", false);
	auto& s = stage.s;
	std::string problem;
	const auto plan = s.ragdoll_plan(stage.fox, &problem);
	ENGINE_REQUIRE(plan.has_value());
	ENGINE_CHECK(problem.empty());
	for (const auto& body : plan->bodies)
		std::printf("  %-18s %5.3f m long, %5.3f m thick, %5.2f kg, %zu riding\n", joint_name(s, stage.fox, body.joint).c_str(), body.length,
			body.radius, body.mass, body.more.size());
	// Its hips, spine, neck, head, legs, arms and tail; not the helper joints at the model's origin.
	for (const char* name : {"b_Hip_01", "b_Spine01_02", "b_Spine02_03", "b_Neck_04", "b_Head_05", "b_LeftLeg01_015", "b_RightUpperArm_06",
							 "b_Tail03_014"})
		ENGINE_CHECK(has_body(s, stage.fox, *plan, name));
	ENGINE_CHECK(!has_body(s, stage.fox, *plan, "_rootJoint"));
	ENGINE_CHECK(!has_body(s, stage.fox, *plan, "b_Root_00"));
	ENGINE_CHECK(plan->bodies.size() >= 12 && plan->bodies.size() <= 20);
	// Sized in meters at 1:100: no part longer than the fox, and less mass than water filling half its box would have.
	const auto& box = s.model(s.scene().inspect(stage.fox)->components.mesh->mesh_asset)->bounds;
	const double volume = (box.max[0] - box.min[0]) * (box.max[1] - box.min[1]) * (box.max[2] - box.min[2]) * 1e-6;
	ENGINE_CHECK(plan->mass > 1 && plan->mass < 0.5 * 1000 * volume);
	for (const auto& body : plan->bodies) {
		ENGINE_CHECK(body.length > 0.01f && body.length < 0.5f);
		ENGINE_CHECK(body.radius > 0.005f && body.radius < 0.2f);
	}
	// At 1:50 every length doubles.
	auto record = *s.scene().inspect(stage.fox);
	record.transform.scale = {0.02, 0.02, 0.02};
	ENGINE_REQUIRE(s.execute(commands::SetTransform{stage.fox, record.transform}));
	const auto bigger = s.ragdoll_plan(stage.fox);
	ENGINE_REQUIRE(bigger.has_value() && bigger->bodies.size() == plan->bodies.size());
	ENGINE_CHECK_NEAR(bigger->bodies.front().length, 2 * plan->bodies.front().length, 1e-4);
}

ENGINE_TEST(ragdoll_settings_shape_the_plan_and_problems_say_why) {
	Stage stage("settings", false);
	auto& s = stage.s;
	const auto plain = s.ragdoll_plan(stage.fox);
	ENGINE_REQUIRE(plain.has_value());
	RagdollComponent tuned;
	tuned.bones = {{"b_Tail01_012", RagdollBody::None}, {"b_Tail02_013", RagdollBody::None}, {"b_Tail03_014", RagdollBody::None},
				   {"b_Head_05", RagdollBody::Auto, 0.05, 2000, 10, 5}};
	ENGINE_REQUIRE(s.execute(commands::SetRagdoll{stage.fox, tuned}));
	const auto plan = s.ragdoll_plan(stage.fox);
	ENGINE_REQUIRE(plan.has_value());
	ENGINE_CHECK(!has_body(s, stage.fox, *plan, "b_Tail02_013"));
	ENGINE_CHECK(plan->bodies.size() < plain->bodies.size());
	const auto head = std::find_if(plan->bodies.begin(), plan->bodies.end(), [&](const auto& b) { return joint_name(s, stage.fox, b.joint) == "b_Head_05"; });
	ENGINE_REQUIRE(head != plan->bodies.end());
	ENGINE_CHECK_NEAR(head->radius, 0.05, 1e-6);
	ENGINE_CHECK_NEAR(head->density, 2000, 1e-3);
	ENGINE_CHECK_NEAR(head->cone, glm::radians(10.0), 1e-6);
	ENGINE_CHECK_NEAR(head->twist, glm::radians(5.0), 1e-6);
	// Squashed, it cannot be built: bodies are round.
	auto record = *s.scene().inspect(stage.fox);
	record.transform.scale = {0.01, 0.02, 0.01};
	ENGINE_REQUIRE(s.execute(commands::SetTransform{stage.fox, record.transform}));
	std::string problem;
	ENGINE_CHECK(!s.ragdoll_plan(stage.fox, &problem));
	ENGINE_CHECK(problem.find("same on every axis") != std::string::npos);
	// No ragdoll, no model.
	ENGINE_CHECK(!s.ragdoll_plan(stage.floor, &problem));
	ENGINE_CHECK(problem.find("no ragdoll") != std::string::npos);
	ENGINE_REQUIRE(s.execute(commands::SetRagdoll{stage.floor, RagdollComponent{}}));
	ENGINE_CHECK(!s.ragdoll_plan(stage.floor, &problem));
	ENGINE_CHECK(problem.find("draws no model") != std::string::npos);
	// Ragdolls fall only in Play.
	ENGINE_CHECK(!s.start_ragdoll(stage.fox));
	ENGINE_CHECK(!s.ragdolled(stage.fox));
}

ENGINE_TEST(fox_falls_in_play_from_its_walk_and_never_reaches_the_scene) {
	Stage stage("fall");
	auto& s = stage.s;
	s.mark_clean();
	const auto editor_revision = s.scene_revision();
	const auto history = s.history_names().size();
	ENGINE_REQUIRE(s.play());
	stage.ticks(20);
	const auto runtime_revision = s.scene_revision();
	const auto head_walking = stage.joint("b_Head_05");
	const auto walking = s.shown_pose(stage.fox);
	ENGINE_REQUIRE(s.start_ragdoll(stage.fox));
	ENGINE_CHECK(s.ragdolled(stage.fox));
	ENGINE_CHECK(s.ragdolled_entities() == std::vector<EntityId>{stage.fox});
	ENGINE_CHECK(s.start_ragdoll(stage.fox)); // Again is nothing new.
	// It starts exactly where the walk left it.
	const auto start = s.shown_pose(stage.fox);
	ENGINE_REQUIRE(start && walking && start->size() == walking->size());
	for (std::size_t j = 0; j < start->size(); ++j)
		for (int k = 12; k < 15; ++k) ENGINE_CHECK_NEAR((*start)[j][std::size_t(k)], (*walking)[j][std::size_t(k)], 1e-3);
	ENGINE_CHECK(glm::length(stage.joint("b_Head_05") - head_walking) < 1e-4);
	const auto& diagnostics = s.physics_bridge().diagnostics();
	ENGINE_CHECK_EQ(diagnostics.ragdolls, std::size_t(1));
	ENGINE_CHECK(diagnostics.ragdoll_parts >= 12);
	// It collapses onto the floor over two seconds, and comes to rest.
	stage.ticks(120);
	const auto head_down = stage.joint("b_Head_05");
	ENGINE_CHECK(head_down.y < head_walking.y - 0.1);
	ENGINE_CHECK(head_down.y > -0.01);
	stage.ticks(300);
	ENGINE_CHECK(!s.physics_bridge().ragdoll(s.physics_bridge().ragdoll_of(stage.fox))->awake);
	// The frame draws the ragdoll's pose; picking still finds it there.
	const auto frame = stage.frame();
	const auto object = std::find_if(frame->objects.begin(), frame->objects.end(), [&](const auto& o) { return o.entity == stage.fox; });
	ENGINE_REQUIRE(object != frame->objects.end());
	ENGINE_CHECK(object->pose == s.shown_pose(stage.fox));
	// Nothing of it is in the run's scene, let alone the editor's.
	ENGINE_CHECK_EQ(s.scene_revision(), runtime_revision);
	ENGINE_REQUIRE(s.stop());
	ENGINE_CHECK(!s.ragdolled(stage.fox));
	ENGINE_CHECK_EQ(s.scene_revision(), editor_revision);
	ENGINE_CHECK_EQ(s.history_names().size(), history);
	ENGINE_CHECK(!s.dirty());
}

ENGINE_TEST(a_ragdoll_moves_on_as_the_pose_was_moving) {
	Stage stage("velocity");
	auto& s = stage.s;
	ENGINE_REQUIRE(s.play());
	stage.ticks(10);
	// How the walk moved the right forefoot over the last tick.
	const auto before = s.animation().previous_pose(stage.fox), now = s.animation().pose(stage.fox);
	ENGINE_REQUIRE(before && now);
	const auto index = *s.rig(stage.fox)->find_joint("b_RightLeg02_020");
	const glm::dmat4 world = glm::make_mat4(s.drawn_world(stage.fox)->matrix.data());
	const auto at = [&](const animation::Pose& p) { return glm::dvec3(world * glm::dvec4((*p)[index][12], (*p)[index][13], (*p)[index][14], 1)); };
	const glm::dvec3 velocity = (at(now) - at(before)) / std::chrono::duration<double>(EditorSession::fixed_step).count();
	ENGINE_CHECK(glm::length(velocity) > 0.05); // The walk moves it.
	ENGINE_REQUIRE(s.start_ragdoll(stage.fox));
	// Over its first tick the joint carries on as it was going; gravity and the joints change that a little.
	const auto start = stage.joint("b_RightLeg02_020");
	stage.ticks(1);
	const glm::dvec3 moved = (stage.joint("b_RightLeg02_020") - start) / std::chrono::duration<double>(EditorSession::fixed_step).count();
	ENGINE_CHECK(glm::length(moved - velocity) < 0.35 * glm::length(velocity) + 0.2);
}

ENGINE_TEST(a_shot_ragdoll_is_pushed_where_it_was_hit_and_can_stand_again) {
	Stage stage("shot", false);
	auto& s = stage.s;
	ENGINE_REQUIRE(s.play());
	stage.ticks(2);
	const auto head = stage.joint("b_Head_05");
	// A shot from the side into the middle of the head's capsule.
	const auto plan = s.ragdoll_plan(stage.fox);
	ENGINE_REQUIRE(plan.has_value());
	const auto head_index = int(*s.rig(stage.fox)->find_joint("b_Head_05"));
	const auto& capsule = std::find_if(plan->bodies.begin(), plan->bodies.end(), [&](const auto& b) { return b.joint == head_index; })->capsule;
	const glm::dmat4 joint = glm::make_mat4(s.joint_world(stage.fox, "b_Head_05")->matrix.data());
	const glm::dmat3 turn(glm::normalize(glm::dvec3(joint[0])), glm::normalize(glm::dvec3(joint[1])), glm::normalize(glm::dvec3(joint[2])));
	const glm::dvec3 middle = head + turn * (glm::dvec3(capsule.point_a.x + capsule.point_b.x, capsule.point_a.y + capsule.point_b.y, capsule.point_a.z + capsule.point_b.z) * 0.5);
	ENGINE_REQUIRE(s.start_ragdoll(stage.fox, std::pair<Vec3, Vec3>{{30, 0, 0}, {middle.x, middle.y, middle.z}}));
	const auto state = s.physics_bridge().ragdoll(s.physics_bridge().ragdoll_of(stage.fox));
	ENGINE_REQUIRE(state.has_value());
	float fastest = 0;
	int fastest_bone = -1;
	for (const auto& p : state->parts)
		if (p.linear_velocity.x > fastest) { fastest = p.linear_velocity.x; fastest_bone = p.bone; }
	ENGINE_CHECK_EQ(fastest_bone, head_index);
	stage.ticks(30);
	ENGINE_CHECK(stage.joint("b_Head_05").x > head.x + 0.05);
	// Standing again: the animator poses it, and its bodies are gone.
	ENGINE_REQUIRE(s.stop_ragdoll(stage.fox));
	ENGINE_CHECK(!s.ragdolled(stage.fox));
	ENGINE_CHECK_EQ(s.physics_bridge().diagnostics().ragdolls, std::size_t(0));
	ENGINE_CHECK(s.shown_pose(stage.fox) == s.animation().pose(stage.fox));
	ENGINE_CHECK(!s.stop_ragdoll(stage.fox));
	// Restart throws the run away, ragdolls and all.
	ENGINE_REQUIRE(s.start_ragdoll(stage.fox));
	ENGINE_REQUIRE(s.restart());
	ENGINE_CHECK(s.ragdolled_entities().empty());
}

ENGINE_TEST(a_ragdoll_goes_with_its_entity) {
	Stage stage("deleted", false);
	auto& s = stage.s;
	ENGINE_REQUIRE(s.play());
	ENGINE_REQUIRE(s.start_ragdoll(stage.fox));
	ENGINE_REQUIRE(s.execute(commands::DeleteEntity{stage.fox}));
	stage.ticks(1);
	ENGINE_CHECK(s.ragdolled_entities().empty());
	ENGINE_CHECK_EQ(s.physics_bridge().diagnostics().ragdolls, std::size_t(0));
	(void)stage.frame();
}

ENGINE_TEST(show_ragdolls_outlines_the_capsules_where_the_pose_puts_them) {
	Stage stage("lines", false);
	auto& s = stage.s;
	const auto plan = s.ragdoll_plan(stage.fox);
	ENGINE_REQUIRE(plan.has_value());
	const auto lines = s.ragdoll_lines();
	ENGINE_CHECK(!lines.empty());
	ENGINE_CHECK(std::all_of(lines.begin(), lines.end(), [&](const auto& l) { return l.entity == stage.fox && l.category == physics::DebugCategory::Ragdoll; }));
	// Around the fox: within its height, above the floor.
	for (const auto& l : lines) {
		ENGINE_CHECK(l.a.y > -0.15f && l.a.y < 1.0f);
		ENGINE_CHECK(std::abs(l.a.x) < 0.5f && std::abs(l.a.z) < 1.2f);
	}
	ENGINE_CHECK(s.ragdoll_lines(stage.floor).empty());
	// A hidden entity draws none while editing.
	s.set_hidden(stage.fox, true);
	ENGINE_CHECK(s.ragdoll_lines().empty());
	s.set_hidden(stage.fox, false);
	// In Play, once it falls, they follow the bodies down.
	ENGINE_REQUIRE(s.play());
	ENGINE_REQUIRE(s.start_ragdoll(stage.fox));
	stage.ticks(180);
	float highest = -1;
	for (const auto& l : s.ragdoll_lines()) highest = std::max(highest, l.a.y);
	float before = -1;
	for (const auto& l : lines) before = std::max(before, l.a.y);
	ENGINE_CHECK(highest < before - 0.05f);
}

ENGINE_TEST(debris_in_play_looks_like_its_template_and_goes_once_it_settles) {
	Stage stage("debris", false);
	auto& s = stage.s;
	Components look;
	look.mesh = MeshComponent{};
	look.mesh->color = {0.2, 0.6, 0.9};
	Transform small;
	small.position = {5, 0.05, 0};
	small.scale = {0.1, 0.1, 0.1};
	const auto made = s.execute(commands::CreateEntity{"Shard", small, 0, 0, look});
	ENGINE_REQUIRE(made);
	const auto shard = made.value().created.front();
	ENGINE_CHECK(!s.spawn_debris(shard, {0, 1, 0}, {}, {}));
	ENGINE_REQUIRE(s.play());
	ENGINE_CHECK(!s.spawn_debris(stage.floor, {0, 1, 0}, {}, {})); // No mesh renderer to look like.
	std::vector<std::uint64_t> pieces;
	for (int i = 0; i < 3; ++i) {
		const auto piece = s.spawn_debris(shard, {double(i) * 0.3, 1.0, 1.0}, {}, {0, 2, 0}, {3, 0, 1});
		ENGINE_REQUIRE(piece);
		pieces.push_back(piece.value());
	}
	stage.ticks(3);
	const auto frame = stage.frame();
	std::size_t drawn = 0;
	const auto& template_object = *std::find_if(frame->objects.begin(), frame->objects.end(), [&](const auto& o) { return o.entity == shard; });
	for (const auto& o : frame->objects) {
		if (o.entity < renderer::runtime_object_base) continue;
		++drawn;
		ENGINE_CHECK(o.mesh == template_object.mesh);
		ENGINE_CHECK(o.surface.linear_color.z == template_object.surface.linear_color.z);
		// At the template's scale, where its body is.
		ENGINE_CHECK_NEAR(glm::length(glm::dvec3(o.world.matrix[0], o.world.matrix[1], o.world.matrix[2])), 0.1, 1e-4);
		ENGINE_CHECK(o.world.matrix[13] > 0.9 && o.world.matrix[13] < 1.2);
	}
	ENGINE_CHECK_EQ(drawn, std::size_t(3));
	// Debris cannot be picked: a click on a piece finds what is behind it, if anything.
	const auto& piece = *std::find_if(frame->objects.begin(), frame->objects.end(), [](const auto& o) { return o.entity >= renderer::runtime_object_base; });
	const glm::dvec4 clip = glm::make_mat4(frame->camera.projection.matrix.data()) * glm::make_mat4(frame->camera.view.matrix.data()) *
		glm::dvec4(piece.world.matrix[12], piece.world.matrix[13], piece.world.matrix[14], 1);
	ENGINE_REQUIRE(clip.w > 0 && std::abs(clip.x / clip.w) < 1 && std::abs(clip.y / clip.w) < 1);
	ENGINE_CHECK(s.pick(*frame, clip.x / clip.w, clip.y / clip.w) < renderer::runtime_object_base);
	// It lands, falls asleep, and is gone two seconds later; none of it was ever in the scene.
	const auto revision = s.scene_revision();
	stage.ticks(360);
	ENGINE_CHECK(s.debris().empty());
	ENGINE_CHECK_EQ(s.physics_bridge().diagnostics().debris_expired, std::uint64_t(3));
	ENGINE_CHECK_EQ(s.scene_revision(), revision);
	// At most physics::max_debris at once; Stop takes the rest.
	for (int i = 0; i < 70; ++i) ENGINE_REQUIRE(s.spawn_debris(shard, {double(i % 8) * 0.3, 2.0 + double(i / 8) * 0.3, 0}, {}, {}));
	ENGINE_CHECK_EQ(s.debris().size(), physics::max_debris);
	ENGINE_REQUIRE(s.stop());
	ENGINE_CHECK(s.debris().empty());
	const auto after = stage.frame();
	ENGINE_CHECK(std::none_of(after->objects.begin(), after->objects.end(), [](const auto& o) { return o.entity >= renderer::runtime_object_base; }));
}
