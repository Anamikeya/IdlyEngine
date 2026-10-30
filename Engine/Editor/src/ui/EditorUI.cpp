#include "EditorUI.hpp"
#include <glm/gtc/quaternion.hpp>
#include <filesystem>
#include <algorithm>
#include <utility>

namespace engine::editor {
void EditorUI::log(std::string text) { if (messages.size() == 128) messages.erase(messages.begin()); messages.push_back(std::move(text)); scroll_console = true; }
void EditorUI::spawn(const char *label, Components c, Transform t, EntityId parent) {
    auto r = session.execute(commands::CreateEntity{label, t, parent, 0, std::move(c)});
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
void EditorUI::create_menu() {
    if (!ImGui::BeginPopup("Create")) return;
    if (ImGui::MenuItem("Cube")) spawn("Cube", {MeshComponent{BuiltinMesh::Cube}, {}, {}});
    if (ImGui::MenuItem("Sphere")) spawn("Sphere", {MeshComponent{BuiltinMesh::Sphere}, {}, {}});
    if (ImGui::MenuItem("Plane")) spawn("Plane", {MeshComponent{BuiltinMesh::Plane}, {}, {}}, {{0,0,0}, {}, {10,1,10}});
    if (ImGui::MenuItem("Empty")) spawn("Empty", {});
    ImGui::Separator();
    if (ImGui::MenuItem("Directional Light")) spawn("Directional Light", {{}, {}, LightComponent{}}, aimed({0,4,0}, -50, 30));
    // Sized for the scene's exposure: a real bulb's output indoors, far more under daylight exposure.
    if (ImGui::MenuItem("Point Light")) spawn("Point Light", {{}, {}, point_light()}, {{0,2,0}, {}, {1,1,1}});
    if (ImGui::MenuItem("Spot Light")) { auto l = point_light(); l.type = LightType::Spot; l.cast_shadows = true; spawn("Spot Light", {{}, {}, l}, aimed({0,4,0}, -90, 0)); }
    if (ImGui::MenuItem("Camera")) spawn("Camera", {{}, CameraComponent{}, {}}, aimed({0,1.5,6}, -8, 0));
    ImGui::Separator();
    if (ImGui::MenuItem("Falling Cube")) spawn("Falling Cube", physical(BuiltinMesh::Cube), {{0,3,0}, {}, {1,1,1}});
    if (ImGui::MenuItem("Falling Sphere")) spawn("Falling Sphere", physical(BuiltinMesh::Sphere), {{0,3,0}, {}, {1,1,1}});
    if (ImGui::MenuItem("Floor")) {
        Components floor{MeshComponent{BuiltinMesh::Plane}, {}, {}};
        floor.colliders = {fitted_collider(BuiltinMesh::Plane)};
        spawn("Floor", std::move(floor), {{0,0,0}, {}, {10,1,10}});
    }
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
    if (node && !node->IsSplitNode() && node->Windows.empty()) {
        ImGuiID center = dock, left = 0, right = 0, bottom = 0, top = 0;
        ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->Size);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.12f, &top, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.19f, &left, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, &right, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.25f, &bottom, &center);
        ImGuiID assets = 0, console = 0;
        ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, 0.45f, &assets, &console);
        ImGui::DockBuilderDockWindow("Toolbar", top);
        ImGui::DockBuilderDockWindow("Scene Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Lighting", right);
        ImGui::DockBuilderDockWindow("Material Editor", right);
        ImGui::DockBuilderDockWindow("3D Viewport", center);
        ImGui::DockBuilderDockWindow("Asset Browser", assets);
        ImGui::DockBuilderDockWindow("Console", console);
        ImGui::DockBuilderFinish(dock);
    }
}
void EditorUI::toolbar() {
    ImGui::Begin("Toolbar");
    const auto state = session.play_state();
    ImGui::BeginDisabled(playing());
    if (ImGui::Button("New")) request(Pending::NewScene);
    ImGui::EndDisabled();
    ImGui::SameLine(); if (ImGui::Button("+ Cube")) report(session.create(), "Created cube");
    ImGui::SameLine(); if (ImGui::Button("Create...")) ImGui::OpenPopup("Create");
    create_menu();
    ImGui::SameLine(); ImGui::BeginDisabled(!session.selection());
    if (ImGui::Button("Duplicate")) duplicate_selection();
    ImGui::SameLine(); if (ImGui::Button("Delete")) delete_selection();
    ImGui::SameLine(); if (ImGui::Button("Focus [F]")) session.focus();
    ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::BeginDisabled(playing() || !session.can_undo());
    if (ImGui::Button("Undo")) report(session.undo(), "Undo"); ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::BeginDisabled(playing() || !session.can_redo());
    if (ImGui::Button("Redo")) report(session.redo(), "Redo"); ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::BeginDisabled(playing());
    if (ImGui::Button("Save")) report(session.save(scene_path.data()), "Scene saved");
    ImGui::SameLine(); if (ImGui::Button("Load")) request(Pending::Load);
    ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
    ImGui::BeginDisabled(playing());
    if (ImGui::Button("Project...")) ImGui::OpenPopup("Project menu");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("Project menu")) {
        if (ImGui::MenuItem("Open Physics Shooter sample")) request(Pending::OpenSample);
        ImGui::Separator();
        if (ImGui::MenuItem("New empty project...")) { project_create = true; project_prompt = true; }
        if (ImGui::MenuItem("Open project...")) { project_create = false; project_prompt = true; }
        ImGui::EndPopup();
    }
    ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
    if (state == editor::PlayState::Playing) { if (ImGui::Button("Pause")) report(session.pause(), "Paused"); }
    else if (ImGui::Button(state == editor::PlayState::Paused ? "Resume" : "Play"))
        report(session.play(), state == editor::PlayState::Paused ? "Resumed" : "Play mode: running a copy of the scene; changes are discarded on Stop");
    ImGui::SameLine(); ImGui::BeginDisabled(state != editor::PlayState::Paused);
    if (ImGui::Button("Step")) report(session.step(), "Stepped one tick"); ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::BeginDisabled(state == editor::PlayState::Editing);
    if (ImGui::Button("Stop")) stop_play(); ImGui::EndDisabled();
    ImGui::SameLine();
    if (playing()) ImGui::TextColored({0.45f, 0.85f, 0.45f, 1}, "%s  tick %llu", state == editor::PlayState::Paused ? "PAUSED" : "PLAYING",
                                      static_cast<unsigned long long>(session.runtime_ticks()));
    else ImGui::TextDisabled("Revision %llu%s", static_cast<unsigned long long>(session.commands().revision()), session.dirty() ? "  (unsaved)" : "");
    ImGui::SetNextItemWidth(px(490)); ImGui::InputText("Scene file", scene_path.data(), scene_path.size());
    ImGui::SameLine(); ImGui::SetNextItemWidth(px(110)); ImGui::Combo("##operation", &operation, "Move\0Rotate\0Scale\0");
    ImGui::SameLine(); ImGui::SetNextItemWidth(px(100)); ImGui::Combo("##coordinates", &coordinates, "Local\0World\0");
    ImGui::SameLine(); ImGui::Checkbox("Colliders", &show_colliders);
    ImGui::End();
}
void EditorUI::duplicate_selection() { if (session.selection()) report(session.duplicate(session.selection()), "Duplicated entity subtree"); }
void EditorUI::delete_selection() { if (session.selection()) report(session.execute(commands::DeleteEntity{session.selection()}), "Deleted entity subtree"); }
void EditorUI::draw() {
    for (const auto &file : std::exchange(dropped_files, {})) import_file(file);
    finish_imports();
    // Before the viewport: it extracts the frame, whose handles must be those of the latest import or reload.
    docking(); toolbar(); hierarchy(); lighting(); material_editor(); inspector(); assets(); viewport();
    ImGui::Begin("Console");
    for (const auto &m : messages) ImGui::TextWrapped("%s", m.c_str());
    // The newest message, such as an import error, is what the user needs to see.
    if (std::exchange(scroll_console, false)) ImGui::SetScrollHereY(1);
    ImGui::End();
    confirm();
    project_dialog();
    const auto &io = ImGui::GetIO();
    if (!io.WantTextInput && !gizmo_active && !game_input.captured && !ImGui::IsPopupOpen("Unsaved changes")) {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) report(io.KeyShift ? session.redo() : session.undo(), io.KeyShift ? "Redo" : "Undo");
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) report(session.redo(), "Redo");
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) report(session.save(scene_path.data()), "Scene saved");
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N)) request(Pending::NewScene);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicate_selection();
        if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) delete_selection();
    }
}
} // namespace engine::editor
