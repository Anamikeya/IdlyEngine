#pragma once
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <engine/editor/EditorSession.hpp>
#include <engine/renderer/DesktopRenderer.hpp>
#include <glm/glm.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine::editor {
// UTF-8 text from ImGui or GLFW to a path.
inline std::filesystem::path utf8_path(std::string_view text) { return std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()); }
// Files dropped on the window, imported on the next frame.
inline std::vector<std::filesystem::path> dropped_files;
// Actions that discard the editor scene ask first when it has unsaved changes.
enum class Pending { None, NewScene, Load, Quit, OpenSample, NewProject, OpenProject };
inline const std::filesystem::path sample_project = std::filesystem::path(ENGINE_SAMPLES_DIR) / "PhysicsShooter";
inline constexpr const char *sample_scene = "physics_shooter.scene";
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
inline float px(float length) { return length * ImGui::GetFontSize() / 13; }
// Euler angles from a quaternion carry rounding noise, which would read as -0.000.
inline float tidy_degrees(double degrees) { return std::abs(degrees) < 0.0005 ? 0.f : float(degrees); }
struct EditorUI {
    EditorSession &session;
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
    explicit EditorUI(EditorSession &s, renderer::DesktopRenderer &r, GameInput &g, GLFWwindow *w) : session(s), renderer(r), game_input(g), window(w) {
        std::snprintf(scene_path.data(), scene_path.size(), "%s", "Untitled.scene");
        std::snprintf(new_material_name.data(), new_material_name.size(), "%s", "New Material");
    }
    bool scroll_console = false;
    void log(std::string text);
    template<class T> bool report(const Result<T> &r, const char *success) {
        if (r) { log(success); return true; } log("Error: " + r.error().message); return false;
    }
    bool playing() const { return session.play_state() != PlayState::Editing; }
    // Creates and selects an entity in one command, so it is one undo step.
    void spawn(const char *label, Components c, Transform t = {{0,0.5,0}, {}, {1,1,1}}, EntityId parent = 0);
    static Transform aimed(Vec3 position, double pitch_degrees, double yaw_degrees);
    // A collider matching a built-in mesh. The plane's is a thin box whose top face is the plane itself.
    static ColliderComponent fitted_collider(BuiltinMesh mesh);
    // A box around a model, in its local space. Falls back to the built-in shape without one.
    ColliderComponent fitted_collider(const std::optional<MeshComponent> &mesh);
    static Components physical(BuiltinMesh mesh);
    void create_menu();
    std::string title() const;
    LightComponent point_light() const;
    void set_scene_path(const std::filesystem::path &path);
    void open_sample();
    // A folder with an Assets folder and an empty scene: nothing of the sample comes along.
    void create_project(const std::filesystem::path &folder);
    void open_project(const std::filesystem::path &folder);
    void perform(Pending action);
    void project_dialog();
    void request(Pending action);
    void confirm();
    void docking();
    void toolbar();
    // ---- Scene Hierarchy: a tree in stable-ID order. Drag a row onto another to parent it there, or onto the
    // empty area below the tree to move it to the root; both keep the world transform. Right-click for actions.
    static constexpr const char *entity_payload = "ENGINE_ENTITY";
    std::map<EntityId, EntityRecord> records;
    std::map<EntityId, std::vector<EntityId>> children;
    EntityId revealed = 0; // Selection whose ancestors were last opened, so viewport picks show up in the tree.
    void drop_onto(EntityId parent);
    void duplicate_selection();
    void delete_selection();
    void tree_node(EntityId id, const std::set<EntityId> &open_path);
    void hierarchy();
    void inspector();
    // Commit helpers: drags commit on release (one undo step); combos, checkboxes and picker changes at once.
    static bool committed(bool changed);
    static bool color_edit(const char *label, Vec3 &c);
    static bool drag(const char *label, double &value, float speed, double low, double high, const char *format = "%.3f");
    // No clamp when low == high.
    static bool drag3(const char *label, Vec3 &value, float speed, float low = 0, float high = 0);
    // Runs a component command and re-reads the drafts, so a rejected edit does not linger in the Inspector.
    void apply(commands::Command command, const char *success);
    // Lists the project's assets of one type. True when the reference changed.
    bool asset_combo(const char *label, AssetRef &ref, assets::AssetType type, const char *none, bool whiten = true);
    bool remove_button(const char *id);
    void component_sections(const EntityRecord &record);
    void physics_sections(const EntityRecord &record);
    // Named numbers read by runtime systems when Play starts; the scene gives them no meaning, so they edit generically.
    void behavior_sections(const EntityRecord &record);
    void lighting();
    // One texture slot of the material: a combo of the project's textures. True when it changed.
    bool texture_slot(const char *label, assets::MaterialTexture &slot);
    void material_editor();
    // Puts physics problems in the Console once, when they appear.
    void report_physics();
    // Puts asset problems in the Console once, when they appear.
    void report_assets();
    // Decodes on a worker; the Console reports when it lands, with how long it took.
    std::map<std::string, std::chrono::steady_clock::time_point> import_started;
    std::vector<std::pair<std::string, long long>> import_times; // For --perf-log.
    void import_file(const std::filesystem::path &file);
    void finish_imports();
    void viewport();
    // The game's HUD, and the choices it offers: a click on the view starts or resumes and takes the mouse.
    void game_overlay(gameplay::Game &game, ImVec2 origin, ImVec2 size, bool hovered);
    void stop_play();
    // Screen position of a world point in the current frame; empty behind the camera.
    std::optional<ImVec2> project(glm::dvec3 p, ImVec2 origin, ImVec2 size) const;
    // Collider wireframes from the physics bridge, drawn over the image without depth testing (contract C-4a).
    // An edge with an end behind the camera is skipped rather than clipped.
    void collider_outlines(ImVec2 origin, ImVec2 size);
    // Lights and cameras have no mesh, so they get clickable screen-space markers. Returns the hovered one.
    EntityId markers(ImVec2 origin, ImVec2 size);
    void assets();
    void draw();
};
} // namespace engine::editor
