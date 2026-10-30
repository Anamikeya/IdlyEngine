#include "EditorUI.hpp"
#include "FileDialog.hpp"
#include <engine/editor/EditorPrefs.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace engine::editor {
namespace {
constexpr const wchar_t *scene_filter = L"Scene files (*.scene)\0*.scene\0All files\0*.*\0";
constexpr const wchar_t *keymap_filter = L"Keymap files (*.ini)\0*.ini\0All files\0*.*\0";
// Every key a chord can use, with what ImGui calls it.
struct KeyPair { Key key; ImGuiKey imgui; };
const std::vector<KeyPair> &key_pairs() {
    static const std::vector<KeyPair> pairs = [] {
        std::vector<KeyPair> list;
        for (int i = 0; i < 26; ++i) list.push_back({Key(int(Key::A) + i), ImGuiKey(ImGuiKey_A + i)});
        for (int i = 0; i < 10; ++i) list.push_back({Key(int(Key::Num0) + i), ImGuiKey(ImGuiKey_0 + i)});
        for (int i = 0; i < 12; ++i) list.push_back({Key(int(Key::F1) + i), ImGuiKey(ImGuiKey_F1 + i)});
        const std::array<KeyPair, 26> rest{{{Key::Space, ImGuiKey_Space}, {Key::Enter, ImGuiKey_Enter}, {Key::Escape, ImGuiKey_Escape}, {Key::Tab, ImGuiKey_Tab},
            {Key::Backspace, ImGuiKey_Backspace}, {Key::Delete, ImGuiKey_Delete}, {Key::Insert, ImGuiKey_Insert}, {Key::Home, ImGuiKey_Home}, {Key::End, ImGuiKey_End},
            {Key::PageUp, ImGuiKey_PageUp}, {Key::PageDown, ImGuiKey_PageDown}, {Key::Left, ImGuiKey_LeftArrow}, {Key::Right, ImGuiKey_RightArrow}, {Key::Up, ImGuiKey_UpArrow},
            {Key::Down, ImGuiKey_DownArrow}, {Key::Grave, ImGuiKey_GraveAccent}, {Key::LeftBracket, ImGuiKey_LeftBracket}, {Key::RightBracket, ImGuiKey_RightBracket},
            {Key::Minus, ImGuiKey_Minus}, {Key::Equal, ImGuiKey_Equal}, {Key::Semicolon, ImGuiKey_Semicolon}, {Key::Apostrophe, ImGuiKey_Apostrophe},
            {Key::Comma, ImGuiKey_Comma}, {Key::Period, ImGuiKey_Period}, {Key::Slash, ImGuiKey_Slash}, {Key::Backslash, ImGuiKey_Backslash}}};
        list.insert(list.end(), rest.begin(), rest.end());
        return list;
    }();
    return pairs;
}
std::uint8_t held_modifiers() {
    const auto &io = ImGui::GetIO();
    return std::uint8_t((io.KeyCtrl ? modifier::ctrl : 0) | (io.KeyShift ? modifier::shift : 0) | (io.KeyAlt ? modifier::alt : 0));
}
}
void EditorUI::mark(std::string_view control) {
    if (ui_map.enabled() && ImGui::IsItemVisible()) ui_map.mark(control, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}
bool EditorUI::begin_panel(const char *title, std::string_view control, std::string_view context, bool *open, ImGuiWindowFlags flags) {
    const bool visible = ImGui::Begin(title, open, flags);
    if (visible && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) hovered_context = context;
    if (visible && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)) focused_context = context;
    if (ui_map.enabled()) {
        const std::string panel(control);
        if (visible) ui_map.mark("Panel." + panel, ImGui::GetWindowPos(), {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y});
        if (ImGui::IsWindowDocked()) ui_map.mark("Tab." + panel, ImGui::GetCurrentWindow()->DC.DockTabItemRect.Min, ImGui::GetCurrentWindow()->DC.DockTabItemRect.Max);
    }
    return visible;
}
bool EditorUI::command_item(const char *id, const char *label) {
    const auto *command = registry.find(id);
    if (!command) return false;
    const auto keys = registry.shortcut(id);
    const bool clicked = ImGui::MenuItem(label ? label : command->info.label.c_str(), keys.empty() ? nullptr : keys.c_str(), registry.checked(id), registry.enabled(id));
    ImGui::SetItemTooltip("%s", registry.tooltip(id).c_str());
    mark(std::string("Item.") + id); // The same command may also be a button on screen.
    if (clicked) registry.run(id);
    return clicked;
}
bool EditorUI::command_button(const char *id, const char *label) {
    const auto *command = registry.find(id);
    if (!command) return false;
    ImGui::BeginDisabled(!registry.enabled(id));
    const bool clicked = ImGui::Button(label ? label : command->info.label.c_str());
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", registry.tooltip(id).c_str());
    mark(id);
    if (clicked) registry.run(id);
    return clicked;
}
bool EditorUI::command_checkbox(const char *id, const char *label) {
    const auto *command = registry.find(id);
    if (!command) return false;
    bool value = registry.checked(id);
    ImGui::BeginDisabled(!registry.enabled(id));
    const bool clicked = ImGui::Checkbox(label ? label : command->info.label.c_str(), &value);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", registry.tooltip(id).c_str());
    mark(id);
    if (clicked) registry.run(id);
    return clicked;
}
void EditorUI::move_selection_to_root() {
    const auto roots = session.selection_roots();
    ScopedTransaction transaction(session, roots.size() == 1 ? "Move to Root" : "Move " + std::to_string(roots.size()) + " Entities to Root");
    for (const auto id : roots) {
        if (!session.scene().inspect(id)->parent) continue;
        const auto moved = session.reparent(id, 0);
        if (!moved) { transaction.cancel(); log("Error: " + moved.error().message); return; }
    }
    log("Moved to root (world transform kept)");
}
void EditorUI::bind_commands() {
    const auto editing = [this] { return !playing(); };
    const auto selected = [this] { return session.selection() != 0; };
    const auto bind = [this](const char *id, std::function<void()> execute, std::function<bool()> can_execute = {}, std::function<bool()> is_checked = {}) {
        if (const auto bound = registry.bind(id, std::move(execute), std::move(can_execute), std::move(is_checked)); !bound) throw std::runtime_error(bound.error().message);
    };
    // ---- File ----
    bind("File.NewScene", [this] { request(Pending::NewScene); }, editing);
    bind("File.OpenScene", [this] {
        if (const auto file = open_file_dialog(window, L"Open Scene", scene_filter)) { set_scene_path(*file); request(Pending::Load); }
    }, editing);
    bind("File.LoadFromPath", [this] { request(Pending::Load); }, editing);
    bind("File.Save", [this] { report(session.save(scene_path.data()), "Scene saved"); }, editing);
    bind("File.SaveAs", [this] {
        if (const auto file = save_file_dialog(window, L"Save Scene As", scene_filter, L"scene")) { set_scene_path(*file); report(session.save(scene_path.data()), "Scene saved"); }
    }, editing);
    bind("File.SaveAll", [this] { report(session.save(scene_path.data()), "Scene saved. Materials save themselves when a control is released."); }, editing);
    bind("File.OpenSample", [this] { request(Pending::OpenSample); }, editing);
    bind("File.NewProject", [this] { project_create = true; project_prompt = true; }, editing);
    bind("File.OpenProject", [this] { project_create = false; project_prompt = true; }, editing);
    bind("File.Quit", [this] { request(Pending::Quit); });
    // ---- Edit ----
    bind("Edit.Undo", [this] { report(session.undo(), "Undo"); }, [this] { return !playing() && session.can_undo() && !session.in_transaction(); });
    bind("Edit.Redo", [this] { report(session.redo(), "Redo"); }, [this] { return !playing() && session.can_redo() && !session.in_transaction(); });
    bind("Edit.Duplicate", [this] { duplicate_selection(); }, selected);
    bind("Edit.Delete", [this] {
        // A selected entity goes first: picking one in the viewport leaves the folder highlighted.
        if (session.selection()) delete_selection();
        else if (selected_folder) { report(session.delete_folder(selected_folder), "Folder deleted; what it held moved up"); selected_folder = 0; }
    }, [this] { return session.selection() != 0 || selected_folder != 0; });
    bind("Edit.Copy", [this] {
        const auto text = session.copy_selection();
        if (report(text, "Copied")) ImGui::SetClipboardText(text.value().c_str());
    }, selected);
    bind("Edit.Paste", [this] {
        const auto *text = ImGui::GetClipboardText();
        report(session.paste(text ? text : ""), "Pasted");
    });
    bind("Edit.Group", [this] { report(session.group_selection(), "Grouped"); }, selected);
    bind("Edit.Rename", [this] {
        if (const auto record = session.scene().inspect(session.selection())) {
            std::snprintf(rename_text.data(), rename_text.size(), "%s", record->name.c_str());
            rename_entity = record->id; rename_focus = true;
            show_hierarchy = true;
        } else if (const auto *folder = session.view_state().folders.find(selected_folder)) {
            std::snprintf(rename_text.data(), rename_text.size(), "%s", folder->name.c_str());
            rename_folder_id = selected_folder; rename_focus = true;
        }
    }, [this] { return session.selection() != 0 || selected_folder != 0; });
    bind("Outliner.NewFolder", [this] {
        const auto made = session.create_folder("New Folder", selected_folder);
        if (report(made, "Folder created")) {
            selected_folder = made.value(); session.select(0);
            std::snprintf(rename_text.data(), rename_text.size(), "%s", "New Folder");
            rename_folder_id = made.value(); rename_focus = true;
        }
    }, editing);
    // ---- Assets ----
    const auto selected_asset_meta = [this]() -> std::optional<assets::AssetMetadata> {
        for (auto &meta : session.assets()) if (meta.id == selected_asset) return meta;
        return std::nullopt;
    };
    const auto entity_asset = [this]() -> AssetRef {
        const auto record = session.scene().inspect(session.selection());
        if (!record || !record->components.mesh) return {};
        const auto &mesh = *record->components.mesh;
        return !mesh.mesh_asset.empty() ? mesh.mesh_asset : !mesh.material.empty() ? mesh.material : mesh.texture;
    };
    bind("Assets.ToggleDrawer", [this] { asset_drawer_open = !asset_drawer_open; asset_drawer_focus = asset_drawer_open; });
    bind("Assets.CloseDrawer", [this] { asset_drawer_open = false; }, [this] { return asset_drawer_open; });
    bind("Assets.FindInBrowser", [this, entity_asset] { reveal_asset(assets::AssetId{entity_asset().id}); }, [entity_asset] { return !entity_asset().empty(); });
    const auto entity_material = [this]() -> AssetRef {
        const auto record = session.scene().inspect(session.selection());
        return record && record->components.mesh ? record->components.mesh->material : AssetRef{};
    };
    bind("Assets.EditAsset", [this, entity_material] {
        selected_asset = assets::AssetId{entity_material().id};
        show_material = true;
        ImGui::SetWindowFocus("Material Editor");
    }, [entity_material] { return !entity_material().empty(); });
    bind("Asset.Assign", [this, selected_asset_meta] {
        const auto meta = selected_asset_meta();
        report(meta->type == assets::AssetType::Material ? session.assign_material(session.selection(), selected_asset) : session.assign_asset(session.selection(), selected_asset), "Asset assigned to the selected entity");
    }, [this, selected_asset_meta] { const auto meta = selected_asset_meta(); return meta && session.selection() && meta->type != assets::AssetType::Environment; });
    bind("Asset.AddToScene", [this] { report(session.place_asset(selected_asset), "Model added to the scene"); }, [selected_asset_meta] { const auto meta = selected_asset_meta(); return meta && meta->type == assets::AssetType::Mesh; });
    bind("Asset.Edit", [this] { show_material = true; ImGui::SetWindowFocus("Material Editor"); }, [selected_asset_meta] { const auto meta = selected_asset_meta(); return meta && meta->type == assets::AssetType::Material; });
    bind("Asset.Reload", [this] {
        report(session.reload_asset(selected_asset), "Asset re-imported from disk");
        thumbnails.erase(selected_asset);
        renderer.forget_thumbnail(selected_asset.value);
    }, [selected_asset_meta] { return selected_asset_meta().has_value(); });
    bind("Asset.Rename", [this, selected_asset_meta] {
        std::snprintf(rename_asset_path.data(), rename_asset_path.size(), "%s", selected_asset_meta()->path.c_str());
        rename_asset_prompt = true;
    }, [this, selected_asset_meta] { return !playing() && selected_asset_meta().has_value(); });
    bind("Asset.ToggleFavorite", [this] {
        settings.set("asset.favorite." + assets::to_string(selected_asset), !asset_is_favorite(selected_asset));
        save_settings();
    }, [selected_asset_meta] { return selected_asset_meta().has_value(); }, [this] { return asset_is_favorite(selected_asset); });
    bind("Edit.MoveToRoot", [this] { move_selection_to_root(); }, [this] {
        return std::ranges::any_of(session.selection_roots(), [this](EntityId id) { return session.scene().inspect(id)->parent != 0; });
    });
    bind("Edit.SelectAll", [this] { session.select_all(); }, [this] { return session.scene().size() != 0; });
    bind("Edit.Deselect", [this] { session.select(0); }, selected);
    bind("Edit.SnapToFloor", [this] { report(session.snap_selection_to_floor(*frame), "Snapped to the floor"); }, [this] { return session.selection() != 0 && frame != nullptr; });
    bind("Edit.Hide", [this] { session.hide_selection(); log("Hidden in the editor. Ctrl+H shows everything again."); }, [this] { return session.selection() != 0 && !playing(); });
    bind("Edit.UnhideAll", [this] { session.unhide_all(); log("Everything is shown again"); }, [this] { return !session.view_state().hidden.empty() && !playing(); });
    // ---- Create ----
    bind("Create.Cube", [this] { spawn("Cube", {MeshComponent{BuiltinMesh::Cube}, {}, {}}); });
    bind("Create.Sphere", [this] { spawn("Sphere", {MeshComponent{BuiltinMesh::Sphere}, {}, {}}); });
    bind("Create.Plane", [this] { spawn("Plane", {MeshComponent{BuiltinMesh::Plane}, {}, {}}, {{0,0,0}, {}, {10,1,10}}); });
    bind("Create.Empty", [this] { spawn("Empty", {}); });
    bind("Create.DirectionalLight", [this] { spawn("Directional Light", {{}, {}, LightComponent{}}, aimed({0,4,0}, -50, 30)); });
    // Sized for the scene's exposure: a real bulb's output indoors, far more under daylight exposure.
    bind("Create.PointLight", [this] { spawn("Point Light", {{}, {}, point_light()}, {{0,2,0}, {}, {1,1,1}}); });
    bind("Create.SpotLight", [this] { auto l = point_light(); l.type = LightType::Spot; l.cast_shadows = true; spawn("Spot Light", {{}, {}, l}, aimed({0,4,0}, -90, 0)); });
    bind("Create.Camera", [this] { spawn("Camera", {{}, CameraComponent{}, {}}, aimed({0,1.5,6}, -8, 0)); });
    bind("Create.FallingCube", [this] { spawn("Falling Cube", physical(BuiltinMesh::Cube), {{0,3,0}, {}, {1,1,1}}); });
    bind("Create.FallingSphere", [this] { spawn("Falling Sphere", physical(BuiltinMesh::Sphere), {{0,3,0}, {}, {1,1,1}}); });
    bind("Create.Floor", [this] {
        Components floor{MeshComponent{BuiltinMesh::Plane}, {}, {}};
        floor.colliders = {fitted_collider(BuiltinMesh::Plane)};
        spawn("Floor", std::move(floor), {{0,0,0}, {}, {10,1,10}});
    });
    const Transform above{{0,1,0}, {}, {1,1,1}};
    bind("Create.ChildEmpty", [this, above] { spawn("Empty", {}, above, session.selection()); }, selected);
    bind("Create.ChildCube", [this, above] { spawn("Cube", {MeshComponent{BuiltinMesh::Cube}, {}, {}}, above, session.selection()); }, selected);
    bind("Create.ChildSphere", [this, above] { spawn("Sphere", {MeshComponent{BuiltinMesh::Sphere}, {}, {}}, above, session.selection()); }, selected);
    bind("Create.ChildPointLight", [this, above] { spawn("Point Light", {{}, {}, point_light()}, above, session.selection()); }, selected);
    // ---- Viewport ----
    bind("Viewport.FocusSelected", [this] { if (const auto goal = session.focus_goal()) camera_control.animate_to(session.camera, *goal); }, selected);
    const auto toggle = [this, &bind](const char *id, bool &flag) {
        bind(id, [this, &flag] { flag = !flag; save_settings(); }, {}, [&flag] { return flag; });
    };
    toggle("Viewport.ToggleColliders", show_colliders);
    toggle("Viewport.ShowGrid", show_grid);
    toggle("Viewport.ShowMarkers", show_markers);
    toggle("Viewport.ShowSkybox", session.view_flags.skybox);
    toggle("Viewport.ShowShadows", session.view_flags.shadows);
    toggle("Viewport.ShowAmbientOcclusion", session.view_flags.ambient_occlusion);
    toggle("Viewport.ShowBloom", session.view_flags.bloom);
    bind("Viewport.GameView", [this] { game_view = !game_view; }, {}, [this] { return game_view; });
    bind("Viewport.Maximize", [this] { viewport_maximized = !viewport_maximized; }, {}, [this] { return viewport_maximized; });
    constexpr const char *views[] = {"Viewport.Perspective", "Viewport.Front", "Viewport.Top", "Viewport.Side"};
    for (int kind = 0; kind < 4; ++kind) bind(views[kind], [this, kind] { set_view(kind); }, {}, [this, kind] { return view_kind == kind; });
    // The renderer has one shading mode for now, so these two wait, greyed out, with their keys reserved.
    bind("Viewport.Wireframe", [] {}, [] { return false; });
    bind("Viewport.Unlit", [] {}, [] { return false; });
    bind("Viewport.Lit", [] {}, {}, [] { return true; });
    // ---- Transform ----
    constexpr const char *gizmos[] = {"Gizmo.Move", "Gizmo.Rotate", "Gizmo.Scale"};
    for (int kind = 0; kind < 3; ++kind) bind(gizmos[kind], [this, kind] { operation = kind; }, {}, [this, kind] { return operation == kind; });
    bind("Gizmo.Cycle", [this] { operation = (operation + 1) % 3; });
    bind("Gizmo.ToggleSpace", [this] { coordinates = 1 - coordinates; });
    toggle("Snap.ToggleGrid", snap.grid);
    toggle("Snap.ToggleRotation", snap.rotation);
    toggle("Snap.ToggleScale", snap.scale);
    bind("Snap.GridSmaller", [this] { step_grid(-1); });
    bind("Snap.GridLarger", [this] { step_grid(1); });
    // ---- Camera: the keys are read while flying, never dispatched ----
    for (const auto *id : {"Camera.FlyForward", "Camera.FlyBack", "Camera.FlyLeft", "Camera.FlyRight", "Camera.FlyDown", "Camera.FlyUp", "Camera.FovOut", "Camera.FovIn"}) bind(id, [] {});
    // ---- Bookmarks ----
    for (std::size_t slot = 0; slot < 10; ++slot) {
        const auto number = std::to_string(slot);
        bind(("Bookmark.Set" + number).c_str(), [this, slot] { session.set_bookmark(slot); log("Bookmark " + std::to_string(slot) + " set"); });
        bind(("Bookmark.Go" + number).c_str(), [this, slot] { if (const auto goal = session.bookmark(slot)) { camera_control.animate_to(session.camera, *goal); view_kind = goal->orthographic ? view_kind : 0; } }, [this, slot] { return session.bookmark(slot).has_value(); });
    }
    // ---- Play ----
    bind("Play.Play", [this] { report(session.play(), "Play mode: running a copy of the scene; changes are discarded on Stop"); }, editing);
    bind("Play.Resume", [this] { report(session.play(), "Resumed"); }, [this] { return session.play_state() == PlayState::Paused; });
    bind("Play.Pause", [this] { report(session.pause(), "Paused"); }, [this] { return session.play_state() == PlayState::Playing; });
    bind("Play.Step", [this] { report(session.step(), "Stepped one tick"); }, [this] { return session.play_state() == PlayState::Paused; });
    bind("Play.Stop", [this] { stop_play(); }, [this] { return playing(); });
    // ---- Window ----
    bind("Window.UndoHistory", [this] { show_history = !show_history; }, {}, [this] { return show_history; });
    bind("Window.KeyboardShortcuts", [this] { show_shortcuts = !show_shortcuts; }, {}, [this] { return show_shortcuts; });
    const auto panel = [&bind](const char *id, bool &flag) { bind(id, [&flag] { flag = !flag; }, {}, [&flag] { return flag; }); };
    panel("Window.Toolbar", show_toolbar);
    panel("Window.Hierarchy", show_hierarchy);
    panel("Window.Inspector", show_inspector);
    panel("Window.Lighting", show_lighting);
    panel("Window.MaterialEditor", show_material);
    panel("Window.AssetBrowser", show_assets);
    panel("Window.Console", show_console);
    bind("Window.SaveLayout", [this] { layout_prompt = true; }, [this] { return !config_folder.empty(); });
    bind("Window.ResetLayout", [this] {
        show_toolbar = show_hierarchy = show_inspector = show_lighting = show_material = show_assets = show_console = true;
        layout_reset_requested = true;
    });
}
void EditorUI::load_keymap() {
    if (config_folder.empty()) return;
    const auto file = config_folder / "keymap.ini";
    std::error_code error;
    if (!std::filesystem::exists(file, error)) return;
    const auto text = read_text_file(file);
    if (!text) { log("Error: " + text.error().message); return; }
    const auto loaded = registry.load_keymap(text.value());
    if (!loaded) { log("Error: " + loaded.error().message + ". Using the default shortcuts."); return; }
    for (const auto &id : loaded.value().unknown) log("Keymap: no command called " + id);
}
void EditorUI::save_keymap() {
    if (config_folder.empty()) return;
    if (const auto saved = write_text_file(config_folder / "keymap.ini", registry.save_keymap()); !saved) log("Error: " + saved.error().message);
}
void EditorUI::route_shortcuts(bool rebinding_at_frame_start) {
    // A panel's own close button sets its flag directly, so changes are noticed here.
    std::string panels;
    for (const bool *flag : panel_flags()) panels += *flag ? '1' : '0';
    if (panels != saved_panels) save_settings();
    const auto &io = ImGui::GetIO();
    InputBlock block;
    block.text_input = io.WantTextInput;
    block.modal = popup_before_frame || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    block.captured = game_input.captured;
    block.interacting = gizmo_active || ImGui::IsAnyItemActive() || camera_control.holds_cursor();
    block.rebinding = rebinding_at_frame_start || !rebind_command.empty();
    if (block.any()) return;
    // The viewport answers while the mouse is over it; other panels when they have the focus.
    std::array<std::string_view, 2> stack{};
    std::size_t depth = 0;
    if (hovered_context == context::viewport) stack[depth++] = context::viewport;
    if (!focused_context.empty()) stack[depth++] = focused_context;
    const auto contexts = std::span<const std::string_view>(stack.data(), depth);
    const auto modifiers = held_modifiers();
    for (const auto &[key, imgui] : key_pairs()) {
        if (!ImGui::IsKeyPressed(imgui, true)) continue;
        const KeyChord chord{key, modifiers};
        const auto *command = registry.resolve(chord, contexts);
        if (!command || (!ImGui::IsKeyPressed(imgui, false) && !command->info.repeats)) continue;
        registry.dispatch(chord, contexts, block);
    }
}
void EditorUI::main_menu() {
    if (!ImGui::BeginMainMenuBar()) return;
    const bool open_file = ImGui::BeginMenu("File");
    mark("Menu.File");
    if (open_file) {
        for (const auto *id : {"File.NewScene", "File.OpenScene", "File.Save", "File.SaveAs", "File.SaveAll"}) command_item(id);
        ImGui::Separator();
        for (const auto *id : {"File.OpenSample", "File.NewProject", "File.OpenProject"}) command_item(id);
        ImGui::Separator();
        command_item("File.Quit");
        ImGui::EndMenu();
    }
    const bool open_edit = ImGui::BeginMenu("Edit");
    mark("Menu.Edit");
    if (open_edit) {
        const auto names = session.history_names();
        const auto applied = session.history_position();
        const auto undo_label = applied ? "Undo " + names[applied - 1] : std::string("Undo");
        const auto redo_label = applied < names.size() ? "Redo " + names[applied] : std::string("Redo");
        command_item("Edit.Undo", undo_label.c_str());
        command_item("Edit.Redo", redo_label.c_str());
        ImGui::Separator();
        for (const auto *id : {"Edit.Copy", "Edit.Paste", "Edit.Duplicate", "Edit.Delete", "Edit.Rename", "Edit.Group", "Edit.MoveToRoot", "Edit.SnapToFloor"}) command_item(id);
        ImGui::Separator();
        for (const auto *id : {"Edit.SelectAll", "Edit.Deselect"}) command_item(id);
        ImGui::Separator();
        for (const auto *id : {"Edit.Hide", "Edit.UnhideAll"}) command_item(id);
        ImGui::EndMenu();
    }
    const bool open_create = ImGui::BeginMenu("Create");
    mark("Menu.Create");
    if (open_create) { create_items(); ImGui::EndMenu(); }
    const bool open_view = ImGui::BeginMenu("View");
    mark("Menu.View");
    if (open_view) {
        for (const auto *id : {"Viewport.FocusSelected", "Viewport.GameView", "Viewport.Maximize"}) command_item(id);
        ImGui::Separator();
        for (const auto *id : {"Viewport.Perspective", "Viewport.Front", "Viewport.Top", "Viewport.Side"}) command_item(id);
        ImGui::Separator();
        for (const auto *id : {"Viewport.Lit", "Viewport.Wireframe", "Viewport.Unlit"}) command_item(id);
        ImGui::Separator();
        for (const auto *id : {"Viewport.ShowGrid", "Viewport.ToggleColliders", "Viewport.ShowMarkers", "Viewport.ShowSkybox", "Viewport.ShowShadows", "Viewport.ShowAmbientOcclusion", "Viewport.ShowBloom"}) command_item(id);
        if (ImGui::BeginMenu("Bookmarks")) {
            for (int slot = 0; slot < 10; ++slot) command_item(("Bookmark.Go" + std::to_string(slot)).c_str());
            ImGui::Separator();
            for (int slot = 0; slot < 10; ++slot) command_item(("Bookmark.Set" + std::to_string(slot)).c_str());
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    const bool open_transform = ImGui::BeginMenu("Transform");
    mark("Menu.Transform");
    if (open_transform) {
        for (const auto *id : {"Gizmo.Move", "Gizmo.Rotate", "Gizmo.Scale", "Gizmo.Cycle", "Gizmo.ToggleSpace"}) command_item(id);
        ImGui::Separator();
        for (const auto *id : {"Snap.ToggleGrid", "Snap.ToggleRotation", "Snap.ToggleScale", "Snap.GridSmaller", "Snap.GridLarger"}) command_item(id);
        ImGui::EndMenu();
    }
    const bool open_play = ImGui::BeginMenu("Play");
    mark("Menu.Play");
    if (open_play) {
        for (const auto *id : {"Play.Play", "Play.Resume", "Play.Pause", "Play.Step", "Play.Stop"}) command_item(id);
        ImGui::EndMenu();
    }
    const bool open_window = ImGui::BeginMenu("Window");
    mark("Menu.Window");
    if (open_window) {
        for (const auto *id : {"Window.Toolbar", "Window.Hierarchy", "Window.Inspector", "Window.Lighting", "Window.MaterialEditor", "Window.AssetBrowser", "Window.Console", "Window.UndoHistory", "Window.KeyboardShortcuts"}) command_item(id);
        ImGui::Separator();
        command_item("Assets.ToggleDrawer");
        ImGui::Separator();
        if (ImGui::BeginMenu("Layouts")) {
            const auto layouts = saved_layouts();
            if (layouts.empty()) ImGui::TextDisabled("None saved yet");
            for (const auto &saved : layouts) {
                if (ImGui::MenuItem(saved.c_str())) {
                    if (const auto text = read_text_file(config_folder / "layouts" / utf8_path(saved + ".ini"))) pending_layout = text.value();
                }
                mark("Layout." + saved);
            }
            ImGui::EndMenu();
        }
        mark("Menu.Layouts");
        for (const auto *id : {"Window.SaveLayout", "Window.ResetLayout"}) command_item(id);
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}
void EditorUI::history_panel() {
    if (!show_history) return;
    ImGui::SetNextWindowSize({px(320), px(380)}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    if (!begin_panel("Undo History", "Undo History", context::global, &show_history)) { ImGui::End(); return; }
    if (playing()) ImGui::TextColored({0.45f, 0.85f, 0.45f, 1}, "Play mode keeps no history; these are the editor scene's edits.");
    ImGui::BeginDisabled(playing());
    const auto names = session.history_names();
    const auto applied = session.history_position();
    std::optional<std::size_t> jump;
    if (ImGui::Selectable("(Start)", applied == 0)) jump = 0;
    mark("History.0");
    for (std::size_t i = 0; i < names.size(); ++i) {
        ImGui::PushID(int(i));
        // Applied steps read normally; the ones past the current point can still be redone.
        if (i >= applied) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(names[i].c_str(), i + 1 == applied)) jump = i + 1;
        if (i >= applied) ImGui::PopStyleColor();
        mark("History." + std::to_string(i + 1));
        if (i + 1 == applied && history_scrolled != applied) { ImGui::SetScrollHereY(); history_scrolled = applied; }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (jump) report(session.jump_history(*jump), "Jumped in the history");
    ImGui::End();
}
void EditorUI::shortcuts_panel() {
    if (!show_shortcuts) { rebind_command.clear(); return; }
    ImGui::SetNextWindowSize({px(640), px(560)}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    if (!begin_panel("Keyboard Shortcuts", "Keyboard Shortcuts", context::global, &show_shortcuts)) { ImGui::End(); return; }
    ImGui::SetNextItemWidth(px(260));
    ImGui::InputTextWithHint("##search", "Search commands or keys", shortcut_search.data(), shortcut_search.size());
    mark("Shortcuts.Search");
    ImGui::SameLine();
    if (ImGui::Button("Import...")) {
        if (const auto file = open_file_dialog(window, L"Import Keymap", keymap_filter)) {
            const auto text = read_text_file(*file);
            const auto loaded = text ? registry.load_keymap(text.value()) : Result<KeymapReport>(text.error());
            if (report(loaded, "Keymap imported")) save_keymap();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Export...")) {
        if (const auto file = save_file_dialog(window, L"Export Keymap", keymap_filter, L"ini")) report(write_text_file(*file, registry.save_keymap()), "Keymap exported");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset All")) { registry.reset_all_chords(); save_keymap(); log("All shortcuts reset"); }
    mark("Shortcuts.ResetAll");
    // ---- Waiting for keys ----
    if (!rebind_command.empty()) {
        const auto *target = registry.find(rebind_command);
        if (!rebind_chord.empty()) {
            ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "%s is also used by:", to_string(rebind_chord).c_str());
            for (const auto *other : registry.conflicts(rebind_command, rebind_chord)) ImGui::BulletText("%s (%s)", other->info.label.c_str(), other->info.context.c_str());
            if (ImGui::Button("Replace")) {
                for (const auto *other : registry.conflicts(rebind_command, rebind_chord)) {
                    auto kept = other->chords;
                    std::erase(kept, rebind_chord);
                    (void)registry.set_chords(other->info.id, std::move(kept));
                }
                finish_rebind();
            }
            mark("Shortcuts.Replace");
            ImGui::SameLine();
            if (ImGui::Button("Keep both")) finish_rebind();
            mark("Shortcuts.KeepBoth");
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) { rebind_command.clear(); rebind_chord = {}; }
            mark("Shortcuts.CancelRebind");
        } else {
            ImGui::TextColored({0.45f, 0.75f, 0.95f, 1}, "Press the keys for '%s'. Esc cancels.", target ? target->info.label.c_str() : "");
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) rebind_command.clear();
            else for (const auto &[key, imgui] : key_pairs())
                if (ImGui::IsKeyPressed(imgui, false)) {
                    rebind_chord = {key, held_modifiers()};
                    if (registry.conflicts(rebind_command, rebind_chord).empty()) finish_rebind();
                    break;
                }
        }
    }
    ImGui::Separator();
    const auto search_for = std::string_view(shortcut_search.data());
    const auto matches = [&](const EditorCommand &command) {
        const auto contains = [&](std::string_view text) {
            return std::ranges::search(text, search_for, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }).begin() != text.end() || search_for.empty();
        };
        if (contains(command.info.label) || contains(command.info.id) || contains(command.info.category)) return true;
        return std::ranges::any_of(command.chords, [&](KeyChord chord) { return contains(to_string(chord)); });
    };
    ImGui::BeginChild("##commands");
    std::vector<std::string> categories;
    for (const auto &command : registry.commands())
        if (std::ranges::find(categories, command.info.category) == categories.end()) categories.push_back(command.info.category);
    for (const auto &category : categories) {
        if (!std::ranges::any_of(registry.commands(), [&](const EditorCommand &c) { return c.info.category == category && matches(c); })) continue;
        ImGui::SeparatorText(category.c_str());
        if (!ImGui::BeginTable(category.c_str(), 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) continue;
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("Context", ImGuiTableColumnFlags_WidthStretch, 0.14f);
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch, 0.44f);
        for (const auto &command : registry.commands()) {
            if (command.info.category != category || !matches(command)) continue;
            const auto &id = command.info.id;
            ImGui::PushID(id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(command.info.label.c_str());
            ImGui::SetItemTooltip("%s\n%s", id.c_str(), command.info.tooltip.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", command.info.context.c_str());
            ImGui::TableNextColumn();
            auto chords = command.chords;
            std::optional<std::size_t> removed;
            for (std::size_t i = 0; i < chords.size(); ++i) {
                ImGui::PushID(int(i));
                if (i) ImGui::SameLine();
                const bool waiting = rebind_command == id && rebind_index == int(i);
                if (ImGui::SmallButton(waiting ? "..." : to_string(chords[i]).c_str())) begin_rebind(id, int(i));
                ImGui::SetItemTooltip("Click to change. Right-click to remove.");
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) removed = i;
                mark("Keys." + id + "." + std::to_string(i));
                if (!registry.conflicts(id, chords[i]).empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "!");
                    std::string list = "Also used by:";
                    for (const auto *other : registry.conflicts(id, chords[i])) list += "\n" + other->info.label + " (" + other->info.context + ")";
                    ImGui::SetItemTooltip("%s", list.c_str());
                }
                ImGui::PopID();
            }
            if (!chords.empty()) ImGui::SameLine();
            if (ImGui::SmallButton(rebind_command == id && rebind_index < 0 ? "..." : "+")) begin_rebind(id, -1);
            ImGui::SetItemTooltip("Add a shortcut");
            mark("Keys." + id + ".add");
            if (registry.overridden(id)) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset")) { (void)registry.reset_chords(id); save_keymap(); }
                ImGui::SetItemTooltip("Back to the default");
                mark("Keys." + id + ".reset");
            }
            if (removed) { chords.erase(chords.begin() + std::ptrdiff_t(*removed)); (void)registry.set_chords(id, std::move(chords)); save_keymap(); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Mouse");
    ImGui::TextDisabled("These cannot be changed yet.");
    if (ImGui::BeginTable("##gestures", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        for (const auto &gesture : mouse_gestures()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(gesture.input.data(), gesture.input.data() + gesture.input.size());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(gesture.action.data(), gesture.action.data() + gesture.action.size());
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::End();
}
bool EditorUI::key_held(const char *id) const {
    const auto *command = registry.find(id);
    if (!command) return false;
    // Modifiers do not matter here: Shift while flying only makes it faster.
    for (const auto chord : command->chords)
        for (const auto &[key, imgui] : key_pairs())
            if (key == chord.key && ImGui::IsKeyDown(imgui)) return true;
    return false;
}
void EditorUI::load_settings() {
    if (config_folder.empty()) return;
    if (const auto text = read_text_file(config_folder / "editor.ini")) settings.parse(text.value());
    camera_control.set_speed(settings.number("camera.speed", camera_control.speed()));
    saved_speed = float(camera_control.speed());
    snap.grid = settings.flag("snap.grid", snap.grid);
    snap.rotation = settings.flag("snap.rotation", snap.rotation);
    snap.scale = settings.flag("snap.scale", snap.scale);
    snap.grid_size = std::max(settings.number("snap.grid_size", snap.grid_size), 0.001);
    snap.rotation_step = std::max(settings.number("snap.rotation_step", snap.rotation_step), 0.001);
    snap.scale_step = std::max(settings.number("snap.scale_step", snap.scale_step), 0.001);
    show_colliders = settings.flag("view.colliders", show_colliders);
    show_grid = settings.flag("view.grid", show_grid);
    show_markers = settings.flag("view.markers", show_markers);
    session.view_flags.skybox = settings.flag("view.skybox", session.view_flags.skybox);
    session.view_flags.shadows = settings.flag("view.shadows", session.view_flags.shadows);
    session.view_flags.ambient_occlusion = settings.flag("view.ambient_occlusion", session.view_flags.ambient_occlusion);
    session.view_flags.bloom = settings.flag("view.bloom", session.view_flags.bloom);
    constexpr const char *panel_keys[] = {"toolbar", "hierarchy", "inspector", "lighting", "material", "assets", "console"};
    const auto flags = panel_flags();
    for (std::size_t i = 0; i < flags.size(); ++i) *flags[i] = settings.flag(std::string("panel.") + panel_keys[i], *flags[i]);
    saved_panels.clear();
    for (const bool *flag : flags) saved_panels += *flag ? '1' : '0';
}
void EditorUI::save_settings() {
    if (config_folder.empty()) return;
    settings.set("camera.speed", camera_control.speed());
    saved_speed = float(camera_control.speed());
    settings.set("snap.grid", snap.grid);
    settings.set("snap.rotation", snap.rotation);
    settings.set("snap.scale", snap.scale);
    settings.set("snap.grid_size", snap.grid_size);
    settings.set("snap.rotation_step", snap.rotation_step);
    settings.set("snap.scale_step", snap.scale_step);
    settings.set("view.colliders", show_colliders);
    settings.set("view.grid", show_grid);
    settings.set("view.markers", show_markers);
    settings.set("view.skybox", session.view_flags.skybox);
    settings.set("view.shadows", session.view_flags.shadows);
    settings.set("view.ambient_occlusion", session.view_flags.ambient_occlusion);
    settings.set("view.bloom", session.view_flags.bloom);
    constexpr const char *panel_keys[] = {"toolbar", "hierarchy", "inspector", "lighting", "material", "assets", "console"};
    const auto flags = panel_flags();
    saved_panels.clear();
    for (std::size_t i = 0; i < flags.size(); ++i) {
        settings.set(std::string("panel.") + panel_keys[i], *flags[i]);
        saved_panels += *flags[i] ? '1' : '0';
    }
    if (const auto saved = write_text_file(config_folder / "editor.ini", settings.text()); !saved) log("Error: " + saved.error().message);
}
std::string EditorUI::log_fields() const {
    const auto &c = session.camera;
    return " mode=" + std::to_string(int(camera_control.mode())) + " yaw=" + std::to_string(c.yaw) + " pitch=" + std::to_string(c.pitch) + " distance=" + std::to_string(c.distance) +
           " fov=" + std::to_string(c.fov_degrees) + " ortho=" + std::to_string(c.orthographic ? 1 : 0) + " speed=" + std::to_string(camera_control.speed()) +
           " operation=" + std::to_string(operation) + " space=" + std::to_string(coordinates) + " hidden=" + std::to_string(session.view_state().hidden.size()) +
           " game_view=" + std::to_string(game_view ? 1 : 0) + " maximized=" + std::to_string(viewport_maximized ? 1 : 0) + " grid_size=" + std::to_string(snap.grid_size) +
           " snap_grid=" + std::to_string(snap.grid ? 1 : 0) + " folders=" + std::to_string(session.view_state().folders.folders.size()) +
           " members=" + std::to_string(session.view_state().folders.members.size()) + " locked=" + std::to_string(session.view_state().locked.size()) +
           " drawer=" + std::to_string(asset_drawer_open ? 1 : 0) + " details_targets=" + std::to_string(details_targets.size()) + " details_locked=" + std::to_string(details_locked ? 1 : 0) +
           " thumbnails=" + std::to_string(std::ranges::count_if(thumbnails, [this](const auto &entry) { return renderer.thumbnail(entry.first.value).texture != 0; })) +
           " selected_asset=" + assets::to_string(selected_asset);
}
std::vector<std::string> EditorUI::saved_layouts() const {
    std::vector<std::string> names;
    std::error_code error;
    for (std::filesystem::directory_iterator it(config_folder / "layouts", error), end; !error && it != end; it.increment(error))
        if (it->path().extension() == ".ini") {
            const auto stem = it->path().stem().u8string();
            names.emplace_back(reinterpret_cast<const char *>(stem.data()), stem.size());
        }
    std::ranges::sort(names);
    return names;
}
std::array<bool *, 7> EditorUI::panel_flags() { return {&show_toolbar, &show_hierarchy, &show_inspector, &show_lighting, &show_material, &show_assets, &show_console}; }
void EditorUI::layout_dialog() {
    if (std::exchange(layout_prompt, false)) ImGui::OpenPopup("Save layout");
    if (!ImGui::BeginPopupModal("Save layout", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("A name for this arrangement of panels:");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(px(280));
    const bool entered = ImGui::InputText("##layout", layout_name.data(), layout_name.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    mark("Layout.Name");
    if ((ImGui::Button("Save") || entered) && layout_name[0]) {
        std::size_t size = 0;
        const char *text = ImGui::SaveIniSettingsToMemory(&size);
        // ImGui's settings say where windows are, not which are open, so that goes first, on a line ImGui skips.
        std::string open = "; panels ";
        for (const bool *flag : panel_flags()) open += *flag ? '1' : '0';
        report(write_text_file(config_folder / "layouts" / utf8_path(std::string(layout_name.data()) + ".ini"), open + "\n" + std::string(text, size)), "Layout saved");
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
void EditorUI::apply_pending_layout() {
    if (pending_layout.empty()) return;
    // Between frames: ImGui takes settings only then, and the docks must be rebuilt from them.
    constexpr std::string_view panels_line = "; panels ";
    if (std::string_view(pending_layout).starts_with(panels_line)) {
        const auto flags = panel_flags();
        for (std::size_t i = 0; i < flags.size() && panels_line.size() + i < pending_layout.size(); ++i) {
            const char state = pending_layout[panels_line.size() + i];
            if (state == '0' || state == '1') *flags[i] = state == '1';
        }
    }
    ImGui::DockContextClearNodes(ImGui::GetCurrentContext(), 0, true);
    ImGui::LoadIniSettingsFromMemory(pending_layout.c_str(), pending_layout.size());
    pending_layout.clear();
    save_settings();
}
void EditorUI::begin_rebind(const std::string &id, int index) {
    rebind_command = id; rebind_index = index; rebind_chord = {};
}
void EditorUI::finish_rebind() {
    if (const auto *command = registry.find(rebind_command)) {
        auto chords = command->chords;
        std::erase(chords, rebind_chord);
        // Replacing one binding keeps its place in the list, which decides what menus show.
        const auto at = rebind_index >= 0 && std::size_t(rebind_index) <= chords.size() ? chords.begin() + rebind_index : chords.end();
        chords.insert(at, rebind_chord);
        if (rebind_index >= 0 && std::size_t(rebind_index) < command->chords.size() && command->chords[std::size_t(rebind_index)] != rebind_chord) std::erase(chords, command->chords[std::size_t(rebind_index)]);
        (void)registry.set_chords(rebind_command, std::move(chords));
        save_keymap();
    }
    rebind_command.clear(); rebind_chord = {};
}
} // namespace engine::editor
