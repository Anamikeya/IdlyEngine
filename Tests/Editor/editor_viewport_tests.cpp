// Viewport behavior without a window: the camera state machine, framing, floor snapping, vertex snapping, hiding,
// bookmarks and orthographic views. Real Scene, CommandBus and EditorSession; no mocks.
#include "engine/test/test.h"
#include "../Assets/glb_writer.hpp"

#include <engine/editor/CameraController.hpp>
#include <engine/editor/EditorCommands.hpp>
#include <engine/editor/EditorPrefs.hpp>
#include <engine/editor/EditorSession.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

using namespace engine;
using namespace engine::editor;

namespace {
constexpr double frame_time = 1.0 / 60;
double length(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z); }
// A frame of nothing but the time passing, with the mouse over the view.
CameraInput idle() {
	CameraInput in;
	in.dt = frame_time;
	in.viewport_height = 600;
	in.can_start = true;
	return in;
}
// Starts a mode: one frame with the button up, one with it down.
void press(CameraController& control, OrbitCamera& camera, CameraInput in) {
	control.update(camera, in);
}
std::filesystem::path fresh_folder(const std::string& name) {
	const auto folder = std::filesystem::temp_directory_path() / ("engine-viewport-" + name);
	std::filesystem::remove_all(folder);
	std::filesystem::create_directories(folder);
	return folder;
}
EntityId make(EditorSession& s, const std::string& name, Vec3 position = {}, Vec3 scale = {1, 1, 1}, BuiltinMesh mesh = BuiltinMesh::Cube) {
	Transform t;
	t.position = position; t.scale = scale;
	const auto created = s.execute(commands::CreateEntity{name, t, 0, 0, Components{MeshComponent{mesh}, {}, {}}});
	ENGINE_REQUIRE(created);
	return created.value().created.front();
}
double y_of(const EditorSession& s, EntityId id) { return s.scene().world_transform(id)->matrix[13]; }
double x_of(const EditorSession& s, EntityId id) { return s.scene().world_transform(id)->matrix[12]; }
renderer::Frame frame_of(EditorSession& s, double aspect = 1.5) {
	const auto f = s.frame(aspect);
	ENGINE_REQUIRE(f);
	return f.value();
}
} // namespace

// ---- OrbitCamera ----
ENGINE_TEST(looking_keeps_the_eye_where_it_is) {
	OrbitCamera c;
	const auto eye = c.eye();
	c.look(120, -40);
	ENGINE_CHECK_NEAR(length(c.eye(), eye), 0, 1e-9);
	ENGINE_CHECK(c.yaw != OrbitCamera{}.yaw && c.pitch != OrbitCamera{}.pitch);
}
ENGINE_TEST(orbiting_around_the_target_matches_a_plain_orbit) {
	OrbitCamera a, b;
	a.orbit(50, 20);
	b.orbit_around(b.target, 50, 20);
	ENGINE_CHECK_NEAR(length(a.eye(), b.eye()), 0, 1e-9);
	ENGINE_CHECK_NEAR(length(a.target, b.target), 0, 1e-9);
}
ENGINE_TEST(orbiting_around_a_point_keeps_the_distance_to_it) {
	OrbitCamera c;
	const Vec3 pivot{3, 1, -2};
	const auto before = length(c.eye(), pivot);
	c.orbit_around(pivot, 80, -35);
	ENGINE_CHECK_NEAR(length(c.eye(), pivot), before, 1e-9);
}
ENGINE_TEST(forward_and_right_are_perpendicular_units) {
	OrbitCamera c;
	c.yaw = 1.1; c.pitch = -0.3;
	const auto f = c.forward(), r = c.right();
	ENGINE_CHECK_NEAR(std::hypot(f.x, f.y, f.z), 1, 1e-9);
	ENGINE_CHECK_NEAR(std::hypot(r.x, r.y, r.z), 1, 1e-9);
	ENGINE_CHECK_NEAR(f.x * r.x + f.y * r.y + f.z * r.z, 0, 1e-9);
	// Looking along -Z from yaw 0, right is +X.
	OrbitCamera straight;
	straight.yaw = 0; straight.pitch = 0;
	ENGINE_CHECK_NEAR(straight.right().x, 1, 1e-9);
	ENGINE_CHECK_NEAR(straight.forward().z, -1, 1e-9);
}

// ---- The state machine ----
ENGINE_TEST(a_press_starts_a_mode_only_over_the_view) {
	OrbitCamera c;
	CameraController control;
	auto in = idle();
	in.can_start = false; in.rmb = true;
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::Idle);
	// Held from elsewhere and then moved over the view: that is not a press.
	in.can_start = true;
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::Idle);
	in.rmb = false;
	control.update(c, in);
	in.rmb = true;
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::Fly);
}
ENGINE_TEST(a_mode_holds_until_its_button_comes_up_wherever_the_mouse_goes) {
	OrbitCamera c;
	CameraController control;
	auto in = idle();
	in.rmb = true;
	control.update(c, in);
	ENGINE_CHECK(control.holds_cursor());
	in.can_start = false;
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::Fly);
	in.rmb = false;
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::Idle);
	ENGINE_CHECK(!control.holds_cursor());
}
ENGINE_TEST(flying_moves_along_the_view_and_eases_in) {
	OrbitCamera c;
	c.yaw = 0; c.pitch = 0;
	CameraController control;
	control.set_speed(10);
	auto in = idle();
	in.rmb = true;
	control.update(c, in);
	in.forward = true;
	const auto start = c.eye();
	control.update(c, in);
	const auto first_step = length(c.eye(), start);
	ENGINE_CHECK(first_step > 0);
	ENGINE_CHECK(first_step < 10 * frame_time * 0.5); // Not at full speed yet.
	for (int i = 0; i < 120; ++i) control.update(c, in);
	const auto a = c.eye();
	control.update(c, in);
	ENGINE_CHECK_NEAR(length(c.eye(), a), 10 * frame_time, 1e-3); // Full speed by now.
	ENGINE_CHECK_NEAR(c.eye().x, start.x, 1e-9);
	ENGINE_CHECK(c.eye().z < start.z); // Forward, along -Z here.
}
ENGINE_TEST(flying_covers_every_direction) {
	const auto fly = [](auto set) {
		OrbitCamera c;
		c.yaw = 0; c.pitch = 0;
		CameraController control;
		auto in = idle();
		in.rmb = true;
		control.update(c, in);
		set(in);
		const auto start = c.eye();
		for (int i = 0; i < 30; ++i) control.update(c, in);
		const auto end = c.eye();
		return Vec3{end.x - start.x, end.y - start.y, end.z - start.z};
	};
	ENGINE_CHECK(fly([](CameraInput& in) { in.back = true; }).z > 0.1);
	ENGINE_CHECK(fly([](CameraInput& in) { in.left = true; }).x < -0.1);
	ENGINE_CHECK(fly([](CameraInput& in) { in.right = true; }).x > 0.1);
	ENGINE_CHECK(fly([](CameraInput& in) { in.up = true; }).y > 0.1);
	ENGINE_CHECK(fly([](CameraInput& in) { in.down = true; }).y < -0.1);
}
ENGINE_TEST(shift_flies_faster) {
	const auto distance = [](bool shift) {
		OrbitCamera c;
		CameraController control;
		auto in = idle();
		in.rmb = true;
		control.update(c, in);
		in.forward = true; in.shift = shift;
		const auto start = c.eye();
		for (int i = 0; i < 120; ++i) control.update(c, in);
		return length(c.eye(), start);
	};
	ENGINE_CHECK_NEAR(distance(true) / distance(false), 3, 0.01);
}
ENGINE_TEST(the_wheel_changes_speed_while_flying_and_says_so) {
	OrbitCamera c;
	CameraController control;
	const auto start = control.speed();
	auto in = idle();
	in.rmb = true;
	control.update(c, in);
	const auto distance = c.distance;
	in.wheel = 2;
	control.update(c, in);
	ENGINE_CHECK(control.speed() > start);
	ENGINE_CHECK(control.speed_shown() > 0);
	ENGINE_CHECK_EQ(c.distance, distance); // It did not zoom.
	in.wheel = -40;
	control.update(c, in);
	ENGINE_CHECK_EQ(control.speed(), CameraController::min_speed);
}
ENGINE_TEST(the_wheel_zooms_when_not_flying_only_over_the_view) {
	OrbitCamera c;
	CameraController control;
	const auto distance = c.distance;
	auto in = idle();
	in.wheel = 1;
	in.can_start = false;
	control.update(c, in);
	ENGINE_CHECK_EQ(c.distance, distance);
	in.can_start = true;
	control.update(c, in);
	ENGINE_CHECK(c.distance < distance);
}
ENGINE_TEST(z_and_c_change_the_field_of_view_until_the_button_comes_up) {
	OrbitCamera c;
	CameraController control;
	auto in = idle();
	in.rmb = true;
	control.update(c, in);
	in.fov_out = true;
	for (int i = 0; i < 30; ++i) control.update(c, in);
	ENGINE_CHECK(c.fov_degrees > 70);
	in.fov_out = false; in.fov_in = true;
	for (int i = 0; i < 90; ++i) control.update(c, in);
	ENGINE_CHECK(c.fov_degrees < 50);
	in = idle();
	for (int i = 0; i < 120; ++i) control.update(c, in);
	ENGINE_CHECK_EQ(c.fov_degrees, 60.0);
}
ENGINE_TEST(alt_buttons_orbit_dolly_and_pan) {
	OrbitCamera c;
	CameraController control;
	const Vec3 pivot{2, 0, 0};
	auto in = idle();
	in.alt = true; in.lmb = true; in.pivot = pivot;
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::Orbit);
	const auto to_pivot = length(c.eye(), pivot);
	in.mouse_dx = 40; in.mouse_dy = 10;
	control.update(c, in);
	ENGINE_CHECK_NEAR(length(c.eye(), pivot), to_pivot, 1e-9);
	in.alt = false; // Letting go of Alt first does not end it.
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::Orbit);
	in = idle();
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::Idle);

	in = idle(); in.alt = true; in.rmb = true;
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::Dolly);
	const auto distance = c.distance;
	in.mouse_dx = 30;
	control.update(c, in);
	ENGINE_CHECK(c.distance < distance);
	in = idle();
	control.update(c, in);

	in = idle(); in.alt = true; in.mmb = true;
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::Pan);
	const auto target = c.target;
	in.mouse_dx = 30;
	control.update(c, in);
	ENGINE_CHECK(length(c.target, target) > 0);
}
ENGINE_TEST(an_lmb_click_is_a_click_and_a_drag_is_not) {
	OrbitCamera c;
	CameraController control;
	auto in = idle();
	in.lmb = true;
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::ClickPending);
	ENGINE_CHECK(!control.holds_cursor());
	in.mouse_dx = 1;
	control.update(c, in);
	in = idle();
	control.update(c, in);
	ENGINE_CHECK(control.clicked());
	ENGINE_CHECK(control.mode() == CameraMode::Idle);
	control.update(c, in);
	ENGINE_CHECK(!control.clicked()); // One frame only.

	in = idle(); in.lmb = true;
	control.update(c, in);
	in.mouse_dx = 30;
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::Walk);
	in = idle();
	control.update(c, in);
	ENGINE_CHECK(!control.clicked());
}
ENGINE_TEST(lmb_drag_walks_and_turns_on_the_ground) {
	OrbitCamera c;
	c.pitch = 0.5;
	CameraController control;
	auto in = idle();
	in.lmb = true;
	control.update(c, in);
	in.mouse_dx = 10;
	control.update(c, in);
	ENGINE_REQUIRE(control.mode() == CameraMode::Walk);
	in.mouse_dx = 0; in.mouse_dy = -50;
	const auto eye = c.eye();
	control.update(c, in);
	ENGINE_CHECK(length(c.eye(), eye) > 0);
	ENGINE_CHECK_NEAR(c.eye().y, eye.y, 1e-9); // Stays level, whatever the camera's pitch.
	in.mouse_dy = 0; in.mouse_dx = 25;
	const auto yaw = c.yaw;
	control.update(c, in);
	ENGINE_CHECK(c.yaw != yaw);
}
ENGINE_TEST(both_buttons_move_up_and_sideways) {
	OrbitCamera c;
	CameraController control;
	auto in = idle();
	in.rmb = true;
	control.update(c, in);
	in.lmb = true;
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::Vertical);
	in.mouse_dy = -20;
	const auto y = c.target.y;
	control.update(c, in);
	ENGINE_CHECK(c.target.y > y);
	in.mouse_dy = 0; in.lmb = false;
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::Fly);
}
ENGINE_TEST(an_orthographic_view_pans_instead_of_turning) {
	OrbitCamera c;
	c.orthographic = true;
	CameraController control;
	auto in = idle();
	in.rmb = true;
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::Pan);
	const auto yaw = c.yaw, pitch = c.pitch;
	const auto target = c.target;
	in.mouse_dx = 30;
	control.update(c, in);
	ENGINE_CHECK_EQ(c.yaw, yaw);
	ENGINE_CHECK_EQ(c.pitch, pitch);
	ENGINE_CHECK(length(c.target, target) > 0);
	in = idle();
	control.update(c, in);
	in = idle(); in.alt = true; in.lmb = true;
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::Idle); // No orbiting.
	in = idle();
	control.update(c, in);
	in = idle(); in.lmb = true;
	control.update(c, in);
	in.mouse_dx = 40;
	control.update(c, in);
	ENGINE_CHECK(control.mode() == CameraMode::ClickPending); // No walking.
}
ENGINE_TEST(an_animation_eases_to_the_goal_and_takes_the_short_way_around) {
	OrbitCamera c;
	c.yaw = 3.0; c.target = {0, 0, 0}; c.distance = 10;
	auto goal = c;
	goal.yaw = -3.0; goal.target = {6, 0, 0}; goal.distance = 2;
	CameraController control;
	control.animate_to(c, goal, 0.2);
	ENGINE_CHECK(control.animating());
	auto in = idle();
	in.dt = 0.1;
	control.update(c, in);
	ENGINE_CHECK_NEAR(c.target.x, 3, 1e-9); // Halfway, at the middle of the ease.
	ENGINE_CHECK_NEAR(c.distance, 6, 1e-9);
	ENGINE_CHECK(std::abs(c.yaw) > 3); // Through pi, not through zero.
	control.update(c, in);
	ENGINE_CHECK(!control.animating());
	ENGINE_CHECK_NEAR(c.target.x, 6, 1e-9);
	ENGINE_CHECK_NEAR(c.yaw, -3, 1e-9);
}
ENGINE_TEST(navigating_cancels_an_animation) {
	OrbitCamera c;
	auto goal = c;
	goal.target = {10, 0, 0};
	CameraController control;
	control.animate_to(c, goal);
	auto in = idle();
	in.rmb = true;
	control.update(c, in);
	ENGINE_CHECK(!control.animating());
}
ENGINE_TEST(reset_ends_a_mode_at_once) {
	OrbitCamera c;
	CameraController control;
	auto in = idle();
	in.rmb = true;
	press(control, c, in);
	ENGINE_REQUIRE(control.holds_cursor());
	control.reset();
	ENGINE_CHECK(control.mode() == CameraMode::Idle);
}

// ---- Framing ----
ENGINE_TEST(focus_goal_frames_the_whole_selection) {
	EditorSession s;
	const auto a = make(s, "A", {-4, 0, 0});
	const auto b = make(s, "B", {4, 2, 0});
	const EntityId both[] = {a, b};
	s.selection_set().set(std::span<const EntityId>(both));
	const auto goal = s.focus_goal();
	ENGINE_REQUIRE(goal.has_value());
	ENGINE_CHECK_NEAR(goal->target.x, 0, 1e-9);
	ENGINE_CHECK_NEAR(goal->target.y, 1, 1e-9);
	ENGINE_CHECK(goal->distance > 10);
	ENGINE_CHECK_EQ(goal->yaw, s.camera.yaw); // The view's angle stays.
	s.select(a);
	ENGINE_CHECK(s.focus_goal()->distance < goal->distance);
	s.select(0);
	ENGINE_CHECK(!s.focus_goal().has_value());
	ENGINE_CHECK(!s.selection_center().has_value());
}
ENGINE_TEST(focus_on_something_without_a_size_still_frames_it) {
	EditorSession s;
	Transform t;
	t.position = {1, 2, 3};
	const auto created = s.execute(commands::CreateEntity{"Lamp", t, 0, 0, Components{{}, {}, LightComponent{}}});
	ENGINE_REQUIRE(created);
	s.select(created.value().created.front());
	s.focus();
	ENGINE_CHECK_NEAR(length(s.camera.target, {1, 2, 3}), 0, 1e-9);
	ENGINE_CHECK_NEAR(s.camera.distance, 3, 1e-9);
}

// ---- Rays and the floor ----
ENGINE_TEST(a_ray_hits_a_cube_then_the_ground_then_nothing) {
	EditorSession s;
	make(s, "Cube", {0, 1, 0});
	s.camera.target = {0, 1, 0}; s.camera.yaw = 0; s.camera.pitch = 0; s.camera.distance = 10;
	const auto f = frame_of(s);
	const auto on_cube = s.ray_hit(*f, 0, 0);
	ENGINE_REQUIRE(on_cube.has_value());
	ENGINE_CHECK_NEAR(on_cube->z, 0.5, 1e-6); // The front face.
	const auto below = s.ray_hit(*f, 0.5, -0.5);
	ENGINE_REQUIRE(below.has_value());
	ENGINE_CHECK_NEAR(below->y, 0, 1e-9);
	ENGINE_CHECK(!s.ray_hit(*f, 0.5, 0.9).has_value()); // Up into the sky.
}
ENGINE_TEST(snap_to_floor_rests_the_bounds_on_the_surface_below) {
	EditorSession s;
	make(s, "Floor", {0, 2, 0}, {10, 1, 10}, BuiltinMesh::Plane);
	const auto cube = make(s, "Cube", {0, 8, 0}, {2, 2, 2});
	s.select(cube);
	const auto steps = s.history_position();
	ENGINE_REQUIRE(s.snap_selection_to_floor(*frame_of(s)));
	ENGINE_CHECK_NEAR(y_of(s, cube), 3, 1e-9); // Floor at 2, half the cube's height above it.
	ENGINE_CHECK_EQ(s.history_position(), steps + 1);
	ENGINE_CHECK_EQ(std::string(s.history_names().back()), std::string("Snap Cube to Floor"));
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_NEAR(y_of(s, cube), 8, 1e-9);
}
ENGINE_TEST(snap_to_floor_uses_the_ground_plane_when_nothing_is_below) {
	EditorSession s;
	const auto cube = make(s, "Cube", {3, 8, 3});
	make(s, "Elsewhere", {-20, 0, -20}, {1, 1, 1}, BuiltinMesh::Plane);
	s.select(cube);
	ENGINE_REQUIRE(s.snap_selection_to_floor(*frame_of(s)));
	ENGINE_CHECK_NEAR(y_of(s, cube), 0.5, 1e-9);
	// Now resting: a second press has nothing to do, and says so.
	ENGINE_CHECK(!s.snap_selection_to_floor(*frame_of(s)));
	ENGINE_CHECK_EQ(s.history_position(), std::size_t{3});
}
ENGINE_TEST(snap_to_floor_ignores_the_selection_itself_and_moves_each_root) {
	EditorSession s;
	const auto a = make(s, "A", {0, 5, 0});
	const auto b = make(s, "B", {0, 9, 0});
	const EntityId both[] = {a, b};
	s.selection_set().set(std::span<const EntityId>(both));
	const auto steps = s.history_position();
	ENGINE_REQUIRE(s.snap_selection_to_floor(*frame_of(s)));
	ENGINE_CHECK_NEAR(y_of(s, a), 0.5, 1e-9); // Neither lands on the other.
	ENGINE_CHECK_NEAR(y_of(s, b), 0.5, 1e-9);
	ENGINE_CHECK_EQ(s.history_position(), steps + 1);
}
ENGINE_TEST(a_model_lands_at_the_point_with_its_base_on_it) {
	const auto folder = fresh_folder("place");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(folder));
	const auto imported = s.import_asset(std::filesystem::path(GAMEENGINE_TEST_ASSETS_DIR) / "editor" / "meshes" / "cube.obj");
	ENGINE_REQUIRE(imported);
	const auto placed = s.place_asset_at(imported.value().id, {4, 1, -2});
	ENGINE_REQUIRE(placed);
	const auto id = placed.value().created.front();
	ENGINE_CHECK_EQ(s.selection(), id);
	const auto bounds = s.local_bounds(id);
	ENGINE_REQUIRE(bounds.has_value());
	ENGINE_CHECK_NEAR(x_of(s, id), 4, 1e-9);
	ENGINE_CHECK_NEAR(y_of(s, id) + bounds->first.y, 1, 1e-6);
}

// ---- Vertex snapping ----
ENGINE_TEST(the_nearest_vertex_is_found_on_screen_and_the_selection_is_left_out) {
	EditorSession s;
	const auto moving = make(s, "Moving", {-3, 0, 0});
	make(s, "Other", {0, 0, 0});
	s.camera.target = {0, 0, 0}; s.camera.yaw = 0; s.camera.pitch = 0; s.camera.distance = 5;
	s.select(moving);
	const auto f = frame_of(s, 1.0);
	// The screen position of Other's front top right corner, (0.5, 0.5, 0.5), by the frame's own matrices.
	const auto& p = f->camera.projection.matrix;
	const auto& v = f->camera.view.matrix;
	const double vx = v[0] * 0.5 + v[4] * 0.5 + v[8] * 0.5 + v[12], vy = v[1] * 0.5 + v[5] * 0.5 + v[9] * 0.5 + v[13], vz = v[2] * 0.5 + v[6] * 0.5 + v[10] * 0.5 + v[14];
	const double w = p[3] * vx + p[7] * vy + p[11] * vz + p[15];
	const double x = ((p[0] * vx + p[4] * vy + p[8] * vz + p[12]) / w + 1) / 2 * 800, y = (1 - (p[1] * vx + p[5] * vy + p[9] * vz + p[13]) / w) / 2 * 800;
	const auto found = s.nearest_vertex(*f, x + 3, y - 3, 800, 800, 30);
	ENGINE_REQUIRE(found.has_value());
	ENGINE_CHECK_NEAR(length(*found, {0.5, 0.5, 0.5}), 0, 1e-9);
	ENGINE_CHECK(!s.nearest_vertex(*f, 5, 5, 800, 800, 30).has_value()); // Nothing near a corner of the screen.
	// The selected cube's corners never count, even under the cursor.
	const auto over_selected = s.nearest_vertex(*f, 800 * 0.05, 400, 800, 800, 10);
	ENGINE_CHECK(!over_selected.has_value());
}
ENGINE_TEST(a_models_vertices_count_too) {
	const auto folder = fresh_folder("vertices");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(folder));
	const auto imported = s.import_asset(std::filesystem::path(GAMEENGINE_TEST_ASSETS_DIR) / "editor" / "meshes" / "cube.obj");
	ENGINE_REQUIRE(imported);
	ENGINE_REQUIRE(s.place_asset(imported.value().id));
	s.select(0);
	s.camera.target = {0, 0.5, 0}; s.camera.yaw = 0; s.camera.pitch = 0; s.camera.distance = 5;
	const auto f = frame_of(s, 1.0);
	const auto found = s.nearest_vertex(*f, 400, 400, 800, 800, 800);
	ENGINE_REQUIRE(found.has_value());
	ENGINE_CHECK(std::abs(found->z) > 0.1 || std::abs(found->x) > 0.1); // A corner of the model, not its origin.
}

// ---- Alt+drag ----
ENGINE_TEST(a_duplicating_drag_moves_copies_in_one_step) {
	EditorSession s;
	const auto original = make(s, "Crate");
	s.select(original);
	const auto steps = s.history_position();
	ENGINE_REQUIRE(s.begin_drag("Move", true));
	const auto copy = s.selection();
	ENGINE_CHECK(copy != original);
	WorldTransform delta;
	delta.matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 0, 0, 1};
	ENGINE_REQUIRE(s.update_drag(delta));
	ENGINE_REQUIRE(s.end_drag());
	ENGINE_CHECK_NEAR(x_of(s, original), 0, 1e-9);
	ENGINE_CHECK_NEAR(x_of(s, copy), 2, 1e-9);
	ENGINE_CHECK_EQ(s.history_position(), steps + 1);
	ENGINE_CHECK_EQ(std::string(s.history_names().back()), std::string("Duplicate and Move Crate"));
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(s.scene().size(), std::size_t{1});
}
ENGINE_TEST(cancelling_a_duplicating_drag_removes_the_copies) {
	EditorSession s;
	const auto original = make(s, "Crate");
	s.select(original);
	const auto steps = s.history_position();
	ENGINE_REQUIRE(s.begin_drag("Move", true));
	ENGINE_REQUIRE(s.cancel_drag());
	ENGINE_CHECK_EQ(s.scene().size(), std::size_t{1});
	ENGINE_CHECK_EQ(s.history_position(), steps);
}

// ---- Hiding ----
ENGINE_TEST(hidden_entities_are_not_drawn_or_picked_and_nothing_gets_dirty) {
	EditorSession s;
	const auto a = make(s, "A");
	s.mark_clean();
	const auto steps = s.history_position();
	s.camera.target = {0, 0, 0}; s.camera.yaw = 0; s.camera.pitch = 0; s.camera.distance = 5;
	ENGINE_CHECK_EQ(s.pick(*frame_of(s), 0, 0), a);
	s.select(a);
	s.hide_selection();
	ENGINE_CHECK(s.hidden(a));
	ENGINE_CHECK(s.selected().empty());
	const auto f = frame_of(s);
	ENGINE_CHECK(f->objects.empty());
	ENGINE_CHECK_EQ(s.pick(*f, 0, 0), EntityId{0});
	ENGINE_CHECK(!s.dirty());
	ENGINE_CHECK_EQ(s.history_position(), steps);
	s.unhide_all();
	ENGINE_CHECK_EQ(frame_of(s)->objects.size(), std::size_t{1});
}
ENGINE_TEST(hiding_takes_the_children_along_and_lights_stop_lighting) {
	EditorSession s;
	const auto parent = make(s, "Parent");
	Components lamp;
	lamp.light = LightComponent{};
	const auto light = s.execute(commands::CreateEntity{"Lamp", Transform{}, parent, 0, lamp});
	ENGINE_REQUIRE(light);
	ENGINE_CHECK_EQ(frame_of(s)->lights.size(), std::size_t{1});
	s.select(parent);
	s.hide_selection();
	ENGINE_CHECK(s.hidden(light.value().created.front()));
	ENGINE_CHECK(frame_of(s)->lights.empty());
}
ENGINE_TEST(play_mode_ignores_what_the_editor_hid) {
	EditorSession s;
	const auto a = make(s, "A");
	s.select(a);
	s.hide_selection();
	ENGINE_REQUIRE(s.play());
	ENGINE_CHECK_EQ(frame_of(s)->objects.size(), std::size_t{1});
	ENGINE_REQUIRE(s.stop());
	ENGINE_CHECK(frame_of(s)->objects.empty());
}

// ---- View state files ----
ENGINE_TEST(view_state_text_round_trips) {
	EditorViewState state;
	state.hidden = {3, 9};
	state.bookmarks[2] = CameraBookmark{{1.5, -2, 3.25}, 0.7, -0.2, 12, false};
	state.bookmarks[9] = CameraBookmark{{0, 0, 0}, 0, 1.5, 4, true};
	const auto read = read_view_state(write_view_state(state));
	ENGINE_REQUIRE(read);
	ENGINE_CHECK(read.value() == state);
	for (const auto* bad : {"", "HIDDEN 3", "EDITORVIEW 2", "EDITORVIEW 1\nHIDDEN x", "EDITORVIEW 1\nBOOKMARK 99 0 0 0 0 0 1 0", "EDITORVIEW 1\nBOOKMARK 1 0 0 0 0 0 -1 0", "EDITORVIEW 1\nWHAT 1"})
		ENGINE_CHECK(!read_view_state(bad));
}
ENGINE_TEST(view_state_lives_beside_the_scene_and_never_in_it) {
	const auto folder = fresh_folder("view-state");
	const auto file = folder / "a.scene", sidecar = folder / "a.scene.editor";
	EditorSession s;
	const auto a = make(s, "A");
	ENGINE_REQUIRE(s.save(file));
	const auto before = read_text_file(file);
	ENGINE_REQUIRE(before);
	ENGINE_CHECK(!std::filesystem::exists(sidecar)); // Nothing to remember yet, so no file.
	s.camera.target = {7, 8, 9};
	s.set_bookmark(3);
	s.select(a);
	s.hide_selection();
	ENGINE_CHECK(std::filesystem::exists(sidecar)); // Written at once, even with the scene unchanged.
	ENGINE_CHECK(!s.dirty());
	ENGINE_REQUIRE(s.save(file));
	ENGINE_CHECK(read_text_file(file).value() == before.value());

	EditorSession other;
	ENGINE_REQUIRE(other.load(file));
	ENGINE_CHECK(other.hidden(a));
	const auto goal = other.bookmark(3);
	ENGINE_REQUIRE(goal.has_value());
	ENGINE_CHECK_NEAR(goal->target.x, 7, 1e-12);
	ENGINE_CHECK(!other.bookmark(4).has_value());
	ENGINE_CHECK(!other.bookmark(99).has_value());
	other.unhide_all();
	other.view_state();
	ENGINE_CHECK(std::filesystem::exists(sidecar)); // The bookmark is still in it.
}
ENGINE_TEST(an_empty_view_state_leaves_no_file) {
	const auto folder = fresh_folder("view-state-empty");
	const auto file = folder / "a.scene", sidecar = folder / "a.scene.editor";
	EditorSession s;
	const auto a = make(s, "A");
	ENGINE_REQUIRE(s.save(file));
	s.select(a);
	s.hide_selection();
	ENGINE_CHECK(std::filesystem::exists(sidecar));
	s.unhide_all();
	ENGINE_CHECK(!std::filesystem::exists(sidecar));
}
ENGINE_TEST(a_broken_view_state_is_reported_and_the_scene_still_loads) {
	const auto folder = fresh_folder("view-state-broken");
	const auto file = folder / "a.scene";
	EditorSession s;
	make(s, "A");
	ENGINE_REQUIRE(s.save(file));
	{ std::ofstream(folder / "a.scene.editor") << "not a view state"; }
	EditorSession other;
	ENGINE_REQUIRE(other.load(file));
	ENGINE_CHECK_EQ(other.scene().size(), std::size_t{1});
	ENGINE_CHECK(other.view_state().empty());
	ENGINE_CHECK(!other.take_view_state_problem().empty());
	ENGINE_CHECK(other.take_view_state_problem().empty());
}
ENGINE_TEST(a_new_scene_starts_with_no_view_state) {
	EditorSession s;
	s.camera.target = {1, 1, 1};
	s.set_bookmark(0);
	const auto a = make(s, "A");
	s.select(a);
	s.hide_selection();
	ENGINE_REQUIRE(s.new_scene());
	ENGINE_CHECK(s.view_state().empty());
}

// ---- Orthographic views, grid, flags ----
ENGINE_TEST(an_orthographic_camera_gives_a_parallel_projection) {
	EditorSession s;
	make(s, "Cube");
	s.camera.target = {0, 0, 0}; s.camera.yaw = 0; s.camera.pitch = 0; s.camera.distance = 10;
	const auto perspective = frame_of(s);
	ENGINE_CHECK_EQ(perspective->camera.projection.matrix[15], 0.0);
	s.camera.orthographic = true;
	const auto flat = frame_of(s);
	ENGINE_CHECK_EQ(flat->camera.projection.matrix[15], 1.0);
	ENGINE_CHECK_NEAR(flat->camera.lens.orthographic_half_height, s.camera.half_height(), 1e-12);
	// The same place on screen hits the same thing, wherever the ray starts.
	ENGINE_CHECK(s.pick(*flat, 0, 0) != 0);
	ENGINE_CHECK_EQ(s.pick(*flat, 0.9, 0), EntityId{0});
	// Things behind the pivot stay in view: the eye stands far back.
	make(s, "Behind", {0, 0, 30});
	const auto behind = frame_of(s);
	const auto hit = s.ray_hit(*behind, 0, 0);
	ENGINE_REQUIRE(hit.has_value());
	ENGINE_CHECK(hit->z > 29);
}
ENGINE_TEST(an_invalid_lens_or_grid_is_refused) {
	Scene scene;
	renderer::PerspectiveCamera lens;
	lens.orthographic_half_height = -1;
	OrbitCamera camera;
	ENGINE_CHECK(!renderer::extract_frame(scene, {1, 0, 1}, lens, camera.world(), {}, {}));
	lens.orthographic_half_height = 0;
	ENGINE_CHECK(!renderer::extract_frame(scene, {1, 0, 1}, lens, camera.world(), {}, {}, {}, true, 0));
	ENGINE_CHECK(renderer::extract_frame(scene, {1, 0, 1}, lens, camera.world(), {}, {}, {}, true, 0.5));
}
ENGINE_TEST(view_flags_only_take_away) {
	EditorSession s;
	s.grid_spacing = 0.5;
	auto f = frame_of(s);
	ENGINE_CHECK(f->show_grid);
	ENGINE_CHECK_EQ(f->grid_spacing, 0.5);
	ENGINE_CHECK(f->environment.show_skybox && f->environment.ambient_occlusion && f->environment.bloom && f->environment.shadow_quality != ShadowQuality::Off);
	s.view_flags = {false, false, false, false, false};
	f = frame_of(s);
	ENGINE_CHECK(!f->show_grid);
	ENGINE_CHECK(!f->environment.show_skybox && !f->environment.ambient_occlusion && !f->environment.bloom);
	ENGINE_CHECK(f->environment.shadow_quality == ShadowQuality::Off);
	// A scene that turned something off stays off with the flag on.
	s.view_flags = {};
	EnvironmentComponent plain;
	plain.bloom = false;
	ENGINE_REQUIRE(s.set_environment(plain));
	ENGINE_CHECK(!frame_of(s)->environment.bloom);
}
ENGINE_TEST(an_orthographic_view_leaves_out_the_sky) {
	EditorSession s;
	ENGINE_CHECK(frame_of(s)->environment.show_skybox);
	s.camera.orthographic = true;
	ENGINE_CHECK(!frame_of(s)->environment.show_skybox);
}
ENGINE_TEST(the_field_of_view_reaches_the_frame) {
	EditorSession s;
	s.camera.fov_degrees = 30;
	const auto f = frame_of(s);
	ENGINE_CHECK_NEAR(f->camera.lens.vertical_fov_radians, 30 * 3.141592653589793 / 180, 1e-9);
}

// ---- Settings and commands ----
ENGINE_TEST(settings_round_trip_and_tolerate_nonsense) {
	EditorSettings a;
	a.set("camera.speed", 12.5);
	a.set("snap.grid", true);
	a.set("snap.rotation", false);
	EditorSettings b;
	b.parse(a.text() + "\n# a comment\nno equals sign\nbad.number = abc\n");
	ENGINE_CHECK_EQ(b.number("camera.speed", 0), 12.5);
	ENGINE_CHECK(b.flag("snap.grid", false));
	ENGINE_CHECK(!b.flag("snap.rotation", true));
	ENGINE_CHECK_EQ(b.number("bad.number", 7), 7.0);
	ENGINE_CHECK_EQ(b.number("missing", 3), 3.0);
	ENGINE_CHECK(b.flag("missing", true));
}
ENGINE_TEST(the_same_key_means_different_things_flying_and_not) {
	CommandRegistry r;
	for (auto command : default_command_table()) ENGINE_REQUIRE(r.add(std::move(command)));
	for (const auto* id : {"Gizmo.Move", "Camera.FlyForward"}) ENGINE_REQUIRE(r.bind(id, [] {}));
	const std::string_view viewport[] = {context::viewport};
	const auto w = parse_chord("W");
	ENGINE_REQUIRE(w.has_value());
	const auto* command = r.resolve(*w, viewport);
	ENGINE_REQUIRE(command != nullptr);
	ENGINE_CHECK_EQ(command->info.id, std::string("Gizmo.Move"));
	ENGINE_CHECK(r.conflicts("Camera.FlyForward", *w).empty()); // Flying has the keys to itself.
}
ENGINE_TEST(the_requested_viewport_keys_are_registered) {
	CommandRegistry r;
	for (auto command : default_command_table()) ENGINE_REQUIRE(r.add(std::move(command)));
	const std::pair<const char*, const char*> expected[] = {
		{"Gizmo.Move", "W"}, {"Gizmo.Rotate", "E"}, {"Gizmo.Scale", "R"}, {"Gizmo.Cycle", "Space"}, {"Gizmo.ToggleSpace", "Ctrl+`"},
		{"Viewport.FocusSelected", "F"}, {"Viewport.Perspective", "Alt+G"}, {"Viewport.Front", "Alt+H"}, {"Viewport.Top", "Alt+J"}, {"Viewport.Side", "Alt+K"},
		{"Viewport.GameView", "G"}, {"Viewport.Maximize", "F11"}, {"Edit.Hide", "H"}, {"Edit.UnhideAll", "Ctrl+H"}, {"Edit.SnapToFloor", "End"},
		{"Snap.GridSmaller", "["}, {"Snap.GridLarger", "]"}, {"Viewport.Wireframe", "Alt+2"}, {"Viewport.Unlit", "Alt+3"}, {"Viewport.Lit", "Alt+4"},
		{"Camera.FlyForward", "W"}, {"Camera.FlyDown", "Q"}, {"Camera.FlyUp", "E"}, {"Camera.FovOut", "Z"}, {"Camera.FovIn", "C"},
		{"Bookmark.Set0", "Ctrl+0"}, {"Bookmark.Go0", "0"}, {"Bookmark.Set9", "Ctrl+9"}, {"Bookmark.Go9", "9"},
	};
	for (const auto& [id, keys] : expected) ENGINE_CHECK_EQ(r.shortcut(id), std::string(keys));
	ENGINE_CHECK(!mouse_gestures().empty());
}

// ---- Skinned models (Milestone 5): a pose is drawn and snapped to, and never edits the scene ----
namespace {
// Rows of vertices at y = 0, 1 and 2, x from 0 to 1. Joint "root" at the origin, "tip" a metre up, bound as they rest:
// the bottom row follows root, the top row tip, the middle both equally.
assets::AssetId skinned_bar(EditorSession& s, const std::filesystem::path& folder) {
	engine::test::Glb g;
	const int positions = g.floats({0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 2, 0, 1, 2, 0}, 3, true);
	const int indices = g.u16({0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4}, 1);
	const int joints = g.u16({0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, 4);
	const int weights = g.floats({1, 0, 0, 0, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0, 0.5f, 0.5f, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, 4);
	const int binds = g.floats({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1}, 16);
	const auto bytes = g.build(R"("scene":0,"scenes":[{"nodes":[0,2]}],"nodes":[{"name":"root","children":[1]},{"name":"tip","translation":[0,1,0]},)"
		R"({"name":"bar","mesh":0,"skin":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":)" + std::to_string(positions) +
		R"(,"JOINTS_0":)" + std::to_string(joints) + R"(,"WEIGHTS_0":)" + std::to_string(weights) + R"(},"indices":)" + std::to_string(indices) +
		R"(}]}],"skins":[{"joints":[0,1],"inverseBindMatrices":)" + std::to_string(binds) + "}]");
	const auto file = folder / "source" / "bar.glb";
	std::filesystem::create_directories(file.parent_path());
	std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	const auto imported = s.import_asset(file);
	ENGINE_REQUIRE(imported);
	return imported.value().id;
}
renderer::Pose tip_at(Vec3 tip) {
	return std::make_shared<std::vector<renderer::JointMatrix>>(std::vector<renderer::JointMatrix>{
		{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1},
		{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, float(tip.x), float(tip.y), float(tip.z), 1}});
}
// Where a world point lands on an 800 x 800 view, by the frame's own matrices.
std::pair<double, double> screen_of(const renderer::FrameSnapshot& f, Vec3 p) {
	const auto& m = f.camera.projection.matrix;
	const auto& v = f.camera.view.matrix;
	const double vx = v[0] * p.x + v[4] * p.y + v[8] * p.z + v[12], vy = v[1] * p.x + v[5] * p.y + v[9] * p.z + v[13], vz = v[2] * p.x + v[6] * p.y + v[10] * p.z + v[14];
	const double w = m[3] * vx + m[7] * vy + m[11] * vz + m[15];
	return {((m[0] * vx + m[4] * vy + m[8] * vz + m[12]) / w + 1) / 2 * 800, (1 - (m[1] * vx + m[5] * vy + m[9] * vz + m[13]) / w) / 2 * 800};
}
} // namespace

ENGINE_TEST(a_pose_is_drawn_but_never_edits_the_scene) {
	const auto folder = fresh_folder("pose");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(folder));
	const auto placed = s.place_asset_at(skinned_bar(s, folder), {0, 0, 0});
	ENGINE_REQUIRE(placed);
	const auto bar = placed.value().created.front();
	s.mark_clean();
	const auto revision = s.scene_revision();
	const auto pose = tip_at({1, 1, 0});
	for (int tick = 0; tick < 60; ++tick) s.set_pose(bar, tip_at({tick * 0.01, 1, 0})); // A second of animation ...
	s.set_pose(bar, pose);
	const auto f = frame_of(s);
	ENGINE_REQUIRE(f->objects.size() == 1);
	ENGINE_CHECK(f->objects[0].pose == pose); // ... reaches the frame as a shared pointer, not a copy,
	ENGINE_CHECK_EQ(s.scene_revision(), revision); // and adds nothing to the scene, its history or its file.
	ENGINE_CHECK(!s.dirty());
	ENGINE_CHECK(s.pose(bar) == pose);
	s.set_pose(bar, nullptr);
	ENGINE_CHECK(frame_of(s)->objects[0].pose == nullptr);
	// A non-finite pose is refused by the frame rather than drawn.
	auto broken = std::make_shared<std::vector<renderer::JointMatrix>>(2, renderer::JointMatrix{});
	(*broken)[1][12] = std::numeric_limits<float>::quiet_NaN();
	s.set_pose(bar, broken);
	ENGINE_CHECK(!s.frame(1.5));
	// A new scene starts with no poses.
	s.set_pose(bar, pose);
	ENGINE_REQUIRE(s.new_scene());
	ENGINE_CHECK(s.pose(bar) == nullptr);
}
ENGINE_TEST(a_skinned_models_vertices_snap_where_its_pose_puts_them) {
	const auto folder = fresh_folder("pose-snap");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(folder));
	const auto placed = s.place_asset_at(skinned_bar(s, folder), {0, 0, 0});
	ENGINE_REQUIRE(placed);
	const auto bar = placed.value().created.front();
	s.select(0);
	s.camera.target = {1, 1, 0}; s.camera.yaw = 0; s.camera.pitch = 0; s.camera.distance = 6;
	// The tip a metre to the right carries the top right corner from (1, 2, 0) to (2, 2, 0).
	s.set_pose(bar, tip_at({1, 1, 0}));
	auto f = frame_of(s, 1.0);
	auto [x, y] = screen_of(*f, {2, 2, 0});
	const auto found = s.nearest_vertex(*f, x + 3, y - 3, 800, 800, 20);
	ENGINE_REQUIRE(found.has_value());
	ENGINE_CHECK_NEAR(length(*found, {2, 2, 0}), 0, 1e-5);
	// A click where only the posed bar is picks it.
	const auto [px, py] = screen_of(*f, {1.6, 1.8, 0});
	ENGINE_CHECK_EQ(s.pick(*f, px / 400 - 1, 1 - py / 400), bar);
	// At rest nothing is there.
	s.set_pose(bar, nullptr);
	f = frame_of(s, 1.0);
	ENGINE_CHECK(!s.nearest_vertex(*f, x + 3, y - 3, 800, 800, 20).has_value());
	ENGINE_CHECK_EQ(s.pick(*f, px / 400 - 1, 1 - py / 400), EntityId{0});
	const auto rest = s.nearest_vertex(*f, screen_of(*f, {1, 2, 0}).first, screen_of(*f, {1, 2, 0}).second, 800, 800, 20);
	ENGINE_REQUIRE(rest.has_value());
	ENGINE_CHECK_NEAR(length(*rest, {1, 2, 0}), 0, 1e-5);
}
