// Milestone 5 slice 7 in the editor, without a window: the selection as Flecs tags and the outline marks read from
// them, package names, the redirector a rename leaves, and Fix Up Redirectors on disk and in the open scene. Real
// Scene, CommandBus, asset database and EditorSession, on files in a scratch project.
#include "engine/test/test.h"

#include <engine/editor/EditorSession.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::editor;
namespace fs = std::filesystem;

namespace {

fs::path fresh_project(const std::string& name)
{
	const auto folder = fs::temp_directory_path() / ("engine-asset-paths-" + name);
	fs::remove_all(folder);
	fs::create_directories(folder / "Assets");
	return folder;
}

std::string read_text(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_text(const fs::path& path, std::string_view text)
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	out << text;
}

// A 1x1 image to import, from outside the project.
fs::path image(const std::string& name)
{
	const auto file = fs::temp_directory_path() / "engine-asset-paths-sources" / (name + ".ppm");
	fs::create_directories(file.parent_path());
	write_text(file, std::string("P6\n1 1\n255\n") + "\x80\x40\x20");
	return file;
}

renderer::Frame frame_of(EditorSession& s)
{
	const auto f = s.frame(1.0);
	ENGINE_REQUIRE(f);
	return f.value();
}

EntityId created(const Result<commands::Receipt>& r)
{
	ENGINE_REQUIRE(r);
	ENGINE_REQUIRE(!r.value().created.empty());
	return r.value().created.front();
}

renderer::Outline outline_of(const renderer::FrameSnapshot& f, EntityId id)
{
	for (const auto& object : f.objects)
		if (object.entity == id) return object.outline;
	return renderer::Outline::None;
}

// Whether the shown scene's Flecs entity carries the tag.
template <class Tag>
bool tagged(const EditorSession& s, EntityId id)
{
	const auto native = s.scene().native(id);
	if (!native) return false;
	return flecs::entity(s.scene().ecs().native().c_ptr(), native).has<Tag>();
}

// By value: the check macros keep references, which must not point into a temporary record.
AssetRef texture_of(const EditorSession& s, EntityId id)
{
	const auto record = s.scene().inspect(id);
	ENGINE_REQUIRE(record && record->components.mesh);
	return record->components.mesh->texture;
}

// A project with one imported image and an entity drawing it.
struct Project {
	EditorSession s;
	fs::path folder;
	assets::AssetId brick;
	EntityId wall = 0;
	explicit Project(const std::string& name)
	{
		folder = fresh_project(name);
		ENGINE_REQUIRE(s.open_project(folder));
		const auto imported = s.import_asset(image("Brick"));
		ENGINE_REQUIRE(imported);
		brick = imported.value().id;
		wall = created(s.create("Wall"));
		ENGINE_REQUIRE(s.assign_asset(wall, brick));
	}
	std::string path_of(assets::AssetId id) const
	{
		for (const auto& meta : s.assets())
			if (meta.id == id) return meta.path;
		return {};
	}
};

} // namespace

ENGINE_TEST(the_selection_is_mirrored_as_flecs_tags_and_outlined_from_them)
{
	EditorSession s;
	const auto a = created(s.create("A"));
	const auto b = created(s.create("B"));
	const std::vector<EntityId> both{a, b};
	s.selection_set().set(both); // B, the last, is the primary one.
	const auto authored = s.editor_scene().snapshot();
	const auto steps = s.history_names().size();
	auto f = frame_of(s);
	ENGINE_CHECK(tagged<SelectedTag>(s, a) && !tagged<PrimarySelectedTag>(s, a));
	ENGINE_CHECK(tagged<SelectedTag>(s, b) && tagged<PrimarySelectedTag>(s, b));
	ENGINE_CHECK(outline_of(*f, a) == renderer::Outline::Selected);
	ENGINE_CHECK(outline_of(*f, b) == renderer::Outline::Primary);
	// Tags are not content: the scene, its history and its saved form are as they were.
	ENGINE_CHECK(s.editor_scene().snapshot() == authored);
	ENGINE_CHECK_EQ(s.history_names().size(), steps);
	const auto saved = fresh_project("tags") / "tags.scene";
	ENGINE_REQUIRE(s.save(saved));
	ENGINE_CHECK(read_text(saved).find("Selected") == std::string::npos);

	s.select(a);
	f = frame_of(s);
	ENGINE_CHECK(tagged<PrimarySelectedTag>(s, a) && !tagged<SelectedTag>(s, b) && !tagged<PrimarySelectedTag>(s, b));
	ENGINE_CHECK(outline_of(*f, a) == renderer::Outline::Primary);
	ENGINE_CHECK(outline_of(*f, b) == renderer::Outline::None);
	// The game's own view shows none, though the tags stay.
	s.view_flags.outlines = false;
	f = frame_of(s);
	ENGINE_CHECK(outline_of(*f, a) == renderer::Outline::None);
	ENGINE_CHECK(tagged<SelectedTag>(s, a));
	s.view_flags.outlines = true;
	s.select(0);
	f = frame_of(s);
	ENGINE_CHECK(!tagged<SelectedTag>(s, a) && outline_of(*f, a) == renderer::Outline::None);
	ENGINE_CHECK(apply_selection_tags(const_cast<Scene&>(s.editor_scene()), s.selected()) == 0u);
}

ENGINE_TEST(a_selected_group_outlines_its_members)
{
	EditorSession s;
	const auto a = created(s.create("A"));
	const auto b = created(s.create("B"));
	const auto other = created(s.create("Other"));
	const std::vector<EntityId> both{a, b};
	s.selection_set().set(both);
	const auto group = s.group_selection();
	ENGINE_REQUIRE(group);
	s.select(group.value());
	const auto f = frame_of(s);
	ENGINE_CHECK(outline_of(*f, a) == renderer::Outline::Primary);
	ENGINE_CHECK(outline_of(*f, b) == renderer::Outline::Primary);
	ENGINE_CHECK(outline_of(*f, other) == renderer::Outline::None);
}

ENGINE_TEST(play_tags_the_runtime_copy_and_leaves_the_editor_scene_alone)
{
	EditorSession s;
	const auto a = created(s.create("A"));
	ENGINE_REQUIRE(s.play());
	s.select(a);
	const auto f = frame_of(s);
	ENGINE_CHECK(tagged<SelectedTag>(s, a)); // s.scene() is the runtime copy now.
	ENGINE_CHECK(outline_of(*f, a) == renderer::Outline::Primary);
	const auto native = s.editor_scene().native(a);
	ENGINE_REQUIRE(native != 0);
	ENGINE_CHECK(!flecs::entity(s.editor_scene().ecs().native().c_ptr(), native).has<SelectedTag>());
	ENGINE_REQUIRE(s.stop());
	(void)frame_of(s);
	ENGINE_CHECK(tagged<SelectedTag>(s, a) == s.selected().contains(a));
}

ENGINE_TEST(assets_have_package_names_in_any_case)
{
	Project p("names");
	const auto name = p.s.package_name_of(p.brick);
	ENGINE_CHECK_EQ(name, std::string("/Game/Brick"));
	for (const char* typed : {"/Game/Brick", "/game/brick", "/GAME/BRICK.PPM"}) {
		const auto found = p.s.resolve_package(typed);
		ENGINE_REQUIRE(found);
		ENGINE_CHECK(found.value().id == p.brick);
		ENGINE_CHECK(found.value().redirects.empty());
		ENGINE_CHECK(fs::exists(found.value().file));
	}
	const auto missing = p.s.resolve_package("/Game/Marble");
	ENGINE_REQUIRE(!missing);
	ENGINE_CHECK(missing.error().code == ErrorCode::NotFound);
	ENGINE_CHECK(p.s.package_name_of(assets::AssetId{12345}).empty());
}

ENGINE_TEST(a_rename_leaves_a_redirector_that_old_names_follow)
{
	Project p("rename");
	// A package name, without the extension: it keeps the file's.
	ENGINE_REQUIRE(p.s.rename_asset(p.brick, "/Game/Textures/Walls/Brick"));
	ENGINE_CHECK_EQ(p.path_of(p.brick), std::string("Textures/Walls/Brick.ppm"));
	ENGINE_CHECK_EQ(p.s.package_name_of(p.brick), std::string("/Game/Textures/Walls/Brick"));
	ENGINE_CHECK(fs::exists(p.folder / "Assets" / "Brick.ppm.redirect"));
	const auto redirectors = p.s.redirectors();
	ENGINE_REQUIRE(redirectors.size() == 1u);
	ENGINE_CHECK_EQ(redirectors.front().target, std::string("Textures/Walls/Brick.ppm"));
	ENGINE_CHECK(p.s.package_conflicts().empty());
	const auto old = p.s.resolve_package("/game/brick");
	ENGINE_REQUIRE(old);
	ENGINE_CHECK(old.value().id == p.brick);
	ENGINE_REQUIRE(old.value().redirects.size() == 1u);
	ENGINE_CHECK_EQ(old.value().redirects.front(), std::string("/Game/Brick"));
	// The open scene was updated as before, in one step.
	const auto updated = texture_of(p.s, p.wall);
	ENGINE_CHECK_EQ(updated.path, std::string("Textures/Walls/Brick.ppm"));
	// A name outside /Game is refused.
	ENGINE_CHECK(!p.s.rename_asset(p.brick, "/Engine/Brick"));
}

ENGINE_TEST(a_reference_whose_id_is_gone_resolves_through_the_redirector)
{
	Project p("dangling");
	ENGINE_REQUIRE(p.s.rename_asset(p.brick, "Textures/Brick.ppm"));
	// As a scene saved before the move names it, by an ID whose .meta was lost and the old path.
	auto mesh = p.s.scene().inspect(p.wall)->components.mesh;
	mesh->texture = AssetRef{0x0123456789abcdefull, "Brick.ppm"};
	ENGINE_REQUIRE(p.s.execute(commands::SetMesh{p.wall, mesh}));
	const auto f = frame_of(p.s);
	ENGINE_CHECK(p.s.asset_problems().empty());
	bool textured = false;
	for (const auto& object : f->objects)
		if (object.entity == p.wall) textured = object.surface.texture.index != 0;
	ENGINE_CHECK(textured);
	// So does one written with a package name.
	mesh->texture = AssetRef{0x0123456789abcdefull, "/game/brick"};
	ENGINE_REQUIRE(p.s.execute(commands::SetMesh{p.wall, mesh}));
	(void)frame_of(p.s);
	ENGINE_CHECK(p.s.asset_problems().empty());
	// Without the redirector, the path leads nowhere and the problem is said.
	ENGINE_REQUIRE(p.s.fix_up_redirectors());
	mesh->texture = AssetRef{0x0123456789abcdefull, "Brick.ppm"};
	ENGINE_REQUIRE(p.s.execute(commands::SetMesh{p.wall, mesh}));
	(void)frame_of(p.s);
	ENGINE_CHECK(!p.s.asset_problems().empty());
}

ENGINE_TEST(fix_up_redirectors_rewrites_scene_files_the_open_scene_and_materials)
{
	Project p("fixup");
	const auto old_path = p.path_of(p.brick);
	fs::create_directories(p.folder / "Levels" / "Deep");
	ENGINE_REQUIRE(p.s.save(p.folder / "Main.scene"));
	// Two more scenes naming the image where it is now, saved in folders of their own.
	ENGINE_REQUIRE(p.s.save(p.folder / "Levels" / "Other.scene"));
	ENGINE_REQUIRE(p.s.save(p.folder / "Levels" / "Deep" / "Third.scene"));
	ENGINE_REQUIRE(p.s.load(p.folder / "Main.scene"));
	ENGINE_REQUIRE(p.s.rename_asset(p.brick, "Textures/Brick.ppm"));
	ENGINE_REQUIRE(p.s.save(p.folder / "Main.scene"));
	// Other.scene and Third.scene still name the old path on disk. Opening Other makes it the open scene.
	ENGINE_REQUIRE(p.s.load(p.folder / "Levels" / "Other.scene"));
	const auto opened = texture_of(p.s, p.wall);
	ENGINE_CHECK_EQ(opened.path, old_path);
	// A material made since, naming the image by its old path.
	assets::MaterialData stale;
	stale.base_color_map = assets::MaterialTexture{p.brick, old_path};
	const auto material = p.s.create_material("Stale", stale);
	ENGINE_REQUIRE(material);
	const auto steps = p.s.history_names().size();

	const auto fixed = p.s.fix_up_redirectors();
	ENGINE_REQUIRE(fixed);
	const auto& r = fixed.value();
	ENGINE_CHECK_EQ(r.redirectors, std::size_t{1});
	ENGINE_REQUIRE(r.scenes.size() == 1u);
	ENGINE_CHECK(fs::equivalent(r.scenes.front(), p.folder / "Levels" / "Deep" / "Third.scene"));
	ENGINE_CHECK(r.open_scene);
	ENGINE_CHECK_EQ(r.references, std::size_t{2});
	ENGINE_CHECK_EQ(r.assets, std::size_t{1});
	ENGINE_CHECK(r.kept.empty());
	ENGINE_CHECK(p.s.redirectors().empty());
	ENGINE_CHECK(!fs::exists(p.folder / "Assets" / (old_path + ".redirect")));
	// On disk: the closed scene names the new path; the open one is changed in memory only, as one undo step.
	const auto third = read_text(p.folder / "Levels" / "Deep" / "Third.scene");
	ENGINE_CHECK(third.find("\"Textures/Brick.ppm\"") != std::string::npos);
	ENGINE_CHECK(third.find("\"" + old_path + "\"") == std::string::npos);
	ENGINE_CHECK(read_text(p.folder / "Levels" / "Other.scene").find("\"" + old_path + "\"") != std::string::npos);
	const auto fixed_ref = texture_of(p.s, p.wall);
	ENGINE_CHECK_EQ(fixed_ref.path, std::string("Textures/Brick.ppm"));
	const auto history = p.s.history_names();
	ENGINE_REQUIRE(!history.empty());
	ENGINE_CHECK_EQ(history.size(), steps + 1);
	ENGINE_CHECK_EQ(history.back(), std::string("Fix Up Redirectors"));
	ENGINE_CHECK(p.s.dirty());
	const auto saved = p.s.material(material.value());
	ENGINE_REQUIRE(saved.has_value());
	ENGINE_CHECK_EQ(saved->base_color_map.path, std::string("Textures/Brick.ppm"));
	ENGINE_REQUIRE(p.s.undo());
	const auto undone = texture_of(p.s, p.wall);
	ENGINE_CHECK_EQ(undone.path, old_path);
	// The old path still resolves by ID, so undoing leaves nothing broken.
	(void)frame_of(p.s);
	ENGINE_CHECK(p.s.asset_problems().empty());
}

ENGINE_TEST(fix_up_keeps_redirectors_that_lead_nowhere)
{
	Project p("loop");
	write_text(p.folder / "Assets" / "A.ppm.redirect", "version = 1\ntarget = B.ppm\n");
	write_text(p.folder / "Assets" / "B.ppm.redirect", "version = 1\ntarget = A.ppm\n");
	write_text(p.folder / "Assets" / "Gone.ppm.redirect", "version = 1\ntarget = Nowhere.ppm\n");
	ENGINE_REQUIRE(p.s.rescan_assets());
	ENGINE_REQUIRE(p.s.redirectors().size() == 3u);
	const auto loop = p.s.resolve_package("/Game/A");
	ENGINE_REQUIRE(!loop);
	ENGINE_CHECK(loop.error().code == ErrorCode::Cycle);
	const auto fixed = p.s.fix_up_redirectors();
	ENGINE_REQUIRE(fixed);
	ENGINE_CHECK_EQ(fixed.value().redirectors, std::size_t{0});
	ENGINE_CHECK_EQ(fixed.value().kept.size(), std::size_t{3});
	ENGINE_CHECK(fs::exists(p.folder / "Assets" / "A.ppm.redirect"));
	ENGINE_CHECK(fs::exists(p.folder / "Assets" / "Gone.ppm.redirect"));
	// Not while playing.
	write_text(p.folder / "Assets" / "Brick2.ppm.redirect", "version = 1\ntarget = Brick.ppm\n");
	ENGINE_REQUIRE(p.s.rescan_assets());
	ENGINE_REQUIRE(p.s.play());
	ENGINE_CHECK(!p.s.fix_up_redirectors());
	ENGINE_REQUIRE(p.s.stop());
}
