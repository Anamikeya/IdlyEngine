#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_INCLUDE_NONE
#define NOMINMAX
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <ImGuizmo.h>
#include <engine/editor/EditorSession.hpp>
#include <engine/renderer/DesktopRenderer.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <optional>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace engine;
struct Host {
    GLFWwindow *window = nullptr;
    bool glfw = false, imgui = false, backend = false;
    ~Host() {
        if (backend) ImGui_ImplGlfw_Shutdown();
        if (imgui) ImGui::DestroyContext();
        if (window) glfwDestroyWindow(window);
        if (glfw) glfwTerminate();
    }
    void start() {
        glfwSetErrorCallback([](int code, const char *message) { std::cerr << "GLFW " << code << ": " << message << '\n'; });
        if (!glfwInit()) throw std::runtime_error("GLFW initialization failed");
        glfw = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window = glfwCreateWindow(1440, 900, "GameEngine Editor", nullptr, nullptr);
        if (!window) throw std::runtime_error("Window creation failed");
        IMGUI_CHECKVERSION(); ImGui::CreateContext(); imgui = true;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        auto &style = ImGui::GetStyle();
        style.WindowRounding = 3; style.FrameRounding = 3; style.FramePadding = {6,5};
        style.Colors[ImGuiCol_WindowBg] = {0.075f,0.085f,0.105f,1};
        style.Colors[ImGuiCol_Header] = {0.16f,0.27f,0.39f,1};
        style.Colors[ImGuiCol_Button] = {0.13f,0.23f,0.34f,1};
        backend = ImGui_ImplGlfw_InitForOther(window, true);
        if (!backend) throw std::runtime_error("ImGui GLFW initialization failed");
    }
};
// Actions that discard the editor scene ask first when it has unsaved changes.
enum class Pending { None, NewScene, Load, Quit };
struct EditorUI {
    editor::EditorSession &session;
    renderer::DesktopRenderer &renderer;
    std::array<char, 1024> scene_path{};
    std::vector<std::string> messages{"Filament OpenGL initialized. All scene edits use typed commands."};
    Pending pending = Pending::None;
    bool confirm_requested = false, quit = false;
    EntityId draft_id = 0;
    uint64_t draft_scene = 0, draft_revision = ~uint64_t(0);
    std::array<char, 257> name{};
    float position[3]{}, rotation[3]{}, scale[3]{1,1,1};
    Components components; // Draft of the selected entity's components, refreshed with the transform draft.
    int operation = 0, coordinates = 0;
    bool gizmo_active = false;
    bool show_colliders = true;
    std::set<std::string> reported_issues; // Physics issues already in the Console; re-reported if they return.
    std::string reported_physics_error;
    EntityId gizmo_id = 0;
    float gizmo_matrix[16]{};
    renderer::Frame frame;
    ImVec2 viewport_center{}; // Window coordinates; used by the GPU smoke readback checks.
    explicit EditorUI(editor::EditorSession &s, renderer::DesktopRenderer &r) : session(s), renderer(r) {
        std::snprintf(scene_path.data(), scene_path.size(), "%s", "Untitled.scene");
    }
    void log(std::string text) { if (messages.size() == 128) messages.erase(messages.begin()); messages.push_back(std::move(text)); }
    template<class T> bool report(const Result<T> &r, const char *success) {
        if (r) { log(success); return true; } log("Error: " + r.error().message); return false;
    }
    bool playing() const { return session.play_state() != editor::PlayState::Editing; }
    // Creates and selects an entity in one command, so it is one undo step.
    void spawn(const char *label, Components c, Transform t = {{0,0.5,0}, {}, {1,1,1}}, EntityId parent = 0) {
        auto r = session.execute(commands::CreateEntity{label, t, parent, 0, std::move(c)});
        if (report(r, (std::string("Created ") + label).c_str())) session.select(r.value().created.front());
    }
    static Transform aimed(Vec3 position, double pitch_degrees, double yaw_degrees) {
        const glm::dquat q(glm::radians(glm::dvec3(pitch_degrees, yaw_degrees, 0)));
        return {position, {q.x, q.y, q.z, q.w}, {1,1,1}};
    }
    // A collider matching a built-in mesh. The plane's is a thin box whose top face is the plane itself.
    static ColliderComponent fitted_collider(BuiltinMesh mesh) {
        ColliderComponent k;
        if (mesh == BuiltinMesh::Sphere) k.shape = ColliderShape::Sphere;
        if (mesh == BuiltinMesh::Plane) { k.half_extents = {0.5, 0.05, 0.5}; k.center = {0, -0.05, 0}; }
        return k;
    }
    static Components physical(BuiltinMesh mesh) {
        Components c{MeshComponent{mesh}, {}, {}};
        c.body = RigidBodyComponent{};
        c.colliders = {fitted_collider(mesh)};
        return c;
    }
    void create_menu() {
        if (!ImGui::BeginPopup("Create")) return;
        if (ImGui::MenuItem("Cube")) spawn("Cube", {MeshComponent{BuiltinMesh::Cube}, {}, {}});
        if (ImGui::MenuItem("Sphere")) spawn("Sphere", {MeshComponent{BuiltinMesh::Sphere}, {}, {}});
        if (ImGui::MenuItem("Plane")) spawn("Plane", {MeshComponent{BuiltinMesh::Plane}, {}, {}}, {{0,0,0}, {}, {10,1,10}});
        if (ImGui::MenuItem("Empty")) spawn("Empty", {});
        ImGui::Separator();
        if (ImGui::MenuItem("Directional Light")) spawn("Directional Light", {{}, {}, LightComponent{}}, aimed({0,4,0}, -50, 30));
        // The editor camera uses daylight exposure (sunny 16), so a point light needs ~10^6 lm to read as bright
        // a few meters away; the Inspector shows the physical value.
        if (ImGui::MenuItem("Point Light")) spawn("Point Light", {{}, {}, LightComponent{LightType::Point, {1,1,1}, 1500000, 10}}, {{0,2,0}, {}, {1,1,1}});
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
    std::string title() const {
        std::string t = "GameEngine Editor - " + std::filesystem::path(scene_path.data()).filename().string();
        if (session.dirty()) t += '*';
        if (session.play_state() == editor::PlayState::Playing) t += " [Playing]";
        if (session.play_state() == editor::PlayState::Paused) t += " [Paused]";
        return t;
    }
    void perform(Pending action) {
        if (action == Pending::NewScene && report(session.new_scene(), "New scene"))
            std::snprintf(scene_path.data(), scene_path.size(), "%s", "Untitled.scene");
        if (action == Pending::Load) report(session.load(scene_path.data()), "Scene loaded (undoable)");
        if (action == Pending::Quit) quit = true;
    }
    void request(Pending action) {
        if (action == Pending::Quit && playing()) report(session.stop(), "Stopped Play mode; runtime copy discarded");
        if (action != Pending::Quit && playing()) { log("Error: Stop Play mode first"); return; }
        if (!session.dirty()) { perform(action); return; }
        pending = action; confirm_requested = true;
    }
    void confirm() {
        if (confirm_requested) { ImGui::OpenPopup("Unsaved changes"); confirm_requested = false; }
        if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        const char *verb = pending == Pending::Quit ? "closing" : pending == Pending::Load ? "loading" : "starting a new scene";
        ImGui::Text("The scene has unsaved changes. Save them to '%s' before %s?", scene_path.data(), verb);
        Pending run = Pending::None;
        if (ImGui::Button("Save")) { if (report(session.save(scene_path.data()), "Scene saved")) run = pending; pending = Pending::None; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if (ImGui::Button("Discard changes")) { run = pending; pending = Pending::None; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if (ImGui::Button("Cancel")) { pending = Pending::None; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
        if (run != Pending::None) perform(run);
    }
    void docking() {
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
            ImGui::DockBuilderDockWindow("3D Viewport", center);
            ImGui::DockBuilderDockWindow("Asset Browser", assets);
            ImGui::DockBuilderDockWindow("Console", console);
            ImGui::DockBuilderFinish(dock);
        }
    }
    void toolbar() {
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
        ImGui::SameLine(); ImGui::BeginDisabled(playing() || !session.commands().undo_size());
        if (ImGui::Button("Undo")) report(session.undo(), "Undo"); ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::BeginDisabled(playing() || !session.commands().redo_size());
        if (ImGui::Button("Redo")) report(session.redo(), "Redo"); ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::BeginDisabled(playing());
        if (ImGui::Button("Save")) report(session.save(scene_path.data()), "Scene saved");
        ImGui::SameLine(); if (ImGui::Button("Load")) request(Pending::Load);
        ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        if (state == editor::PlayState::Playing) { if (ImGui::Button("Pause")) report(session.pause(), "Paused"); }
        else if (ImGui::Button(state == editor::PlayState::Paused ? "Resume" : "Play"))
            report(session.play(), state == editor::PlayState::Paused ? "Resumed" : "Play mode: running a copy of the scene; changes are discarded on Stop");
        ImGui::SameLine(); ImGui::BeginDisabled(state != editor::PlayState::Paused);
        if (ImGui::Button("Step")) report(session.step(), "Stepped one tick"); ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::BeginDisabled(state == editor::PlayState::Editing);
        if (ImGui::Button("Stop")) report(session.stop(), "Stopped; runtime copy discarded, editor scene shown"); ImGui::EndDisabled();
        ImGui::SameLine();
        if (playing()) ImGui::TextColored({0.45f, 0.85f, 0.45f, 1}, "%s  tick %llu", state == editor::PlayState::Paused ? "PAUSED" : "PLAYING",
                                          static_cast<unsigned long long>(session.runtime_ticks()));
        else ImGui::TextDisabled("Revision %llu%s", static_cast<unsigned long long>(session.commands().revision()), session.dirty() ? "  (unsaved)" : "");
        ImGui::SetNextItemWidth(490); ImGui::InputText("Scene file", scene_path.data(), scene_path.size());
        ImGui::SameLine(); ImGui::SetNextItemWidth(110); ImGui::Combo("##operation", &operation, "Move\0Rotate\0Scale\0");
        ImGui::SameLine(); ImGui::SetNextItemWidth(100); ImGui::Combo("##coordinates", &coordinates, "Local\0World\0");
        ImGui::SameLine(); ImGui::Checkbox("Colliders", &show_colliders);
        ImGui::End();
    }
    // ---- Scene Hierarchy: a tree in stable-ID order. Drag a row onto another to parent it there, or onto the
    // empty area below the tree to move it to the root; both keep the world transform. Right-click for actions.
    static constexpr const char *entity_payload = "ENGINE_ENTITY";
    std::map<EntityId, EntityRecord> records;
    std::map<EntityId, std::vector<EntityId>> children;
    EntityId revealed = 0; // Selection whose ancestors were last opened, so viewport picks show up in the tree.
    void drop_onto(EntityId parent) {
        if (!ImGui::BeginDragDropTarget()) return;
        if (const auto *payload = ImGui::AcceptDragDropPayload(entity_payload)) {
            EntityId dragged = 0; std::memcpy(&dragged, payload->Data, sizeof dragged);
            if (dragged != parent) report(session.reparent(dragged, parent), parent ? "Reparented (world transform kept)" : "Moved to root (world transform kept)");
        }
        ImGui::EndDragDropTarget();
    }
    void duplicate_selection() { if (session.selection()) report(session.duplicate(session.selection()), "Duplicated entity subtree"); }
    void delete_selection() { if (session.selection()) report(session.execute(commands::DeleteEntity{session.selection()}), "Deleted entity subtree"); }
    void tree_node(EntityId id, const std::set<EntityId> &open_path) {
        const auto &e = records.at(id);
        const auto kids = children.find(id);
        const bool leaf = kids == children.end();
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
        if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (session.selection() == id) flags |= ImGuiTreeNodeFlags_Selected;
        if (open_path.contains(id)) ImGui::SetNextItemOpen(true);
        const bool open = ImGui::TreeNodeEx(reinterpret_cast<void *>(static_cast<uintptr_t>(id)), flags, "%s", e.name.c_str());
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) session.select(id);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { session.select(id); session.focus(); }
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(entity_payload, &id, sizeof id);
            ImGui::Text("Move '%s'", e.name.c_str());
            ImGui::EndDragDropSource();
        }
        drop_onto(id);
        if (ImGui::BeginPopupContextItem()) {
            session.select(id);
            if (ImGui::BeginMenu("Create child")) {
                const Transform above{{0,1,0}, {}, {1,1,1}};
                if (ImGui::MenuItem("Empty")) spawn("Empty", {}, above, id);
                if (ImGui::MenuItem("Cube")) spawn("Cube", {MeshComponent{BuiltinMesh::Cube}, {}, {}}, above, id);
                if (ImGui::MenuItem("Sphere")) spawn("Sphere", {MeshComponent{BuiltinMesh::Sphere}, {}, {}}, above, id);
                if (ImGui::MenuItem("Point Light")) spawn("Point Light", {{}, {}, LightComponent{LightType::Point, {1,1,1}, 1500000, 10}}, above, id);
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate_selection();
            if (ImGui::MenuItem("Move to root", nullptr, false, e.parent != 0)) report(session.reparent(id, 0), "Moved to root (world transform kept)");
            if (ImGui::MenuItem("Focus", "F")) session.focus();
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del")) delete_selection();
            ImGui::EndPopup();
        }
        const auto &c = e.components;
        if (c.light || c.camera || c.body || !c.colliders.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s%s%s", c.light ? (c.light->type == LightType::Point ? " point light" : " directional light") : "",
                                c.camera ? (c.camera->primary ? " camera (primary)" : " camera") : "",
                                c.body ? (c.body->kind == BodyKind::Dynamic ? " dynamic" : c.body->kind == BodyKind::Kinematic ? " kinematic" : " static")
                                       : !c.colliders.empty() ? " collider" : "");
        }
        if (open && !leaf) {
            for (const auto child : kids->second) tree_node(child, open_path);
            ImGui::TreePop();
        }
    }
    void hierarchy() {
        ImGui::Begin("Scene Hierarchy");
        records.clear(); children.clear();
        for (auto &e : session.scene().snapshot().entities) { children[e.parent].push_back(e.id); records.emplace(e.id, std::move(e)); }
        children.erase(EntityId{0});
        ImGui::TextDisabled("%zu entities  (drag to reparent, right-click for actions)", records.size());
        std::set<EntityId> open_path;
        if (session.selection() != revealed) {
            revealed = session.selection();
            if (records.contains(revealed))
                for (EntityId p = records.at(revealed).parent; p && records.contains(p); p = records.at(p).parent) open_path.insert(p);
        }
        for (const auto &[id, e] : records) if (!e.parent) tree_node(id, open_path);
        // The rest of the panel is a drop zone for moving entities back to the root.
        const auto room = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("##hierarchy-root", {std::max(room.x, 1.f), std::max(room.y, 24.f)});
        drop_onto(0);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) session.select(0);
        ImGui::End();
    }
    void inspector() {
        ImGui::Begin("Inspector");
        const auto record = session.scene().inspect(session.selection());
        if (!record) { ImGui::TextWrapped("Select an entity in the hierarchy, or click an object or a light/camera marker in the viewport."); ImGui::End(); return; }
        // Physics changes the revision every tick in Play mode; a field being edited keeps its value until released.
        if (draft_id != record->id || draft_scene != session.scene_identity() ||
            (draft_revision != session.scene_revision() && !ImGui::IsAnyItemActive())) {
            draft_id = record->id; draft_scene = session.scene_identity(); draft_revision = session.scene_revision();
            std::snprintf(name.data(), name.size(), "%s", record->name.c_str());
            const auto &t = record->transform;
            position[0] = float(t.position.x); position[1] = float(t.position.y); position[2] = float(t.position.z);
            const auto euler = glm::degrees(glm::eulerAngles(glm::dquat(t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z)));
            for (int i = 0; i < 3; ++i) rotation[i] = float(euler[i]);
            scale[0] = float(t.scale.x); scale[1] = float(t.scale.y); scale[2] = float(t.scale.z);
            components = record->components;
        }
        if (playing()) ImGui::TextColored({0.45f, 0.85f, 0.45f, 1}, "Play mode: edits affect the runtime copy only");
        ImGui::TextDisabled("Stable ID: %llu", static_cast<unsigned long long>(record->id));
        if (ImGui::InputText("Name", name.data(), name.size(), ImGuiInputTextFlags_EnterReturnsTrue))
            report(session.execute(commands::RenameEntity{record->id, name.data()}), "Renamed entity");
        ImGui::TextDisabled("Press Enter to rename");
        ImGui::SeparatorText("Local transform");
        bool commit = false;
        ImGui::DragFloat3("Position", position, 0.03f); commit |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::DragFloat3("Rotation", rotation, 0.5f); commit |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::DragFloat3("Scale", scale, 0.01f); commit |= ImGui::IsItemDeactivatedAfterEdit();
        if (commit) {
            const glm::dquat q(glm::radians(glm::dvec3(rotation[0], rotation[1], rotation[2])));
            Transform t{{position[0], position[1], position[2]}, {q.x,q.y,q.z,q.w}, {scale[0],scale[1],scale[2]}};
            report(session.execute(commands::SetTransform{record->id, t}), "Transform committed");
            draft_revision = ~uint64_t(0);
        }
        ImGui::TextDisabled("Rotation: XYZ Euler degrees");
        ImGui::TextWrapped("Each drag commits on release as one undo step.");
        if (ImGui::BeginCombo("Parent", record->parent ? session.scene().inspect(record->parent)->name.c_str() : "(root)")) {
            if (ImGui::Selectable("(root)", !record->parent)) report(session.reparent(record->id, 0), "Moved to root (world transform kept)");
            for (const auto &e : session.scene().snapshot().entities) {
                // The entity itself and its descendants would form a cycle.
                bool own = false;
                for (EntityId p = e.id; p && !own; p = session.scene().inspect(p)->parent) own = p == record->id;
                ImGui::PushID(reinterpret_cast<void *>(static_cast<uintptr_t>(e.id)));
                if (ImGui::Selectable(e.name.c_str(), record->parent == e.id, own ? ImGuiSelectableFlags_Disabled : 0))
                    report(session.reparent(record->id, e.id), "Reparented (world transform kept)");
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SeparatorText("World position");
        const auto w = session.scene().world_transform(record->id);
        ImGui::Text("%.3f, %.3f, %.3f", w->matrix[12], w->matrix[13], w->matrix[14]);
        component_sections(*record);
        ImGui::End();
    }
    // Commit helpers: drags commit on release (one undo step); combos, checkboxes and picker changes at once.
    static bool committed(bool changed) { return ImGui::IsItemDeactivatedAfterEdit() || (changed && !ImGui::IsItemActive()); }
    static bool color_edit(const char *label, Vec3 &c) {
        float v[3]{float(c.x), float(c.y), float(c.z)};
        const bool changed = ImGui::ColorEdit3(label, v, ImGuiColorEditFlags_Float);
        if (changed) c = {std::clamp(double(v[0]), 0.0, 1.0), std::clamp(double(v[1]), 0.0, 1.0), std::clamp(double(v[2]), 0.0, 1.0)};
        return committed(changed);
    }
    static bool drag(const char *label, double &value, float speed, double low, double high, const char *format = "%.3f") {
        float v = float(value);
        const bool changed = ImGui::DragFloat(label, &v, speed, float(low), float(high), format, ImGuiSliderFlags_AlwaysClamp);
        if (changed) value = v;
        return committed(changed);
    }
    // No clamp when low == high.
    static bool drag3(const char *label, Vec3 &value, float speed, float low = 0, float high = 0) {
        float v[3]{float(value.x), float(value.y), float(value.z)};
        const bool changed = ImGui::DragFloat3(label, v, speed, low, high, "%.3f", low < high ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None);
        if (changed) value = {v[0], v[1], v[2]};
        return committed(changed);
    }
    // Runs a component command and re-reads the drafts, so a rejected edit does not linger in the Inspector.
    void apply(commands::Command command, const char *success) { report(session.execute(std::move(command)), success); draft_revision = ~uint64_t(0); }
    bool remove_button(const char *id) {
        ImGui::PushID(id); const bool clicked = ImGui::SmallButton("Remove"); ImGui::PopID(); return clicked;
    }
    void component_sections(const EntityRecord &record) {
        auto &c = components;
        if (c.mesh) {
            ImGui::SeparatorText("Mesh Renderer");
            auto &m = *c.mesh; bool commit = false;
            int shape = int(m.mesh) - 1;
            if (ImGui::Combo("Mesh", &shape, "Cube\0Sphere\0Plane\0")) { m.mesh = BuiltinMesh(shape + 1); commit = true; }
            commit |= color_edit("Color", m.color);
            commit |= drag("Roughness", m.roughness, 0.01f, 0, 1);
            commit |= drag("Metallic", m.metallic, 0.01f, 0, 1);
            if (commit) apply(commands::SetMesh{record.id, m}, "Mesh renderer changed");
            if (remove_button("mesh")) apply(commands::SetMesh{record.id, std::nullopt}, "Mesh renderer removed");
        }
        if (c.camera) {
            ImGui::SeparatorText("Camera");
            auto &cam = *c.camera; bool commit = false;
            commit |= drag("Field of view", cam.vertical_fov_degrees, 0.2f, 1, 179, "%.1f deg");
            commit |= drag("Near", cam.near_plane, 0.01f, 0.001, cam.far_plane * 0.999);
            commit |= drag("Far", cam.far_plane, 1.0f, cam.near_plane * 1.001, 1e6, "%.1f");
            commit |= ImGui::Checkbox("Primary (Play mode view)", &cam.primary);
            if (commit) apply(commands::SetCamera{record.id, cam}, "Camera changed");
            ImGui::TextDisabled("Looks along local -Z; must not be scaled.");
            if (remove_button("camera")) apply(commands::SetCamera{record.id, std::nullopt}, "Camera removed");
        }
        if (c.light) {
            ImGui::SeparatorText("Light");
            auto &l = *c.light; bool commit = false;
            int type = int(l.type) - 1;
            if (ImGui::Combo("Type", &type, "Directional\0Point\0")) { l.type = LightType(type + 1); commit = true; }
            commit |= color_edit("Light color", l.color);
            commit |= drag(l.type == LightType::Point ? "Intensity (lm)" : "Intensity (lux)", l.intensity, 500.0f, 0, 1e7, "%.0f");
            if (l.type == LightType::Point) commit |= drag("Range (m)", l.range, 0.05f, 0.01, 1000);
            else ImGui::TextDisabled("Shines along local -Z.");
            if (commit) apply(commands::SetLight{record.id, l}, "Light changed");
            if (remove_button("light")) apply(commands::SetLight{record.id, std::nullopt}, "Light removed");
        }
        physics_sections(record);
        ImGui::Spacing();
        if (ImGui::Button("Add Component")) ImGui::OpenPopup("Add Component");
        if (ImGui::BeginPopup("Add Component")) {
            if (ImGui::MenuItem("Mesh Renderer", nullptr, false, !c.mesh)) apply(commands::SetMesh{record.id, MeshComponent{}}, "Mesh renderer added");
            if (ImGui::MenuItem("Camera", nullptr, false, !c.camera)) apply(commands::SetCamera{record.id, CameraComponent{}}, "Camera added");
            if (ImGui::MenuItem("Light", nullptr, false, !c.light)) apply(commands::SetLight{record.id, LightComponent{}}, "Light added");
            if (ImGui::MenuItem("Rigid Body", nullptr, false, !c.body)) apply(commands::SetRigidBody{record.id, RigidBodyComponent{}}, "Rigid body added");
            if (ImGui::MenuItem("Collider (fitted to mesh)", nullptr, false, c.colliders.size() < max_colliders_per_entity)) {
                auto list = c.colliders;
                list.push_back(fitted_collider(c.mesh ? c.mesh->mesh : BuiltinMesh::Cube));
                apply(commands::SetColliders{record.id, std::move(list)}, "Collider added");
            }
            ImGui::EndPopup();
        }
    }
    void physics_sections(const EntityRecord &record) {
        auto &c = components;
        if (c.body) {
            ImGui::SeparatorText("Rigid Body");
            auto &b = *c.body; bool commit = false;
            int kind = int(b.kind) - 1;
            if (ImGui::Combo("Body type", &kind, "Static\0Kinematic\0Dynamic\0")) { b.kind = BodyKind(kind + 1); commit = true; }
            commit |= drag3("Initial velocity", b.linear_velocity, 0.05f);
            commit |= drag3("Initial spin", b.angular_velocity, 0.05f);
            commit |= drag("Linear damping", b.linear_damping, 0.01f, 0, 100);
            commit |= drag("Angular damping", b.angular_damping, 0.01f, 0, 100);
            commit |= drag("Gravity scale", b.gravity_scale, 0.01f, -100, 100);
            commit |= ImGui::Checkbox("Can sleep", &b.enable_sleep);
            commit |= ImGui::Checkbox("Continuous collision", &b.is_bullet);
            if (commit) apply(commands::SetRigidBody{record.id, b}, "Rigid body changed");
            if (c.colliders.empty()) ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "No colliders: this body will not collide.");
            if (playing())
                if (const auto state = session.physics_bridge().body_state(record.id)) {
                    const auto &v = state->linear_velocity;
                    ImGui::TextDisabled("Speed %.2f m/s, mass %.1f kg, %s", std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z), state->mass, state->awake ? "awake" : "asleep");
                }
            if (remove_button("body")) apply(commands::SetRigidBody{record.id, std::nullopt}, "Rigid body removed");
        }
        for (std::size_t i = 0; i < c.colliders.size(); ++i) {
            ImGui::PushID(int(i));
            ImGui::SeparatorText(("Collider " + std::to_string(i + 1) + (c.body ? "" : " (static)")).c_str());
            auto &k = c.colliders[i]; bool commit = false;
            int shape = int(k.shape) - 1;
            if (ImGui::Combo("Shape", &shape, "Sphere\0Box\0Capsule\0")) { k.shape = ColliderShape(shape + 1); commit = true; }
            commit |= drag3("Center", k.center, 0.01f);
            if (k.shape == ColliderShape::Box) commit |= drag3("Half extents", k.half_extents, 0.01f, 0.001f, 10000);
            else commit |= drag("Radius", k.radius, 0.01f, 0.001, 10000);
            if (k.shape == ColliderShape::Capsule) commit |= drag("Half height", k.half_height, 0.01f, 0, 10000);
            if (k.shape != ColliderShape::Sphere) {
                const auto euler = glm::degrees(glm::eulerAngles(glm::dquat(k.rotation.w, k.rotation.x, k.rotation.y, k.rotation.z)));
                float r[3]{float(euler.x), float(euler.y), float(euler.z)};
                if (ImGui::DragFloat3("Local rotation", r, 0.5f)) {
                    const glm::dquat q(glm::radians(glm::dvec3(r[0], r[1], r[2])));
                    k.rotation = {q.x, q.y, q.z, q.w};
                }
                commit |= ImGui::IsItemDeactivatedAfterEdit();
            }
            commit |= drag("Density", k.density, 10.0f, 0, 100000, "%.0f kg/m3");
            commit |= drag("Friction", k.friction, 0.01f, 0, 10);
            commit |= drag("Restitution", k.restitution, 0.01f, 0, 1);
            commit |= ImGui::Checkbox("Sensor", &k.sensor);
            ImGui::SameLine(); commit |= ImGui::Checkbox("Report events", &k.events);
            if (commit) apply(commands::SetColliders{record.id, c.colliders}, "Collider changed");
            const bool removed = remove_button("collider");
            ImGui::PopID();
            if (removed) {
                auto list = c.colliders;
                list.erase(list.begin() + std::ptrdiff_t(i));
                apply(commands::SetColliders{record.id, std::move(list)}, "Collider removed");
                break;
            }
        }
        for (const auto &issue : session.physics_bridge().diagnostics().issues)
            if (issue.entity == record.id) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.35f, 1));
                ImGui::TextWrapped("Physics: %s", issue.message.c_str());
                ImGui::PopStyleColor();
            }
    }
    // Puts physics problems in the Console once, when they appear.
    void report_physics() {
        std::set<std::string> current;
        for (const auto &issue : session.physics_bridge().diagnostics().issues) {
            const auto record = session.scene().inspect(issue.entity);
            current.insert("Physics: " + (record ? record->name : std::to_string(issue.entity)) + ": " + issue.message);
        }
        for (const auto &message : current) if (!reported_issues.contains(message)) log(message);
        reported_issues = std::move(current);
        if (session.physics_error() != reported_physics_error) {
            reported_physics_error = session.physics_error();
            if (!reported_physics_error.empty()) log("Error: " + reported_physics_error);
        }
    }
    void viewport() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0,0});
        const bool visible = ImGui::Begin("3D Viewport");
        ImGui::PopStyleVar();
        if (!visible) { ImGui::End(); return; }
        const auto origin = ImGui::GetCursorScreenPos();
        auto size = ImGui::GetContentRegionAvail(); size.x = std::max(size.x, 1.f); size.y = std::max(size.y, 1.f);
        viewport_center = {origin.x + size.x / 2, origin.y + size.y / 2};
        const auto &io = ImGui::GetIO();
        renderer.resize_viewport(unsigned(size.x * io.DisplayFramebufferScale.x), unsigned(size.y * io.DisplayFramebufferScale.y));
        ImGui::Image(ImTextureRef(renderer.viewport_texture()), size, {0,1}, {1,0});
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && !ImGuizmo::IsUsing()) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) session.camera.orbit(io.MouseDelta.x, io.MouseDelta.y);
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) session.camera.pan(io.MouseDelta.x, io.MouseDelta.y, size.y);
            session.camera.zoom(io.MouseWheel);
            if (ImGui::IsKeyPressed(ImGuiKey_F)) session.focus();
        }
        auto snapshot = session.frame(double(size.x) / size.y);
        if (!snapshot) { log(snapshot.error().message); ImGui::End(); return; }
        frame = snapshot.value();
        ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList()); ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
        ImGuizmo::SetOrthographic(false);
        if (const auto world = session.scene().world_transform(session.selection())) {
            float view[16], projection[16];
            for (int i = 0; i < 16; ++i) { view[i] = float(frame->camera.view.matrix[i]); projection[i] = float(frame->camera.projection.matrix[i]); }
            if (!gizmo_active || gizmo_id != session.selection()) {
                for (int i = 0; i < 16; ++i) gizmo_matrix[i] = float(world->matrix[i]);
                gizmo_id = session.selection();
            }
            const ImGuizmo::OPERATION op[] = {ImGuizmo::TRANSLATE, ImGuizmo::ROTATE, ImGuizmo::SCALE};
            ImGuizmo::Manipulate(view, projection, op[operation], coordinates == 0 ? ImGuizmo::LOCAL : ImGuizmo::WORLD, gizmo_matrix);
            const bool using_now = ImGuizmo::IsUsing();
            if (gizmo_active && !using_now && gizmo_id == session.selection()) {
                WorldTransform result;
                for (int i = 0; i < 16; ++i) result.matrix[i] = gizmo_matrix[i];
                report(session.set_world(gizmo_id, result), "Gizmo transform committed");
            }
            gizmo_active = using_now;
        } else gizmo_active = false;
        if (show_colliders) collider_outlines(origin, size);
        report_physics();
        const EntityId marker = markers(origin, size);
        if (hovered && ImGui::IsMouseClicked(0) && !ImGuizmo::IsOver() && !gizmo_active)
            session.select(marker ? marker : session.pick(*frame, (io.MousePos.x-origin.x) / size.x * 2-1, 1-(io.MousePos.y-origin.y) / size.y * 2));
        auto *draw = ImGui::GetWindowDrawList();
        draw->AddText({origin.x+12,origin.y+10}, IM_COL32(210,220,235,255), "RMB orbit  |  MMB pan  |  Wheel zoom  |  F focus  |  Click select");
        if (playing()) {
            draw->AddRect(origin, ImVec2(origin.x+size.x, origin.y+size.y), IM_COL32(115,215,115,255), 0.0f, ImDrawFlags_None, 3.0f);
            draw->AddText({origin.x+12,origin.y+28}, IM_COL32(115,215,115,255),
                session.play_state() == editor::PlayState::Paused ? "PLAY MODE (paused) - changes are discarded on Stop"
                                                                  : "PLAY MODE - changes are discarded on Stop");
            std::string view = "Viewing through the editor camera (no primary camera)";
            if (const auto camera = session.scene().inspect(session.view_camera())) view = "Viewing through camera '" + camera->name + "'";
            if (!session.view_camera_issue().empty()) view = session.view_camera_issue() + "; using the editor camera";
            draw->AddText({origin.x+12,origin.y+46}, IM_COL32(115,215,115,255), view.c_str());
        }
        if (!playing() && frame->lights.empty())
            draw->AddText({origin.x+12,origin.y+size.y-24}, IM_COL32(150,160,175,255), "No lights in the scene: editor lighting is used");
        ImGui::End();
    }
    // Screen position of a world point in the current frame; empty behind the camera.
    std::optional<ImVec2> project(glm::dvec3 p, ImVec2 origin, ImVec2 size) const {
        const auto clip = glm::make_mat4(frame->camera.projection.matrix.data()) * glm::make_mat4(frame->camera.view.matrix.data()) * glm::dvec4(p, 1);
        if (clip.w <= 1e-6) return std::nullopt;
        return ImVec2(origin.x + float((clip.x / clip.w + 1) / 2) * size.x, origin.y + float((1 - clip.y / clip.w) / 2) * size.y);
    }
    // Collider wireframes from the physics bridge, drawn over the image without depth testing (contract C-4a).
    // An edge with an end behind the camera is skipped rather than clipped.
    void collider_outlines(ImVec2 origin, ImVec2 size) {
        auto *draw = ImGui::GetWindowDrawList();
        for (const auto &line : session.physics_bridge().debug_geometry().lines) {
            const auto a = project({line.a.x, line.a.y, line.a.z}, origin, size), b = project({line.b.x, line.b.y, line.b.z}, origin, size);
            if (!a || !b) continue;
            const auto c = physics::default_color(line.category);
            draw->AddLine(*a, *b, ImGui::ColorConvertFloat4ToU32({c[0], c[1], c[2], c[3]}));
        }
    }
    // Lights and cameras have no mesh, so they get clickable screen-space markers. Returns the hovered one.
    EntityId markers(ImVec2 origin, ImVec2 size) {
        const auto project = [&](glm::dvec3 p) { return this->project(p, origin, size); };
        auto *draw = ImGui::GetWindowDrawList();
        const auto mouse = ImGui::GetIO().MousePos;
        EntityId hovered = 0;
        for (const auto &e : session.scene().snapshot().entities) {
            if (!e.components.light && !e.components.camera) continue;
            if (e.id == session.view_camera()) continue; // The camera being looked through sits at the eye.
            const auto m = glm::make_mat4(session.scene().world_transform(e.id)->matrix.data());
            const glm::dvec3 at(m[3]);
            const auto screen = project(at);
            if (!screen) continue;
            const bool selected = e.id == session.selection();
            const ImU32 color = e.components.light ? IM_COL32(250,215,90,255) : IM_COL32(110,200,240,255);
            if (e.components.light && e.components.light->type == LightType::Directional)
                if (const auto tip = project(at - glm::normalize(glm::dvec3(m[2])))) draw->AddLine(*screen, *tip, color, 2);
            if (e.components.light) draw->AddCircleFilled(*screen, 7, color);
            else draw->AddRectFilled({screen->x-8, screen->y-6}, {screen->x+8, screen->y+6}, color);
            if (selected) draw->AddCircle(*screen, 11, IM_COL32(255,255,255,255), 0, 2);
            draw->AddText({screen->x+11, screen->y-7}, color, e.name.c_str());
            const float dx = mouse.x - screen->x, dy = mouse.y - screen->y;
            if (dx*dx + dy*dy <= 10*10) hovered = e.id;
        }
        return hovered;
    }
    void assets() {
        ImGui::Begin("Asset Browser");
        ImGui::TextDisabled("Built-in meshes");
        if (ImGui::Button("Cube")) spawn("Cube", {MeshComponent{BuiltinMesh::Cube}, {}, {}});
        ImGui::SameLine(); if (ImGui::Button("Sphere")) spawn("Sphere", {MeshComponent{BuiltinMesh::Sphere}, {}, {}});
        ImGui::SameLine(); if (ImGui::Button("Plane")) spawn("Plane", {MeshComponent{BuiltinMesh::Plane}, {}, {}}, {{0,0,0}, {}, {10,1,10}});
        ImGui::TextDisabled("Scene files in current scene folder");
        auto directory = std::filesystem::path(scene_path.data()).parent_path();
        if (directory.empty()) directory = ".";
        std::error_code error;
        for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
            if (it->path().extension() != ".scene") continue;
            const auto path = it->path().string();
            if (ImGui::Selectable(it->path().filename().string().c_str())) std::snprintf(scene_path.data(), scene_path.size(), "%s", path.c_str());
        }
        if (error) ImGui::TextWrapped("Cannot browse folder: %s", error.message().c_str());
        ImGui::End();
    }
    void draw() {
        docking(); toolbar(); hierarchy(); inspector(); viewport(); assets();
        ImGui::Begin("Console");
        for (const auto &m : messages) ImGui::TextWrapped("%s", m.c_str());
        ImGui::End();
        confirm();
        const auto &io = ImGui::GetIO();
        if (!io.WantTextInput && !gizmo_active && !ImGui::IsPopupOpen("Unsaved changes")) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) report(io.KeyShift ? session.redo() : session.undo(), io.KeyShift ? "Redo" : "Undo");
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) report(session.redo(), "Redo");
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) report(session.save(scene_path.data()), "Scene saved");
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N)) request(Pending::NewScene);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicate_selection();
            if (!io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) delete_selection();
        }
    }
};
}
int main(int argc, char **argv) {
    try {
        const bool smoke = argc > 1 && std::string_view(argv[1]) == "--smoke";
        Host host; host.start();
        if (smoke) ImGui::GetIO().IniFilename = nullptr;
        editor::EditorSession session;
        auto created = session.create("Cube");
        if (!created) throw std::runtime_error(created.error().message);
        session.mark_clean(); // The starter cube is not a user change.
        const auto cube = session.selection();
        renderer::DesktopRenderer renderer(glfwGetWin32Window(host.window), EDITOR_MATERIAL_DIR);
        renderer.resize_viewport(800, 600);
        EditorUI ui(session, renderer);
        int frame = 0, rendered = 0;
        SceneSnapshot before_play;
        std::uint64_t revision_before_play = 0;
        EntityId probe = 0;
        std::array<int, 3> probe_color{}, lit_color{};
        std::string shown_title;
        auto last = std::chrono::steady_clock::now();
        while (!ui.quit) {
            glfwPollEvents();
            if (glfwWindowShouldClose(host.window)) {
                if (smoke) break;
                // Closing goes through the unsaved-changes prompt; restore first so a minimized window shows it.
                glfwSetWindowShouldClose(host.window, GLFW_FALSE);
                if (glfwGetWindowAttrib(host.window, GLFW_ICONIFIED)) glfwRestoreWindow(host.window);
                ui.request(Pending::Quit);
                if (ui.quit) break;
            }
            const auto now = std::chrono::steady_clock::now();
            session.update(now - last); last = now;
            if (const auto title = ui.title(); title != shown_title) { glfwSetWindowTitle(host.window, title.c_str()); shown_title = title; }
            int width, height; glfwGetFramebufferSize(host.window, &width, &height);
            if (width <= 0 || height <= 0) { glfwWaitEventsTimeout(0.05); continue; }
            ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame(); ImGuizmo::BeginFrame();
            ui.draw(); ImGui::Render();
            if (!ui.frame) { auto f = session.frame(4.0 / 3); if (f) ui.frame = f.value(); }
            if (ui.frame && renderer.render(*ui.frame, ImGui::GetDrawData(), width, height)) ++rendered;
            if (smoke) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                if (frame == 15) {
                    Transform t; t.position = {1.5,0.75,0}; t.scale = {1.5,1.5,1.5};
                    if (!session.execute(commands::SetTransform{cube,t})) throw std::runtime_error("Smoke transform failed");
                }
                if (frame == 25 && !session.undo()) throw std::runtime_error("Smoke undo failed");
                if (frame == 35 && !session.redo()) throw std::runtime_error("Smoke redo failed");
                if (frame == 45) {
                    if (!session.save("smoke.scene") || !session.load("smoke.scene")) throw std::runtime_error("Smoke persistence failed");
                    session.select(cube); session.focus();
                    glfwSetWindowSize(host.window, 1280, 800);
                }
                if (frame == 65) glfwIconifyWindow(host.window);
                if (frame == 65) { glfwPollEvents(); glfwRestoreWindow(host.window); }
                if (frame == 70) {
                    before_play = session.editor_scene().snapshot(); revision_before_play = session.commands().revision();
                    if (!session.play()) throw std::runtime_error("Smoke play failed");
                }
                if (frame == 72) {
                    Transform t; t.position = {-2,0.5,0};
                    if (!session.execute(commands::SetTransform{cube,t}) || !session.create("RuntimeOnly")) throw std::runtime_error("Smoke runtime edit failed");
                }
                if (frame == 80) {
                    if (!session.runtime_ticks() || renderer.object_count() != 2) throw std::runtime_error("Smoke runtime scene not ticking or not rendered");
                    if (!session.stop()) throw std::runtime_error("Smoke stop failed");
                    if (session.editor_scene().snapshot() != before_play || session.commands().revision() != revision_before_play)
                        throw std::runtime_error("Smoke play mode changed the editor scene");
                }
                // Surface color and scene lights reach the GPU: read the viewport center back and check its hue.
                const auto center_pixel = [&] {
                    const auto pixels = renderer.readback(width, height);
                    const auto scale = ImGui::GetIO().DisplayFramebufferScale;
                    const auto x = std::clamp(int(ui.viewport_center.x * scale.x), 0, width - 1), y = std::clamp(int(ui.viewport_center.y * scale.y), 0, height - 1);
                    const auto *p = pixels.data() + (std::size_t(y) * width + x) * 4;
                    return std::array<int, 3>{p[0], p[1], p[2]};
                };
                const auto dominant = [](const std::array<int, 3> &c, int channel) {
                    return c[channel] > 60 && c[channel] > 3 * c[(channel + 1) % 3] && c[channel] > 3 * c[(channel + 2) % 3];
                };
                if (frame == 82) {
                    Transform t; t.position = {-3,0.5,0};
                    auto r = session.execute(commands::CreateEntity{"Probe", t, 0, 0, {MeshComponent{BuiltinMesh::Cube, {1,0,0}, 0.9, 0}, {}, {}}});
                    if (!r) throw std::runtime_error("Smoke probe creation failed");
                    probe = r.value().created.front();
                    session.select(probe); session.focus(); session.select(0); // No gizmo over the sampled pixel.
                }
                if (frame == 86 && !dominant(probe_color = center_pixel(), 0))
                    throw std::runtime_error("Smoke: red surface not visible under editor lighting");
                if (frame == 87) {
                    // White surface lit only by a blue scene light shining along the view direction.
                    const auto q = glm::quat_cast(glm::make_mat4(session.camera.world().matrix.data()));
                    Transform t; t.rotation = {q.x, q.y, q.z, q.w};
                    if (!session.execute(commands::SetMesh{probe, MeshComponent{BuiltinMesh::Cube, {1,1,1}, 0.9, 0}}) ||
                        !session.execute(commands::CreateEntity{"Blue light", t, 0, 0, {{}, {}, LightComponent{LightType::Directional, {0,0,1}, 100000, 10}}}))
                        throw std::runtime_error("Smoke component edit failed");
                }
                if (frame == 91 && (!dominant(lit_color = center_pixel(), 2) || renderer.light_count() != 1))
                    throw std::runtime_error("Smoke: scene light did not replace editor lighting");
                if (++frame == 100) {
                    const auto meshes = std::ranges::count_if(session.scene().snapshot().entities, [](const EntityRecord &e) { return e.components.mesh.has_value(); });
                    if (rendered < 20 || renderer.object_count() != std::size_t(meshes)) throw std::runtime_error("Smoke renderer did not reconcile: rendered=" + std::to_string(rendered) + " objects=" + std::to_string(renderer.object_count()));
                    renderer.capture("editor-smoke.ppm", width, height);
                    std::cout << "GPU smoke passed: " << rendered << " frames; transform, undo/redo, save/load, play/stop isolation, resize, "
                                 "surface color (" << probe_color[0] << ',' << probe_color[1] << ',' << probe_color[2] << "), scene light ("
                              << lit_color[0] << ',' << lit_color[1] << ',' << lit_color[2] << ") and capture.\n";
                    break;
                }
            } else glfwWaitEventsTimeout(0.005);
        }
        if (smoke && frame != 100) throw std::runtime_error("Smoke run closed before completing all checks");
        std::cout << "Editor stopped cleanly.\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "Editor failed: " << e.what() << '\n'; return 1; }
}
