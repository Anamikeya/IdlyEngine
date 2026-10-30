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
#include "GameHud.hpp"
#include "RenderTest.hpp"
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
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace engine;
struct Host {
    GLFWwindow *window = nullptr;
    bool glfw = false, imgui = false, backend = false;
    ImGuiStyle base_style; // Unscaled.
    float font_scale = 1, ui_scale = 0;
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
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        // 1440x900 at the monitor's scale, kept within its work area.
        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        float scale = 1, unused = 1;
        int area_x = 0, area_y = 0, area_width = 1440, area_height = 900;
        if (monitor) { glfwGetMonitorContentScale(monitor, &scale, &unused); glfwGetMonitorWorkarea(monitor, &area_x, &area_y, &area_width, &area_height); }
        const float fit = std::min({std::max(scale, 1.f), 0.9f * float(area_width) / 1440, 0.9f * float(area_height) / 900});
        const int window_width = std::max(int(1440 * fit), 640), window_height = std::max(int(900 * fit), 400);
        window = glfwCreateWindow(window_width, window_height, "GameEngine Editor", nullptr, nullptr);
        if (!window) throw std::runtime_error("Window creation failed");
        if (monitor) glfwSetWindowPos(window, area_x + std::max(area_width - window_width, 0) / 2, area_y + std::max(area_height - window_height, 0) / 2);
        glfwShowWindow(window);
        IMGUI_CHECKVERSION(); ImGui::CreateContext(); imgui = true;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        auto &style = ImGui::GetStyle();
        style.WindowRounding = 3; style.FrameRounding = 3; style.FramePadding = {6,5};
        style.Colors[ImGuiCol_WindowBg] = {0.075f,0.085f,0.105f,1};
        style.Colors[ImGuiCol_Header] = {0.16f,0.27f,0.39f,1};
        style.Colors[ImGuiCol_Button] = {0.13f,0.23f,0.34f,1};
        base_style = style;
        // The renderer uploads the font atlas once, so the font is built for the scale the editor opens at;
        // follow_monitor() scales it on another monitor.
        font_scale = std::max(scale, 1.f);
        ImFontConfig font;
        font.SizePixels = 13 * font_scale;
        if (font_scale > 1) ImGui::GetIO().Fonts->AddFontDefaultVector(&font); else ImGui::GetIO().Fonts->AddFontDefaultBitmap(&font);
        follow_monitor();
        backend = ImGui_ImplGlfw_InitForOther(window, true);
        if (!backend) throw std::runtime_error("ImGui GLFW initialization failed");
    }
    // Between frames: sizes and text follow the content scale of the monitor the window is on.
    void follow_monitor() {
        float scale = 0, unused = 0;
        glfwGetWindowContentScale(window, &scale, &unused);
        if (scale <= 0 || scale == ui_scale) return;
        ui_scale = scale;
        auto &style = ImGui::GetStyle();
        style = base_style;
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale / font_scale;
    }
};
// UTF-8 text from ImGui or GLFW to a path.
std::filesystem::path utf8_path(std::string_view text) { return std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()); }
// Files dropped on the window, imported on the next frame.
std::vector<std::filesystem::path> dropped_files;
// Actions that discard the editor scene ask first when it has unsaved changes.
enum class Pending { None, NewScene, Load, Quit, OpenSample, NewProject, OpenProject };
const std::filesystem::path sample_project = std::filesystem::path(ENGINE_SAMPLES_DIR) / "PhysicsShooter";
constexpr const char *sample_scene = "physics_shooter.scene";
// The device side of the game's input: whether the game holds the mouse, and what the keys are doing.
struct GameInput {
    gameplay::InputMap map = gameplay::InputMap::shooter_defaults();
    bool captured = false;
    double last_x = 0, last_y = 0;
    void capture(GLFWwindow *window) {
        if (captured) return;
        captured = true;
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        glfwGetCursorPos(window, &last_x, &last_y);
        auto &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    }
    void release(GLFWwindow *window) {
        if (!captured) return;
        captured = false;
        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        map.release_all();
        auto &io = ImGui::GetIO();
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    }
    // Once per frame while the game holds the mouse: devices to actions, mouse to aim.
    void feed(GLFWwindow *window, gameplay::Game &game) {
        using namespace gameplay::code;
        for (const int key : {w, a, s, d, r, space, left_shift}) map.set(key, glfwGetKey(window, key) == GLFW_PRESS);
        map.set(mouse_left, glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
        double x = 0, y = 0;
        glfwGetCursorPos(window, &x, &y);
        game.look(x - last_x, y - last_y);
        last_x = x; last_y = y;
        game.set_input(map.take());
    }
};
// A length in pixels at 100% scale, in pixels at the current scale.
float px(float length) { return length * ImGui::GetFontSize() / 13; }
// Euler angles from a quaternion carry rounding noise, which would read as -0.000.
float tidy_degrees(double degrees) { return std::abs(degrees) < 0.0005 ? 0.f : float(degrees); }
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
    std::set<std::string> reported_asset_problems; // The same for asset references and the asset folder.
    std::array<char, 1024> import_path{};
    assets::AssetId selected_asset;
    EntityId gizmo_id = 0;
    float gizmo_matrix[16]{};
    renderer::Frame frame;
    ImVec2 viewport_center{}, viewport_size{}; // Window coordinates; used by the GPU smoke readback checks.
    GameInput &game_input;
    GLFWwindow *window;
    assets::MaterialData material_edit; // The Material Editor's working copy of the chosen material.
    assets::AssetId material_edit_id;
    std::array<char, 128> new_material_name{};
    std::array<char, 1024> project_path{};
    bool project_prompt = false, project_create = true;
    std::uint64_t preview_token = 0;
    explicit EditorUI(editor::EditorSession &s, renderer::DesktopRenderer &r, GameInput &g, GLFWwindow *w) : session(s), renderer(r), game_input(g), window(w) {
        std::snprintf(scene_path.data(), scene_path.size(), "%s", "Untitled.scene");
        std::snprintf(new_material_name.data(), new_material_name.size(), "%s", "New Material");
    }
    bool scroll_console = false;
    void log(std::string text) { if (messages.size() == 128) messages.erase(messages.begin()); messages.push_back(std::move(text)); scroll_console = true; }
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
    // A box around a model, in its local space. Falls back to the built-in shape without one.
    ColliderComponent fitted_collider(const std::optional<MeshComponent> &mesh) {
        if (!mesh) return fitted_collider(BuiltinMesh::Cube);
        if (const auto fitted = session.model_collider(mesh->mesh_asset)) return *fitted;
        return fitted_collider(mesh->mesh_asset.empty() ? mesh->mesh : BuiltinMesh::Cube);
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
    std::string title() const {
        std::string t = "GameEngine Editor - " + std::filesystem::path(scene_path.data()).filename().string();
        if (session.dirty()) t += '*';
        if (session.play_state() == editor::PlayState::Playing) t += " [Playing]";
        if (session.play_state() == editor::PlayState::Paused) t += " [Paused]";
        return t;
    }
    LightComponent point_light() const { return LightComponent{LightType::Point, {1,1,1}, editor::EditorSession::point_light_lumens(session.environment().ev100), 10}; }
    void set_scene_path(const std::filesystem::path &path) { std::snprintf(scene_path.data(), scene_path.size(), "%s", path.string().c_str()); }
    void open_sample() {
        if (!report(session.open_project(sample_project), "Opened the Physics Shooter sample project")) return;
        set_scene_path(sample_project / sample_scene);
        report(session.load(scene_path.data()), "Scene loaded. Press Play, then click the view to start.");
    }
    // A folder with an Assets folder and an empty scene: nothing of the sample comes along.
    void create_project(const std::filesystem::path &folder) {
        std::error_code error;
        std::filesystem::create_directories(folder / "Assets", error);
        if (error) { log("Error: Cannot create the project folder: " + error.message()); return; }
        if (!report(session.open_project(folder), "Created an empty project")) return;
        if (!report(session.new_scene(), "New scene")) return;
        set_scene_path(folder / "main.scene");
        report(session.save(scene_path.data()), "Scene saved");
    }
    void open_project(const std::filesystem::path &folder) {
        if (!report(session.open_project(folder), "Project opened")) return;
        set_scene_path(folder / "main.scene");
        std::error_code error;
        if (std::filesystem::is_regular_file(scene_path.data(), error)) report(session.load(scene_path.data()), "Scene loaded (undoable)");
    }
    void perform(Pending action) {
        if (action == Pending::NewScene && report(session.new_scene(), "New scene"))
            std::snprintf(scene_path.data(), scene_path.size(), "%s", "Untitled.scene");
        if (action == Pending::Load) report(session.load(scene_path.data()), "Scene loaded (undoable)");
        if (action == Pending::OpenSample) open_sample();
        if (action == Pending::NewProject) create_project(utf8_path(project_path.data()));
        if (action == Pending::OpenProject) open_project(utf8_path(project_path.data()));
        if (action == Pending::Quit) quit = true;
    }
    void project_dialog() {
        if (std::exchange(project_prompt, false)) ImGui::OpenPopup("Project folder");
        if (!ImGui::BeginPopupModal("Project folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
        ImGui::TextUnformatted(project_create ? "Create an empty project in this folder:" : "Open the project in this folder:");
        ImGui::SetNextItemWidth(px(480)); ImGui::InputText("##project", project_path.data(), project_path.size());
        ImGui::BeginDisabled(!project_path[0]);
        if (ImGui::Button(project_create ? "Create" : "Open")) { ImGui::CloseCurrentPopup(); request(project_create ? Pending::NewProject : Pending::OpenProject); }
        ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
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
        const char *verb = pending == Pending::Quit ? "closing" : pending == Pending::Load ? "loading" : pending == Pending::NewScene ? "starting a new scene" : "opening another project";
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
            ImGui::DockBuilderDockWindow("Lighting", right);
            ImGui::DockBuilderDockWindow("Material Editor", right);
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
                if (ImGui::MenuItem("Point Light")) spawn("Point Light", {{}, {}, point_light()}, above, id);
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
            // A behavior-driven entity's tag is left off: the Inspector lists them.
            ImGui::TextDisabled("%s%s%s", c.light ? (c.light->type == LightType::Point ? " point light" : c.light->type == LightType::Spot ? " spot light" : " directional light") : "",
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
        ImGui::InvisibleButton("##hierarchy-root", {std::max(room.x, 1.f), std::max(room.y, px(24))});
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
            for (int i = 0; i < 3; ++i) rotation[i] = tidy_degrees(euler[i]);
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
    // Lists the project's assets of one type. True when the reference changed.
    bool asset_combo(const char *label, AssetRef &ref, assets::AssetType type, const char *none, bool whiten = true) {
        bool changed = false;
        if (ImGui::BeginCombo(label, ref.empty() ? none : ref.path.c_str())) {
            if (ImGui::Selectable(none, ref.empty())) { ref = {}; changed = true; }
            for (const auto &meta : session.assets()) {
                if (meta.type != type) continue;
                if (ImGui::Selectable(meta.path.c_str(), ref.id == meta.id.value)) {
                    ref = {meta.id.value, meta.path}; changed = true;
                    if (whiten) components.mesh->color = {1, 1, 1};
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }
    bool remove_button(const char *id) {
        ImGui::PushID(id); const bool clicked = ImGui::SmallButton("Remove"); ImGui::PopID(); return clicked;
    }
    void component_sections(const EntityRecord &record) {
        auto &c = components;
        if (c.mesh) {
            ImGui::SeparatorText("Mesh Renderer");
            auto &m = *c.mesh; bool commit = false;
            commit |= asset_combo("Model", m.mesh_asset, assets::AssetType::Mesh, "(built-in mesh)");
            if (m.mesh_asset.empty()) {
                int shape = int(m.mesh) - 1;
                if (ImGui::Combo("Mesh", &shape, "Cube\0Sphere\0Plane\0")) { m.mesh = BuiltinMesh(shape + 1); commit = true; }
            }
            commit |= asset_combo("Material", m.material, assets::AssetType::Material, m.mesh_asset.empty() ? "(none)" : "(from the model)");
            commit |= asset_combo("Texture", m.texture, assets::AssetType::Texture, m.mesh_asset.empty() ? "(none)" : "(from the model)");
            commit |= color_edit("Color", m.color);
            const auto model = session.model(m.mesh_asset);
            if (!m.material.empty()) ImGui::TextDisabled("Roughness and metallic come from the material.");
            else if (model && !model->submeshes.empty()) ImGui::TextDisabled("Roughness and metallic come from the model.");
            else {
                commit |= drag("Roughness", m.roughness, 0.01f, 0, 1);
                commit |= drag("Metallic", m.metallic, 0.01f, 0, 1);
            }
            float uv[2]{float(m.uv_scale_u), float(m.uv_scale_v)};
            if (ImGui::DragFloat2("UV scale", uv, 0.01f, 0.001f, 1000, "%.3f", ImGuiSliderFlags_AlwaysClamp)) { m.uv_scale_u = uv[0]; m.uv_scale_v = uv[1]; }
            commit |= ImGui::IsItemDeactivatedAfterEdit();
            // A model's parts can each take a material.
            if (const auto parts = session.mesh_part_count(m); parts > 1) {
                ImGui::TextDisabled("Materials per part (%zu parts)", parts);
                for (std::uint32_t part = 0; part < parts && part < max_material_slots; ++part) {
                    ImGui::PushID(int(part));
                    AssetRef chosen;
                    const auto slot = std::find_if(m.material_slots.begin(), m.material_slots.end(), [&](const MaterialSlot &s) { return s.slot == part; });
                    if (slot != m.material_slots.end()) chosen = slot->material;
                    if (asset_combo(("Part " + std::to_string(part)).c_str(), chosen, assets::AssetType::Material, "(default)", false)) {
                        report(session.assign_material(record.id, assets::AssetId{chosen.id}, part), "Part material changed");
                        draft_revision = ~uint64_t(0);
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
            }
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
            if (ImGui::Combo("Type", &type, "Directional\0Point\0Spot\0")) { l.type = LightType(type + 1); commit = true; }
            commit |= color_edit("Light color", l.color);
            commit |= drag(l.type == LightType::Directional ? "Intensity (lux)" : "Intensity (lm)", l.intensity, 500.0f, 0, 1e7, "%.0f");
            if (l.type != LightType::Directional) commit |= drag("Range (m)", l.range, 0.05f, 0.01, 1000);
            if (l.type == LightType::Spot) {
                commit |= drag("Inner cone (deg)", l.inner_cone_degrees, 0.2f, 0, l.outer_cone_degrees, "%.1f");
                commit |= drag("Outer cone (deg)", l.outer_cone_degrees, 0.2f, l.inner_cone_degrees, 90, "%.1f");
            }
            if (l.type != LightType::Point) ImGui::TextDisabled("Shines along local -Z.");
            commit |= ImGui::Checkbox("Cast shadows", &l.cast_shadows);
            if (commit) apply(commands::SetLight{record.id, l}, "Light changed");
            if (remove_button("light")) apply(commands::SetLight{record.id, std::nullopt}, "Light removed");
        }
        if (c.environment) {
            ImGui::SeparatorText("Environment");
            ImGui::TextDisabled("Edited in the Lighting panel.");
            if (remove_button("environment")) apply(commands::SetEnvironment{record.id, std::nullopt}, "Environment removed");
        }
        physics_sections(record);
        behavior_sections(record);
        ImGui::Spacing();
        if (ImGui::Button("Add Component")) ImGui::OpenPopup("Add Component");
        if (ImGui::BeginPopup("Add Component")) {
            if (ImGui::MenuItem("Mesh Renderer", nullptr, false, !c.mesh)) apply(commands::SetMesh{record.id, MeshComponent{}}, "Mesh renderer added");
            if (ImGui::MenuItem("Camera", nullptr, false, !c.camera)) apply(commands::SetCamera{record.id, CameraComponent{}}, "Camera added");
            if (ImGui::MenuItem("Light", nullptr, false, !c.light)) apply(commands::SetLight{record.id, LightComponent{}}, "Light added");
            if (ImGui::MenuItem("Rigid Body", nullptr, false, !c.body)) apply(commands::SetRigidBody{record.id, RigidBodyComponent{}}, "Rigid body added");
            if (ImGui::MenuItem("Collider (fitted to mesh)", nullptr, false, c.colliders.size() < max_colliders_per_entity)) {
                auto list = c.colliders;
                list.push_back(fitted_collider(c.mesh));
                apply(commands::SetColliders{record.id, std::move(list)}, "Collider added");
            }
            if (ImGui::BeginMenu("Behavior")) {
                for (const auto &behavior : gameplay::behavior_templates()) {
                    const bool present = std::ranges::any_of(c.behaviors, [&](const BehaviorComponent &b) { return b.type == behavior.type; });
                    if (ImGui::MenuItem(behavior.type.c_str(), nullptr, false, !present && c.behaviors.size() < max_behaviors_per_entity)) {
                        auto list = c.behaviors;
                        list.push_back(behavior);
                        apply(commands::SetBehaviors{record.id, std::move(list)}, "Behavior added");
                    }
                }
                ImGui::EndMenu();
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
            commit |= ImGui::Checkbox("Lock rotation", &b.lock_rotation);
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
                float r[3]{tidy_degrees(euler.x), tidy_degrees(euler.y), tidy_degrees(euler.z)};
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
    // Named numbers read by runtime systems when Play starts; the scene gives them no meaning, so they edit generically.
    void behavior_sections(const EntityRecord &record) {
        auto &c = components;
        for (std::size_t i = 0; i < c.behaviors.size(); ++i) {
            ImGui::PushID(int(1000 + i));
            ImGui::SeparatorText(("Behavior: " + c.behaviors[i].type).c_str());
            bool commit = false;
            for (auto &[key, value] : c.behaviors[i].values) commit |= drag(key.c_str(), value, 0.05f, -1e9, 1e9);
            if (commit) apply(commands::SetBehaviors{record.id, c.behaviors}, "Behavior changed");
            const bool removed = remove_button("behavior");
            ImGui::PopID();
            if (removed) {
                auto list = c.behaviors;
                list.erase(list.begin() + std::ptrdiff_t(i));
                apply(commands::SetBehaviors{record.id, std::move(list)}, "Behavior removed");
                break;
            }
        }
    }
    void lighting() {
        ImGui::Begin("Lighting");
        auto e = session.environment();
        const auto owner = session.environment_entity();
        if (owner) ImGui::TextDisabled("On entity '%s'", session.scene().inspect(owner)->name.c_str());
        else ImGui::TextWrapped("This scene has no environment, so the defaults below apply. Changing anything adds an Environment entity.");
        if (playing()) ImGui::TextColored({0.45f, 0.85f, 0.45f, 1}, "Play mode: edits affect the runtime copy only");
        ImGui::SeparatorText("Presets");
        if (ImGui::Button("Outdoor")) { report(session.set_environment(editor::EditorSession::outdoor_environment()), "Outdoor lighting applied"); return ImGui::End(); }
        ImGui::SameLine();
        if (ImGui::Button("Indoor")) { report(session.set_environment(editor::EditorSession::indoor_environment()), "Indoor lighting applied"); return ImGui::End(); }
        ImGui::SameLine();
        if (ImGui::Button("Add sun")) report(session.create_preset_lights(true), "Sun created");
        ImGui::SameLine();
        if (ImGui::Button("Add key light")) report(session.create_preset_lights(false), "Key light created");
        bool commit = false;
        ImGui::SeparatorText("Environment");
        int source = int(e.source);
        if (ImGui::Combo("Source", &source, "None\0Procedural sky\0HDR image\0")) { e.source = EnvironmentSource(source); commit = true; }
        if (e.source == EnvironmentSource::Image) {
            bool chosen = false;
            if (ImGui::BeginCombo("Image", e.image.empty() ? "(choose an .hdr)" : e.image.path.c_str())) {
                for (const auto &meta : session.assets())
                    if (meta.type == assets::AssetType::Environment && ImGui::Selectable(meta.path.c_str(), e.image.id == meta.id.value)) { e.image = {meta.id.value, meta.path}; chosen = true; }
                ImGui::EndCombo();
            }
            commit |= chosen;
            if (e.image.empty()) { commit = false; ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "Import an .hdr image, then choose it here."); }
        }
        if (e.source == EnvironmentSource::Procedural) {
            commit |= color_edit("Sky", e.sky_color);
            commit |= color_edit("Horizon", e.horizon_color);
            commit |= color_edit("Ground", e.ground_color);
        }
        commit |= drag("Ambient intensity", e.intensity, 100.0f, 0, 200000, "%.0f");
        commit |= drag("Rotation (deg)", e.rotation_degrees, 1.0f, -360, 360, "%.0f");
        commit |= ImGui::Checkbox("Show skybox", &e.show_skybox);
        ImGui::SeparatorText("Camera");
        commit |= drag("Exposure (EV100)", e.ev100, 0.05f, -6, 20, "%.2f");
        int tone = int(e.tone_mapping) - 1;
        if (ImGui::Combo("Tone mapping", &tone, "ACES\0Filmic\0AgX\0PBR Neutral\0Linear\0")) { e.tone_mapping = ToneMapping(tone + 1); commit = true; }
        commit |= ImGui::Checkbox("Ambient occlusion", &e.ambient_occlusion);
        ImGui::SameLine(); commit |= ImGui::Checkbox("Bloom", &e.bloom);
        ImGui::SeparatorText("Shadows");
        int quality = int(e.shadow_quality);
        if (ImGui::Combo("Quality", &quality, "Off\0Low\0Medium\0High\0")) { e.shadow_quality = ShadowQuality(quality); commit = true; }
        commit |= drag("Distance (m)", e.shadow_distance, 1.0f, 1, 1000, "%.0f");
        ImGui::TextDisabled("Each light has its own Cast shadows box in the Inspector.");
        if (commit) report(session.set_environment(e), "Lighting changed");
        ImGui::End();
    }
    // One texture slot of the material: a combo of the project's textures. True when it changed.
    bool texture_slot(const char *label, assets::MaterialTexture &slot) {
        bool changed = false;
        if (ImGui::BeginCombo(label, slot.empty() ? "(none)" : slot.path.c_str())) {
            if (ImGui::Selectable("(none)", slot.empty())) { slot = {}; changed = true; }
            for (const auto &meta : session.assets())
                if (meta.type == assets::AssetType::Texture && ImGui::Selectable(meta.path.c_str(), slot.id == meta.id)) { slot = {meta.id, meta.path}; changed = true; }
            ImGui::EndCombo();
        }
        return changed;
    }
    void material_editor() {
        const bool open = ImGui::Begin("Material Editor");
        const auto catalogue = session.assets();
        const auto chosen = std::find_if(catalogue.begin(), catalogue.end(), [&](const assets::AssetMetadata &a) { return a.id == selected_asset && a.type == assets::AssetType::Material; });
        if (chosen == catalogue.end() || !open) {
            if (open) ImGui::TextWrapped("Choose a material in the Asset Browser, or create one there.");
            renderer.set_material_preview({});
            ImGui::End();
            return;
        }
        // Re-read unless a field is being dragged: undo and other sources may have changed the file.
        if (material_edit_id != chosen->id || !ImGui::IsAnyItemActive()) {
            if (const auto current = session.material(chosen->id)) { material_edit = *current; material_edit_id = chosen->id; }
            else { ImGui::TextColored({0.95f, 0.45f, 0.35f, 1}, "This material cannot be read: its file is missing or invalid."); renderer.set_material_preview({}); ImGui::End(); return; }
        }
        ImGui::TextDisabled("%s", chosen->path.c_str());
        renderer.set_material_preview(session.material_handle(chosen->id));
        const float preview = std::min(ImGui::GetContentRegionAvail().x, px(150));
        ImGui::Image(ImTextureRef(renderer.preview_texture()), {preview, preview}, {0,1}, {1,0});
        auto &m = material_edit;
        bool live = false, commit = false;
        const auto color4 = [&](const char *label, std::array<double, 4> &value) {
            float v[4]{float(value[0]), float(value[1]), float(value[2]), float(value[3])};
            const bool changed = ImGui::ColorEdit4(label, v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
            if (changed) for (int i = 0; i < 4; ++i) value[std::size_t(i)] = std::clamp(double(v[i]), 0.0, 1.0);
            live |= changed; commit |= committed(changed);
        };
        const auto color3 = [&](const char *label, std::array<double, 3> &value) {
            float v[3]{float(value[0]), float(value[1]), float(value[2])};
            const bool changed = ImGui::ColorEdit3(label, v, ImGuiColorEditFlags_Float);
            if (changed) for (int i = 0; i < 3; ++i) value[std::size_t(i)] = std::clamp(double(v[i]), 0.0, 1.0);
            live |= changed; commit |= committed(changed);
        };
        const auto number = [&](const char *label, double &value, float speed, double low, double high) {
            float v = float(value);
            const bool changed = ImGui::DragFloat(label, &v, speed, float(low), float(high), "%.3f", ImGuiSliderFlags_AlwaysClamp);
            if (changed) value = v;
            live |= changed; commit |= committed(changed);
        };
        ImGui::SeparatorText("Surface");
        color4("Base color", m.base_color);
        number("Metallic", m.metallic, 0.01f, 0, 1);
        number("Roughness", m.roughness, 0.01f, 0, 1);
        ImGui::SeparatorText("Emission");
        color3("Emissive", m.emissive);
        number("Strength", m.emissive_strength, 0.05f, 0, 1000);
        ImGui::SeparatorText("Alpha and sides");
        int alpha = int(m.alpha_mode);
        if (ImGui::Combo("Alpha mode", &alpha, "Opaque\0Mask\0Blend\0")) { m.alpha_mode = assets::AlphaMode(alpha); commit = true; }
        if (m.alpha_mode == assets::AlphaMode::Mask) number("Alpha cutoff", m.alpha_cutoff, 0.01f, 0, 1);
        if (ImGui::Checkbox("Double-sided", &m.double_sided)) commit = true;
        ImGui::SeparatorText("Textures");
        commit |= texture_slot("Base color map", m.base_color_map);
        commit |= texture_slot("Metallic-roughness map", m.metallic_roughness_map);
        commit |= texture_slot("Normal map", m.normal_map);
        number("Normal scale", m.normal_scale, 0.01f, 0, 10);
        commit |= texture_slot("Occlusion map", m.occlusion_map);
        number("Occlusion strength", m.occlusion_strength, 0.01f, 0, 1);
        commit |= texture_slot("Emissive map", m.emissive_map);
        float uv[2]{float(m.uv_scale[0]), float(m.uv_scale[1])};
        if (ImGui::DragFloat2("UV scale", uv, 0.01f, 0.001f, 1000, "%.3f", ImGuiSliderFlags_AlwaysClamp)) { m.uv_scale = {uv[0], uv[1]}; live = true; }
        commit |= ImGui::IsItemDeactivatedAfterEdit();
        if (commit) report(session.save_material(chosen->id, m), "Material saved");
        else if (live) { if (auto shown = session.preview_material(chosen->id, m); !shown) log("Error: " + shown.error().message); }
        ImGui::TextDisabled("Changes save to the file when a control is released; Ctrl+Z undoes them.");
        ImGui::End();
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
    // Puts asset problems in the Console once, when they appear.
    void report_assets() {
        std::set<std::string> current(session.asset_problems().begin(), session.asset_problems().end());
        for (const auto &issue : session.asset_issues()) current.insert("Asset folder: " + issue.path + ": " + issue.message);
        for (const auto &issue : renderer.take_issues()) log("Renderer: " + issue);
        for (const auto &message : current) if (!reported_asset_problems.contains(message)) log(message);
        reported_asset_problems = std::move(current);
    }
    // Decodes on a worker; the Console reports when it lands, with how long it took.
    std::map<std::string, std::chrono::steady_clock::time_point> import_started;
    void import_file(const std::filesystem::path &file) {
        import_started[file.string()] = std::chrono::steady_clock::now();
        session.begin_import(file);
        log("Importing " + file.filename().string() + "...");
    }
    void finish_imports() {
        for (const auto &finished : session.poll_imports()) {
            const auto started = import_started.find(finished.source.string());
            const auto took = started == import_started.end() ? 0 : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started->second).count();
            if (started != import_started.end()) import_started.erase(started);
            if (!finished.result) { log("Import failed: " + finished.result.error().message); continue; }
            const auto &asset = finished.result.value();
            log("Imported " + asset.path + " as a " + assets::to_string(asset.type) + " in " + std::to_string(took) + " ms");
            for (const auto &warning : asset.warnings) log("Warning: " + asset.path + ": " + warning);
            selected_asset = asset.id;
        }
    }
    void viewport() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0,0});
        const bool visible = ImGui::Begin("3D Viewport");
        ImGui::PopStyleVar();
        if (!visible) { ImGui::End(); return; }
        const auto origin = ImGui::GetCursorScreenPos();
        auto size = ImGui::GetContentRegionAvail(); size.x = std::max(size.x, 1.f); size.y = std::max(size.y, 1.f);
        viewport_center = {origin.x + size.x / 2, origin.y + size.y / 2}; viewport_size = size;
        const auto &io = ImGui::GetIO();
        renderer.resize_viewport(unsigned(size.x * io.DisplayFramebufferScale.x), unsigned(size.y * io.DisplayFramebufferScale.y));
        ImGui::Image(ImTextureRef(renderer.viewport_texture()), size, {0,1}, {1,0});
        const bool hovered = ImGui::IsItemHovered();
        auto *game = session.game();
        if (game && game_input.captured) session.select(0); // The view belongs to the player: no gizmo, no picking.
        if (hovered && !ImGuizmo::IsUsing() && !game) {
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
        if (const auto world = session.scene().world_transform(session.selection()); world && !game) {
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
        if (show_colliders && !game) collider_outlines(origin, size);
        report_physics();
        report_assets();
        const EntityId marker = game ? 0 : markers(origin, size);
        if (hovered && ImGui::IsMouseClicked(0) && !ImGuizmo::IsOver() && !gizmo_active && !game)
            session.select(marker ? marker : session.pick(*frame, (io.MousePos.x-origin.x) / size.x * 2-1, 1-(io.MousePos.y-origin.y) / size.y * 2));
        auto *draw = ImGui::GetWindowDrawList();
        const float margin = px(12), line = ImGui::GetTextLineHeightWithSpacing();
        if (game) {
            game_overlay(*game, origin, size, hovered);
            ImGui::End();
            return;
        }
        draw->AddText({origin.x+margin,origin.y+px(10)}, IM_COL32(210,220,235,255), "RMB orbit  |  MMB pan  |  Wheel zoom  |  F focus  |  Click select");
        if (playing()) {
            draw->AddRect(origin, ImVec2(origin.x+size.x, origin.y+size.y), IM_COL32(115,215,115,255), 0.0f, ImDrawFlags_None, px(3));
            draw->AddText({origin.x+margin,origin.y+px(10)+line}, IM_COL32(115,215,115,255),
                session.play_state() == editor::PlayState::Paused ? "PLAY MODE (paused) - changes are discarded on Stop"
                                                                  : "PLAY MODE - changes are discarded on Stop");
            std::string view = "Viewing through the editor camera (no primary camera)";
            if (const auto camera = session.scene().inspect(session.view_camera())) view = "Viewing through camera '" + camera->name + "'";
            if (!session.view_camera_issue().empty()) view = session.view_camera_issue() + "; using the editor camera";
            draw->AddText({origin.x+margin,origin.y+px(10)+2*line}, IM_COL32(115,215,115,255), view.c_str());
        }
        if (!playing() && frame->lights.empty())
            draw->AddText({origin.x+margin,origin.y+size.y-px(24)}, IM_COL32(150,160,175,255), "No lights in the scene: editor lighting is used");
        ImGui::End();
    }
    // The game's HUD, and the choices it offers: a click on the view starts or resumes and takes the mouse.
    void game_overlay(gameplay::Game &game, ImVec2 origin, ImVec2 size, bool hovered) {
        const auto hud = game.hud();
        const auto clicked_view = hovered && !game_input.captured && ImGui::IsMouseReleased(0);
        auto action = editor::draw_game_hud(hud, origin, size, game_input.captured);
        const bool on_button = ImGui::IsAnyItemHovered() && !hovered;
        if (action == editor::HudAction::None && clicked_view && !on_button) {
            if (hud.phase == gameplay::GamePhase::Ready) action = editor::HudAction::Start;
            if (hud.phase == gameplay::GamePhase::Paused) action = editor::HudAction::Resume;
            // Resumed from the toolbar, which left the mouse free: a click takes it back.
            if (hud.phase == gameplay::GamePhase::Playing) game_input.capture(window);
        }
        // The game ended: give the mouse back, so the buttons can be clicked.
        if ((hud.phase == gameplay::GamePhase::Won || hud.phase == gameplay::GamePhase::Lost) && game_input.captured) game_input.release(window);
        if ((hud.phase == gameplay::GamePhase::Won || hud.phase == gameplay::GamePhase::Lost) && !game_input.captured && ImGui::IsKeyPressed(ImGuiKey_R, false)) action = editor::HudAction::Restart;
        switch (action) {
        case editor::HudAction::Start:
            report(session.play(), "Game started"); // The toolbar's Pause may have frozen the world before the start.
            game.start(); game_input.capture(window);
            break;
        case editor::HudAction::Resume: report(session.play(), "Resumed"); game_input.capture(window); break;
        case editor::HudAction::Restart:
            if (report(session.restart(), "Restarted")) { if (auto *fresh = session.game()) fresh->start(); game_input.capture(window); }
            break;
        case editor::HudAction::Stop: stop_play(); break;
        default: break;
        }
    }
    void stop_play() {
        game_input.release(window);
        report(session.stop(), "Stopped; runtime copy discarded, editor scene shown");
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
                if (const auto tip = project(at - glm::normalize(glm::dvec3(m[2])))) draw->AddLine(*screen, *tip, color, px(2));
            if (e.components.light) draw->AddCircleFilled(*screen, px(7), color);
            else draw->AddRectFilled({screen->x-px(8), screen->y-px(6)}, {screen->x+px(8), screen->y+px(6)}, color);
            if (selected) draw->AddCircle(*screen, px(11), IM_COL32(255,255,255,255), 0, px(2));
            draw->AddText({screen->x+px(11), screen->y-px(7)}, color, e.name.c_str());
            const float dx = mouse.x - screen->x, dy = mouse.y - screen->y;
            if (dx*dx + dy*dy <= px(10)*px(10)) hovered = e.id;
        }
        return hovered;
    }
    void assets() {
        ImGui::Begin("Asset Browser");
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - px(150), px(80)));
        const bool entered = ImGui::InputText("##import", import_path.data(), import_path.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Import") || entered) {
            if (import_path[0]) import_file(utf8_path(import_path.data()));
            else log("Error: Enter a file to import, or drop one on the window");
        }
        ImGui::SameLine();
        if (ImGui::Button("Rescan")) report(session.rescan_assets(), "Asset folder rescanned");
        ImGui::TextDisabled("Assets in %s (.glb .obj .png .jpg .tga .ppm .hdr .material; or drop files on the window)", session.asset_folder().generic_string().c_str());
        if (session.imports_pending()) ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "Importing %zu file(s) in the background...", session.imports_pending());
        ImGui::SetNextItemWidth(px(160)); ImGui::InputText("##material-name", new_material_name.data(), new_material_name.size());
        ImGui::SameLine();
        if (ImGui::Button("New material")) {
            const auto created = session.create_material(new_material_name.data());
            if (report(created, "Material created")) selected_asset = created.value();
        }
        const auto catalogue = session.assets();
        const auto chosen = std::find_if(catalogue.begin(), catalogue.end(), [&](const assets::AssetMetadata &a) { return a.id == selected_asset; });
        const bool have = chosen != catalogue.end();
        const bool have_material = have && chosen->type == assets::AssetType::Material;
        ImGui::SameLine(); ImGui::BeginDisabled(!have_material);
        if (ImGui::Button("Duplicate material")) {
            const auto copied = session.duplicate_material(selected_asset);
            if (report(copied, "Material duplicated")) selected_asset = copied.value();
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!have || !session.selection() || chosen->type == assets::AssetType::Environment);
        if (ImGui::Button("Assign to selection"))
            report(have_material ? session.assign_material(session.selection(), selected_asset) : session.assign_asset(session.selection(), selected_asset), "Asset assigned to the selected entity");
        ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::BeginDisabled(!have || chosen->type != assets::AssetType::Mesh);
        if (ImGui::Button("Add to scene")) report(session.place_asset(selected_asset), "Model added to the scene");
        ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::BeginDisabled(!have);
        if (ImGui::Button("Reload")) report(session.reload_asset(selected_asset), "Asset re-imported from disk");
        ImGui::EndDisabled();
        for (const auto &meta : catalogue) {
            const auto state = session.asset_state(meta.id);
            const bool failed = state == assets::AssetState::Failed || state == assets::AssetState::Missing;
            if (failed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.35f, 1));
            const char *kind = meta.type == assets::AssetType::Mesh ? "[model]    " : meta.type == assets::AssetType::Material ? "[material] " : meta.type == assets::AssetType::Environment ? "[hdr]      " : "[texture]  ";
            const auto label = std::string(kind) + meta.path + "  (" + assets::to_string(state) + ")##" + assets::to_string(meta.id);
            if (ImGui::Selectable(label.c_str(), meta.id == selected_asset)) selected_asset = meta.id;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && meta.type == assets::AssetType::Mesh)
                report(session.place_asset(meta.id), "Model added to the scene");
            if (failed) ImGui::PopStyleColor();
        }
        if (catalogue.empty()) ImGui::TextDisabled("No assets yet.");
        ImGui::Separator();
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
};
}
// Frame, physics and render timings over the run, for --perf-log. Measured here, in the release loop, not estimated.
struct PerformanceLog {
    std::chrono::nanoseconds startup{0};
    std::vector<float> frame_ms;
    double tick_ms_total = 0, tick_ms_max = 0, render_ms_total = 0;
    std::uint64_t ticks = 0, frames = 0;
    renderer::RendererStats last;
    int bodies = 0;
    void simulated(std::uint64_t count, std::chrono::nanoseconds took) {
        if (!count) return;
        const double each = std::chrono::duration<double, std::milli>(took).count() / double(count);
        tick_ms_total += each * double(count); ticks += count;
        tick_ms_max = std::max(tick_ms_max, each);
    }
    void rendered(std::chrono::nanoseconds elapsed, std::chrono::nanoseconds took, const renderer::RendererStats &stats, int body_count) {
        if (frames++ > 10) frame_ms.push_back(std::chrono::duration<float, std::milli>(elapsed).count()); // Skips start-up frames.
        render_ms_total += std::chrono::duration<double, std::milli>(took).count();
        last = stats; bodies = body_count;
    }
    void write(const std::filesystem::path &path) {
        std::ofstream out(path);
        std::sort(frame_ms.begin(), frame_ms.end());
        const auto percentile = [&](double p) { return frame_ms.empty() ? 0.f : frame_ms[std::min(std::size_t(p * double(frame_ms.size())), frame_ms.size() - 1)]; };
        double total = 0;
        for (const float v : frame_ms) total += v;
        out << "startup_ms " << std::chrono::duration<double, std::milli>(startup).count() << "\nframes " << frames
            << "\nframe_ms_mean " << (frame_ms.empty() ? 0.0 : total / double(frame_ms.size())) << "\nframe_ms_p50 " << percentile(0.5) << "\nframe_ms_p99 " << percentile(0.99)
            << "\nrender_submit_ms_mean " << (frames ? render_ms_total / double(frames) : 0.0)
            << "\nticks " << ticks << "\ntick_ms_mean " << (ticks ? tick_ms_total / double(ticks) : 0.0) << "\ntick_ms_max " << tick_ms_max
            << "\nbodies " << bodies << "\nobjects " << last.objects << "\nprimitives " << last.parts << "\nlights " << last.lights
            << "\ngpu_asset_meshes " << last.meshes << "\ngpu_asset_textures " << last.textures << "\nmaterial_instances " << last.material_instances << "\nibl_builds " << last.environment_builds << '\n';
    }
};
int main(int argc, char **argv) {
    try {
        const auto program_start = std::chrono::steady_clock::now();
        bool smoke = false, open_sample_at_start = false;
        std::filesystem::path project = std::filesystem::current_path(), render_test_output, perf_log;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--smoke") smoke = true;
            else if (arg == "--project" && i + 1 < argc) project = utf8_path(argv[++i]);
            else if (arg == "--sample") open_sample_at_start = true;
            else if (arg == "--render-test" && i + 1 < argc) render_test_output = utf8_path(argv[++i]);
            else if (arg == "--perf-log" && i + 1 < argc) perf_log = utf8_path(argv[++i]);
            else throw std::runtime_error("Usage: engine_editor [--project <folder>] [--sample] [--smoke] [--render-test <folder>] [--perf-log <file>]");
        }
        Host host; host.start();
        if (smoke || !render_test_output.empty()) ImGui::GetIO().IniFilename = nullptr;
        if (!render_test_output.empty()) return editor::run_render_test(host.window, EDITOR_MATERIAL_DIR, render_test_output);
        glfwSetDropCallback(host.window, [](GLFWwindow *, int count, const char **paths) {
            for (int i = 0; i < count; ++i) dropped_files.push_back(utf8_path(paths[i]));
        });
        editor::EditorSession session;
        // Not fatal: the editor works without a project, and imports then say why they cannot run.
        const auto project_opened = session.open_project(project);
        auto created = session.create("Cube");
        if (!created) throw std::runtime_error(created.error().message);
        session.mark_clean(); // The starter cube is not a user change.
        const auto cube = session.selection();
        renderer::DesktopRenderer renderer(glfwGetWin32Window(host.window), EDITOR_MATERIAL_DIR);
        renderer.resize_viewport(800, 600);
        GameInput game_input;
        EditorUI ui(session, renderer, game_input, host.window);
        if (!project_opened) ui.log("Error: " + project_opened.error().message);
        if (open_sample_at_start) ui.open_sample();
        int frame = 0, rendered = 0;
        SceneSnapshot before_play;
        std::uint64_t revision_before_play = 0;
        EntityId probe = 0;
        std::array<int, 3> probe_color{}, lit_color{}, texture_top{}, texture_bottom{};
        std::string shown_title;
        auto last = std::chrono::steady_clock::now();
        PerformanceLog performance;
        bool escape_was_down = false;
        while (!ui.quit) {
            glfwPollEvents();
            if (glfwWindowShouldClose(host.window)) {
                if (smoke) break;
                game_input.release(host.window);
                // Closing goes through the unsaved-changes prompt; restore first so a minimized window shows it.
                glfwSetWindowShouldClose(host.window, GLFW_FALSE);
                if (glfwGetWindowAttrib(host.window, GLFW_ICONIFIED)) glfwRestoreWindow(host.window);
                ui.request(Pending::Quit);
                if (ui.quit) break;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = now - last; last = now;
            // Esc, or losing the window, gives the mouse back and pauses. A click on the view takes it again.
            const bool escape_down = glfwGetKey(host.window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
            if (game_input.captured && ((escape_down && !escape_was_down) || !glfwGetWindowAttrib(host.window, GLFW_FOCUSED))) {
                game_input.release(host.window);
                if (session.game() && session.game()->phase() == gameplay::GamePhase::Playing) ui.report(session.pause(), "Paused");
            }
            escape_was_down = escape_down;
            auto *game = session.game();
            if (game && game_input.captured) game_input.feed(host.window, *game);
            const auto update_start = std::chrono::steady_clock::now();
            const auto ticks_before = session.runtime_ticks();
            session.update(elapsed);
            if (game) game->update_view();
            performance.simulated(session.runtime_ticks() - ticks_before, std::chrono::steady_clock::now() - update_start);
            if (const auto title = ui.title(); title != shown_title) { glfwSetWindowTitle(host.window, title.c_str()); shown_title = title; }
            host.follow_monitor();
            int width, height; glfwGetFramebufferSize(host.window, &width, &height);
            if (width <= 0 || height <= 0) { glfwWaitEventsTimeout(0.05); continue; }
            ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame(); ImGuizmo::BeginFrame();
            ui.draw(); ImGui::Render();
            if (!ui.frame) { auto f = session.frame(4.0 / 3); if (f) ui.frame = f.value(); }
            const auto render_start = std::chrono::steady_clock::now();
            if (ui.frame && renderer.render(*ui.frame, session.resources(), ImGui::GetDrawData(), width, height)) ++rendered;
            performance.rendered(elapsed, std::chrono::steady_clock::now() - render_start, renderer.stats(), session.physics_bridge().diagnostics().world.body_count);
            if (rendered == 1) performance.startup = std::chrono::steady_clock::now() - program_start;
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
                // `offset` is a fraction of the viewport height below the center.
                const auto center_pixel = [&](float offset = 0) {
                    const auto pixels = renderer.readback(width, height);
                    const auto scale = ImGui::GetIO().DisplayFramebufferScale;
                    const auto x = std::clamp(int(ui.viewport_center.x * scale.x), 0, width - 1);
                    const auto y = std::clamp(int((ui.viewport_center.y + offset * ui.viewport_size.y) * scale.y), 0, height - 1);
                    const auto *p = pixels.data() + (std::size_t(y) * width + x) * 4;
                    return std::array<int, 3>{p[0], p[1], p[2]};
                };
                // Read after the display transform, which compresses ratios: 2 here is about 5 in linear light.
                const auto dominant = [](const std::array<int, 3> &c, int channel) {
                    return c[channel] > 60 && c[channel] > 2 * c[(channel + 1) % 3] && c[channel] > 2 * c[(channel + 2) % 3];
                };
                if (frame == 82) {
                    // No image-based light and no tone curve, so the hues below are the surface's and the light's alone.
                    EnvironmentComponent plain;
                    plain.source = EnvironmentSource::None; plain.tone_mapping = ToneMapping::Linear; plain.bloom = false; plain.ambient_occlusion = false; plain.shadow_quality = ShadowQuality::Off;
                    if (!session.set_environment(plain)) throw std::runtime_error("Smoke environment failed");
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
                // An imported texture reaches the GPU the right way up: a plane seen from above, with -Z up the screen,
                // shows the image's first row (red) at the top.
                if (frame == 100) {
                    namespace fs = std::filesystem;
                    fs::remove_all("smoke-project"); fs::remove_all("smoke-source");
                    fs::create_directories("smoke-project"); fs::create_directories("smoke-source");
                    {
                        std::ofstream image("smoke-source/two_tone.ppm", std::ios::binary);
                        image << "P6\n2 2\n255\n";
                        for (const auto *pixel : {"\xff\x01\x01", "\xff\x01\x01", "\x01\x01\xff", "\x01\x01\xff"}) image.write(pixel, 3);
                    }
                    const auto opened = session.open_project("smoke-project");
                    const auto imported = session.import_asset("smoke-source/two_tone.ppm");
                    if (!opened || !imported || !session.new_scene()) throw std::runtime_error("Smoke import failed");
                    EnvironmentComponent plain;
                    plain.source = EnvironmentSource::None; plain.tone_mapping = ToneMapping::Linear; plain.bloom = false; plain.ambient_occlusion = false; plain.shadow_quality = ShadowQuality::Off;
                    if (!session.set_environment(plain)) throw std::runtime_error("Smoke environment failed");
                    Transform t; t.position = {0,0.05,0}; t.scale = {4,1,4}; // Above the grid's axis lines.
                    auto plane = session.execute(commands::CreateEntity{"Textured", t, 0, 0, {MeshComponent{BuiltinMesh::Plane, {1,1,1}, 0.9, 0}, {}, {}}});
                    if (!plane || !session.assign_asset(plane.value().created.front(), imported.value().id)) throw std::runtime_error("Smoke texture assignment failed");
                    session.select(0);
                    session.camera.target = {0,0,0}; session.camera.yaw = 0; session.camera.pitch = 1.5; session.camera.distance = 6;
                }
                if (frame == 105) {
                    texture_top = center_pixel(-0.12f); texture_bottom = center_pixel(0.12f);
                    if (!dominant(texture_top, 0) || !dominant(texture_bottom, 2) || renderer.asset_texture_count() != 1)
                        throw std::runtime_error("Smoke: imported texture not shown upright: top " + std::to_string(texture_top[0]) + "," + std::to_string(texture_top[2]) +
                                                 ", bottom " + std::to_string(texture_bottom[0]) + "," + std::to_string(texture_bottom[2]));
                }
                if (++frame == 112) {
                    const auto meshes = std::ranges::count_if(session.scene().snapshot().entities, [](const EntityRecord &e) { return e.components.mesh.has_value(); });
                    if (rendered < 20 || renderer.object_count() != std::size_t(meshes)) throw std::runtime_error("Smoke renderer did not reconcile: rendered=" + std::to_string(rendered) + " objects=" + std::to_string(renderer.object_count()));
                    renderer.capture("editor-smoke.ppm", width, height);
                    std::cout << "GPU smoke passed: " << rendered << " frames; transform, undo/redo, save/load, play/stop isolation, resize, "
                                 "surface color (" << probe_color[0] << ',' << probe_color[1] << ',' << probe_color[2] << "), scene light ("
                              << lit_color[0] << ',' << lit_color[1] << ',' << lit_color[2] << "), imported texture (top "
                              << texture_top[0] << ',' << texture_top[1] << ',' << texture_top[2] << "; bottom "
                              << texture_bottom[0] << ',' << texture_bottom[1] << ',' << texture_bottom[2] << ") and capture.\n";
                    break;
                }
            } else if (!game_input.captured) glfwWaitEventsTimeout(0.005);
        }
        game_input.release(host.window);
        if (!perf_log.empty()) performance.write(perf_log);
        if (smoke && frame != 112) throw std::runtime_error("Smoke run closed before completing all checks");
        std::cout << "Editor stopped cleanly.\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "Editor failed: " << e.what() << '\n'; return 1; }
}
