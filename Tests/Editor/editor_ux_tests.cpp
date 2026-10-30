// The editor's foundation, without a window: command registry, keymaps, routing, transactions, history, selection and
// property metadata. Real Scene, CommandBus and EditorSession; no mocks.
#include "engine/test/test.h"

#include <engine/editor/EditorCommands.hpp>
#include <engine/editor/EditorPrefs.hpp>
#include <engine/editor/EditorSession.hpp>
#include <engine/editor/PropertyMeta.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <span>
#include <sstream>
#include <string>

using namespace engine;
using namespace engine::editor;

namespace {
const std::string_view viewport_only[] = {context::viewport};
const std::string_view outliner_first[] = {context::outliner};
CommandInfo info(std::string id, std::string context_name, std::vector<KeyChord> chords = {}) {
	return {std::move(id), "Label", "", "Test", std::move(context_name), std::move(chords)};
}
KeyChord key(std::string_view text) {
	const auto chord = parse_chord(text);
	ENGINE_REQUIRE(chord);
	return *chord;
}
const commands::Context editor_context{"test", 3};
std::filesystem::path fresh_folder(const std::string& name) {
	const auto folder = std::filesystem::temp_directory_path() / ("engine-ux-" + name);
	std::filesystem::remove_all(folder);
	std::filesystem::create_directories(folder);
	return folder;
}
WorldTransform translation(double x, double y, double z) {
	WorldTransform t;
	t.matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
	return t;
}
double world_x(const EditorSession& s, EntityId id) { return s.scene().world_transform(id)->matrix[12]; }
EntityId make(EditorSession& s, const std::string& name, EntityId parent = 0) {
	const auto created = s.create(name, parent);
	ENGINE_REQUIRE(created);
	return created.value().created.front();
}
} // namespace

// ---- Chords ----
ENGINE_TEST(chord_text_round_trips_every_key) {
	for (int k = 1; k < int(Key::Count); ++k)
		for (const std::uint8_t modifiers : {modifier::none, modifier::ctrl, std::uint8_t(modifier::ctrl | modifier::shift), std::uint8_t(modifier::ctrl | modifier::shift | modifier::alt)}) {
			const KeyChord chord{Key(k), modifiers};
			const auto parsed = parse_chord(to_string(chord));
			ENGINE_REQUIRE(parsed);
			ENGINE_CHECK(*parsed == chord);
		}
}
ENGINE_TEST(chords_parse_in_any_order_and_case) {
	ENGINE_CHECK((key("shift+CTRL+s") == KeyChord{Key::S, std::uint8_t(modifier::ctrl | modifier::shift)}));
	ENGINE_CHECK((key(" Alt + f4 ") == KeyChord{Key::F4, modifier::alt}));
	ENGINE_CHECK((key("Ctrl+`") == KeyChord{Key::Grave, modifier::ctrl}));
	ENGINE_CHECK_EQ(to_string(key("shift+ctrl+alt+z")), std::string("Ctrl+Shift+Alt+Z"));
}
ENGINE_TEST(bad_chords_are_refused) {
	for (const auto* text : {"", "Ctrl", "Ctrl+", "Banana", "Ctrl+Banana", "Super+A", "A+B"}) ENGINE_CHECK(!parse_chord(text));
}

// ---- Registry ----
ENGINE_TEST(registry_refuses_duplicates_and_empty_ids) {
	CommandRegistry r;
	ENGINE_CHECK(r.add(info("A.One", "Global")));
	ENGINE_CHECK(!r.add(info("A.One", "Global")));
	ENGINE_CHECK(!r.add(info("", "Global")));
	ENGINE_CHECK(!r.bind("A.Missing", [] {}));
}
ENGINE_TEST(enabled_and_checked_follow_the_handlers) {
	CommandRegistry r;
	ENGINE_REQUIRE(r.add(info("A.One", "Global")));
	ENGINE_CHECK(!r.enabled("A.One")); // No behavior yet.
	ENGINE_CHECK_EQ(r.unbound().size(), std::size_t{1});
	bool can = false, on = false;
	int runs = 0;
	ENGINE_REQUIRE(r.bind("A.One", [&] { ++runs; }, [&] { return can; }, [&] { return on; }));
	ENGINE_CHECK(r.unbound().empty());
	ENGINE_CHECK(!r.enabled("A.One"));
	ENGINE_CHECK(!r.run("A.One"));
	can = true; on = true;
	ENGINE_CHECK(r.enabled("A.One"));
	ENGINE_CHECK(r.checked("A.One"));
	ENGINE_CHECK(r.run("A.One"));
	ENGINE_CHECK_EQ(runs, 1);
}
ENGINE_TEST(shortcut_and_tooltip_show_the_current_binding) {
	CommandRegistry r;
	auto command = info("A.One", "Global", {key("F")});
	command.label = "Focus Selected"; command.tooltip = "Frames it.";
	ENGINE_REQUIRE(r.add(command));
	ENGINE_CHECK_EQ(r.shortcut("A.One"), std::string("F"));
	ENGINE_CHECK_EQ(r.tooltip("A.One"), std::string("Focus Selected (F)\nFrames it."));
	ENGINE_REQUIRE(r.set_chords("A.One", {key("Ctrl+G")}));
	ENGINE_CHECK_EQ(r.tooltip("A.One"), std::string("Focus Selected (Ctrl+G)\nFrames it."));
	ENGINE_CHECK(r.overridden("A.One"));
	ENGINE_REQUIRE(r.reset_chords("A.One"));
	ENGINE_CHECK(!r.overridden("A.One"));
	ENGINE_CHECK_EQ(r.shortcut("A.One"), std::string("F"));
}

// ---- Routing ----
ENGINE_TEST(the_active_context_wins_over_global) {
	CommandRegistry r;
	ENGINE_REQUIRE(r.add(info("G.One", "Global", {key("F")})));
	ENGINE_REQUIRE(r.add(info("V.One", "Viewport", {key("F")})));
	std::string ran;
	ENGINE_REQUIRE(r.bind("G.One", [&] { ran = "global"; }));
	ENGINE_REQUIRE(r.bind("V.One", [&] { ran = "viewport"; }));
	ENGINE_CHECK(r.dispatch(key("F"), viewport_only));
	ENGINE_CHECK_EQ(ran, std::string("viewport"));
	ENGINE_CHECK(r.dispatch(key("F"), outliner_first));
	ENGINE_CHECK_EQ(ran, std::string("global"));
}
ENGINE_TEST(another_panels_binding_does_not_fire) {
	CommandRegistry r;
	ENGINE_REQUIRE(r.add(info("V.One", "Viewport", {key("F")})));
	ENGINE_REQUIRE(r.bind("V.One", [] {}));
	ENGINE_CHECK(!r.dispatch(key("F"), outliner_first));
	ENGINE_CHECK(r.resolve(key("F"), {}) == nullptr);
}
ENGINE_TEST(a_disabled_command_passes_to_the_next_context) {
	CommandRegistry r;
	ENGINE_REQUIRE(r.add(info("G.One", "Global", {key("X")})));
	ENGINE_REQUIRE(r.add(info("V.One", "Viewport", {key("X")})));
	std::string ran;
	ENGINE_REQUIRE(r.bind("G.One", [&] { ran = "global"; }));
	ENGINE_REQUIRE(r.bind("V.One", [&] { ran = "viewport"; }, [] { return false; }));
	ENGINE_CHECK(r.dispatch(key("X"), viewport_only));
	ENGINE_CHECK_EQ(ran, std::string("global"));
}
ENGINE_TEST(modifiers_must_match_exactly) {
	CommandRegistry r;
	ENGINE_REQUIRE(r.add(info("G.One", "Global", {key("Ctrl+Z")})));
	ENGINE_REQUIRE(r.bind("G.One", [] {}));
	ENGINE_CHECK(!r.dispatch(key("Z"), {}));
	ENGINE_CHECK(!r.dispatch(key("Ctrl+Shift+Z"), {}));
	ENGINE_CHECK(r.dispatch(key("Ctrl+Z"), {}));
}
ENGINE_TEST(nothing_fires_while_a_text_field_or_other_block_is_active) {
	CommandRegistry r;
	ENGINE_REQUIRE(r.add(info("G.One", "Global", {key("Delete")})));
	int runs = 0;
	ENGINE_REQUIRE(r.bind("G.One", [&] { ++runs; }));
	InputBlock block;
	block.text_input = true;
	ENGINE_CHECK(!r.dispatch(key("Delete"), {}, block));
	block = {}; block.modal = true;
	ENGINE_CHECK(!r.dispatch(key("Delete"), {}, block));
	block = {}; block.captured = true;
	ENGINE_CHECK(!r.dispatch(key("Delete"), {}, block));
	block = {}; block.interacting = true;
	ENGINE_CHECK(!r.dispatch(key("Delete"), {}, block));
	block = {}; block.rebinding = true;
	ENGINE_CHECK(!r.dispatch(key("Delete"), {}, block));
	ENGINE_CHECK_EQ(runs, 0);
	ENGINE_CHECK(r.dispatch(key("Delete"), {}, {}));
	ENGINE_CHECK_EQ(runs, 1);
}
ENGINE_TEST(conflicts_are_found_in_overlapping_contexts_only) {
	CommandRegistry r;
	ENGINE_REQUIRE(r.add(info("G.One", "Global", {key("F")})));
	ENGINE_REQUIRE(r.add(info("V.One", "Viewport", {key("F")})));
	ENGINE_REQUIRE(r.add(info("O.One", "Outliner", {key("F")})));
	ENGINE_REQUIRE(r.add(info("V.Two", "Viewport")));
	ENGINE_CHECK_EQ(r.conflicts("G.One", key("F")).size(), std::size_t{2}); // Global overlaps every context.
	const auto viewport = r.conflicts("V.Two", key("F"));
	ENGINE_REQUIRE(viewport.size() == 2);
	ENGINE_CHECK(viewport[0]->info.id == "G.One" && viewport[1]->info.id == "V.One"); // Not the Outliner's.
	ENGINE_CHECK(r.conflicts("V.Two", key("Q")).empty());
}

// ---- Keymap files ----
ENGINE_TEST(keymap_saves_only_overrides_and_loads_them_back) {
	CommandRegistry r;
	ENGINE_REQUIRE(r.add(info("A.One", "Global", {key("F")})));
	ENGINE_REQUIRE(r.add(info("A.Two", "Global", {key("G")})));
	ENGINE_REQUIRE(r.add(info("A.Three", "Global", {key("H")})));
	ENGINE_REQUIRE(r.set_chords("A.One", {key("Ctrl+1"), key("Ctrl+,")}));
	ENGINE_REQUIRE(r.set_chords("A.Three", {})); // Unbound.
	const auto text = r.save_keymap();
	ENGINE_CHECK(text.find("A.Two") == std::string::npos);
	CommandRegistry other;
	ENGINE_REQUIRE(other.add(info("A.One", "Global", {key("F")})));
	ENGINE_REQUIRE(other.add(info("A.Two", "Global", {key("G")})));
	ENGINE_REQUIRE(other.add(info("A.Three", "Global", {key("H")})));
	const auto loaded = other.load_keymap(text);
	ENGINE_REQUIRE(loaded);
	ENGINE_CHECK_EQ(loaded.value().applied, std::size_t{2});
	ENGINE_CHECK((other.find("A.One")->chords == std::vector<KeyChord>{key("Ctrl+1"), key("Ctrl+,")}));
	ENGINE_CHECK(other.find("A.Three")->chords.empty());
	ENGINE_CHECK((other.find("A.Two")->chords == std::vector<KeyChord>{key("G")}));
}
ENGINE_TEST(keymap_accepts_separator_characters_as_keys) {
	CommandRegistry r;
	ENGINE_REQUIRE(r.add(info("A.One", "Global")));
	const auto loaded = r.load_keymap("A.One = Ctrl+=, Ctrl+,, ,\n");
	ENGINE_REQUIRE(loaded);
	ENGINE_CHECK((r.find("A.One")->chords == std::vector<KeyChord>{key("Ctrl+="), key("Ctrl+,"), KeyChord{Key::Comma, modifier::none}}));
}
ENGINE_TEST(keymap_with_a_bad_line_changes_nothing) {
	CommandRegistry r;
	ENGINE_REQUIRE(r.add(info("A.One", "Global", {key("F")})));
	ENGINE_REQUIRE(r.set_chords("A.One", {key("G")}));
	const auto loaded = r.load_keymap("# comment\nA.One = H\nA.Two = Banana\n");
	ENGINE_REQUIRE(!loaded);
	ENGINE_CHECK(loaded.error().message.find("line 3") != std::string::npos);
	ENGINE_CHECK((r.find("A.One")->chords == std::vector<KeyChord>{key("G")}));
	ENGINE_CHECK(!r.load_keymap("no equals sign"));
}
ENGINE_TEST(keymap_reports_commands_this_build_does_not_have) {
	CommandRegistry r;
	ENGINE_REQUIRE(r.add(info("A.One", "Global", {key("F")})));
	const auto loaded = r.load_keymap("A.Gone = H\nA.One = G\n");
	ENGINE_REQUIRE(loaded);
	ENGINE_REQUIRE(loaded.value().unknown.size() == 1);
	ENGINE_CHECK_EQ(loaded.value().unknown[0], std::string("A.Gone"));
	ENGINE_CHECK_EQ(r.shortcut("A.One"), std::string("G"));
}
ENGINE_TEST(keymap_file_survives_a_save_and_read) {
	const auto folder = fresh_folder("keymap");
	const auto file = folder / "deep" / "keymap.ini";
	ENGINE_REQUIRE(write_text_file(file, "A.One = F\n"));
	const auto text = read_text_file(file);
	ENGINE_REQUIRE(text);
	ENGINE_CHECK_EQ(text.value(), std::string("A.One = F\n"));
	ENGINE_CHECK(!read_text_file(folder / "missing.ini"));
	ENGINE_CHECK(!std::filesystem::exists(folder / "deep" / "keymap.ini.tmp"));
}

// ---- The real table ----
ENGINE_TEST(default_table_has_unique_ids_and_no_conflicts_within_a_context) {
	CommandRegistry r;
	for (auto command : default_command_table()) {
		ENGINE_CHECK(!command.category.empty());
		ENGINE_CHECK(!command.tooltip.empty());
		ENGINE_CHECK(r.add(std::move(command)));
	}
	// Two commands in one context would be a real conflict. A panel's binding over a global one is a choice: it is what
	// the routing is for. Those are listed here so that a new one is a decision too.
	const std::string allowed[] = {"Edit.Rename F2", "Asset.Rename F2", "Edit.Deselect Esc", "Assets.CloseDrawer Esc"};
	for (const auto& command : r.commands())
		for (const auto chord : command.chords)
			for (const auto* other : r.conflicts(command.info.id, chord)) {
				const auto entry = command.info.id + " " + to_string(chord);
				ENGINE_CHECK(other->info.context != command.info.context);
				if (std::ranges::find(allowed, entry) == std::end(allowed)) ENGINE_CHECK_EQ(entry, std::string("no overlap"));
			}
}
ENGINE_TEST(default_table_keeps_the_existing_shortcuts) {
	CommandRegistry r;
	for (auto command : default_command_table()) ENGINE_REQUIRE(r.add(std::move(command)));
	ENGINE_CHECK_EQ(r.shortcut("Edit.Undo"), std::string("Ctrl+Z"));
	ENGINE_CHECK_EQ(r.find("Edit.Redo")->chords.size(), std::size_t{2});
	ENGINE_CHECK_EQ(r.shortcut("File.Save"), std::string("Ctrl+S"));
	ENGINE_CHECK_EQ(r.shortcut("File.NewScene"), std::string("Ctrl+N"));
	ENGINE_CHECK_EQ(r.shortcut("Edit.Duplicate"), std::string("Ctrl+D"));
	ENGINE_CHECK_EQ(r.shortcut("Edit.Delete"), std::string("Delete"));
	ENGINE_CHECK_EQ(r.shortcut("Viewport.FocusSelected"), std::string("F"));
}
ENGINE_TEST(shortcuts_document_is_current) {
	const auto on_disk = read_text_file(std::filesystem::path(GAMEENGINE_TEST_DOCS_DIR) / "EDITOR_SHORTCUTS.md");
	ENGINE_REQUIRE(on_disk);
	const auto table = default_command_table();
	ENGINE_CHECK(on_disk.value() == shortcuts_markdown(table));
}

// ---- Transactions on the bus ----
ENGINE_TEST(bus_transaction_is_one_step_and_each_execute_commits) {
	Scene scene;
	commands::CommandBus bus(scene);
	const auto created = bus.execute(editor_context, 0, commands::CreateEntity{"A"});
	ENGINE_REQUIRE(created);
	const auto id = created.value().created.front();
	ENGINE_REQUIRE(bus.begin_transaction("Drag"));
	for (int i = 1; i <= 5; ++i) {
		Transform t; t.position.x = i;
		ENGINE_REQUIRE(bus.execute(editor_context, bus.revision(), commands::SetTransform{id, t}));
		ENGINE_CHECK_EQ(scene.inspect(id)->transform.position.x, double(i)); // Live.
	}
	ENGINE_CHECK_EQ(bus.undo_size(), std::size_t{1});
	ENGINE_REQUIRE(bus.end_transaction());
	ENGINE_CHECK_EQ(bus.undo_size(), std::size_t{2});
	ENGINE_CHECK_EQ(bus.latest_name(), std::string_view("Drag"));
	ENGINE_REQUIRE(bus.undo(editor_context, bus.revision()));
	ENGINE_CHECK_EQ(scene.inspect(id)->transform.position.x, 0.0);
	ENGINE_REQUIRE(bus.redo(editor_context, bus.revision()));
	ENGINE_CHECK_EQ(scene.inspect(id)->transform.position.x, 5.0);
}
ENGINE_TEST(bus_transaction_that_changes_nothing_adds_no_step_and_keeps_redo) {
	Scene scene;
	commands::CommandBus bus(scene);
	const auto id = bus.execute(editor_context, 0, commands::CreateEntity{"A"}).value().created.front();
	Transform moved; moved.position.x = 1;
	ENGINE_REQUIRE(bus.execute(editor_context, bus.revision(), commands::SetTransform{id, moved}));
	ENGINE_REQUIRE(bus.undo(editor_context, bus.revision()));
	ENGINE_REQUIRE(bus.begin_transaction("Back and forth"));
	Transform there; there.position.x = 9;
	ENGINE_REQUIRE(bus.execute(editor_context, bus.revision(), commands::SetTransform{id, there}));
	ENGINE_REQUIRE(bus.execute(editor_context, bus.revision(), commands::SetTransform{id, Transform{}}));
	ENGINE_REQUIRE(bus.end_transaction());
	ENGINE_CHECK_EQ(bus.undo_size(), std::size_t{1});
	ENGINE_CHECK_EQ(bus.redo_size(), std::size_t{1});
}
ENGINE_TEST(bus_transaction_create_then_delete_nets_to_nothing) {
	Scene scene;
	commands::CommandBus bus(scene);
	ENGINE_REQUIRE(bus.begin_transaction("Nothing"));
	const auto created = bus.execute(editor_context, 0, commands::CreateEntity{"A"});
	ENGINE_REQUIRE(created);
	ENGINE_REQUIRE(bus.execute(editor_context, bus.revision(), commands::DeleteEntity{created.value().created.front()}));
	ENGINE_REQUIRE(bus.end_transaction());
	ENGINE_CHECK_EQ(bus.undo_size(), std::size_t{0});
}
ENGINE_TEST(bus_abort_restores_and_adds_no_step) {
	Scene scene;
	commands::CommandBus bus(scene);
	const auto id = bus.execute(editor_context, 0, commands::CreateEntity{"A"}).value().created.front();
	ENGINE_REQUIRE(bus.begin_transaction("Cancelled"));
	Transform t; t.position.x = 3;
	ENGINE_REQUIRE(bus.execute(editor_context, bus.revision(), commands::SetTransform{id, t}));
	const auto extra = bus.execute(editor_context, bus.revision(), commands::CreateEntity{"B"});
	ENGINE_REQUIRE(extra);
	ENGINE_REQUIRE(bus.execute(editor_context, bus.revision(), commands::DeleteEntity{id}));
	ENGINE_REQUIRE(bus.abort_transaction(editor_context));
	ENGINE_CHECK_EQ(bus.undo_size(), std::size_t{1});
	ENGINE_REQUIRE(scene.inspect(id));
	ENGINE_CHECK_EQ(scene.inspect(id)->transform.position.x, 0.0);
	ENGINE_CHECK(!scene.inspect(extra.value().created.front()));
	ENGINE_CHECK(!bus.in_transaction());
}
ENGINE_TEST(bus_refuses_undo_and_a_second_begin_while_open) {
	Scene scene;
	commands::CommandBus bus(scene);
	ENGINE_REQUIRE(bus.execute(editor_context, 0, commands::CreateEntity{"A"}));
	ENGINE_REQUIRE(bus.begin_transaction("Open"));
	ENGINE_CHECK(!bus.begin_transaction("Again"));
	ENGINE_CHECK(!bus.undo(editor_context, bus.revision()));
	ENGINE_CHECK(!bus.redo(editor_context, bus.revision()));
	ENGINE_REQUIRE(bus.end_transaction());
	ENGINE_CHECK(!bus.end_transaction());
	ENGINE_CHECK(bus.undo(editor_context, bus.revision()));
}
ENGINE_TEST(bus_transaction_failure_inside_keeps_earlier_changes_until_aborted) {
	Scene scene;
	commands::CommandBus bus(scene);
	const auto id = bus.execute(editor_context, 0, commands::CreateEntity{"A"}).value().created.front();
	ENGINE_REQUIRE(bus.begin_transaction("Partly"));
	ENGINE_REQUIRE(bus.execute(editor_context, bus.revision(), commands::RenameEntity{id, "B"}));
	ENGINE_CHECK(!bus.execute(editor_context, bus.revision(), commands::DeleteEntity{999}));
	ENGINE_CHECK_EQ(std::string(scene.inspect(id)->name), std::string("B"));
	ENGINE_REQUIRE(bus.abort_transaction(editor_context));
	ENGINE_CHECK_EQ(std::string(scene.inspect(id)->name), std::string("A"));
}
ENGINE_TEST(bus_without_history_tracks_but_cannot_abort) {
	Scene scene;
	commands::CommandBus bus(scene, 0);
	ENGINE_REQUIRE(bus.begin_transaction("Runtime"));
	ENGINE_REQUIRE(bus.execute(editor_context, 0, commands::CreateEntity{"A"}));
	ENGINE_CHECK(!bus.abort_transaction(editor_context));
	ENGINE_CHECK(bus.in_transaction()); // Refused, so still open.
	ENGINE_REQUIRE(bus.end_transaction());
	ENGINE_CHECK_EQ(bus.undo_size(), std::size_t{0});
}
ENGINE_TEST(commands_have_readable_names_in_history) {
	Scene scene;
	commands::CommandBus bus(scene);
	const auto id = bus.execute(editor_context, 0, commands::CreateEntity{"A"}).value().created.front();
	ENGINE_CHECK_EQ(bus.latest_name(), std::string_view("Create Entity"));
	ENGINE_REQUIRE(bus.execute(editor_context, bus.revision(), commands::RenameEntity{id, "B"}, "Rename to B"));
	ENGINE_CHECK_EQ(bus.latest_name(), std::string_view("Rename to B"));
	for (const auto& descriptor : commands::registry()) ENGINE_CHECK(!descriptor.label.empty());
}

// ---- Transactions and history in the session ----
ENGINE_TEST(session_scoped_transactions_nest_into_one_step) {
	EditorSession s;
	const auto id = make(s, "A");
	const auto before = s.history_position();
	{
		ScopedTransaction outer(s, "Outer name");
		ENGINE_REQUIRE(s.execute(commands::RenameEntity{id, "B"}));
		{
			ScopedTransaction inner(s, "Inner name");
			ENGINE_REQUIRE(s.execute(commands::RenameEntity{id, "C"}));
		}
		ENGINE_CHECK(s.in_transaction());
		ENGINE_REQUIRE(s.execute(commands::RenameEntity{id, "D"}));
	}
	ENGINE_CHECK(!s.in_transaction());
	ENGINE_CHECK_EQ(s.history_position(), before + 1);
	ENGINE_CHECK_EQ(std::string(s.history_names().back()), std::string("Outer name"));
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(std::string(s.scene().inspect(id)->name), std::string("A"));
}
ENGINE_TEST(session_cancelled_transaction_restores) {
	EditorSession s;
	const auto id = make(s, "A");
	const auto before = s.history_position();
	{
		ScopedTransaction t(s, "Cancelled");
		ENGINE_REQUIRE(s.execute(commands::RenameEntity{id, "B"}));
		t.cancel();
	}
	ENGINE_CHECK_EQ(std::string(s.scene().inspect(id)->name), std::string("A"));
	ENGINE_CHECK_EQ(s.history_position(), before);
	ENGINE_CHECK(!s.can_redo());
}
ENGINE_TEST(session_refuses_undo_during_a_transaction) {
	EditorSession s;
	make(s, "A");
	ScopedTransaction t(s, "Open");
	ENGINE_CHECK(!s.undo());
	ENGINE_CHECK(!s.redo());
	ENGINE_CHECK(!s.play());
}
ENGINE_TEST(history_names_are_readable_and_ordered) {
	EditorSession s;
	const auto id = make(s, "A");
	ENGINE_REQUIRE(s.execute(commands::RenameEntity{id, "B"}));
	ENGINE_REQUIRE(s.duplicate(id));
	const auto names = s.history_names();
	ENGINE_REQUIRE(names.size() == 3);
	ENGINE_CHECK_EQ(names[0], std::string("Create A"));
	ENGINE_CHECK_EQ(names[1], std::string("Rename Entity"));
	ENGINE_CHECK_EQ(names[2], std::string("Duplicate B"));
}
ENGINE_TEST(history_jump_goes_both_ways) {
	EditorSession s;
	const auto id = make(s, "Start");
	for (const auto* name : {"One", "Two", "Three"}) ENGINE_REQUIRE(s.execute(commands::RenameEntity{id, name}));
	ENGINE_CHECK_EQ(s.history_position(), std::size_t{4});
	ENGINE_REQUIRE(s.jump_history(2));
	ENGINE_CHECK_EQ(std::string(s.scene().inspect(id)->name), std::string("One"));
	ENGINE_CHECK_EQ(s.history_position(), std::size_t{2});
	ENGINE_CHECK_EQ(s.history_names().size(), std::size_t{4}); // The rest can still be redone.
	ENGINE_REQUIRE(s.jump_history(4));
	ENGINE_CHECK_EQ(std::string(s.scene().inspect(id)->name), std::string("Three"));
	ENGINE_REQUIRE(s.jump_history(0));
	ENGINE_CHECK_EQ(s.scene().size(), std::size_t{0});
	ENGINE_CHECK(!s.jump_history(99));
}
ENGINE_TEST(a_new_edit_after_undo_ends_the_redo_chain) {
	EditorSession s;
	const auto id = make(s, "A");
	ENGINE_REQUIRE(s.execute(commands::RenameEntity{id, "B"}));
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK(s.can_redo());
	ENGINE_REQUIRE(s.execute(commands::RenameEntity{id, "C"}));
	ENGINE_CHECK(!s.can_redo());
	ENGINE_CHECK_EQ(s.history_names().size(), std::size_t{2});
}
ENGINE_TEST(history_follows_the_bus_past_its_limit) {
	EditorSession s;
	const auto id = make(s, "A");
	for (int i = 0; i < 200; ++i) ENGINE_REQUIRE(s.execute(commands::RenameEntity{id, std::to_string(i)}));
	ENGINE_CHECK_EQ(s.commands().undo_size(), std::size_t{128});
	ENGINE_CHECK_EQ(s.history_position(), std::size_t{128});
	ENGINE_REQUIRE(s.jump_history(0));
	ENGINE_CHECK_EQ(std::string(s.scene().inspect(id)->name), std::string("71"));
	ENGINE_CHECK(!s.can_undo());
}
ENGINE_TEST(material_and_scene_edits_share_one_order) {
	const auto folder = fresh_folder("history-materials");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(folder));
	const auto material = s.create_material("Steel");
	ENGINE_REQUIRE(material);
	const auto id = make(s, "A");
	assets::MaterialData data;
	data.metallic = 1;
	ENGINE_REQUIRE(s.save_material(material.value(), data));
	ENGINE_REQUIRE(s.execute(commands::RenameEntity{id, "B"}));
	data.metallic = 0.5;
	ENGINE_REQUIRE(s.save_material(material.value(), data));
	const auto names = s.history_names();
	ENGINE_REQUIRE(names.size() == 4);
	ENGINE_CHECK_EQ(names[1], std::string("Edit Material Steel"));
	ENGINE_CHECK_EQ(names[3], std::string("Edit Material Steel"));
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(s.material(material.value())->metallic, 1.0);
	ENGINE_CHECK_EQ(std::string(s.scene().inspect(id)->name), std::string("B"));
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(std::string(s.scene().inspect(id)->name), std::string("A"));
	ENGINE_CHECK_EQ(s.material(material.value())->metallic, 1.0);
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(s.material(material.value())->metallic, assets::MaterialData{}.metallic);
	ENGINE_REQUIRE(s.jump_history(4));
	ENGINE_CHECK_EQ(s.material(material.value())->metallic, 0.5);
	ENGINE_CHECK_EQ(std::string(s.scene().inspect(id)->name), std::string("B"));
}
ENGINE_TEST(a_material_edit_after_a_scene_undo_ends_redo) {
	const auto folder = fresh_folder("history-redo");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(folder));
	const auto material = s.create_material("Steel");
	ENGINE_REQUIRE(material);
	const auto id = make(s, "A");
	ENGINE_REQUIRE(s.execute(commands::RenameEntity{id, "B"}));
	ENGINE_REQUIRE(s.undo());
	assets::MaterialData data;
	data.metallic = 1;
	ENGINE_REQUIRE(s.save_material(material.value(), data));
	ENGINE_CHECK(!s.can_redo());
	ENGINE_CHECK_EQ(s.commands().redo_size(), std::size_t{0});
	ENGINE_REQUIRE(s.undo());
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(s.scene().size(), std::size_t{0});
}
ENGINE_TEST(opening_a_project_drops_only_material_steps) {
	const auto folder = fresh_folder("history-project");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(folder));
	const auto material = s.create_material("Steel");
	ENGINE_REQUIRE(material);
	make(s, "A");
	assets::MaterialData data;
	data.metallic = 1;
	ENGINE_REQUIRE(s.save_material(material.value(), data));
	ENGINE_REQUIRE(s.open_project(fresh_folder("history-project-2")));
	const auto names = s.history_names();
	ENGINE_REQUIRE(names.size() == 1);
	ENGINE_CHECK_EQ(names[0], std::string("Create A"));
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(s.scene().size(), std::size_t{0});
}
ENGINE_TEST(dirty_follows_transactions_and_undo) {
	EditorSession s;
	const auto id = make(s, "A");
	s.mark_clean();
	ENGINE_CHECK(!s.dirty());
	{
		ScopedTransaction t(s, "Rename");
		ENGINE_REQUIRE(s.execute(commands::RenameEntity{id, "B"}));
		ENGINE_CHECK(s.dirty());
	}
	ENGINE_CHECK(s.dirty());
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK(!s.dirty());
}

// ---- Gizmo drags ----
ENGINE_TEST(a_drag_is_live_and_one_named_step) {
	EditorSession s;
	const auto id = make(s, "Crate");
	const auto before = s.history_position();
	ENGINE_REQUIRE(s.begin_drag("Move"));
	ENGINE_CHECK(s.dragging());
	for (int i = 1; i <= 120; ++i) {
		ENGINE_REQUIRE(s.update_drag(translation(i * 0.1, 0, 0)));
		ENGINE_CHECK_NEAR(world_x(s, id), i * 0.1, 1e-9);
	}
	ENGINE_REQUIRE(s.end_drag());
	ENGINE_CHECK_EQ(s.history_position(), before + 1);
	ENGINE_CHECK_EQ(std::string(s.history_names().back()), std::string("Move Crate"));
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_NEAR(world_x(s, id), 0.0, 1e-9);
	ENGINE_REQUIRE(s.redo());
	ENGINE_CHECK_NEAR(world_x(s, id), 12.0, 1e-9);
}
ENGINE_TEST(a_drag_moves_every_selected_root_once) {
	EditorSession s;
	const auto a = make(s, "A");
	const auto child = make(s, "Child", a);
	const auto b = make(s, "B");
	const EntityId all[] = {a, child, b};
	s.selection_set().set(std::span<const EntityId>(all));
	ENGINE_REQUIRE(s.selection_roots().size() == 2); // The child follows its parent.
	const auto child_start = world_x(s, child);
	ENGINE_REQUIRE(s.begin_drag("Move"));
	ENGINE_REQUIRE(s.update_drag(translation(2, 0, 0)));
	ENGINE_REQUIRE(s.end_drag());
	ENGINE_CHECK_NEAR(world_x(s, a), 2.0, 1e-9);
	ENGINE_CHECK_NEAR(world_x(s, b), 2.0, 1e-9);
	ENGINE_CHECK_NEAR(world_x(s, child), child_start + 2.0, 1e-9); // Not moved twice.
	ENGINE_CHECK_EQ(std::string(s.history_names().back()), std::string("Move 2 Entities"));
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_NEAR(world_x(s, a), 0.0, 1e-9);
	ENGINE_CHECK_NEAR(world_x(s, b), 0.0, 1e-9);
}
ENGINE_TEST(a_cancelled_drag_leaves_no_trace) {
	EditorSession s;
	const auto id = make(s, "A");
	s.mark_clean();
	const auto before = s.history_position();
	ENGINE_REQUIRE(s.begin_drag("Move"));
	ENGINE_REQUIRE(s.update_drag(translation(5, 0, 0)));
	ENGINE_REQUIRE(s.cancel_drag());
	ENGINE_CHECK_NEAR(world_x(s, id), 0.0, 1e-9);
	ENGINE_CHECK_EQ(s.history_position(), before);
	ENGINE_CHECK(!s.dirty());
	ENGINE_CHECK(!s.dragging());
}
ENGINE_TEST(a_drag_that_changes_nothing_adds_no_step) {
	EditorSession s;
	make(s, "A");
	const auto before = s.history_position();
	ENGINE_REQUIRE(s.begin_drag("Move"));
	ENGINE_REQUIRE(s.end_drag());
	ENGINE_CHECK_EQ(s.history_position(), before);
}
ENGINE_TEST(a_sheared_step_leaves_the_last_good_place) {
	EditorSession s;
	const auto parent = make(s, "Parent");
	Transform stretched; stretched.scale = {1, 3, 1};
	ENGINE_REQUIRE(s.execute(commands::SetTransform{parent, stretched}));
	const auto child = make(s, "Child", parent);
	s.select(child);
	ENGINE_REQUIRE(s.begin_drag("Rotate"));
	ENGINE_REQUIRE(s.update_drag(translation(1, 0, 0)));
	const auto good = world_x(s, child);
	// A rotation of a child under a non-uniform parent would need shear.
	WorldTransform turn;
	turn.matrix = {0.7071, 0.7071, 0, 0, -0.7071, 0.7071, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	ENGINE_CHECK(!s.update_drag(turn));
	ENGINE_CHECK_NEAR(world_x(s, child), good, 1e-9);
	ENGINE_REQUIRE(s.end_drag());
}
ENGINE_TEST(a_drag_in_play_mode_keeps_no_history_and_can_be_cancelled) {
	EditorSession s;
	const auto id = make(s, "A");
	const auto before = s.history_position();
	ENGINE_REQUIRE(s.play());
	s.select(id);
	ENGINE_REQUIRE(s.begin_drag("Move"));
	ENGINE_REQUIRE(s.update_drag(translation(3, 0, 0)));
	ENGINE_CHECK_NEAR(world_x(s, id), 3.0, 1e-9);
	ENGINE_REQUIRE(s.cancel_drag());
	ENGINE_CHECK_NEAR(world_x(s, id), 0.0, 1e-9);
	ENGINE_REQUIRE(s.begin_drag("Move"));
	ENGINE_REQUIRE(s.update_drag(translation(4, 0, 0)));
	ENGINE_REQUIRE(s.stop()); // Stopping ends it.
	ENGINE_CHECK(!s.in_transaction());
	ENGINE_CHECK(!s.dragging());
	ENGINE_CHECK_NEAR(world_x(s, id), 0.0, 1e-9);
	ENGINE_CHECK_EQ(s.history_position(), before);
}

// ---- Selection ----
ENGINE_TEST(selection_set_keeps_a_primary_and_tells_listeners_of_real_changes) {
	SelectionSet set;
	int told = 0;
	const auto token = set.subscribe([&] { ++told; });
	set.set(EntityId{5});
	ENGINE_CHECK_EQ(set.primary(), EntityId{5});
	ENGINE_CHECK_EQ(told, 1);
	set.set(EntityId{5}); // No change.
	ENGINE_CHECK_EQ(told, 1);
	set.toggle(7);
	ENGINE_CHECK_EQ(set.primary(), EntityId{7});
	ENGINE_CHECK_EQ(set.size(), std::size_t{2});
	ENGINE_CHECK_EQ(told, 2);
	set.toggle(7);
	ENGINE_CHECK_EQ(set.primary(), EntityId{5});
	ENGINE_CHECK_EQ(told, 3);
	const EntityId many[] = {1, 2, 2, 0, 3};
	set.set(std::span<const EntityId>(many));
	ENGINE_CHECK_EQ(set.size(), std::size_t{3});
	ENGINE_CHECK_EQ(set.primary(), EntityId{3});
	ENGINE_CHECK_EQ(told, 4);
	set.retain([](EntityId id) { return id != 3; });
	ENGINE_CHECK_EQ(set.primary(), EntityId{2});
	ENGINE_CHECK_EQ(told, 5);
	set.retain([](EntityId) { return true; });
	ENGINE_CHECK_EQ(told, 5);
	set.unsubscribe(token);
	set.clear();
	ENGINE_CHECK_EQ(told, 5);
	ENGINE_CHECK_EQ(set.primary(), EntityId{0});
	ENGINE_CHECK(set.empty());
}
ENGINE_TEST(selecting_is_not_an_edit) {
	EditorSession s;
	const auto a = make(s, "A");
	const auto b = make(s, "B");
	s.mark_clean();
	const auto steps = s.history_position();
	const auto revision = s.commands().revision();
	s.select(a);
	s.selection_set().toggle(b);
	s.select_all();
	s.select(0);
	ENGINE_CHECK_EQ(s.history_position(), steps);
	ENGINE_CHECK_EQ(s.commands().revision(), revision);
	ENGINE_CHECK(!s.dirty());
}
ENGINE_TEST(deleted_entities_leave_the_selection) {
	EditorSession s;
	const auto a = make(s, "A");
	const auto b = make(s, "B");
	s.select_all();
	ENGINE_REQUIRE(s.execute(commands::DeleteEntity{b}));
	ENGINE_CHECK_EQ(s.selected().size(), std::size_t{1});
	ENGINE_CHECK_EQ(s.selection(), a);
	s.select(987654);
	ENGINE_CHECK_EQ(s.selection(), EntityId{0});
}
ENGINE_TEST(selection_listeners_hear_of_a_deleted_entity) {
	EditorSession s;
	const auto a = make(s, "A");
	int told = 0;
	s.selection_set().subscribe([&] { ++told; });
	ENGINE_REQUIRE(s.execute(commands::DeleteEntity{a}));
	ENGINE_CHECK_EQ(told, 1);
}
ENGINE_TEST(multi_duplicate_and_delete_are_one_step) {
	EditorSession s;
	const auto a = make(s, "A");
	make(s, "Child", a);
	make(s, "B");
	s.select_all();
	const auto steps = s.history_position();
	ENGINE_REQUIRE(s.duplicate_selection());
	ENGINE_CHECK_EQ(s.scene().size(), std::size_t{6}); // A's subtree and B, each copied once.
	ENGINE_CHECK_EQ(s.selected().size(), std::size_t{2});
	ENGINE_CHECK_EQ(s.history_position(), steps + 1);
	ENGINE_CHECK_EQ(std::string(s.history_names().back()), std::string("Duplicate 2 Entities"));
	ENGINE_REQUIRE(s.delete_selection());
	ENGINE_CHECK_EQ(s.scene().size(), std::size_t{3});
	ENGINE_CHECK_EQ(s.history_position(), steps + 2);
	ENGINE_CHECK(s.selected().empty());
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(s.scene().size(), std::size_t{6});
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(s.scene().size(), std::size_t{3});
}
ENGINE_TEST(deleting_a_parent_and_its_child_together_works) {
	EditorSession s;
	const auto a = make(s, "A");
	const auto child = make(s, "Child", a);
	const EntityId both[] = {child, a};
	s.selection_set().set(std::span<const EntityId>(both));
	ENGINE_REQUIRE(s.delete_selection());
	ENGINE_CHECK_EQ(s.scene().size(), std::size_t{0});
}

// ---- Property metadata ----
namespace {
// Sets every property away from its default, checks it reads back, and returns the changed value.
template<class T> T change_everything(const PropertyTable<T>& table) {
	T value{};
	for (const auto& p : table.properties) {
		ENGINE_REQUIRE(p.get && p.set);
		ENGINE_CHECK(!p.id.empty() && !p.display_name.empty() && !p.category.empty() && !p.tooltip.empty());
		const auto before = p.get(value);
		PropertyValue other = before;
		std::visit([&](auto& v) {
			using V = std::decay_t<decltype(v)>;
			if constexpr (std::is_same_v<V, bool>) v = !v;
			else if constexpr (std::is_same_v<V, double>) v = v == p.min ? (p.min < p.max ? p.max : v + 1) : (p.min < p.max ? p.min : v + 1);
			else if constexpr (std::is_same_v<V, Vec3>) v = {v.x + 1, v.y + 2, v.z + 3};
			else if constexpr (std::is_same_v<V, Quaternion>) v = {0, 1, 0, 0};
			else if constexpr (std::is_same_v<V, int>) { ENGINE_REQUIRE(p.choices.size() > 1); v = v == 0 ? 1 : 0; }
			else if constexpr (std::is_same_v<V, AssetRef>) v = {42, "models/a.glb"};
			else v = 7;
		}, other);
		const T unchanged = value;
		p.set(value, other);
		ENGINE_CHECK(!(value == unchanged));
		ENGINE_CHECK(p.get(value) == other);
	}
	return value;
}
}
ENGINE_TEST(light_table_covers_every_field) {
	const auto v = change_everything(light_properties());
	const LightComponent d;
	ENGINE_CHECK(v.type != d.type && v.color != d.color && v.intensity != d.intensity && v.range != d.range && v.inner_cone_degrees != d.inner_cone_degrees && v.outer_cone_degrees != d.outer_cone_degrees && v.cast_shadows != d.cast_shadows);
}
ENGINE_TEST(camera_table_covers_every_field) {
	const auto v = change_everything(camera_properties());
	const CameraComponent d;
	ENGINE_CHECK(v.vertical_fov_degrees != d.vertical_fov_degrees && v.near_plane != d.near_plane && v.far_plane != d.far_plane && v.primary != d.primary);
}
ENGINE_TEST(mesh_table_covers_every_field_but_the_slot_list) {
	const auto v = change_everything(mesh_properties());
	const MeshComponent d;
	ENGINE_CHECK(v.mesh != d.mesh && v.color != d.color && v.roughness != d.roughness && v.metallic != d.metallic && v.mesh_asset != d.mesh_asset && v.texture != d.texture && v.material != d.material && v.uv_scale_u != d.uv_scale_u && v.uv_scale_v != d.uv_scale_v);
}
ENGINE_TEST(body_table_covers_every_field) {
	const auto v = change_everything(body_properties());
	const RigidBodyComponent d;
	ENGINE_CHECK(v.kind != d.kind && v.linear_velocity != d.linear_velocity && v.angular_velocity != d.angular_velocity && v.linear_damping != d.linear_damping && v.angular_damping != d.angular_damping && v.gravity_scale != d.gravity_scale && v.enable_sleep != d.enable_sleep && v.is_bullet != d.is_bullet && v.lock_rotation != d.lock_rotation);
}
ENGINE_TEST(collider_table_covers_every_field) {
	const auto v = change_everything(collider_properties());
	const ColliderComponent d;
	ENGINE_CHECK(v.shape != d.shape && v.center != d.center && v.rotation != d.rotation && v.half_extents != d.half_extents && v.radius != d.radius && v.half_height != d.half_height && v.density != d.density && v.friction != d.friction && v.restitution != d.restitution && v.sensor != d.sensor && v.events != d.events && v.category != d.category && v.mask != d.mask);
}
ENGINE_TEST(environment_table_covers_every_field) {
	const auto v = change_everything(environment_properties());
	const EnvironmentComponent d;
	ENGINE_CHECK(v.source != d.source && v.image != d.image && v.sky_color != d.sky_color && v.horizon_color != d.horizon_color && v.ground_color != d.ground_color && v.intensity != d.intensity && v.rotation_degrees != d.rotation_degrees && v.show_skybox != d.show_skybox && v.ev100 != d.ev100 && v.tone_mapping != d.tone_mapping && v.shadow_quality != d.shadow_quality && v.shadow_distance != d.shadow_distance && v.ambient_occlusion != d.ambient_occlusion && v.bloom != d.bloom);
}
ENGINE_TEST(transform_table_covers_every_field) {
	const auto v = change_everything(transform_properties());
	const Transform d;
	ENGINE_CHECK(v.position != d.position && v.rotation != d.rotation && v.scale != d.scale);
}
ENGINE_TEST(conditions_follow_the_component) {
	const auto& table = light_properties();
	const auto* cone = table.find("inner_cone_degrees");
	ENGINE_REQUIRE(cone && cone->condition);
	LightComponent light;
	ENGINE_CHECK(!cone->condition(light));
	ENGINE_CHECK(cone->condition_hides);
	light.type = LightType::Spot;
	ENGINE_CHECK(cone->condition(light));
	const auto* range = table.find("range");
	ENGINE_REQUIRE(range && range->condition);
	ENGINE_CHECK(range->condition(light));
	ENGINE_CHECK(table.find("no_such_field") == nullptr);
	const auto* roughness = mesh_properties().find("roughness");
	ENGINE_REQUIRE(roughness && roughness->condition);
	ENGINE_CHECK(!roughness->condition_hides); // Greyed out, not hidden.
}
ENGINE_TEST(enum_properties_map_to_the_right_enumerators) {
	const auto* tone = environment_properties().find("tone_mapping");
	ENGINE_REQUIRE(tone);
	EnvironmentComponent e;
	ENGINE_CHECK_EQ(tone->choices[std::size_t(std::get<int>(tone->get(e)))], std::string_view("ACES"));
	tone->set(e, PropertyValue(4));
	ENGINE_CHECK(e.tone_mapping == ToneMapping::Linear);
	const auto* quality = environment_properties().find("shadow_quality");
	ENGINE_REQUIRE(quality);
	quality->set(e, PropertyValue(0));
	ENGINE_CHECK(e.shadow_quality == ShadowQuality::Off);
}

// ---- What a live drag costs on a large scene: printed, for the plan's record, and not asserted ----
ENGINE_TEST(live_drag_cost_on_ten_thousand_entities) {
	EditorSession s;
	std::vector<commands::Command> batch;
	for (int i = 0; i < 10000; ++i) {
		batch.emplace_back(commands::CreateEntity{"E" + std::to_string(i), Transform{}, 0, 0, Components{MeshComponent{}, {}, {}}});
		if (batch.size() == commands::CommandBus::max_transaction) {
			ENGINE_REQUIRE(s.execute(std::span<const commands::Command>(batch), "Fill"));
			batch.clear();
		}
	}
	ENGINE_REQUIRE(s.execute(std::span<const commands::Command>(batch), "Fill"));
	s.select(s.scene().snapshot().entities.front().id);
	ENGINE_REQUIRE(s.begin_drag("Move"));
	const auto start = std::chrono::steady_clock::now();
	constexpr int steps = 20;
	for (int i = 1; i <= steps; ++i) {
		ENGINE_REQUIRE(s.update_drag(translation(i * 0.1, 0, 0)));
		(void)s.dirty();
	}
	const auto per_step = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / steps;
	ENGINE_REQUIRE(s.end_drag());
	std::cout << "10000 entities: one live drag step and dirty check take " << per_step << " ms\n";
}
