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
#include <chrono>
#include <cstdio>
#include <iostream>
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
        window = glfwCreateWindow(1440, 900, "GameEngine | Milestone 2 Editor", nullptr, nullptr);
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
struct EditorUI {
    editor::EditorSession &session;
    renderer::DesktopRenderer &renderer;
    std::array<char, 1024> scene_path{};
    std::vector<std::string> messages{"Filament OpenGL initialized. All scene edits use typed commands."};
    EntityId draft_id = 0;
    uint64_t draft_revision = ~uint64_t(0);
    std::array<char, 257> name{};
    float position[3]{}, rotation[3]{}, scale[3]{1,1,1};
    int operation = 0, coordinates = 0;
    bool gizmo_active = false;
    EntityId gizmo_id = 0;
    float gizmo_matrix[16]{};
    renderer::Frame frame;
    explicit EditorUI(editor::EditorSession &s, renderer::DesktopRenderer &r) : session(s), renderer(r) {
        std::snprintf(scene_path.data(), scene_path.size(), "%s", "Untitled.scene");
    }
    void log(std::string text) { if (messages.size() == 128) messages.erase(messages.begin()); messages.push_back(std::move(text)); }
    template<class T> bool report(const Result<T> &r, const char *success) {
        if (r) { log(success); return true; } log("Error: " + r.error().message); return false;
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
        if (ImGui::Button("+ Cube")) report(session.create(), "Created cube");
        ImGui::SameLine(); ImGui::BeginDisabled(!session.selection());
        if (ImGui::Button("Delete")) report(session.execute(commands::DeleteEntity{session.selection()}), "Deleted entity subtree");
        ImGui::SameLine(); if (ImGui::Button("Focus [F]")) session.focus();
        ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::BeginDisabled(!session.commands().undo_size());
        if (ImGui::Button("Undo")) report(session.undo(), "Undo"); ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::BeginDisabled(!session.commands().redo_size());
        if (ImGui::Button("Redo")) report(session.redo(), "Redo"); ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button("Save")) report(session.save(scene_path.data()), "Scene saved");
        ImGui::SameLine(); if (ImGui::Button("Load")) report(session.load(scene_path.data()), "Scene loaded (undoable)");
        ImGui::SameLine(); ImGui::TextDisabled("Revision %llu", static_cast<unsigned long long>(session.commands().revision()));
        ImGui::SetNextItemWidth(490); ImGui::InputText("Scene file", scene_path.data(), scene_path.size());
        ImGui::SameLine(); ImGui::SetNextItemWidth(110); ImGui::Combo("##operation", &operation, "Move\0Rotate\0Scale\0");
        ImGui::SameLine(); ImGui::SetNextItemWidth(100); ImGui::Combo("##coordinates", &coordinates, "Local\0World\0");
        ImGui::End();
    }
    void hierarchy() {
        ImGui::Begin("Scene Hierarchy");
        const auto snapshot = session.scene().snapshot();
        ImGui::TextDisabled("%zu entities", snapshot.entities.size());
        for (const auto &entity : snapshot.entities) {
            unsigned depth = 0; EntityId parent = entity.parent;
            while (parent && depth < 32) { auto p = session.scene().inspect(parent); if (!p) break; parent = p->parent; ++depth; }
            ImGui::PushID(reinterpret_cast<void *>(static_cast<uintptr_t>(entity.id)));
            // Indent(0) would apply the default IndentSpacing, so roots must skip it entirely.
            if (depth) ImGui::Indent(float(depth) * 12);
            if (ImGui::Selectable(entity.name.c_str(), session.selection() == entity.id)) session.select(entity.id);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) session.focus();
            if (depth) ImGui::Unindent(float(depth) * 12);
            ImGui::PopID();
        }
        ImGui::End();
    }
    void inspector() {
        ImGui::Begin("Inspector");
        const auto record = session.scene().inspect(session.selection());
        if (!record) { ImGui::TextWrapped("Select an entity in the hierarchy or click a cube in the viewport."); ImGui::End(); return; }
        if (draft_id != record->id || draft_revision != session.commands().revision()) {
            draft_id = record->id; draft_revision = session.commands().revision();
            std::snprintf(name.data(), name.size(), "%s", record->name.c_str());
            const auto &t = record->transform;
            position[0] = float(t.position.x); position[1] = float(t.position.y); position[2] = float(t.position.z);
            const auto euler = glm::degrees(glm::eulerAngles(glm::dquat(t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z)));
            for (int i = 0; i < 3; ++i) rotation[i] = float(euler[i]);
            scale[0] = float(t.scale.x); scale[1] = float(t.scale.y); scale[2] = float(t.scale.z);
        }
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
            if (ImGui::Selectable("(root)", !record->parent)) report(session.execute(commands::SetParent{record->id,0}), "Reparented (local transform preserved)");
            for (const auto &e : session.scene().snapshot().entities) if (e.id != record->id) {
                ImGui::PushID(reinterpret_cast<void *>(static_cast<uintptr_t>(e.id)));
                if (ImGui::Selectable(e.name.c_str(), record->parent == e.id)) report(session.execute(commands::SetParent{record->id,e.id}), "Reparented (local transform preserved)");
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SeparatorText("World position");
        const auto w = session.scene().world_transform(record->id);
        ImGui::Text("%.3f, %.3f, %.3f", w->matrix[12], w->matrix[13], w->matrix[14]);
        ImGui::SeparatorText("Render binding");
        ImGui::TextWrapped("Built-in unit cube / blue material. M2 renders each managed entity as a cube.");
        ImGui::End();
    }
    void viewport() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0,0});
        const bool visible = ImGui::Begin("3D Viewport");
        ImGui::PopStyleVar();
        if (!visible) { ImGui::End(); return; }
        const auto origin = ImGui::GetCursorScreenPos();
        auto size = ImGui::GetContentRegionAvail(); size.x = std::max(size.x, 1.f); size.y = std::max(size.y, 1.f);
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
        if (hovered && ImGui::IsMouseClicked(0) && !ImGuizmo::IsOver() && !gizmo_active)
            session.select(session.pick(*frame, (io.MousePos.x-origin.x) / size.x * 2-1, 1-(io.MousePos.y-origin.y) / size.y * 2));
        ImGui::GetWindowDrawList()->AddText({origin.x+12,origin.y+10}, IM_COL32(210,220,235,255), "RMB orbit  |  MMB pan  |  Wheel zoom  |  F focus  |  Click select");
        ImGui::End();
    }
    void assets() {
        ImGui::Begin("Asset Browser");
        if (ImGui::Button("Built-in: Cube")) report(session.create(), "Created cube from asset browser");
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
        const auto &io = ImGui::GetIO();
        if (!io.WantTextInput && !gizmo_active) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) report(io.KeyShift ? session.redo() : session.undo(), io.KeyShift ? "Redo" : "Undo");
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) report(session.redo(), "Redo");
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) report(session.save(scene_path.data()), "Scene saved");
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
        const auto cube = session.selection();
        renderer::DesktopRenderer renderer(glfwGetWin32Window(host.window), EDITOR_MATERIAL_DIR);
        renderer.resize_viewport(800, 600);
        EditorUI ui(session, renderer);
        int frame = 0, rendered = 0;
        while (!glfwWindowShouldClose(host.window)) {
            glfwPollEvents();
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
                if (++frame == 100) {
                    if (rendered < 20 || renderer.object_count() != session.scene().size()) throw std::runtime_error("Smoke renderer did not reconcile: rendered=" + std::to_string(rendered) + " objects=" + std::to_string(renderer.object_count()));
                    renderer.capture("editor-smoke.ppm", width, height);
                    std::cout << "GPU smoke passed: " << rendered << " frames; transform, undo/redo, save/load, resize and capture.\n";
                    break;
                }
            } else glfwWaitEventsTimeout(0.005);
        }
        if (smoke && frame != 100) throw std::runtime_error("Smoke run closed before completing all checks");
        std::cout << "Editor stopped cleanly.\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "Editor failed: " << e.what() << '\n'; return 1; }
}
