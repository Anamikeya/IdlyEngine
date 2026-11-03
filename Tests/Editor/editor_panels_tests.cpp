// What the panels stand on, without a window: folders, lock, copy and paste, grouping, moving assets with their
// references, property text and thumbnail decoding. Real Scene, CommandBus, AssetDatabase and EditorSession; no mocks.
#include "engine/test/test.h"

#include <engine/assets/asset_database.h>
#include <engine/assets/importers.h>
#include <engine/editor/EditorCommands.hpp>
#include <engine/editor/EditorPrefs.hpp>
#include <engine/editor/EditorSession.hpp>
#include <engine/editor/PropertyMeta.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace engine;
using namespace engine::editor;
namespace fs = std::filesystem;

namespace {
fs::path fresh_folder(const std::string& name) {
	const auto folder = fs::temp_directory_path() / ("engine-panels-" + name);
	fs::remove_all(folder);
	fs::create_directories(folder);
	return folder;
}
EntityId make(EditorSession& s, const std::string& name, Vec3 position = {}, EntityId parent = 0) {
	Transform t;
	t.position = position;
	const auto created = s.execute(commands::CreateEntity{name, t, parent, 0, Components{MeshComponent{}, {}, {}}});
	ENGINE_REQUIRE(created);
	return created.value().created.front();
}
double x_of(const EditorSession& s, EntityId id) { return s.scene().world_transform(id)->matrix[12]; }
renderer::Frame frame_of(EditorSession& s) {
	const auto f = s.frame(1.0);
	ENGINE_REQUIRE(f);
	return f.value();
}
void write_ppm(const fs::path& file, int width, int height) {
	std::ofstream out(file, std::ios::binary);
	out << "P6\n" << width << ' ' << height << "\n255\n";
	for (int i = 0; i < width * height; ++i) out.write("\xFF\0\0", 3); // Red.
}
const fs::path cube_obj = fs::path(GAMEENGINE_TEST_ASSETS_DIR) / "editor" / "meshes" / "cube.obj";
EntityId find_by_name(const EditorSession& s, const std::string& name) {
	for (const auto& e : s.scene().snapshot().entities) if (e.name == name) return e.id;
	return 0;
}
} // namespace

// ---- Folders ----
ENGINE_TEST(folders_nest_and_rename_with_undo) {
	EditorSession s;
	const auto outer = s.create_folder("Outer");
	ENGINE_REQUIRE(outer);
	const auto inner = s.create_folder("Inner", outer.value());
	ENGINE_REQUIRE(inner);
	ENGINE_CHECK_EQ(s.view_state().folders.find(inner.value())->parent, outer.value());
	ENGINE_REQUIRE(s.rename_folder(inner.value(), "Renamed"));
	ENGINE_CHECK_EQ(std::string(s.view_state().folders.find(inner.value())->name), std::string("Renamed"));
	ENGINE_CHECK_EQ(s.history_position(), std::size_t{3});
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(std::string(s.view_state().folders.find(inner.value())->name), std::string("Inner"));
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK(s.view_state().folders.find(inner.value()) == nullptr);
	ENGINE_REQUIRE(s.redo());
	ENGINE_CHECK(s.view_state().folders.find(inner.value()) != nullptr);
	ENGINE_CHECK(!s.create_folder(""));
	ENGINE_CHECK(!s.create_folder("X", 999));
	ENGINE_CHECK(!s.rename_folder(999, "X"));
}
ENGINE_TEST(a_folder_cannot_go_inside_itself) {
	EditorSession s;
	const auto a = s.create_folder("A").value();
	const auto b = s.create_folder("B", a).value();
	ENGINE_CHECK(!s.move_folder(a, b));
	ENGINE_CHECK(!s.move_folder(a, a));
	ENGINE_REQUIRE(s.move_folder(b, 0));
	ENGINE_CHECK_EQ(s.view_state().folders.find(b)->parent, std::uint64_t{0});
}
ENGINE_TEST(deleting_a_folder_keeps_what_it_held) {
	EditorSession s;
	const auto outer = s.create_folder("Outer").value();
	const auto inner = s.create_folder("Inner", outer).value();
	const auto nested = s.create_folder("Nested", inner).value();
	const auto a = make(s, "A");
	ENGINE_REQUIRE(s.file_entity(a, inner));
	ENGINE_REQUIRE(s.delete_folder(inner));
	ENGINE_CHECK_EQ(s.scene().size(), std::size_t{1});
	ENGINE_CHECK_EQ(s.folder_of(a), outer);
	ENGINE_CHECK_EQ(s.view_state().folders.find(nested)->parent, outer);
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(s.folder_of(a), inner);
	ENGINE_REQUIRE(s.delete_folder(outer) && s.delete_folder(inner) && s.delete_folder(nested));
	ENGINE_CHECK_EQ(s.folder_of(a), std::uint64_t{0});
}
ENGINE_TEST(filing_a_child_takes_it_out_of_its_parent_in_one_step) {
	EditorSession s;
	const auto folder = s.create_folder("Props").value();
	const auto parent = make(s, "Parent", {3, 0, 0});
	const auto child = make(s, "Child", {1, 0, 0}, parent);
	const auto steps = s.history_position();
	ENGINE_REQUIRE(s.file_entity(child, folder));
	ENGINE_CHECK_EQ(s.scene().inspect(child)->parent, EntityId{0});
	ENGINE_CHECK_NEAR(x_of(s, child), 4, 1e-9); // Its place in the world stays.
	ENGINE_CHECK_EQ(s.folder_of(child), folder);
	ENGINE_CHECK_EQ(s.history_position(), steps + 1);
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(s.scene().inspect(child)->parent, parent); // Both undone together.
	ENGINE_CHECK_EQ(s.folder_of(child), std::uint64_t{0});
	ENGINE_REQUIRE(s.redo());
	ENGINE_CHECK_EQ(s.folder_of(child), folder);
	ENGINE_CHECK_EQ(s.scene().inspect(child)->parent, EntityId{0});
}
ENGINE_TEST(filing_a_root_is_a_step_of_its_own_and_keeps_the_scene_clean) {
	EditorSession s;
	const auto a = make(s, "A");
	s.mark_clean();
	const auto folder = s.create_folder("Props").value();
	const auto revision = s.commands().revision();
	ENGINE_REQUIRE(s.file_entity(a, folder));
	ENGINE_CHECK_EQ(s.commands().revision(), revision); // Folders are not scene edits.
	ENGINE_CHECK(!s.dirty());
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(s.folder_of(a), std::uint64_t{0});
	ENGINE_REQUIRE(s.file_entity(a, folder));
	ENGINE_REQUIRE(s.file_entity(a, 0));
	ENGINE_CHECK_EQ(s.folder_of(a), std::uint64_t{0});
	ENGINE_CHECK(!s.file_entity(a, 999));
	ENGINE_CHECK(!s.file_entity(12345, folder));
}
ENGINE_TEST(folders_and_scene_edits_share_one_history_order) {
	EditorSession s;
	const auto a = make(s, "A");
	const auto folder = s.create_folder("Props").value();
	ENGINE_REQUIRE(s.execute(commands::RenameEntity{a, "B"}));
	ENGINE_REQUIRE(s.rename_folder(folder, "Renamed"));
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(std::string(s.view_state().folders.find(folder)->name), std::string("Props"));
	ENGINE_CHECK_EQ(std::string(s.scene().inspect(a)->name), std::string("B"));
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(std::string(s.scene().inspect(a)->name), std::string("A"));
	ENGINE_CHECK(s.view_state().folders.find(folder) != nullptr);
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK(s.view_state().folders.find(folder) == nullptr);
	ENGINE_REQUIRE(s.jump_history(s.history_names().size()));
	ENGINE_CHECK_EQ(std::string(s.view_state().folders.find(folder)->name), std::string("Renamed"));
}
ENGINE_TEST(a_cancelled_transaction_puts_folders_back) {
	EditorSession s;
	const auto a = make(s, "A");
	const auto folder = s.create_folder("Props").value();
	const auto steps = s.history_position();
	{
		ScopedTransaction transaction(s, "Cancelled");
		ENGINE_REQUIRE(s.file_entity(a, folder));
		transaction.cancel();
	}
	ENGINE_CHECK_EQ(s.folder_of(a), std::uint64_t{0});
	ENGINE_CHECK_EQ(s.history_position(), steps);
}
ENGINE_TEST(play_mode_cannot_change_folders) {
	EditorSession s;
	make(s, "A");
	ENGINE_REQUIRE(s.play());
	ENGINE_CHECK(!s.create_folder("X"));
}
ENGINE_TEST(folders_and_locks_are_saved_beside_the_scene) {
	const auto folder_path = fresh_folder("view");
	const auto file = folder_path / "a.scene";
	EditorSession s;
	const auto a = make(s, "A");
	const auto b = make(s, "B");
	ENGINE_REQUIRE(s.save(file));
	const auto bytes = read_text_file(file).value();
	const auto folder = s.create_folder("Set Dressing").value();
	ENGINE_REQUIRE(s.file_entity(a, folder));
	s.set_locked(b, true);
	ENGINE_CHECK(!s.dirty());
	ENGINE_REQUIRE(s.save(file));
	ENGINE_CHECK(read_text_file(file).value() == bytes);
	EditorSession other;
	ENGINE_REQUIRE(other.load(file));
	ENGINE_CHECK_EQ(other.folder_of(a), folder);
	ENGINE_CHECK(other.locked(b));
	ENGINE_CHECK_EQ(std::string(other.view_state().folders.find(folder)->name), std::string("Set Dressing")); // Spaces survive.
	const auto next = other.create_folder("Another").value();
	ENGINE_CHECK(next != folder); // IDs are not reused after loading.
}

// ---- Lock and visibility per entity ----
ENGINE_TEST(a_locked_entity_is_not_picked_and_what_is_behind_it_is) {
	EditorSession s;
	const auto front = make(s, "Front", {0, 0, 1});
	const auto back = make(s, "Back", {0, 0, -2});
	s.camera.target = {0, 0, 0}; s.camera.yaw = 0; s.camera.pitch = 0; s.camera.distance = 8;
	ENGINE_CHECK_EQ(s.pick(*frame_of(s), 0, 0), front);
	s.set_locked(front, true);
	ENGINE_CHECK_EQ(s.pick(*frame_of(s), 0, 0), back);
	s.set_locked(front, false);
	ENGINE_CHECK_EQ(s.pick(*frame_of(s), 0, 0), front);
}
ENGINE_TEST(lock_and_hide_take_the_children_along) {
	EditorSession s;
	const auto parent = make(s, "Parent");
	const auto child = make(s, "Child", {}, parent);
	const auto grandchild = make(s, "Grandchild", {}, child);
	s.select(grandchild);
	s.set_hidden(parent, true);
	ENGINE_CHECK(s.hidden(parent) && s.hidden(child) && s.hidden(grandchild));
	ENGINE_CHECK(s.selected().empty()); // What cannot be seen is not left selected.
	s.set_hidden(child, false);
	ENGINE_CHECK(s.hidden(parent) && !s.hidden(child) && !s.hidden(grandchild));
	s.set_locked(parent, true);
	ENGINE_CHECK(s.locked(grandchild));
	s.mark_clean();
	ENGINE_CHECK(!s.dirty());
}

// ---- Copy and paste ----
ENGINE_TEST(copy_and_paste_brings_the_subtree_with_new_ids_in_one_step) {
	EditorSession s;
	const auto parent = make(s, "Parent", {2, 0, 0});
	const auto child = make(s, "Child", {1, 0, 0}, parent);
	s.select(parent);
	const auto text = s.copy_selection();
	ENGINE_REQUIRE(text);
	const auto steps = s.history_position();
	ENGINE_REQUIRE(s.paste(text.value()));
	ENGINE_CHECK_EQ(s.scene().size(), std::size_t{4});
	ENGINE_CHECK_EQ(s.history_position(), steps + 1);
	ENGINE_CHECK_EQ(std::string(s.history_names().back()), std::string("Paste 2 Entities"));
	ENGINE_REQUIRE(s.selected().size() == 1);
	const auto copy = s.selection();
	ENGINE_CHECK(copy != parent);
	ENGINE_CHECK_NEAR(x_of(s, copy), 2, 1e-9);
	bool found_child = false;
	for (const auto& e : s.scene().snapshot().entities)
		if (e.parent == copy && e.id != child) { found_child = true; ENGINE_CHECK_NEAR(x_of(s, e.id), 3, 1e-9); }
	ENGINE_CHECK(found_child);
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(s.scene().size(), std::size_t{2});
}
ENGINE_TEST(a_copied_child_keeps_its_place_in_the_world) {
	EditorSession s;
	const auto parent = make(s, "Parent", {2, 0, 0});
	const auto child = make(s, "Child", {1, 0, 0}, parent);
	s.select(child);
	ENGINE_REQUIRE(s.paste(s.copy_selection().value()));
	ENGINE_CHECK_EQ(s.scene().inspect(s.selection())->parent, EntityId{0});
	ENGINE_CHECK_NEAR(x_of(s, s.selection()), 3, 1e-9);
}
ENGINE_TEST(paste_keeps_components_and_refuses_anything_else) {
	EditorSession s;
	Components components{MeshComponent{BuiltinMesh::Sphere}, {}, LightComponent{}};
	components.colliders.push_back(ColliderComponent{});
	const auto made = s.execute(commands::CreateEntity{"Rich", Transform{}, 0, 0, components});
	ENGINE_REQUIRE(made);
	s.select(made.value().created.front());
	ENGINE_REQUIRE(s.paste(s.copy_selection().value()));
	ENGINE_CHECK(s.scene().inspect(s.selection())->components == components);
	const auto size = s.scene().size();
	const auto steps = s.history_position();
	for (const auto* bad : {"", "hello", "SCENE nonsense", "cube.obj"}) ENGINE_CHECK(!s.paste(bad));
	ENGINE_CHECK_EQ(s.scene().size(), size);
	ENGINE_CHECK_EQ(s.history_position(), steps);
	s.select(0);
	ENGINE_CHECK(!s.copy_selection());
}
ENGINE_TEST(pasting_a_parent_filed_under_a_higher_id_still_works) {
	EditorSession s;
	const auto child = make(s, "Child");
	const auto parent = make(s, "Parent", {5, 0, 0});
	ENGINE_REQUIRE(s.reparent(child, parent)); // The child's ID is lower than its parent's.
	s.select(parent);
	ENGINE_REQUIRE(s.paste(s.copy_selection().value()));
	ENGINE_CHECK_EQ(s.scene().size(), std::size_t{4});
}

// ---- Group ----
ENGINE_TEST(grouping_makes_a_parent_at_the_middle_and_keeps_places) {
	EditorSession s;
	const auto a = make(s, "A", {-2, 0, 0});
	const auto b = make(s, "B", {4, 0, 0});
	const EntityId both[] = {a, b};
	s.selection_set().set(std::span<const EntityId>(both));
	const auto steps = s.history_position();
	const auto group = s.group_selection();
	ENGINE_REQUIRE(group);
	ENGINE_CHECK_EQ(s.selection(), group.value());
	ENGINE_CHECK_EQ(s.selected().size(), std::size_t{1});
	ENGINE_CHECK_NEAR(x_of(s, group.value()), 1, 1e-9);
	ENGINE_CHECK_EQ(s.scene().inspect(a)->parent, group.value());
	ENGINE_CHECK_NEAR(x_of(s, a), -2, 1e-9);
	ENGINE_CHECK_NEAR(x_of(s, b), 4, 1e-9);
	ENGINE_CHECK_EQ(s.history_position(), steps + 1);
	ENGINE_REQUIRE(s.undo());
	ENGINE_CHECK_EQ(s.scene().size(), std::size_t{2});
	ENGINE_CHECK_EQ(s.scene().inspect(a)->parent, EntityId{0});
	s.select(0);
	ENGINE_CHECK(!s.group_selection());
}

// ---- Moving assets ----
ENGINE_TEST(moving_an_asset_keeps_its_id_and_takes_its_meta_along) {
	const auto root = fresh_folder("move");
	fs::copy_file(cube_obj, root / "cube.obj");
	auto database = assets::AssetDatabase::open(root);
	ENGINE_REQUIRE(database);
	auto& db = database.value();
	ENGINE_REQUIRE(db.scan());
	const auto id = db.assets().front().id; // Derived from the path, with no .meta yet.
	ENGINE_REQUIRE(db.move(id, "props/box.obj"));
	ENGINE_CHECK(fs::exists(root / "props" / "box.obj"));
	ENGINE_CHECK(fs::exists(root / "props" / "box.obj.meta"));
	ENGINE_CHECK(!fs::exists(root / "cube.obj"));
	const auto meta = db.find(id);
	ENGINE_REQUIRE(meta.has_value());
	ENGINE_CHECK_EQ(meta->path, std::string("props/box.obj"));
	ENGINE_CHECK(db.load_mesh(id));
	ENGINE_CHECK(db.issues().empty());
	// Moving again takes the old .meta away too.
	ENGINE_REQUIRE(db.move(id, "box.obj"));
	ENGINE_CHECK(!fs::exists(root / "props" / "box.obj.meta"));
	ENGINE_CHECK(db.move(id, "box.obj")); // Nothing to do.
}
ENGINE_TEST(a_move_that_cannot_work_changes_nothing) {
	const auto root = fresh_folder("move-refused");
	fs::copy_file(cube_obj, root / "a.obj");
	fs::copy_file(cube_obj, root / "b.obj");
	auto database = assets::AssetDatabase::open(root);
	ENGINE_REQUIRE(database);
	auto& db = database.value();
	ENGINE_REQUIRE(db.scan());
	const auto id = db.find(std::string_view("a.obj"))->id;
	ENGINE_CHECK(!db.move(id, "b.obj"));          // Taken.
	ENGINE_CHECK(!db.move(id, "a.png"));          // Another type.
	ENGINE_CHECK(!db.move(id, "../outside.obj")); // Not in the root.
	ENGINE_CHECK(!db.move(assets::AssetId{12345}, "c.obj"));
	ENGINE_CHECK(fs::exists(root / "a.obj"));
	ENGINE_CHECK_EQ(std::string(db.find(id)->path), std::string("a.obj"));
	ENGINE_CHECK(!fs::exists(root / "a.png.meta"));
}
ENGINE_TEST(renaming_an_asset_updates_the_scene_and_materials_that_use_it) {
	const auto root = fresh_folder("rename");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(root));
	const auto image_source = root / "wood.ppm";
	write_ppm(image_source, 4, 4);
	const auto image = s.import_asset(image_source);
	ENGINE_REQUIRE(image);
	const auto model = s.import_asset(cube_obj);
	ENGINE_REQUIRE(model);
	assets::MaterialData data;
	data.base_color_map = {image.value().id, image.value().path};
	const auto material = s.create_material("Wood", data);
	ENGINE_REQUIRE(material);
	const auto placed = s.place_asset(model.value().id);
	ENGINE_REQUIRE(placed);
	const auto entity = placed.value().created.front();
	ENGINE_REQUIRE(s.assign_material(entity, material.value()));
	const auto steps = s.history_position();
	ENGINE_REQUIRE(s.rename_asset(model.value().id, "models/box.obj"));
	ENGINE_CHECK_EQ(std::string(s.scene().inspect(entity)->components.mesh->mesh_asset.path), std::string("models/box.obj"));
	ENGINE_CHECK_EQ(s.history_position(), steps + 1);
	ENGINE_REQUIRE(s.rename_asset(image.value().id, "textures/wood.ppm"));
	ENGINE_CHECK_EQ(std::string(s.material(material.value())->base_color_map.path), std::string("textures/wood.ppm"));
	ENGINE_REQUIRE(s.rename_asset(material.value(), "materials/oak.material"));
	ENGINE_CHECK_EQ(std::string(s.scene().inspect(entity)->components.mesh->material.path), std::string("materials/oak.material"));
	// It still draws: nothing lost its asset.
	const auto f = frame_of(s);
	ENGINE_CHECK(s.asset_problems().empty());
	ENGINE_CHECK_EQ(f->objects.size(), std::size_t{1});
	// An unused asset moves without adding a step.
	const auto other = s.create_material("Unused");
	ENGINE_REQUIRE(other);
	const auto before = s.history_position();
	ENGINE_REQUIRE(s.rename_asset(other.value(), "materials/spare.material"));
	ENGINE_CHECK_EQ(s.history_position(), before);
}
ENGINE_TEST(renaming_is_refused_in_play_mode) {
	const auto root = fresh_folder("rename-play");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(root));
	const auto model = s.import_asset(cube_obj);
	ENGINE_REQUIRE(model);
	ENGINE_REQUIRE(s.play());
	ENGINE_CHECK(!s.rename_asset(model.value().id, "b.obj"));
}

// ---- Property text ----
ENGINE_TEST(every_kind_of_value_survives_the_clipboard) {
	const PropertyValue values[] = {
		PropertyValue(true), PropertyValue(false), PropertyValue(0.1234567890123), PropertyValue(Vec3{1.5, -2, 3e10}),
		PropertyValue(Quaternion{0.1, 0.2, 0.3, 0.9}), PropertyValue(2), PropertyValue(AssetRef{42, "models/a box.glb"}), PropertyValue(std::uint64_t{0xFFFFFFFFFFFFFFFFull}),
	};
	for (const auto& value : values) {
		const auto back = property_from_text(property_to_text(value));
		ENGINE_REQUIRE(back.has_value());
		ENGINE_CHECK(*back == value);
	}
	for (const auto* bad : {"", "hello world", "number", "vector 1 2", "bool", "asset"}) ENGINE_CHECK(!property_from_text(bad).has_value());
}
ENGINE_TEST(a_whole_component_survives_the_clipboard) {
	LightComponent light;
	light.type = LightType::Spot;
	light.intensity = 1234;
	light.color = {0.1, 0.2, 0.3};
	light.cast_shadows = true;
	const auto text = component_to_text(light_properties(), light);
	LightComponent pasted;
	ENGINE_REQUIRE(component_from_text(light_properties(), text, pasted));
	ENGINE_CHECK(pasted == light);
	CameraComponent camera;
	const CameraComponent untouched = camera;
	ENGINE_CHECK(!component_from_text(camera_properties(), text, camera)); // Not a camera.
	ENGINE_CHECK(camera == untouched);
	ENGINE_CHECK(!component_from_text(light_properties(), "COMPONENT Nope\n", pasted));
}
ENGINE_TEST(a_component_with_an_unknown_line_keeps_the_rest) {
	LightComponent light;
	ENGINE_REQUIRE(component_from_text(light_properties(), "COMPONENT Light\nintensity number 7\nno_such_field number 1\nrange bool 1\n", light));
	ENGINE_CHECK_EQ(light.intensity, 7.0);
	ENGINE_CHECK_EQ(light.range, LightComponent{}.range); // The wrong kind is left alone.
}
ENGINE_TEST(choosing_an_asset_for_a_mesh_shows_it_as_authored) {
	const auto* model = mesh_properties().find("mesh_asset");
	ENGINE_REQUIRE(model != nullptr);
	MeshComponent mesh;
	mesh.color = {0.2, 0.3, 0.4};
	model->set(mesh, PropertyValue(AssetRef{7, "a.glb"}));
	ENGINE_CHECK(mesh.color == (Vec3{1, 1, 1}));
	mesh.color = {0.2, 0.3, 0.4};
	model->set(mesh, PropertyValue(AssetRef{7, "a.glb"})); // Unchanged: the tint stays.
	ENGINE_CHECK(mesh.color == (Vec3{0.2, 0.3, 0.4}));
	model->set(mesh, PropertyValue(AssetRef{})); // Cleared: the tint stays.
	ENGINE_CHECK(mesh.color == (Vec3{0.2, 0.3, 0.4}));
}

// ---- Thumbnails ----
namespace {
std::vector<EditorSession::ThumbnailData> wait_for_thumbnails(EditorSession& s) {
	std::vector<EditorSession::ThumbnailData> all;
	for (int i = 0; i < 500 && s.thumbnail_jobs(); ++i) {
		for (auto& data : s.poll_thumbnail_data()) all.push_back(std::move(data));
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	for (auto& data : s.poll_thumbnail_data()) all.push_back(std::move(data));
	return all;
}
}
ENGINE_TEST(a_large_image_is_reduced_for_its_thumbnail) {
	const auto root = fresh_folder("thumb-image");
	write_ppm(root / "wide.ppm", 512, 256);
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(root));
	const auto image = s.import_asset(root / "wide.ppm");
	ENGINE_REQUIRE(image);
	s.request_thumbnail_data(image.value().id);
	const auto done = wait_for_thumbnails(s);
	ENGINE_REQUIRE(done.size() == 1);
	const auto& t = done.front();
	ENGINE_CHECK(!t.failed);
	ENGINE_CHECK_EQ(t.width, std::uint32_t{128});
	ENGINE_CHECK_EQ(t.height, std::uint32_t{64});
	ENGINE_CHECK_EQ(t.source_width, std::uint32_t{512});
	ENGINE_CHECK_EQ(t.source_height, std::uint32_t{256});
	ENGINE_CHECK_EQ(t.pixels.size(), std::size_t{128 * 64 * 4});
	ENGINE_CHECK_EQ(int(t.pixels[0]), 255); // Red stays red.
	ENGINE_CHECK_EQ(int(t.pixels[1]), 0);
	ENGINE_CHECK_EQ(int(t.pixels[3]), 255);
}
ENGINE_TEST(a_small_image_is_not_enlarged) {
	const auto root = fresh_folder("thumb-small");
	write_ppm(root / "tiny.ppm", 2, 3);
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(root));
	const auto image = s.import_asset(root / "tiny.ppm");
	ENGINE_REQUIRE(image);
	s.request_thumbnail_data(image.value().id);
	const auto done = wait_for_thumbnails(s);
	ENGINE_REQUIRE(done.size() == 1);
	ENGINE_CHECK_EQ(done.front().width, std::uint32_t{2});
	ENGINE_CHECK_EQ(done.front().height, std::uint32_t{3});
}
ENGINE_TEST(a_model_thumbnail_is_ready_to_draw_and_says_how_big_it_is) {
	const auto root = fresh_folder("thumb-model");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(root));
	const auto model = s.import_asset(cube_obj);
	ENGINE_REQUIRE(model);
	s.request_thumbnail_data(model.value().id);
	const auto done = wait_for_thumbnails(s);
	ENGINE_REQUIRE(done.size() == 1);
	ENGINE_CHECK(!done.front().failed);
	ENGINE_CHECK(s.resources().find(done.front().mesh) != nullptr);
	ENGINE_CHECK(done.front().triangles > 0);
	ENGINE_CHECK(done.front().vertices > 0);
}
ENGINE_TEST(an_unreadable_asset_fails_quietly) {
	const auto root = fresh_folder("thumb-bad");
	write_ppm(root / "good.ppm", 2, 2);
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(root));
	const auto image = s.import_asset(root / "good.ppm");
	ENGINE_REQUIRE(image);
	{ std::ofstream(root / "Assets" / "good.ppm", std::ios::binary) << "not an image"; }
	s.request_thumbnail_data(image.value().id);
	const auto done = wait_for_thumbnails(s);
	ENGINE_REQUIRE(done.size() == 1);
	ENGINE_CHECK(done.front().failed);
	s.request_thumbnail_data(assets::AssetId{999}); // Unknown: nothing to do.
	ENGINE_CHECK_EQ(s.thumbnail_jobs(), std::size_t{0});
}
ENGINE_TEST(a_material_has_no_worker_job_and_no_model_mesh) {
	const auto root = fresh_folder("thumb-material");
	EditorSession s;
	ENGINE_REQUIRE(s.open_project(root));
	const auto material = s.create_material("M");
	ENGINE_REQUIRE(material);
	s.request_thumbnail_data(material.value());
	ENGINE_CHECK_EQ(s.thumbnail_jobs(), std::size_t{0});
	// Its picture comes from its handle, which changes with its values.
	const auto before = s.material_handle(material.value());
	assets::MaterialData data;
	data.metallic = 0.75;
	ENGINE_REQUIRE(s.save_material(material.value(), data));
	ENGINE_CHECK(!(s.material_handle(material.value()) == before));
}

// ---- Commands ----
ENGINE_TEST(the_requested_panel_keys_are_registered) {
	CommandRegistry r;
	for (auto command : default_command_table()) ENGINE_REQUIRE(r.add(std::move(command)));
	const std::pair<const char*, const char*> expected[] = {
		{"Edit.Copy", "Ctrl+C"}, {"Edit.Paste", "Ctrl+V"}, {"Edit.Group", "Ctrl+G"}, {"Edit.Rename", "F2"},
		{"Assets.ToggleDrawer", "Ctrl+Space"}, {"Assets.FindInBrowser", "Ctrl+B"}, {"Assets.EditAsset", "Ctrl+E"},
	};
	for (const auto& [id, keys] : expected) ENGINE_CHECK_EQ(r.shortcut(id), std::string(keys));
}
ENGINE_TEST(f2_and_esc_answer_the_asset_browser_first_when_it_has_the_focus) {
	CommandRegistry r;
	for (auto command : default_command_table()) ENGINE_REQUIRE(r.add(std::move(command)));
	for (const auto* id : {"Asset.Rename", "Edit.Rename", "Assets.CloseDrawer", "Edit.Deselect"}) ENGINE_REQUIRE(r.bind(id, [] {}));
	const std::string_view browser[] = {context::asset_browser};
	const std::string_view outliner[] = {context::outliner};
	ENGINE_CHECK_EQ(std::string(r.resolve(*parse_chord("F2"), browser)->info.id), std::string("Asset.Rename"));
	ENGINE_CHECK_EQ(std::string(r.resolve(*parse_chord("F2"), outliner)->info.id), std::string("Edit.Rename"));
	ENGINE_CHECK_EQ(std::string(r.resolve(*parse_chord("Esc"), browser)->info.id), std::string("Assets.CloseDrawer"));
	ENGINE_REQUIRE(r.bind("Assets.CloseDrawer", [] {}, [] { return false; }));
	ENGINE_CHECK_EQ(std::string(r.resolve(*parse_chord("Esc"), browser)->info.id), std::string("Edit.Deselect")); // No drawer open.
}
