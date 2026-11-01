#include "EditorUI.hpp"
#include <engine/editor/EditorPrefs.hpp>
#include <glm/gtc/quaternion.hpp>
#include <stdexcept>
#include <filesystem>
#include <algorithm>
#include <utility>

namespace engine::editor {
void UiMap::flush() {
    if (!enabled()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_write_ < std::chrono::milliseconds(50)) return;
    std::string text;
    for (const auto &[name, r] : items_)
        text += name + '\t' + std::to_string(r[0]) + '\t' + std::to_string(r[1]) + '\t' + std::to_string(r[2]) + '\t' + std::to_string(r[3]) + '\n';
    if (text == written_) return;
    last_write_ = now;
    if (write_text_file(file_, text)) written_ = std::move(text);
}
EditorUI::EditorUI(EditorSession &s, renderer::DesktopRenderer &r, GameInput &g, GLFWwindow *w) : session(s), renderer(r), game_input(g), window(w) {
    std::snprintf(scene_path.data(), scene_path.size(), "%s", "Untitled.scene");
    std::snprintf(new_material_name.data(), new_material_name.size(), "%s", "New Material");
    for (auto &info : default_command_table())
        if (const auto added = registry.add(std::move(info)); !added) throw std::runtime_error(added.error().message);
    bind_commands();
    bind_console();
    if (const auto missing = registry.unbound(); !missing.empty()) throw std::runtime_error("Command with no behavior: " + missing.front());
}
void EditorUI::log(std::string text) { if (messages.size() == 512) messages.erase(messages.begin()); messages.push_back(std::move(text)); scroll_console = true; }
void EditorUI::spawn(const char *label, Components c, Transform t, EntityId parent) {
    auto r = session.execute(commands::CreateEntity{label, t, parent, 0, std::move(c)}, std::string("Create ") + label);
    if (report(r, (std::string("Created ") + label).c_str())) session.select(r.value().created.front());
}
Transform EditorUI::aimed(Vec3 position, double pitch_degrees, double yaw_degrees) {
    const glm::dquat q(glm::radians(glm::dvec3(pitch_degrees, yaw_degrees, 0)));
    return {position, {q.x, q.y, q.z, q.w}, {1,1,1}};
}
ColliderComponent EditorUI::fitted_collider(BuiltinMesh mesh) {
    ColliderComponent k;
    if (mesh == BuiltinMesh::Sphere) k.shape = ColliderShape::Sphere;
    if (mesh == BuiltinMesh::Plane) { k.half_extents = {0.5, 0.05, 0.5}; k.center = {0, -0.05, 0}; }
    return k;
}
ColliderComponent EditorUI::fitted_collider(const std::optional<MeshComponent> &mesh) {
    if (!mesh) return fitted_collider(BuiltinMesh::Cube);
    if (const auto fitted = session.model_collider(mesh->mesh_asset)) return *fitted;
    return fitted_collider(mesh->mesh_asset.empty() ? mesh->mesh : BuiltinMesh::Cube);
}
Components EditorUI::physical(BuiltinMesh mesh) {
    Components c{MeshComponent{mesh}, {}, {}};
    c.body = RigidBodyComponent{};
    c.colliders = {fitted_collider(mesh)};
    return c;
}
void EditorUI::create_items() {
    for (const auto *id : {"Create.Cube", "Create.Sphere", "Create.Plane", "Create.Empty"}) command_item(id);
    ImGui::Separator();
    for (const auto *id : {"Create.DirectionalLight", "Create.PointLight", "Create.SpotLight", "Create.Camera"}) command_item(id);
    ImGui::Separator();
    for (const auto *id : {"Create.FallingCube", "Create.FallingSphere", "Create.Floor"}) command_item(id);
}
void EditorUI::create_menu() {
    if (!ImGui::BeginPopup("Create")) return;
    create_items();
    ImGui::EndPopup();
}
std::string EditorUI::title() const {
    std::string t = "GameEngine Editor - " + std::filesystem::path(scene_path.data()).filename().string();
    if (session.dirty()) t += '*';
    if (session.play_state() == editor::PlayState::Playing) t += " [Playing]";
    if (session.play_state() == editor::PlayState::Paused) t += " [Paused]";
    return t;
}
LightComponent EditorUI::point_light() const { return LightComponent{LightType::Point, {1,1,1}, editor::EditorSession::point_light_lumens(session.environment().ev100), 10}; }
void EditorUI::set_scene_path(const std::filesystem::path &path) { std::snprintf(scene_path.data(), scene_path.size(), "%s", path.string().c_str()); }
void EditorUI::open_sample() {
    if (!report(session.open_project(sample_project), "Opened the Physics Shooter sample project")) return;
    set_scene_path(sample_project / sample_scene);
    report(session.load(scene_path.data()), "Scene loaded. Press Play, then click the view to start.");
}
void EditorUI::create_project(const std::filesystem::path &folder) {
    std::error_code error;
    std::filesystem::create_directories(folder / "Assets", error);
    if (error) { log("Error: Cannot create the project folder: " + error.message()); return; }
    if (!report(session.open_project(folder), "Created an empty project")) return;
    if (!report(session.new_scene(), "New scene")) return;
    set_scene_path(folder / "main.scene");
    report(session.save(scene_path.data()), "Scene saved");
}
void EditorUI::open_project(const std::filesystem::path &folder) {
    if (!report(session.open_project(folder), "Project opened")) return;
    set_scene_path(folder / "main.scene");
    std::error_code error;
    if (std::filesystem::is_regular_file(scene_path.data(), error)) report(session.load(scene_path.data()), "Scene loaded (undoable)");
}
void EditorUI::perform(Pending action) {
    if (action == Pending::NewScene && report(session.new_scene(), "New scene"))
        std::snprintf(scene_path.data(), scene_path.size(), "%s", "Untitled.scene");
    if (action == Pending::Load) report(session.load(scene_path.data()), "Scene loaded (undoable)");
    if (action == Pending::OpenSample) open_sample();
    if (action == Pending::NewProject) create_project(utf8_path(project_path.data()));
    if (action == Pending::OpenProject) open_project(utf8_path(project_path.data()));
    if (action == Pending::Quit) quit = true;
}
void EditorUI::project_dialog() {
    if (std::exchange(project_prompt, false)) ImGui::OpenPopup("Project folder");
    if (!ImGui::BeginPopupModal("Project folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(project_create ? "Create an empty project in this folder:" : "Open the project in this folder:");
    // The field takes the keyboard at once, and Enter accepts: a dialog with one question should not need the mouse.
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(px(480));
    const bool entered = ImGui::InputText("##project", project_path.data(), project_path.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::BeginDisabled(!project_path[0]);
    if ((ImGui::Button(project_create ? "Create" : "Open") || entered) && project_path[0]) { ImGui::CloseCurrentPopup(); request(project_create ? Pending::NewProject : Pending::OpenProject); }
    ImGui::EndDisabled();
    ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
void EditorUI::request(Pending action) {
    if (action == Pending::Quit && playing()) report(session.stop(), "Stopped Play mode; runtime copy discarded");
    if (action != Pending::Quit && playing()) { log("Error: Stop Play mode first"); return; }
    if (!session.dirty()) { perform(action); return; }
    pending = action; confirm_requested = true;
}
void EditorUI::confirm() {
    if (confirm_requested) { ImGui::OpenPopup("Unsaved changes"); confirm_requested = false; }
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const char *verb = pending == Pending::Quit ? "closing" : pending == Pending::Load ? "loading" : pending == Pending::NewScene ? "starting a new scene" : "opening another project";
    ImGui::Text("The scene has unsaved changes. Save them to '%s' before %s?", scene_path.data(), verb);
    Pending run = Pending::None;
    if (ImGui::Button("Save")) { if (report(session.save(scene_path.data()), "Scene saved")) run = pending; pending = Pending::None; ImGui::CloseCurrentPopup(); }
    ImGui::SameLine(); if (ImGui::Button("Discard changes")) { run = pending; pending = Pending::None; ImGui::CloseCurrentPopup(); }
    ImGui::SameLine(); if (ImGui::Button("Cancel")) { pending = Pending::None; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
    if (run != Pending::None) perform(run);
}
void EditorUI::docking() {
    const ImGuiID dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    auto *node = ImGui::DockBuilderGetNode(dock);
    if (std::exchange(layout_reset_requested, false)) {
        // Starts over: every window leaves its dock, and the code below docks it again.
        ImGui::DockBuilderRemoveNode(dock);
        ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
        node = ImGui::DockBuilderGetNode(dock);
    }
    if (node && !node->IsSplitNode() && node->Windows.empty()) {
        ImGuiID center = dock, left = 0, right = 0, bottom = 0, top = 0;
        ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->Size);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.12f, &top, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.19f, &left, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, &right, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.34f, &bottom, &center);
        ImGuiID assets = 0, messages_dock = 0;
        ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, 0.68f, &assets, &messages_dock);
        ImGui::DockBuilderDockWindow("Toolbar", top);
        ImGui::DockBuilderDockWindow("Scene Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Lighting", right);
        ImGui::DockBuilderDockWindow("Material Editor", right);
        ImGui::DockBuilderDockWindow("###3D Viewport", center);
        ImGui::DockBuilderDockWindow("Asset Browser", assets);
        ImGui::DockBuilderDockWindow("Console", messages_dock);
        ImGui::DockBuilderFinish(dock);
    }
}
void EditorUI::toolbar() {
    if (!show_toolbar) return;
    if (!begin_panel("Toolbar", "Toolbar", context::global, &show_toolbar)) { ImGui::End(); return; }
    const auto state = session.play_state();
    command_button("File.NewScene", "New");
    ImGui::SameLine(); command_button("Create.Cube", "+ Cube");
    ImGui::SameLine(); if (ImGui::Button("Create...")) ImGui::OpenPopup("Create");
    mark("Toolbar.Create");
    create_menu();
    ImGui::SameLine(); command_button("Edit.Duplicate");
    ImGui::SameLine(); command_button("Edit.Delete");
    ImGui::SameLine(); command_button("Viewport.FocusSelected", "Focus");
    ImGui::SameLine(); command_button("Edit.Undo");
    ImGui::SameLine(); command_button("Edit.Redo");
    ImGui::SameLine(); command_button("File.Save");
    ImGui::SameLine(); command_button("File.LoadFromPath");
    ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
    ImGui::BeginDisabled(playing());
    if (ImGui::Button("Project...")) ImGui::OpenPopup("Project menu");
    ImGui::EndDisabled();
    mark("Toolbar.Project");
    if (ImGui::BeginPopup("Project menu")) {
        command_item("File.OpenSample");
        ImGui::Separator();
        command_item("File.NewProject");
        command_item("File.OpenProject");
        ImGui::EndPopup();
    }
    ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
    command_button(state == PlayState::Playing ? "Play.Pause" : state == PlayState::Paused ? "Play.Resume" : "Play.Play");
    if (state == PlayState::Editing) { ImGui::SameLine(); command_button("Play.Simulate"); }
    else { ImGui::SameLine(); command_button("Play.Eject", session.ejected() ? "Possess" : "Eject"); }
    ImGui::SameLine(); command_button("Play.Step");
    ImGui::SameLine(); command_button("Play.Stop");
    ImGui::SameLine();
    if (playing()) {
        const char *what = state == PlayState::Paused ? "PAUSED" : session.simulating() ? "SIMULATING" : session.ejected() ? "EJECTED" : "PLAYING";
        ImGui::TextColored({0.45f, 0.85f, 0.45f, 1}, "%s  tick %llu", what, static_cast<unsigned long long>(session.runtime_ticks()));
    }
    else ImGui::TextDisabled("Revision %llu%s", static_cast<unsigned long long>(session.commands().revision()), session.dirty() ? "  (unsaved)" : "");
    ImGui::SetNextItemWidth(px(490)); ImGui::InputText("Scene file", scene_path.data(), scene_path.size());
    mark("Toolbar.ScenePath");
    ImGui::SameLine(); ImGui::SetNextItemWidth(px(110)); ImGui::Combo("##operation", &operation, "Move\0Rotate\0Scale\0");
    ImGui::SameLine(); ImGui::SetNextItemWidth(px(100)); ImGui::Combo("##coordinates", &coordinates, "Local\0World\0");
    ImGui::SameLine(); command_checkbox("Viewport.ToggleColliders", "Colliders");
    ImGui::End();
}
void EditorUI::duplicate_selection() { if (session.selection()) report(session.duplicate_selection(), "Duplicated entity subtree"); }
void EditorUI::delete_selection() { if (session.selection()) report(session.delete_selection(), "Deleted entity subtree"); }
void EditorUI::draw() {
    ui_map.begin_frame();
    hovered_context = {}; focused_context = {};
    // Claimed, so that releasing Alt after Alt+drag does not move the keyboard to the menu bar.
    if (ImGui::GetIO().KeyAlt) ImGui::SetKeyOwner(ImGuiMod_Alt, ImGui::GetID("##alt-for-viewport"));
    // The keys that finish a rebinding this frame must not also run a command.
    const bool rebinding_at_frame_start = !rebind_command.empty();
    for (const auto &file : std::exchange(dropped_files, {})) import_file(file);
    finish_imports();
    main_menu();
    // Before the viewport: it extracts the frame, whose handles must be those of the latest import or reload.
    docking(); toolbar(); hierarchy(); lighting(); material_editor(); inspector(); assets(); viewport();
    console_panel();
    picker_window();
    history_panel();
    shortcuts_panel();
    confirm();
    project_dialog();
    layout_dialog();
    route_shortcuts(rebinding_at_frame_start);
    ui_map.flush();
}
} // namespace engine::editor
