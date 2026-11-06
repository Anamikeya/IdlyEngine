// Scripts in the editor, without a window (Milestone 6, slice 2): New Script, reading what a script declares, the
// Script component and its values with undo, values a script no longer declares, errors, saving, copy and paste,
// duplication and renames. Real Scene, CommandBus, AssetDatabase, script runtime and EditorSession.
#include "engine/test/test.h"

#include <engine/editor/EditorSession.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::editor;
namespace fs = std::filesystem;

namespace {

fs::path fresh_project(const std::string& name)
{
	const auto folder = fs::temp_directory_path() / ("engine-scripts-" + name);
	fs::remove_all(folder);
	fs::create_directories(folder);
	return folder;
}

const char* gate_text = R"lua(local Gate = {
	properties = {
		speed = 1.5,
		locked = false,
		message = "The gate opens",
		offset = Vec3(0, 3, 0),
		tint = Color(1, 0.5, 0),
		trigger = Entity(),
		sound = Asset(),
	},
}
function Gate:tick(dt) end
return Gate
)lua";

// Writes a script's text in place, as an editor outside the engine would, and makes sure its time moves on.
void write_script(const EditorSession& s, assets::AssetId id, const std::string& text)
{
	const auto catalogue = s.assets();
	const auto meta = std::ranges::find_if(catalogue, [&](const assets::AssetMetadata& m) { return m.id == id; });
	ENGINE_REQUIRE(meta != catalogue.end());
	const fs::path file = s.asset_folder() / meta->path;
	const auto before = fs::last_write_time(file);
	{
		std::ofstream out(file, std::ios::binary | std::ios::trunc);
		out << text;
	}
	if (fs::last_write_time(file) == before) fs::last_write_time(file, before + std::chrono::seconds(1));
}

// By value: the check macros keep references, which must not point into a temporary record.
ScriptComponent script_of(const EditorSession& s, EntityId id)
{
	const auto record = s.scene().inspect(id);
	ENGINE_REQUIRE(record && record->components.script);
	return *record->components.script;
}

EntityId make_entity(EditorSession& s, const std::string& name, Components components = {})
{
	const auto made = s.execute(commands::CreateEntity{name, {}, 0, 0, std::move(components)});
	ENGINE_REQUIRE(made);
	return made.value().created.front();
}

struct Project {
	EditorSession s;
	assets::AssetId gate;
	explicit Project(const std::string& name)
	{
		ENGINE_REQUIRE(s.open_project(fresh_project(name)));
		const auto created = s.create_script("Gate");
		ENGINE_REQUIRE(created);
		gate = created.value();
		write_script(s, gate, gate_text);
	}
};

} // namespace

ENGINE_TEST(new_script_starts_from_a_template_that_runs)
{
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(fresh_project("template")));
	const auto created = s.create_script("Door");
	ENGINE_REQUIRE(created);
	const auto catalogue = s.assets();
	const auto meta = std::ranges::find_if(catalogue, [&](const assets::AssetMetadata& m) { return m.id == created.value(); });
	ENGINE_REQUIRE(meta != catalogue.end());
	ENGINE_CHECK_EQ(meta->path, std::string("Scripts/Door.lua"));
	ENGINE_CHECK(meta->type == assets::AssetType::Script);
	ENGINE_CHECK_EQ(s.package_name_of(created.value()), std::string("/Game/Scripts/Door"));
	const auto info = s.script_info(created.value());
	ENGINE_REQUIRE(info);
	ENGINE_CHECK_EQ(info.value().name, std::string("/Game/Scripts/Door"));
	ENGINE_REQUIRE(info.value().properties.size() == 1);
	ENGINE_CHECK_EQ(info.value().properties[0].name, std::string("speed"));
	ENGINE_CHECK(info.value().callbacks == (std::vector<std::string>{"start", "tick"}));
	// A second one gets a free name.
	const auto again = s.create_script("Door");
	ENGINE_REQUIRE(again);
	ENGINE_CHECK(again.value() != created.value());
}

ENGINE_TEST(assigning_a_script_adds_the_component_in_one_undo_step)
{
	Project p("assign");
	const auto gate = make_entity(p.s, "Gate");
	ENGINE_REQUIRE(p.s.assign_script(gate, p.gate));
	const auto component = script_of(p.s, gate);
	ENGINE_CHECK_EQ(component.script.id, p.gate.value);
	ENGINE_CHECK_EQ(component.script.path, std::string("Scripts/Gate.lua"));
	ENGINE_CHECK(component.properties.empty());
	ENGINE_CHECK_EQ(std::string(p.s.history_names().back()), std::string("Assign Script"));
	ENGINE_REQUIRE(p.s.undo());
	ENGINE_CHECK(!p.s.scene().inspect(gate)->components.script);
	ENGINE_REQUIRE(p.s.redo());
	ENGINE_CHECK(p.s.scene().inspect(gate)->components.script.has_value());
	// Only scripts.
	const auto material = p.s.create_material("Stone");
	ENGINE_REQUIRE(material);
	const auto refused = p.s.assign_script(gate, material.value());
	ENGINE_CHECK(!refused);
	if (!refused) ENGINE_CHECK(refused.error().code == ErrorCode::InvalidArgument);
}

ENGINE_TEST(an_entity_keeps_only_the_values_that_differ_from_the_defaults)
{
	Project p("values");
	const auto gate = make_entity(p.s, "Gate");
	const auto lever = make_entity(p.s, "Lever");
	ENGINE_REQUIRE(p.s.assign_script(gate, p.gate));
	const auto info = p.s.script_info(p.gate);
	ENGINE_REQUIRE(info);
	ENGINE_CHECK_EQ(info.value().properties.size(), std::size_t{7});

	ENGINE_REQUIRE(p.s.set_script_property(gate, "speed", 2.5));
	ENGINE_REQUIRE(p.s.set_script_property(gate, "message", std::string("Open!")));
	ENGINE_REQUIRE(p.s.set_script_property(gate, "offset", Vec3{0, 4, 0}));
	ENGINE_REQUIRE(p.s.set_script_property(gate, "tint", ScriptColor{0, 1, 0, 1}));
	ENGINE_REQUIRE(p.s.set_script_property(gate, "trigger", EntityLink{lever}));
	ENGINE_REQUIRE(p.s.set_script_property(gate, "sound", AssetRef{p.gate.value, "Scripts/Gate.lua"}));
	auto component = script_of(p.s, gate);
	std::vector<std::string> names;
	for (const auto& property : component.properties) names.push_back(property.name);
	ENGINE_CHECK(names == (std::vector<std::string>{"message", "offset", "sound", "speed", "tint", "trigger"})); // In name order.
	ENGINE_CHECK(std::get<double>(component.find("speed")->value) == 2.5);
	ENGINE_CHECK(std::get<EntityLink>(component.find("trigger")->value).id == lever);
	ENGINE_CHECK_EQ(std::string(p.s.history_names().back()), std::string("Change sound"));

	// Back to the default: the entity no longer holds its own value, and follows the script's again.
	ENGINE_REQUIRE(p.s.set_script_property(gate, "speed", 1.5));
	ENGINE_CHECK(!script_of(p.s, gate).find("speed"));
	// The wrong type is refused.
	const auto wrong = p.s.set_script_property(gate, "speed", true);
	ENGINE_CHECK(!wrong);
	if (!wrong) ENGINE_CHECK(wrong.error().code == ErrorCode::InvalidArgument);
	// Undo brings the value back.
	ENGINE_REQUIRE(p.s.undo());
	ENGINE_CHECK(std::get<double>(script_of(p.s, gate).find("speed")->value) == 2.5);
	// The scene's values turn into what the script sees, assets by package name.
	const auto seen = p.s.script_value(script_of(p.s, gate).find("sound")->value);
	ENGINE_CHECK(std::get<scripting::AssetRef>(seen).name == "/Game/Scripts/Gate");
	ENGINE_CHECK(p.s.scene_value(scripting::AssetRef{0, "/Game/Scripts/Gate"}) == ScriptValue(AssetRef{p.gate.value, "Scripts/Gate.lua"}));
}

ENGINE_TEST(a_value_the_script_no_longer_declares_is_kept_until_removed)
{
	Project p("stale");
	const auto gate = make_entity(p.s, "Gate");
	ENGINE_REQUIRE(p.s.assign_script(gate, p.gate));
	ENGINE_REQUIRE(p.s.set_script_property(gate, "speed", 4.0));
	// The script renames speed to velocity.
	std::string renamed = gate_text;
	renamed.replace(renamed.find("speed = 1.5"), 11, "velocity = 1.5");
	write_script(p.s, p.gate, renamed);
	const auto info = p.s.script_info(p.gate);
	ENGINE_REQUIRE(info);
	ENGINE_CHECK(info.value().property("velocity") != nullptr);
	ENGINE_CHECK(info.value().property("speed") == nullptr);
	ENGINE_CHECK(script_of(p.s, gate).find("speed") != nullptr); // Nothing is lost by the rename.
	ENGINE_REQUIRE(p.s.reset_script_property(gate, "speed"));
	ENGINE_CHECK(!script_of(p.s, gate).find("speed"));
	ENGINE_CHECK(!p.s.reset_script_property(gate, "speed"));
}

ENGINE_TEST(a_broken_script_says_where_and_mending_it_reads_it_again)
{
	Project p("broken");
	write_script(p.s, p.gate, "local Gate = {\n\tproperties = { speed = 1 \n}\nreturn Gate\n");
	const auto broken = p.s.script_info(p.gate);
	ENGINE_CHECK(!broken);
	if (!broken) ENGINE_CHECK(broken.error().message.find("/Game/Scripts/Gate:4:") != std::string::npos);
	write_script(p.s, p.gate, gate_text);
	const auto mended = p.s.script_info(p.gate);
	ENGINE_REQUIRE(mended);
	ENGINE_CHECK_EQ(mended.value().properties.size(), std::size_t{7});
	// A script that runs forever at its top level is stopped, and says so.
	write_script(p.s, p.gate, "while true do end\nreturn {}\n");
	const auto forever = p.s.script_info(p.gate);
	ENGINE_CHECK(!forever);
	if (!forever) ENGINE_CHECK(forever.error().message.find("took too long") != std::string::npos);
}

ENGINE_TEST(script_values_are_saved_with_the_scene)
{
	Project p("save");
	const auto lever = make_entity(p.s, "Lever");
	const auto gate = make_entity(p.s, "Gate");
	ENGINE_REQUIRE(p.s.assign_script(gate, p.gate));
	ENGINE_REQUIRE(p.s.set_script_property(gate, "message", std::string("Say \"open\"")));
	ENGINE_REQUIRE(p.s.set_script_property(gate, "trigger", EntityLink{lever}));
	const auto saved = script_of(p.s, gate);
	const fs::path file = p.s.project() / "gate.scene";
	ENGINE_REQUIRE(p.s.save(file));
	ENGINE_REQUIRE(p.s.new_scene());
	ENGINE_REQUIRE(p.s.load(file));
	ENGINE_CHECK(script_of(p.s, gate) == saved);
	std::ifstream in(file, std::ios::binary);
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	ENGINE_CHECK(text.rfind("ENGINE_SCENE 8 ", 0) == 0);
	ENGINE_CHECK(text.find("SCRIPT " + std::to_string(gate) + " ") != std::string::npos);
}

ENGINE_TEST(copies_point_their_links_at_the_copies)
{
	Project p("paste");
	const auto lever = make_entity(p.s, "Lever");
	const auto other = make_entity(p.s, "Other");
	const auto gate = make_entity(p.s, "Gate");
	ENGINE_REQUIRE(p.s.assign_script(gate, p.gate));
	ENGINE_REQUIRE(p.s.set_script_property(gate, "trigger", EntityLink{lever}));
	// Gate and Lever copied together: the copy of Gate names the copy of Lever.
	const std::vector<EntityId> both{gate, lever};
	p.s.selection_set().set(std::span<const EntityId>(both));
	const auto text = p.s.copy_selection();
	ENGINE_REQUIRE(text);
	ENGINE_REQUIRE(p.s.paste(text.value()));
	EntityId gate_copy = 0, lever_copy = 0;
	for (const auto& e : p.s.scene().snapshot().entities) {
		if (e.id == gate || e.id == lever || e.id == other) continue;
		(e.name == "Gate" ? gate_copy : lever_copy) = e.id;
	}
	ENGINE_REQUIRE(gate_copy && lever_copy);
	ENGINE_CHECK(std::get<EntityLink>(script_of(p.s, gate_copy).find("trigger")->value).id == lever_copy);
	// Gate alone: its copy still names the Lever that is in the scene.
	p.s.select(gate);
	const auto alone = p.s.copy_selection();
	ENGINE_REQUIRE(alone);
	ENGINE_REQUIRE(p.s.paste(alone.value()));
	const EntityId pasted = p.s.selection();
	ENGINE_CHECK(std::get<EntityLink>(script_of(p.s, pasted).find("trigger")->value).id == lever);
	// Into a scene without that Lever, the link is cleared rather than left to name a stranger.
	ENGINE_REQUIRE(p.s.new_scene());
	ENGINE_REQUIRE(p.s.paste(alone.value()));
	ENGINE_CHECK(std::get<EntityLink>(script_of(p.s, p.s.selection()).find("trigger")->value).id == 0);
}

ENGINE_TEST(renaming_a_script_updates_what_names_it)
{
	Project p("rename");
	const auto gate = make_entity(p.s, "Gate");
	ENGINE_REQUIRE(p.s.assign_script(gate, p.gate));
	ENGINE_REQUIRE(p.s.set_script_property(gate, "sound", AssetRef{p.gate.value, "Scripts/Gate.lua"}));
	ENGINE_REQUIRE(p.s.rename_asset(p.gate, "/Game/Logic/Portcullis"));
	const auto component = script_of(p.s, gate);
	ENGINE_CHECK_EQ(component.script.path, std::string("Logic/Portcullis.lua"));
	ENGINE_CHECK_EQ(std::get<AssetRef>(component.find("sound")->value).path, std::string("Logic/Portcullis.lua"));
	ENGINE_CHECK_EQ(p.s.package_name_of(p.gate), std::string("/Game/Logic/Portcullis"));
	const auto info = p.s.script_info(p.gate);
	ENGINE_REQUIRE(info);
	ENGINE_CHECK_EQ(info.value().name, std::string("/Game/Logic/Portcullis"));
}
