// What the editor says and how it answers, without a window: the message log, toasts, tasks, cancelled imports, the cheat
// sheet and typing during a gizmo drag. Real CommandRegistry and EditorSession; no mocks.
#include "engine/test/test.h"

#include <engine/editor/EditorCommands.hpp>
#include <engine/editor/EditorSession.hpp>
#include <engine/editor/Feedback.hpp>
#include <engine/editor/GizmoEntry.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::editor;

namespace {
LogEntry entry(Severity severity, std::string text = "text") { return {severity, std::move(text)}; }
EntityId make(EditorSession& s, const std::string& name) {
	const auto created = s.execute(commands::CreateEntity{name, Transform{}, 0, 0, Components{MeshComponent{}, {}, {}}});
	ENGINE_REQUIRE(created);
	return created.value().created.front();
}
WorldTransform to_world(const glm::dmat4& m) {
	WorldTransform out;
	const auto* values = glm::value_ptr(m);
	for (int i = 0; i < 16; ++i) out.matrix[std::size_t(i)] = values[i];
	return out;
}
glm::dvec3 place_after(const WorldTransform& change, const WorldTransform& start) {
	return glm::dvec3((glm::make_mat4(change.matrix.data()) * glm::make_mat4(start.matrix.data()))[3]);
}
bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
GizmoEntry typed(std::u32string_view keys) {
	GizmoEntry e;
	for (const auto key : keys) e.feed(key);
	return e;
}
CommandRegistry bound_table() {
	CommandRegistry r;
	for (auto command : default_command_table()) ENGINE_REQUIRE(r.add(std::move(command)));
	for (const auto& command : r.commands()) ENGINE_REQUIRE(r.bind(command.info.id, [] {}));
	return r;
}
const CheatSheetRow* find_row(const std::vector<CheatSheetGroup>& groups, std::string_view context, std::string_view label) {
	for (const auto& group : groups)
		if (group.context == context)
			for (const auto& row : group.rows)
				if (row.label == label) return &row;
	return nullptr;
}
struct TempFolder {
	std::filesystem::path path;
	explicit TempFolder(const char* name) : path(std::filesystem::temp_directory_path() / name) {
		std::filesystem::remove_all(path);
		std::filesystem::create_directories(path / "project");
	}
	~TempFolder() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
void write_image(const std::filesystem::path& file) {
	std::ofstream image(file, std::ios::binary);
	image << "P6\n2 2\n255\n";
	for (const auto* pixel : {"\xff\x01\x01", "\xff\x01\x01", "\x01\x01\xff", "\x01\x01\xff"}) image.write(pixel, 3);
}
}

// ---- The message log ----
ENGINE_TEST(the_log_counts_by_severity_and_filters) {
	MessageLog log;
	log.add(entry(Severity::Info));
	log.add(entry(Severity::Error));
	log.add(entry(Severity::Error));
	log.add(entry(Severity::Warning));
	ENGINE_CHECK_EQ(log.count(Severity::Error), std::size_t{2});
	ENGINE_CHECK_EQ(log.count(Severity::Success), std::size_t{0});
	LogFilter filter;
	ENGINE_CHECK_EQ(log.shown(filter), std::size_t{4});
	filter.error = false;
	ENGINE_CHECK_EQ(log.shown(filter), std::size_t{2});
	filter.info = filter.warning = false;
	ENGINE_CHECK_EQ(log.shown(filter), std::size_t{0});
}
ENGINE_TEST(the_console_lines_stay_out_of_the_log_view) {
	MessageLog log;
	log.add({Severity::Error, "typed wrong", {}, true});
	log.add(entry(Severity::Error));
	ENGINE_CHECK_EQ(log.entries().size(), std::size_t{2});
	ENGINE_CHECK_EQ(log.count(Severity::Error), std::size_t{1});
	ENGINE_CHECK_EQ(log.shown(LogFilter{}), std::size_t{1});
}
ENGINE_TEST(the_log_drops_its_oldest_at_the_limit) {
	MessageLog log;
	log.add(entry(Severity::Info, "first"));
	for (std::size_t i = 1; i < MessageLog::limit; ++i) log.add(entry(Severity::Info));
	ENGINE_CHECK_EQ(log.entries().size(), MessageLog::limit);
	ENGINE_CHECK_EQ(log.entries().front().text, std::string("first"));
	log.add(entry(Severity::Info, "last"));
	ENGINE_CHECK_EQ(log.entries().size(), MessageLog::limit);
	ENGINE_CHECK(log.entries().front().text != "first");
	ENGINE_CHECK_EQ(log.entries().back().text, std::string("last"));
	log.clear();
	ENGINE_CHECK(log.entries().empty());
}
ENGINE_TEST(a_link_knows_what_it_points_at) {
	ENGINE_CHECK(!LogLink{});
	const auto to_entity = LogLink::entity(7, "Crate");
	ENGINE_CHECK(bool(to_entity));
	ENGINE_CHECK(to_entity.kind == LogLink::Kind::Entity);
	ENGINE_CHECK_EQ(to_entity.id, std::uint64_t{7});
	ENGINE_CHECK(LogLink::asset(9, "wood.jpg").kind == LogLink::Kind::Asset);
}

// ---- Toasts ----
ENGINE_TEST(toasts_go_when_their_time_is_up_and_errors_stay_longest) {
	ToastQueue queue;
	queue.show(Severity::Success, "saved");
	queue.show(Severity::Error, "broke");
	ENGINE_CHECK_EQ(queue.toasts().size(), std::size_t{2});
	ENGINE_CHECK(ToastQueue::duration(Severity::Error) > ToastQueue::duration(Severity::Warning));
	ENGINE_CHECK(ToastQueue::duration(Severity::Warning) > ToastQueue::duration(Severity::Success));
	queue.update(ToastQueue::duration(Severity::Success) + 0.1);
	ENGINE_CHECK_EQ(queue.toasts().size(), std::size_t{1});
	ENGINE_CHECK_EQ(queue.count(Severity::Error), std::size_t{1});
	queue.update(ToastQueue::duration(Severity::Error));
	ENGINE_CHECK(queue.toasts().empty());
}
ENGINE_TEST(the_same_toast_counts_up_and_starts_its_time_again) {
	ToastQueue queue;
	const auto first = queue.show(Severity::Error, "same");
	queue.update(5);
	const auto again = queue.show(Severity::Error, "same");
	ENGINE_CHECK_EQ(first, again);
	ENGINE_CHECK_EQ(queue.toasts().size(), std::size_t{1});
	ENGINE_CHECK_EQ(queue.toasts().front().count, 2);
	ENGINE_CHECK(queue.toasts().front().remaining == ToastQueue::duration(Severity::Error));
	// The same words as another kind are another message.
	queue.show(Severity::Info, "same");
	ENGINE_CHECK_EQ(queue.toasts().size(), std::size_t{2});
}
ENGINE_TEST(a_toast_waits_while_the_mouse_is_on_it) {
	ToastQueue queue;
	const auto id = queue.show(Severity::Info, "read me");
	queue.update(1000, id);
	ENGINE_CHECK_EQ(queue.toasts().size(), std::size_t{1});
	queue.update(1000);
	ENGINE_CHECK(queue.toasts().empty());
}
ENGINE_TEST(a_toast_can_be_dismissed) {
	ToastQueue queue;
	const auto id = queue.show(Severity::Error, "gone");
	queue.show(Severity::Error, "stays");
	queue.dismiss(id);
	ENGINE_CHECK_EQ(queue.toasts().size(), std::size_t{1});
	ENGINE_CHECK_EQ(queue.toasts().front().text, std::string("stays"));
}
ENGINE_TEST(too_many_toasts_lose_the_oldest_that_is_not_an_error) {
	ToastQueue queue;
	queue.show(Severity::Error, "error");
	queue.show(Severity::Info, "oldest info");
	for (std::size_t i = 0; i < ToastQueue::visible_limit - 1; ++i) queue.show(Severity::Info, "info " + std::to_string(i));
	ENGINE_CHECK_EQ(queue.toasts().size(), ToastQueue::visible_limit);
	ENGINE_CHECK_EQ(queue.toasts().front().text, std::string("error"));
	ENGINE_CHECK(std::ranges::none_of(queue.toasts(), [](const Toast& t) { return t.text == "oldest info"; }));
	// When all are errors the oldest goes after all.
	ToastQueue errors;
	for (std::size_t i = 0; i <= ToastQueue::visible_limit; ++i) errors.show(Severity::Error, "error " + std::to_string(i));
	ENGINE_CHECK_EQ(errors.toasts().size(), ToastQueue::visible_limit);
	ENGINE_CHECK_EQ(errors.toasts().front().text, std::string("error 1"));
}

// ---- Tasks ----
ENGINE_TEST(a_task_shows_only_once_it_has_taken_a_while) {
	TaskList tasks;
	const auto id = tasks.begin("Importing", true);
	ENGINE_CHECK_EQ(tasks.visible(), std::size_t{0});
	tasks.update(TaskList::show_after / 2);
	ENGINE_CHECK_EQ(tasks.visible(), std::size_t{0});
	tasks.update(TaskList::show_after);
	ENGINE_CHECK_EQ(tasks.visible(), std::size_t{1});
	ENGINE_CHECK(tasks.tasks().front().can_cancel);
	tasks.finish(id);
	ENGINE_CHECK(tasks.tasks().empty());
}
ENGINE_TEST(a_task_starts_not_knowing_how_far_along_it_is) {
	TaskList tasks;
	const auto id = tasks.begin("Thumbnails");
	ENGINE_CHECK(tasks.tasks().front().progress < 0);
	ENGINE_CHECK(!tasks.tasks().front().can_cancel);
	tasks.set_progress(id, 0.5);
	ENGINE_CHECK_EQ(tasks.tasks().front().progress, 0.5);
	tasks.set_progress(id, 7);
	ENGINE_CHECK_EQ(tasks.tasks().front().progress, 1.0);
	tasks.set_name(id, "Thumbnails: 3 done");
	ENGINE_CHECK_EQ(tasks.tasks().front().name, std::string("Thumbnails: 3 done"));
	tasks.finish(999); // Not there: nothing happens.
	ENGINE_CHECK_EQ(tasks.tasks().size(), std::size_t{1});
}

// ---- Imports ----
ENGINE_TEST(a_cancelled_import_adds_nothing) {
	TempFolder folder("engine-feedback-cancel");
	write_image(folder.path / "two_tone.ppm");
	EditorSession session;
	ENGINE_REQUIRE(session.open_project(folder.path / "project"));
	const auto before = session.assets().size();
	const auto id = session.begin_import(folder.path / "two_tone.ppm");
	ENGINE_CHECK(session.cancel_import(id));
	const auto finished = session.wait_for_imports();
	ENGINE_REQUIRE(finished.size() == 1);
	ENGINE_CHECK_EQ(finished.front().id, id);
	ENGINE_CHECK(finished.front().cancelled);
	ENGINE_CHECK(!finished.front().result);
	ENGINE_CHECK_EQ(session.assets().size(), before);
	ENGINE_CHECK_EQ(session.imports_pending(), std::size_t{0});
	ENGINE_CHECK(!session.cancel_import(id)); // It is over.
}
ENGINE_TEST(cancelling_one_import_leaves_the_other) {
	TempFolder folder("engine-feedback-cancel-one");
	write_image(folder.path / "keep.ppm");
	write_image(folder.path / "drop.ppm");
	EditorSession session;
	ENGINE_REQUIRE(session.open_project(folder.path / "project"));
	const auto before = session.assets().size();
	const auto kept = session.begin_import(folder.path / "keep.ppm");
	const auto dropped = session.begin_import(folder.path / "drop.ppm");
	ENGINE_CHECK(kept != dropped);
	ENGINE_CHECK(session.cancel_import(dropped));
	const auto finished = session.wait_for_imports();
	ENGINE_REQUIRE(finished.size() == 2);
	for (const auto& done : finished) {
		ENGINE_CHECK_EQ(done.cancelled, done.id == dropped);
		ENGINE_CHECK_EQ(bool(done.result), done.id == kept);
	}
	ENGINE_CHECK_EQ(session.assets().size(), before + 1);
}
ENGINE_TEST(an_import_that_is_not_cancelled_is_delivered_with_its_id) {
	TempFolder folder("engine-feedback-import");
	write_image(folder.path / "two_tone.ppm");
	EditorSession session;
	ENGINE_REQUIRE(session.open_project(folder.path / "project"));
	const auto id = session.begin_import(folder.path / "two_tone.ppm");
	ENGINE_CHECK(!session.cancel_import(id + 1));
	const auto finished = session.wait_for_imports();
	ENGINE_REQUIRE(finished.size() == 1);
	ENGINE_CHECK_EQ(finished.front().id, id);
	ENGINE_CHECK(!finished.front().cancelled);
	ENGINE_CHECK(bool(finished.front().result));
}

// ---- The cheat sheet ----
ENGINE_TEST(the_cheat_sheet_lists_the_context_then_global) {
	const auto r = bound_table();
	const std::array<std::string_view, 1> stack{context::viewport};
	const auto groups = r.cheat_sheet(stack);
	ENGINE_REQUIRE(groups.size() == 2);
	ENGINE_CHECK_EQ(groups[0].context, std::string(context::viewport));
	ENGINE_CHECK_EQ(groups[1].context, std::string(context::global));
	ENGINE_CHECK(find_row(groups, context::viewport, "Focus Selected") != nullptr);
	ENGINE_CHECK(find_row(groups, context::global, "Save") != nullptr);
	// Not another panel's.
	ENGINE_CHECK(std::ranges::none_of(groups, [](const CheatSheetGroup& g) { return g.context == context::outliner; }));
}
ENGINE_TEST(the_cheat_sheet_shows_a_key_once_where_it_counts) {
	auto r = bound_table();
	const std::array<std::string_view, 1> stack{context::playing};
	const auto groups = r.cheat_sheet(stack);
	// Esc means pause or stop while running, so the global Deselect, if it has Esc, must not list it.
	const auto* escape = find_row(groups, context::playing, "Pause or Stop");
	ENGINE_REQUIRE(escape != nullptr);
	ENGINE_CHECK_EQ(escape->keys, std::string("Esc"));
	const auto* deselect = find_row(groups, context::global, "Deselect");
	ENGINE_CHECK(deselect == nullptr || deselect->keys.find("Esc") == std::string::npos);
}
ENGINE_TEST(the_cheat_sheet_greys_what_cannot_run_and_it_keeps_no_key_from_others) {
	auto r = bound_table();
	ENGINE_REQUIRE(r.bind("Play.Escape", [] {}, [] { return false; }));
	const std::array<std::string_view, 1> stack{context::playing};
	const auto groups = r.cheat_sheet(stack);
	const auto* escape = find_row(groups, context::playing, "Pause or Stop");
	ENGINE_REQUIRE(escape != nullptr);
	ENGINE_CHECK(!escape->enabled);
	// As with resolve(): a command that cannot run passes the key on.
	const auto* deselect = find_row(groups, context::global, "Deselect");
	ENGINE_REQUIRE(deselect != nullptr);
	ENGINE_CHECK(deselect->keys.find("Esc") != std::string::npos);
}
ENGINE_TEST(the_cheat_sheet_follows_a_rebinding_and_leaves_out_what_has_no_key) {
	auto r = bound_table();
	ENGINE_REQUIRE(r.set_chords("File.Save", {*parse_chord("Ctrl+K")}));
	std::span<const std::string_view> nothing;
	auto groups = r.cheat_sheet(nothing);
	const auto* save = find_row(groups, context::global, "Save");
	ENGINE_REQUIRE(save != nullptr);
	ENGINE_CHECK_EQ(save->keys, std::string("Ctrl+K"));
	ENGINE_REQUIRE(r.set_chords("File.Save", {}));
	groups = r.cheat_sheet(nothing);
	ENGINE_CHECK(find_row(groups, context::global, "Save") == nullptr);
}
ENGINE_TEST(the_cheat_sheet_key_is_held_not_pressed) {
	auto r = bound_table();
	const auto* command = r.find("Help.CheatSheet");
	ENGINE_REQUIRE(command != nullptr);
	ENGINE_CHECK(read_directly(command->info));
	ENGINE_CHECK_EQ(command->chords.size(), std::size_t{2});
	ENGINE_CHECK(command->chords[0] == *parse_chord("F1"));
	ENGINE_CHECK(command->chords[1] == *parse_chord("Shift+/"));
	// Asked for like any shortcut, it answers nothing: only the overlay reads it.
	std::span<const std::string_view> nothing;
	ENGINE_CHECK(r.resolve(*parse_chord("F1"), nothing) == nullptr);
}

// ---- Tooltips ----
ENGINE_TEST(a_tooltip_names_the_bound_shortcut_and_follows_a_rebinding) {
	auto r = bound_table();
	const auto tip = r.tooltip("Viewport.FocusSelected");
	ENGINE_CHECK(tip.starts_with("Focus Selected (F)\n"));
	ENGINE_REQUIRE(r.set_chords("Viewport.FocusSelected", {*parse_chord("Ctrl+K")}));
	ENGINE_CHECK(r.tooltip("Viewport.FocusSelected").starts_with("Focus Selected (Ctrl+K)\n"));
	ENGINE_REQUIRE(r.set_chords("Viewport.FocusSelected", {}));
	ENGINE_CHECK(r.tooltip("Viewport.FocusSelected").starts_with("Focus Selected\n"));
}
ENGINE_TEST(every_command_says_what_it_does) {
	for (const auto& info : default_command_table()) ENGINE_CHECK(!info.tooltip.empty());
}

// ---- Typing during a gizmo drag ----
ENGINE_TEST(typing_builds_a_number) {
	ENGINE_CHECK_EQ(typed(U"12.5").value().value_or(0), 12.5);
	ENGINE_CHECK_EQ(typed(U"-3").value().value_or(0), -3.0);
	ENGINE_CHECK_EQ(typed(U".5").value().value_or(0), 0.5);
	ENGINE_CHECK_EQ(typed(U"5.").value().value_or(0), 5.0);
	ENGINE_CHECK(!typed(U"-").value());
	ENGINE_CHECK(!typed(U".").value());
	ENGINE_CHECK(!typed(U"-.").value());
	ENGINE_CHECK(!GizmoEntry{}.value());
	ENGINE_CHECK(!GizmoEntry{}.active());
}
ENGINE_TEST(typing_ignores_what_makes_no_number) {
	GizmoEntry e = typed(U"1.2");
	ENGINE_CHECK(!e.feed(U'.'));
	ENGINE_CHECK(!e.feed(U'-'));
	ENGINE_CHECK(!e.feed(U'q'));
	ENGINE_CHECK(!e.feed(U'v')); // Hold V to snap to a vertex.
	ENGINE_CHECK_EQ(e.number(), std::string("1.2"));
	GizmoEntry longest;
	for (int i = 0; i < 100; ++i) (void)longest.feed(U'9');
	ENGINE_CHECK_EQ(longest.number().size(), std::size_t{24});
}
ENGINE_TEST(an_axis_letter_chooses_the_axis_either_case_at_any_time) {
	ENGINE_CHECK_EQ(typed(U"x5").axis(), 0);
	ENGINE_CHECK_EQ(typed(U"Y5").axis(), 1);
	ENGINE_CHECK_EQ(typed(U"z5").axis(), 2);
	ENGINE_CHECK_EQ(typed(U"5").axis(), -1);
	const auto late = typed(U"5y");
	ENGINE_CHECK_EQ(late.axis(), 1);
	ENGINE_CHECK_EQ(late.value().value_or(0), 5.0);
	ENGINE_CHECK_EQ(typed(U"x-2.5").text(), std::string("x-2.5"));
	ENGINE_CHECK(typed(U"x").active());
	ENGINE_CHECK(!typed(U"x").value());
}
ENGINE_TEST(backspace_takes_the_number_then_the_axis) {
	auto e = typed(U"x12");
	e.backspace();
	ENGINE_CHECK_EQ(e.text(), std::string("x1"));
	e.backspace();
	ENGINE_CHECK_EQ(e.text(), std::string("x"));
	ENGINE_CHECK(e.active());
	e.backspace();
	ENGINE_CHECK(!e.active());
	e.backspace(); // Nothing left: nothing happens.
	ENGINE_CHECK(!e.active());
	e = typed(U"y2");
	e.clear();
	ENGINE_CHECK(!e.active());
	ENGINE_CHECK_EQ(e.axis(), -1);
}
ENGINE_TEST(a_handle_says_which_axis_unless_it_stands_for_more) {
	ENGINE_CHECK_EQ(gizmo_handle_axis(GizmoOperation::Move, 0), 0);
	ENGINE_CHECK_EQ(gizmo_handle_axis(GizmoOperation::Move, 2), 2);
	ENGINE_CHECK_EQ(gizmo_handle_axis(GizmoOperation::Move, 3), -1); // A plane.
	ENGINE_CHECK_EQ(gizmo_handle_axis(GizmoOperation::Move, 6), -1); // The screen.
	ENGINE_CHECK_EQ(gizmo_handle_axis(GizmoOperation::Rotate, 1), 1);
	ENGINE_CHECK_EQ(gizmo_handle_axis(GizmoOperation::Rotate, 3), -1);
	ENGINE_CHECK_EQ(gizmo_handle_axis(GizmoOperation::Scale, 2), 2);
	ENGINE_CHECK_EQ(gizmo_handle_axis(GizmoOperation::Scale, 3), gizmo_all_axes);
	ENGINE_CHECK_EQ(gizmo_handle_axis(GizmoOperation::Move, -1), -1);
}
ENGINE_TEST(a_typed_move_goes_exactly_that_far) {
	const auto start = to_world(glm::translate(glm::dmat4(1.0), glm::dvec3(1.25, 0.5, -3)));
	const auto change = gizmo_typed_change(GizmoOperation::Move, start, false, 0, 5);
	ENGINE_REQUIRE(change.has_value());
	const auto place = place_after(*change, start);
	ENGINE_CHECK_EQ(place.x, 6.25);
	ENGINE_CHECK_EQ(place.y, 0.5);
	ENGINE_CHECK_EQ(place.z, -3.0);
	const auto back = place_after(*gizmo_typed_change(GizmoOperation::Move, start, false, 1, -2.5), start);
	ENGINE_CHECK_EQ(back.y, -2.0);
}
ENGINE_TEST(a_local_move_goes_along_the_objects_own_axis) {
	// Turned a quarter around Y, its own X is the world's -Z.
	const auto turned = glm::rotate(glm::translate(glm::dmat4(1.0), glm::dvec3(1, 2, 3)), glm::radians(90.0), glm::dvec3(0, 1, 0));
	const auto start = to_world(turned);
	const auto local = place_after(*gizmo_typed_change(GizmoOperation::Move, start, true, 0, 2), start);
	ENGINE_CHECK(near(local.x, 1) && near(local.y, 2) && near(local.z, 1));
	const auto world = place_after(*gizmo_typed_change(GizmoOperation::Move, start, false, 0, 2), start);
	ENGINE_CHECK(near(world.x, 3) && near(world.y, 2) && near(world.z, 3));
}
ENGINE_TEST(a_typed_rotation_turns_around_where_the_object_is) {
	const auto start = to_world(glm::translate(glm::dmat4(1.0), glm::dvec3(4, 1, 0)));
	const auto change = gizmo_typed_change(GizmoOperation::Rotate, start, false, 1, 90);
	ENGINE_REQUIRE(change.has_value());
	const auto after = glm::make_mat4(change->matrix.data()) * glm::make_mat4(start.matrix.data());
	const auto place = glm::dvec3(after[3]);
	ENGINE_CHECK(near(place.x, 4) && near(place.y, 1) && near(place.z, 0)); // It stays put.
	const auto own_x = glm::dvec3(after[0]);
	ENGINE_CHECK(near(own_x.x, 0) && near(own_x.z, -1)); // And turns.
}
ENGINE_TEST(a_typed_scale_is_along_the_objects_axes_and_stays_positive) {
	const auto turned = glm::rotate(glm::translate(glm::dmat4(1.0), glm::dvec3(1, 2, 3)), glm::radians(90.0), glm::dvec3(0, 1, 0));
	const auto start = to_world(turned);
	const auto change = gizmo_typed_change(GizmoOperation::Scale, start, false, 0, 3);
	ENGINE_REQUIRE(change.has_value());
	const auto after = glm::make_mat4(change->matrix.data()) * turned;
	ENGINE_CHECK(near(glm::length(glm::dvec3(after[0])), 3));
	ENGINE_CHECK(near(glm::length(glm::dvec3(after[1])), 1));
	ENGINE_CHECK(near(after[3].x, 1) && near(after[3].y, 2) && near(after[3].z, 3));
	const auto all = glm::make_mat4(gizmo_typed_change(GizmoOperation::Scale, start, true, gizmo_all_axes, 2)->matrix.data()) * turned;
	for (int i = 0; i < 3; ++i) ENGINE_CHECK(near(glm::length(glm::dvec3(all[i])), 2));
	ENGINE_CHECK(!gizmo_typed_change(GizmoOperation::Scale, start, true, 0, 0));
	ENGINE_CHECK(!gizmo_typed_change(GizmoOperation::Scale, start, true, 0, -1));
}
ENGINE_TEST(a_typed_change_with_no_axis_or_a_bad_one_is_refused) {
	const auto start = to_world(glm::dmat4(1.0));
	ENGINE_CHECK(!gizmo_typed_change(GizmoOperation::Move, start, false, -1, 1));
	ENGINE_CHECK(!gizmo_typed_change(GizmoOperation::Move, start, false, gizmo_all_axes, 1));
	ENGINE_CHECK(!gizmo_typed_change(GizmoOperation::Rotate, start, false, gizmo_all_axes, 1));
	ENGINE_CHECK(!gizmo_typed_change(GizmoOperation::Move, start, false, 4, 1));
	ENGINE_CHECK(!gizmo_typed_change(GizmoOperation::Move, start, false, 0, std::nan("")));
}
ENGINE_TEST(a_typed_move_through_a_drag_is_exact_and_one_undo_step) {
	EditorSession s;
	const auto first = make(s, "First");
	const auto second = make(s, "Second");
	Transform apart; apart.position = {1.5, 0, 0};
	ENGINE_REQUIRE(s.execute(commands::SetTransform{second, apart}));
	s.select_all();
	const auto steps = s.history_position();
	const auto start = s.scene().world_transform(first);
	ENGINE_REQUIRE(start.has_value());
	ENGINE_REQUIRE(s.begin_drag("Move"));
	// Several values while typing, as each key arrives; only the last counts.
	for (const double value : {1.0, 12.0, 12.5}) ENGINE_REQUIRE(s.update_drag(*gizmo_typed_change(GizmoOperation::Move, *start, false, 0, value)));
	ENGINE_REQUIRE(s.end_drag());
	ENGINE_CHECK_EQ(s.history_position(), steps + 1);
	ENGINE_CHECK_EQ(s.scene().inspect(first)->transform.position.x, 12.5);
	ENGINE_CHECK_EQ(s.scene().inspect(second)->transform.position.x, 14.0); // Each keeps its own place.
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(s.scene().inspect(first)->transform.position.x, 0.0);
	ENGINE_CHECK_EQ(s.scene().inspect(second)->transform.position.x, 1.5);
}
ENGINE_TEST(cancelling_a_typed_move_restores_everything) {
	EditorSession s;
	const auto id = make(s, "Crate");
	s.select(id);
	const auto start = s.scene().world_transform(id);
	ENGINE_REQUIRE(start.has_value());
	ENGINE_REQUIRE(s.begin_drag("Move"));
	ENGINE_REQUIRE(s.update_drag(*gizmo_typed_change(GizmoOperation::Move, *start, false, 2, 9)));
	ENGINE_CHECK_EQ(s.scene().inspect(id)->transform.position.z, 9.0);
	ENGINE_REQUIRE(s.cancel_drag());
	ENGINE_CHECK_EQ(s.scene().inspect(id)->transform.position.z, 0.0);
	ENGINE_CHECK(!s.dragging());
}
