// Reaching things by typing, and running without leaving the editor: fuzzy matching, the console's variables and actions,
// its history, simulate and eject. Real Scene, CommandBus and EditorSession; no mocks.
#include "engine/test/test.h"

#include <engine/editor/ConsoleRegistry.hpp>
#include <engine/editor/EditorCommands.hpp>
#include <engine/editor/EditorPrefs.hpp>
#include <engine/editor/EditorSession.hpp>
#include <engine/editor/FuzzyMatch.hpp>

#include <string>
#include <vector>

using namespace engine;
using namespace engine::editor;

namespace {
EntityId make(EditorSession& s, const std::string& name, Components components = Components{MeshComponent{}, {}, {}}) {
	const auto created = s.execute(commands::CreateEntity{name, Transform{}, 0, 0, std::move(components)});
	ENGINE_REQUIRE(created);
	return created.value().created.front();
}
renderer::Frame frame_of(EditorSession& s) {
	const auto f = s.frame(1.5);
	ENGINE_REQUIRE(f);
	return f.value();
}
Components primary_camera() {
	Components c;
	c.camera = CameraComponent{};
	return c;
}
std::string first_of(std::string_view query, const std::vector<std::string>& texts) {
	const auto ranked = fuzzy_rank(query, texts);
	return ranked.empty() ? std::string{} : texts[ranked.front()];
}
} // namespace

// ---- Fuzzy matching ----
ENGINE_TEST(letters_must_appear_in_order) {
	ENGINE_CHECK(fuzzy_score("fcs", "Focus").has_value());
	ENGINE_CHECK(fuzzy_score("FOCUS", "focus selected").has_value()); // Case does not matter.
	ENGINE_CHECK(!fuzzy_score("sf", "focus").has_value());            // Wrong order.
	ENGINE_CHECK(!fuzzy_score("xyz", "focus").has_value());
	ENGINE_CHECK(!fuzzy_score("focused", "focus").has_value());       // Longer than the text.
	ENGINE_CHECK(!fuzzy_score("a", "").has_value());
	ENGINE_CHECK_EQ(fuzzy_score("", "anything").value_or(-1), 0);
	ENGINE_CHECK_EQ(fuzzy_score("", "").value_or(-1), 0);
}
ENGINE_TEST(a_run_of_letters_beats_scattered_ones) {
	ENGINE_CHECK(*fuzzy_score("cube", "Cube") > *fuzzy_score("cube", "Clear Up Behind Everything"));
	ENGINE_CHECK(*fuzzy_score("und", "Undo") > *fuzzy_score("und", "Unbound"));
}
ENGINE_TEST(the_start_of_a_word_counts_most) {
	ENGINE_CHECK(*fuzzy_score("fs", "Focus Selected") > *fuzzy_score("fs", "Offset"));
	ENGINE_CHECK(*fuzzy_score("t", "Top") > *fuzzy_score("t", "Cat"));
	ENGINE_CHECK(*fuzzy_score("sa", "SaveAll") > *fuzzy_score("sa", "Usage"));          // A capital starts a word.
	ENGINE_CHECK(*fuzzy_score("mod", "models/box.glb") > *fuzzy_score("mod", "a_mod"));
}
ENGINE_TEST(the_best_alignment_is_found_not_just_the_first) {
	// A greedy match would take the first f in both, and score them the same. The second f starts a word, with the o right after it.
	ENGINE_CHECK(*fuzzy_score("fo", "xf foo") > *fuzzy_score("fo", "xf xo"));
}
ENGINE_TEST(ranking_puts_the_best_first_and_drops_the_rest) {
	const std::vector<std::string> texts = {"Delete", "Duplicate", "Undo", "Redo", "Save All"};
	ENGINE_CHECK_EQ(first_of("dup", texts), std::string("Duplicate"));
	ENGINE_CHECK_EQ(first_of("sa", texts), std::string("Save All"));
	ENGINE_CHECK_EQ(fuzzy_rank("zzz", texts).size(), std::size_t{0});
	ENGINE_CHECK_EQ(fuzzy_rank("e", texts).size(), std::size_t{4}); // All but Undo.
}
ENGINE_TEST(with_nothing_typed_the_given_order_stays) {
	const std::vector<std::string> texts = {"Zebra", "Apple", "Mango"};
	const auto ranked = fuzzy_rank("", texts);
	ENGINE_REQUIRE(ranked.size() == 3);
	ENGINE_CHECK_EQ(ranked[0], std::size_t{0});
	ENGINE_CHECK_EQ(ranked[1], std::size_t{1});
	ENGINE_CHECK_EQ(ranked[2], std::size_t{2});
}
ENGINE_TEST(ties_go_to_the_shorter_text) {
	const std::vector<std::string> texts = {"Cube Long Name", "Cube"};
	ENGINE_CHECK_EQ(first_of("cube", texts), std::string("Cube"));
}
ENGINE_TEST(a_bonus_lifts_recent_things_between_equal_matches) {
	const std::vector<std::string> texts = {"Cube One", "Cube Two"};
	const std::vector<int> bonus = {0, 5};
	const auto ranked = fuzzy_rank("cube", texts, bonus);
	ENGINE_REQUIRE(!ranked.empty());
	ENGINE_CHECK_EQ(ranked.front(), std::size_t{1});
	// But not over a far better match.
	const std::vector<std::string> other = {"Cube", "Clear Up Behind Everything"};
	const std::vector<int> small = {0, 3};
	ENGINE_CHECK_EQ(std::size_t(fuzzy_rank("cube", other, small).front()), std::size_t{0});
}
ENGINE_TEST(the_real_commands_can_be_found_by_typing) {
	std::vector<std::string> labels;
	for (const auto& info : default_command_table()) labels.push_back(info.category + " " + info.label);
	const auto finds = [&](std::string_view query, std::string_view wanted) {
		const auto ranked = fuzzy_rank(query, labels);
		ENGINE_REQUIRE(!ranked.empty());
		ENGINE_CHECK_EQ(labels[ranked.front()], std::string(wanted));
	};
	finds("focus", "Viewport Focus Selected");
	finds("undo hist", "Window Undo History");
	finds("save as", "File Save As...");
	finds("palette", "Find Command Palette");
}

// ---- Console ----
ENGINE_TEST(a_variable_shows_and_sets_its_real_value) {
	ConsoleRegistry console;
	bool value = false;
	console.add_bool("r.bloom", "Glow.", [&] { return value; }, [&](bool v) { value = v; });
	const auto shown = console.execute("r.bloom");
	ENGINE_REQUIRE(shown);
	ENGINE_CHECK(shown.value().find("r.bloom = 0") != std::string::npos);
	ENGINE_CHECK(shown.value().find("Glow.") != std::string::npos);
	ENGINE_REQUIRE(console.execute("r.bloom 1"));
	ENGINE_CHECK(value);
	value = false; // Changed from elsewhere: the console sees it.
	ENGINE_CHECK(console.execute("r.bloom").value().find("= 0") != std::string::npos);
	for (const auto* on : {"on", "ON", "true", "1"}) { value = false; ENGINE_REQUIRE(console.execute(std::string("r.bloom ") + on)); ENGINE_CHECK(value); }
	for (const auto* off : {"off", "false", "0"}) { value = true; ENGINE_REQUIRE(console.execute(std::string("r.bloom ") + off)); ENGINE_CHECK(!value); }
	value = true;
	ENGINE_CHECK(!console.execute("r.bloom maybe"));
	ENGINE_CHECK(value); // Unchanged.
}
ENGINE_TEST(a_number_is_checked_before_it_is_set) {
	ConsoleRegistry console;
	double value = 4;
	console.add_number("camera.speed", "Speed.", [&] { return value; }, [&](double v) { value = v; }, 1, 10);
	ENGINE_REQUIRE(console.execute("camera.speed 7.5"));
	ENGINE_CHECK_EQ(value, 7.5);
	for (const auto* bad : {"camera.speed fast", "camera.speed 100", "camera.speed 0", "camera.speed 5x", "camera.speed nan", "camera.speed inf"}) {
		ENGINE_CHECK(!console.execute(bad));
		ENGINE_CHECK_EQ(value, 7.5);
	}
	ENGINE_CHECK(console.execute("camera.speed").value().find("default 4") != std::string::npos);
	double open = 0;
	console.add_number("any", "Any.", [&] { return open; }, [&](double v) { open = v; });
	ENGINE_REQUIRE(console.execute("any -1000"));
	ENGINE_CHECK_EQ(open, -1000.0);
}
ENGINE_TEST(an_action_gets_the_rest_of_the_line) {
	ConsoleRegistry console;
	std::string got;
	console.add_action("say", "Says.", [&](std::string_view rest) { got = std::string(rest); return std::string("said"); });
	const auto done = console.execute("  say   hello there  ");
	ENGINE_REQUIRE(done);
	ENGINE_CHECK_EQ(std::string(done.value()), std::string("said"));
	ENGINE_CHECK_EQ(got, std::string("hello there"));
	ENGINE_CHECK(console.execute("SAY again")); // Case does not matter.
}
ENGINE_TEST(the_longest_name_wins_and_a_name_must_end_at_a_space) {
	ConsoleRegistry console;
	std::string ran;
	console.add_action("stat", "All.", [&](std::string_view) { ran = "stat"; return std::string{}; });
	console.add_action("stat fps", "Frames.", [&](std::string_view) { ran = "stat fps"; return std::string{}; });
	ENGINE_REQUIRE(console.execute("stat fps"));
	ENGINE_CHECK_EQ(ran, std::string("stat fps"));
	ENGINE_REQUIRE(console.execute("stat timings"));
	ENGINE_CHECK_EQ(ran, std::string("stat"));
	ENGINE_CHECK(!console.execute("statistics"));
	ENGINE_CHECK(!console.execute("nope"));
	ENGINE_CHECK(console.execute("   ")); // Nothing to do is not an error.
}
ENGINE_TEST(registering_a_name_again_replaces_it) {
	ConsoleRegistry console;
	console.add_action("x", "First.", [](std::string_view) { return std::string("first"); });
	console.add_action("x", "Second.", [](std::string_view) { return std::string("second"); });
	ENGINE_CHECK_EQ(std::string(console.execute("x").value()), std::string("second"));
	ENGINE_CHECK_EQ(console.all().size(), std::size_t{1});
}
ENGINE_TEST(suggestions_match_what_is_typed_with_descriptions_and_values) {
	ConsoleRegistry console;
	double speed = 4;
	console.add_number("camera.speed", "How fast.", [&] { return speed; }, [&](double v) { speed = v; });
	console.add_action("clear", "Empty it.", [](std::string_view) { return std::string{}; });
	console.add_action("help", "List.", [](std::string_view) { return std::string{}; });
	const auto found = console.suggest("cam");
	ENGINE_REQUIRE(found.size() == 1);
	ENGINE_CHECK_EQ(found[0].name, std::string("camera.speed"));
	ENGINE_CHECK_EQ(found[0].description, std::string("How fast."));
	ENGINE_CHECK_EQ(found[0].value, std::string("4"));
	ENGINE_CHECK(console.suggest("cl").front().value.empty()); // An action has no value.
	ENGINE_CHECK_EQ(console.suggest("").size(), std::size_t{3});
	ENGINE_CHECK_EQ(console.suggest("", 2).size(), std::size_t{2});
	ENGINE_CHECK(console.suggest("zzz").empty());
	// While typing a value, the variable stays in view.
	const auto typing_value = console.suggest("camera.speed 12");
	ENGINE_REQUIRE(typing_value.size() == 1);
	ENGINE_CHECK_EQ(typing_value[0].name, std::string("camera.speed"));
}
ENGINE_TEST(everything_is_listed_by_name) {
	ConsoleRegistry console;
	console.add_action("b", "", [](std::string_view) { return std::string{}; });
	console.add_action("a", "", [](std::string_view) { return std::string{}; });
	const auto all = console.all();
	ENGINE_REQUIRE(all.size() == 2);
	ENGINE_CHECK_EQ(all[0].name, std::string("a"));
}
ENGINE_TEST(history_goes_back_and_comes_forward) {
	ConsoleHistory history;
	ENGINE_CHECK(history.previous().empty());
	ENGINE_CHECK(history.next().empty());
	history.add("one");
	history.add("two");
	history.add("three");
	ENGINE_CHECK_EQ(history.previous(), std::string("three"));
	ENGINE_CHECK_EQ(history.previous(), std::string("two"));
	ENGINE_CHECK_EQ(history.previous(), std::string("one"));
	ENGINE_CHECK_EQ(history.previous(), std::string("one")); // Stops at the oldest.
	ENGINE_CHECK_EQ(history.next(), std::string("two"));
	ENGINE_CHECK_EQ(history.next(), std::string("three"));
	ENGINE_CHECK(history.next().empty()); // Back at the empty line.
	ENGINE_CHECK(history.next().empty());
	ENGINE_CHECK_EQ(history.previous(), std::string("three"));
}
ENGINE_TEST(history_keeps_repeats_once_and_has_a_limit) {
	ConsoleHistory history;
	history.add("same");
	history.add("same");
	history.add("");
	ENGINE_CHECK_EQ(history.size(), std::size_t{1});
	(void)history.previous();
	history.add("new"); // A new line goes back to the end.
	ENGINE_CHECK_EQ(history.previous(), std::string("new"));
	for (int i = 0; i < 500; ++i) history.add(std::to_string(i));
	ENGINE_CHECK_EQ(history.size(), ConsoleHistory::limit);
	history.reset_cursor();
	ENGINE_CHECK_EQ(history.previous(), std::string("499"));
}

// ---- Asset types ----
ENGINE_TEST(every_asset_type_has_a_name) {
	ENGINE_CHECK_EQ(std::string(assets::to_string(assets::AssetType::Mesh)), std::string("mesh"));
	ENGINE_CHECK_EQ(std::string(assets::to_string(assets::AssetType::Texture)), std::string("texture"));
	ENGINE_CHECK_EQ(std::string(assets::to_string(assets::AssetType::Material)), std::string("material"));
	ENGINE_CHECK_EQ(std::string(assets::to_string(assets::AssetType::Environment)), std::string("environment"));
}

// ---- Settings text ----
ENGINE_TEST(settings_keep_text_as_written) {
	EditorSettings a;
	a.set_text("palette.recent", "Edit.Undo,File.Save");
	EditorSettings b;
	b.parse(a.text());
	ENGINE_CHECK_EQ(b.text_of("palette.recent"), std::string("Edit.Undo,File.Save"));
	ENGINE_CHECK(b.text_of("missing").empty());
}

// ---- Simulate and eject ----
ENGINE_TEST(simulating_runs_the_scene_with_the_editors_view) {
	EditorSession s;
	make(s, "Eye", primary_camera());
	Components falling{MeshComponent{}, {}, {}};
	falling.body = RigidBodyComponent{};
	falling.colliders.push_back(ColliderComponent{});
	const auto crate = make(s, "Crate", falling);
	s.mark_clean();
	ENGINE_REQUIRE(s.simulate());
	ENGINE_CHECK(s.simulating());
	ENGINE_CHECK(s.ejected());
	ENGINE_CHECK(s.play_state() == PlayState::Playing);
	(void)frame_of(s);
	ENGINE_CHECK_EQ(s.view_camera(), EntityId{0}); // Not through the primary camera.
	ENGINE_CHECK(frame_of(s)->show_grid);
	const auto start = s.scene().inspect(crate)->transform.position.y;
	for (int i = 0; i < 30; ++i) s.update(EditorSession::fixed_step);
	ENGINE_CHECK(s.scene().inspect(crate)->transform.position.y < start); // The world runs.
	ENGINE_REQUIRE(s.stop());
	ENGINE_CHECK(!s.simulating());
	ENGINE_CHECK(!s.ejected());
	ENGINE_CHECK_EQ(s.scene().inspect(crate)->transform.position.y, start); // Restored exactly.
	ENGINE_CHECK(!s.dirty());
}
ENGINE_TEST(a_second_simulate_is_refused_until_stopped) {
	EditorSession s;
	make(s, "A");
	ENGINE_REQUIRE(s.simulate());
	ENGINE_CHECK(!s.simulate());
	ENGINE_REQUIRE(s.stop());
	ENGINE_REQUIRE(s.play());
	ENGINE_CHECK(!s.simulate());
}
ENGINE_TEST(ejecting_switches_the_view_and_possessing_switches_it_back) {
	EditorSession s;
	const auto eye = make(s, "Eye", primary_camera());
	ENGINE_CHECK(!s.set_ejected(true)); // Nothing is running.
	ENGINE_REQUIRE(s.play());
	(void)frame_of(s);
	ENGINE_CHECK_EQ(s.view_camera(), eye);
	ENGINE_CHECK(!frame_of(s)->show_grid);
	ENGINE_REQUIRE(s.set_ejected(true));
	ENGINE_CHECK(s.ejected());
	ENGINE_CHECK(!s.simulating());
	(void)frame_of(s);
	ENGINE_CHECK_EQ(s.view_camera(), EntityId{0});
	ENGINE_CHECK(frame_of(s)->show_grid);
	ENGINE_REQUIRE(s.set_ejected(false));
	(void)frame_of(s);
	ENGINE_CHECK_EQ(s.view_camera(), eye);
}
ENGINE_TEST(possessing_a_simulation_makes_it_a_run) {
	EditorSession s;
	make(s, "Eye", primary_camera());
	ENGINE_REQUIRE(s.simulate());
	ENGINE_REQUIRE(s.set_ejected(false));
	ENGINE_CHECK(!s.simulating());
	ENGINE_CHECK(!s.ejected());
}
ENGINE_TEST(the_runtime_scene_can_be_edited_while_ejected_and_stop_discards_it) {
	EditorSession s;
	const auto a = make(s, "A");
	const auto before = s.editor_scene().snapshot();
	const auto steps = s.history_position();
	ENGINE_REQUIRE(s.play());
	ENGINE_REQUIRE(s.set_ejected(true));
	s.select(a);
	ENGINE_REQUIRE(s.execute(commands::RenameEntity{a, "Live"}));
	ENGINE_CHECK_EQ(std::string(s.scene().inspect(a)->name), std::string("Live"));
	ENGINE_REQUIRE(s.stop());
	ENGINE_CHECK(s.editor_scene().snapshot() == before);
	ENGINE_CHECK_EQ(s.history_position(), steps);
}
ENGINE_TEST(restart_goes_back_to_the_players_view) {
	EditorSession s;
	make(s, "A");
	ENGINE_REQUIRE(s.simulate());
	ENGINE_REQUIRE(s.restart());
	ENGINE_CHECK(!s.ejected());
	ENGINE_CHECK(!s.simulating());
}

// ---- The keys that work while the game holds the keyboard ----
ENGINE_TEST(only_the_way_out_works_while_the_game_holds_the_keyboard) {
	CommandRegistry r;
	for (auto command : default_command_table()) ENGINE_REQUIRE(r.add(std::move(command)));
	std::vector<std::string> ran;
	for (const auto& command : r.commands()) ENGINE_REQUIRE(r.bind(command.info.id, [&ran, id = command.info.id] { ran.push_back(id); }));
	const std::string_view contexts[] = {context::playing};
	InputBlock block;
	block.captured = true;
	const auto press = [&](const char* keys) { ran.clear(); r.dispatch(*parse_chord(keys), contexts, block); return ran.empty() ? std::string{} : ran.front(); };
	ENGINE_CHECK_EQ(press("Esc"), std::string("Play.Escape"));
	ENGINE_CHECK_EQ(press("F8"), std::string("Play.Eject"));
	ENGINE_CHECK_EQ(press("Shift+F1"), std::string("Play.FreeMouse"));
	ENGINE_CHECK(press("Ctrl+Z").empty());
	ENGINE_CHECK(press("Delete").empty());
	ENGINE_CHECK(press("Alt+P").empty());
	ENGINE_CHECK(press("Space").empty());
	// A text field still stops even these.
	block.text_input = true;
	ENGINE_CHECK(press("Esc").empty());
}
ENGINE_TEST(only_the_palette_keys_work_from_a_text_field) {
	CommandRegistry r;
	for (auto command : default_command_table()) ENGINE_REQUIRE(r.add(std::move(command)));
	std::vector<std::string> ran;
	for (const auto& command : r.commands()) ENGINE_REQUIRE(r.bind(command.info.id, [&ran, id = command.info.id] { ran.push_back(id); }));
	InputBlock block;
	block.text_input = true;
	const auto press = [&](const char* keys) { ran.clear(); r.dispatch(*parse_chord(keys), {}, block); return ran.empty() ? std::string{} : ran.front(); };
	ENGINE_CHECK_EQ(press("Ctrl+Shift+P"), std::string("Palette.Commands"));
	ENGINE_CHECK_EQ(press("Ctrl+P"), std::string("Palette.QuickOpen"));
	ENGINE_CHECK(press("Ctrl+Z").empty()); // The field's own undo.
	ENGINE_CHECK(press("Ctrl+D").empty());
	ENGINE_CHECK(press("Delete").empty());
	ENGINE_CHECK(press("`").empty()); // The console takes its own key from the characters.
	// A dialog still stops them.
	block.modal = true;
	ENGINE_CHECK(press("Ctrl+P").empty());
	block = {};
	block.interacting = true;
	ENGINE_CHECK(press("Ctrl+P").empty());
}
ENGINE_TEST(esc_means_stop_while_running_and_deselect_otherwise) {
	CommandRegistry r;
	for (auto command : default_command_table()) ENGINE_REQUIRE(r.add(std::move(command)));
	for (const auto* id : {"Play.Escape", "Edit.Deselect"}) ENGINE_REQUIRE(r.bind(id, [] {}));
	const std::string_view running[] = {context::playing};
	ENGINE_CHECK_EQ(std::string(r.resolve(*parse_chord("Esc"), running)->info.id), std::string("Play.Escape"));
	ENGINE_CHECK_EQ(std::string(r.resolve(*parse_chord("Esc"), {})->info.id), std::string("Edit.Deselect"));
}
ENGINE_TEST(the_requested_workflow_keys_are_registered) {
	CommandRegistry r;
	for (auto command : default_command_table()) ENGINE_REQUIRE(r.add(std::move(command)));
	const std::pair<const char*, const char*> expected[] = {
		{"Play.Play", "Alt+P"}, {"Play.Simulate", "Alt+S"}, {"Play.Eject", "F8"}, {"Play.FreeMouse", "Shift+F1"}, {"Play.Escape", "Esc"},
		{"Palette.Commands", "Ctrl+Shift+P"}, {"Palette.QuickOpen", "Ctrl+P"}, {"Console.Toggle", "`"},
	};
	for (const auto& [id, keys] : expected) ENGINE_CHECK_EQ(r.shortcut(id), std::string(keys));
}
