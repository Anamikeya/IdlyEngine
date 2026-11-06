#include "EditorUI.hpp"
#include "../GameHud.hpp"
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <span>

namespace engine::editor {
void EditorUI::report_physics() {
    std::set<std::string> current;
    for (const auto &issue : session.physics_bridge().diagnostics().issues) {
        const auto record = session.scene().inspect(issue.entity);
        const auto entity_name = record ? record->name : std::to_string(issue.entity);
        auto message = "Physics: " + entity_name + ": " + issue.message;
        if (!reported_issues.contains(message)) notify(Severity::Warning, message, record ? LogLink::entity(issue.entity, entity_name) : LogLink{});
        current.insert(std::move(message));
    }
    reported_issues = std::move(current);
    if (session.physics_error() != reported_physics_error) {
        reported_physics_error = session.physics_error();
        if (!reported_physics_error.empty()) log_error(reported_physics_error);
    }
}
void EditorUI::report_assets() {
    std::set<std::string> current(session.asset_problems().begin(), session.asset_problems().end());
    for (const auto &issue : session.asset_issues()) current.insert("Asset folder: " + issue.path + ": " + issue.message);
    for (const auto &issue : renderer.take_issues()) notify(Severity::Warning, "Renderer: " + issue);
    for (const auto &message : current) if (!reported_asset_problems.contains(message)) notify(Severity::Warning, message);
    reported_asset_problems = std::move(current);
}
void EditorUI::viewport() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0,0});
    bool visible;
    if (viewport_maximized) {
        // A window of its own over everything, so the docked one and its place in the layout stay as they were.
        const auto *main = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(main->Pos);
        ImGui::SetNextWindowSize(main->Size);
        visible = begin_panel("###3D Viewport Maximized", "3D Viewport", context::viewport, nullptr,
                              ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    } else {
        // The tab shows the same unsaved-changes star as the title bar; the ID after ### stays the same.
        visible = begin_panel(session.dirty() ? "3D Viewport*###3D Viewport" : "3D Viewport###3D Viewport", "3D Viewport", context::viewport);
    }
    ImGui::PopStyleVar();
    if (!visible) {
        if (camera_holding) { camera_control.reset(); hold_cursor(false); }
        ImGui::End();
        return;
    }
    const auto origin = ImGui::GetCursorScreenPos();
    auto size = ImGui::GetContentRegionAvail(); size.x = std::max(size.x, 1.f); size.y = std::max(size.y, 1.f);
    viewport_center = {origin.x + size.x / 2, origin.y + size.y / 2}; viewport_size = size;
    const auto &io = ImGui::GetIO();
    renderer.resize_viewport(unsigned(size.x * io.DisplayFramebufferScale.x), unsigned(size.y * io.DisplayFramebufferScale.y));
    ImGui::Image(ImTextureRef(renderer.viewport_texture()), size, {0,1}, {1,0});
    const bool hovered = ImGui::IsItemHovered();
    // A model dragged from the Asset Browser lands where the cursor's ray hits.
    if (frame && ImGui::BeginDragDropTarget()) {
        if (const auto *payload = ImGui::AcceptDragDropPayload(asset_payload)) {
            std::uint64_t id = 0; std::memcpy(&id, payload->Data, sizeof id);
            const auto point = session.ray_hit(*frame, (io.MousePos.x-origin.x) / size.x * 2-1, 1-(io.MousePos.y-origin.y) / size.y * 2);
            report(session.place_asset_at(assets::AssetId{id}, point.value_or(Vec3{})), "Model added to the scene");
        }
        ImGui::EndDragDropTarget();
    }
    auto *game = possessed() ? session.game() : nullptr; // Ejected or simulating, the view is the editor's.
    if (game && game_input.captured) session.select(0); // The view belongs to the player: no gizmo, no picking.
    update_camera(hovered, size.y);
    session.view_flags.grid = show_grid && !game_view;
    // The renderer outlines what is selected around its silhouette (slice 7); the game's own view shows none.
    session.view_flags.outlines = !game && !game_view;
    renderer.set_outline_width(px(2) * io.DisplayFramebufferScale.x);
    session.grid_spacing = snap.grid_size;
    auto snapshot = session.frame(double(size.x) / size.y);
    if (!snapshot) { log(snapshot.error().message); ImGui::End(); return; }
    frame = snapshot.value();
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList()); ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
    ImGuizmo::SetOrthographic(frame->camera.lens.orthographic_half_height > 0);
    gizmo(origin, size);
    if (show_colliders && !game && !game_view) collider_outlines(origin, size);
    if (session.view_flags.bones && !game && !game_view) bone_overlay(origin, size);
    if (session.view_flags.ragdolls && !game && !game_view) ragdoll_overlay(origin, size);
    report_physics();
    report_assets();
    if (const auto problem = session.take_view_state_problem(); !problem.empty()) log_error(problem);
    const EntityId marker = game || game_view || !show_markers ? 0 : markers(origin, size);
    if (camera_control.clicked() && !game) {
        const auto hit = marker ? marker : session.pick(*frame, (io.MousePos.x-origin.x) / size.x * 2-1, 1-(io.MousePos.y-origin.y) / size.y * 2);
        if (!io.KeyCtrl) session.select(hit);
        else session.selection_set().toggle(hit);
    }
    auto *draw = ImGui::GetWindowDrawList();
    const float margin = px(12), line = ImGui::GetTextLineHeightWithSpacing();
    // A colored edge says at a glance that this is not the editor scene: green playing, blue simulating, yellow ejected.
    if (playing()) {
        const ImU32 edge = session.simulating() ? IM_COL32(90, 170, 255, 255) : session.ejected() ? IM_COL32(245, 205, 80, 255) : IM_COL32(115, 215, 115, 255);
        draw->AddRect(origin, ImVec2(origin.x+size.x, origin.y+size.y), edge, 0.0f, ImDrawFlags_None, px(3));
    }
    draw_stats(origin, size);
    if (game) {
        game_overlay(*game, origin, size, hovered);
        ImGui::End();
        return;
    }
    viewport_overlay(origin, size);
    if (camera_control.speed_shown() > 0) {
        const auto text = "Camera speed " + std::to_string(camera_control.speed()).substr(0, 5) + " m/s";
        const auto width = ImGui::CalcTextSize(text.c_str()).x;
        draw->AddText({origin.x + (size.x - width) / 2, origin.y + px(44)}, IM_COL32(255,255,255,255), text.c_str());
    }
    const float below_overlay = origin.y + px(40);
    if (!game_view) draw->AddText({origin.x+margin,below_overlay}, IM_COL32(210,220,235,255), session.camera.orthographic
        ? "RMB or MMB pan  |  Wheel zoom  |  F focus  |  Click select  |  Alt+G perspective"
        : "RMB + WASD fly  |  Alt+LMB orbit  |  MMB pan  |  Wheel zoom  |  F focus  |  Click select");
    if (playing()) {
        const char *mode = session.play_state() == editor::PlayState::Paused ? "PLAY MODE (paused) - changes are discarded on Stop"
                         : session.simulating() ? "SIMULATING - F8 possesses, Esc stops; changes are discarded"
                         : session.ejected() ? "EJECTED - the game runs on; F8 goes back, Esc stops"
                                             : "PLAY MODE - changes are discarded on Stop";
        draw->AddText({origin.x+margin,below_overlay+line}, IM_COL32(115,215,115,255), mode);
        std::string view = session.ejected() ? "Viewing through the editor camera" : "Viewing through the editor camera (no primary camera)";
        if (const auto camera = session.scene().inspect(session.view_camera())) view = "Viewing through camera '" + camera->name + "'";
        if (!session.view_camera_issue().empty()) view = session.view_camera_issue() + "; using the editor camera";
        draw->AddText({origin.x+margin,below_overlay+2*line}, IM_COL32(115,215,115,255), view.c_str());
    }
    if (!playing() && frame->lights.empty() && !game_view)
        draw->AddText({origin.x+margin,origin.y+size.y-px(24)}, IM_COL32(150,160,175,255), "No lights in the scene: editor lighting is used");
    ImGui::End();
}
void EditorUI::hold_cursor(bool hold) {
    camera_holding = hold;
    auto &io = ImGui::GetIO();
    if (hold) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported()) glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    } else {
        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL); // GLFW puts the cursor back where it was.
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        if (float(camera_control.speed()) != saved_speed) save_settings();
    }
    glfwGetCursorPos(window, &cursor_x, &cursor_y);
}
void EditorUI::update_camera(bool hovered, float height) {
    const auto &io = ImGui::GetIO();
    if (possessed() || (camera_holding && !glfwGetWindowAttrib(window, GLFW_FOCUSED))) {
        camera_control.reset();
        if (camera_holding) hold_cursor(false);
        if (possessed()) return;
    }
    double x = 0, y = 0;
    glfwGetCursorPos(window, &x, &y);
    CameraInput in;
    in.dt = io.DeltaTime;
    in.mouse_dx = x - cursor_x; in.mouse_dy = y - cursor_y;
    cursor_x = x; cursor_y = y;
    in.wheel = io.MouseWheel;
    in.viewport_height = height;
    in.lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    in.rmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    in.mmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    in.alt = io.KeyAlt; in.shift = io.KeyShift;
    in.can_start = hovered && !ImGuizmo::IsOver() && !overlay_hovered && !ImGui::GetDragDropPayload();
    if (in.alt && in.lmb) in.pivot = session.selection_center();
    if (camera_control.mode() == CameraMode::Fly) {
        in.forward = key_held("Camera.FlyForward"); in.back = key_held("Camera.FlyBack");
        in.left = key_held("Camera.FlyLeft"); in.right = key_held("Camera.FlyRight");
        in.down = key_held("Camera.FlyDown"); in.up = key_held("Camera.FlyUp");
        in.fov_out = key_held("Camera.FovOut"); in.fov_in = key_held("Camera.FovIn");
    }
    camera_control.update(session.camera, in);
    if (camera_control.holds_cursor() != camera_holding) hold_cursor(camera_control.holds_cursor());
}
void EditorUI::set_view(int kind) {
    auto &c = session.camera;
    if (view_kind == 0 && kind != 0) { perspective_yaw = c.yaw; perspective_pitch = c.pitch; }
    view_kind = kind;
    c.orthographic = kind != 0;
    // Straight down would leave no direction for "up" on screen, so the top view stays a hair short of it.
    constexpr double straight_down = 1.5695, quarter_turn = 1.5707963267948966;
    switch (kind) {
    case 1: c.yaw = 0; c.pitch = 0; break;
    case 2: c.yaw = 0; c.pitch = straight_down; break;
    case 3: c.yaw = quarter_turn; c.pitch = 0; break;
    default: c.yaw = perspective_yaw; c.pitch = perspective_pitch; break;
    }
}
namespace {
constexpr double grid_sizes[] = {0.01, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 50, 100};
constexpr double rotation_steps[] = {1, 5, 10, 15, 30, 45, 60, 90};
constexpr double scale_steps[] = {0.01, 0.05, 0.1, 0.25, 0.5, 1};
std::string short_number(double value) {
    char text[32];
    std::snprintf(text, sizeof text, "%g", value);
    return text;
}
}
void EditorUI::step_grid(int direction) {
    const auto *at = std::lower_bound(std::begin(grid_sizes), std::end(grid_sizes), snap.grid_size);
    std::ptrdiff_t index = at - std::begin(grid_sizes);
    index = std::clamp<std::ptrdiff_t>(index + direction, 0, std::ptrdiff_t(std::size(grid_sizes)) - 1);
    snap.grid_size = grid_sizes[index];
    log("Grid size " + short_number(snap.grid_size) + " m");
    save_settings();
}
void EditorUI::viewport_overlay(ImVec2 origin, ImVec2 size) {
    const float pad = px(6);
    const auto &style = ImGui::GetStyle();
    // ---- Top left: the view ----
    ImGui::SetCursorScreenPos({origin.x + pad, origin.y + pad});
    constexpr const char *view_names[] = {"Perspective", "Front", "Top", "Side"};
    if (tip_button(view_names[view_kind], ("Which way the view looks. " + registry.shortcut("Viewport.Perspective") + ", " + registry.shortcut("Viewport.Front") + ", " + registry.shortcut("Viewport.Top") + ", " + registry.shortcut("Viewport.Side")).c_str())) ImGui::OpenPopup("##view-kind");
    mark("View.Kind");
    if (ImGui::BeginPopup("##view-kind")) {
        for (const auto *id : {"Viewport.Perspective", "Viewport.Front", "Viewport.Top", "Viewport.Side"}) command_item(id);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (tip_button("Lit", "How the view is drawn, and what it shows")) ImGui::OpenPopup("##view-mode");
    mark("View.Mode");
    if (ImGui::BeginPopup("##view-mode")) {
        for (const auto *id : {"Viewport.Lit", "Viewport.Wireframe", "Viewport.Unlit"}) command_item(id);
        ImGui::SeparatorText("Show");
        for (const auto *id : {"Viewport.ShowGrid", "Viewport.ToggleColliders", "Viewport.ShowMarkers", "Viewport.ShowSkybox", "Viewport.ShowShadows", "Viewport.ShowAmbientOcclusion", "Viewport.ShowBloom", "Viewport.ShowEffects", "Viewport.ShowAnimations", "Viewport.ShowBones"}) command_item(id);
        command_item("Viewport.RestartEffects");
        command_item("Viewport.RestartAnimations");
        ImGui::EndPopup();
    }
    // ---- Top right: snapping and camera speed, each a toggle and a value ----
    struct Snapper { const char *command, *label; std::string value; std::span<const double> presets; double *setting; };
    const Snapper snappers[] = {
        {"Snap.ToggleGrid", "Grid", short_number(snap.grid_size), grid_sizes, &snap.grid_size},
        {"Snap.ToggleRotation", "Rot", short_number(snap.rotation_step), rotation_steps, &snap.rotation_step},
        {"Snap.ToggleScale", "Scale", short_number(snap.scale_step), scale_steps, &snap.scale_step},
    };
    const auto speed_text = "Speed " + short_number(std::round(camera_control.speed() * 100) / 100);
    const auto button_width = [&](const std::string &text) { return ImGui::CalcTextSize(text.c_str()).x + style.FramePadding.x * 2; };
    float total = button_width(speed_text);
    for (const auto &item : snappers) total += button_width(item.label) + button_width(item.value) + style.ItemSpacing.x * 2;
    ImGui::SetCursorScreenPos({origin.x + size.x - total - pad, origin.y + pad});
    for (const auto &item : snappers) {
        const bool on = registry.checked(item.command);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.5f, 0.85f, 1));
        else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.15f, 0.19f, 0.85f));
        if (ImGui::Button(item.label)) registry.run(item.command);
        ImGui::PopStyleColor();
        tip(registry.tooltip(item.command));
        mark(std::string("Overlay.") + item.label);
        ImGui::SameLine(0, 1);
        ImGui::PushID(item.label);
        if (ImGui::Button(item.value.c_str())) ImGui::OpenPopup("##presets");
        mark(std::string("Overlay.") + item.label + ".Value");
        if (ImGui::BeginPopup("##presets")) {
            for (const double preset : item.presets) {
                if (ImGui::Selectable(short_number(preset).c_str(), preset == *item.setting)) { *item.setting = preset; save_settings(); }
                mark(std::string("Overlay.") + item.label + "." + short_number(preset));
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    if (ImGui::Button(speed_text.c_str())) ImGui::OpenPopup("##speed");
    tip("Camera speed in meters a second. RMB + Wheel changes it while flying.");
    mark("Overlay.Speed");
    if (ImGui::BeginPopup("##speed")) {
        auto speed = float(camera_control.speed());
        ImGui::SetNextItemWidth(px(120));
        if (ImGui::DragFloat("m/s", &speed, 0.05f, float(CameraController::min_speed), float(CameraController::max_speed), "%.2f", ImGuiSliderFlags_Logarithmic)) camera_control.set_speed(speed);
        if (ImGui::IsItemDeactivatedAfterEdit()) save_settings();
        ImGui::EndPopup();
    }
    overlay_hovered = ImGui::IsAnyItemHovered() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}
void EditorUI::gizmo(ImVec2 origin, ImVec2 size) {
    const auto &io = ImGui::GetIO();
    // Where the selection is drawn: on its joint, when it is attached to one.
    const auto world = session.drawn_world(session.selection());
    if (!world || possessed() || game_view) {
        if (gizmo_active && session.dragging()) (void)session.end_drag();
        if (gizmo_active) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        gizmo_active = false;
        gizmo_entry.clear();
        return;
    }
    if (ui_map.enabled())
        if (const auto center = project({world->matrix[12], world->matrix[13], world->matrix[14]}, origin, size))
            ui_map.mark("Gizmo.Center", {center->x - 1, center->y - 1}, {center->x + 1, center->y + 1});
    float view[16], projection[16];
    for (int i = 0; i < 16; ++i) { view[i] = float(frame->camera.view.matrix[i]); projection[i] = float(frame->camera.projection.matrix[i]); }
    if (!gizmo_active) {
        for (int i = 0; i < 16; ++i) gizmo_matrix[i] = float(world->matrix[i]);
        gizmo_id = session.selection();
    }
    const float before_drag[16] = {gizmo_matrix[0], gizmo_matrix[1], gizmo_matrix[2], gizmo_matrix[3], gizmo_matrix[4], gizmo_matrix[5], gizmo_matrix[6], gizmo_matrix[7],
                                   gizmo_matrix[8], gizmo_matrix[9], gizmo_matrix[10], gizmo_matrix[11], gizmo_matrix[12], gizmo_matrix[13], gizmo_matrix[14], gizmo_matrix[15]};
    const ImGuizmo::OPERATION op[] = {ImGuizmo::TRANSLATE, ImGuizmo::ROTATE, ImGuizmo::SCALE};
    if (gizmo_active && session.dragging() && operation == 0 && io.KeyShift && !gizmo_following) {
        gizmo_following = true;
        std::copy_n(view, 16, gizmo_frozen_view.begin());
    }
    // Drawn against the old view it would be in the wrong place, so it is drawn behind the viewport, out of sight; the
    // selection box and the readout show what is happening.
    if (gizmo_following) ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    const bool snapping = (operation == 0 && snap.grid) || (operation == 1 && snap.rotation) || (operation == 2 && snap.scale);
    const auto step = float(operation == 0 ? snap.grid_size : operation == 1 ? snap.rotation_step : snap.scale_step);
    const float snap_values[3] = {step, step, step};
    bool changed = ImGuizmo::Manipulate(gizmo_following ? gizmo_frozen_view.data() : view, projection, op[operation], coordinates == 0 ? ImGuizmo::LOCAL : ImGuizmo::WORLD, gizmo_matrix, nullptr, snapping ? snap_values : nullptr);
    const bool using_now = ImGuizmo::IsUsing();
    if (using_now && !gizmo_active) {
        std::copy_n(before_drag, 16, gizmo_start.begin());
        std::copy_n(before_drag + 12, 3, gizmo_last_position.begin());
        drag_error.clear();
        gizmo_cancelled = false;
        gizmo_camera_start = session.camera;
        gizmo_entry.clear();
        gizmo_typed_applied.clear();
        gizmo_confirmed = false;
        gizmo_start_world = *world;
        constexpr int first_handle[] = {ImGuizmo::MT_MOVE_X, ImGuizmo::MT_ROTATE_X, ImGuizmo::MT_SCALE_X};
        gizmo_handle = int(ImGuizmo::GetActiveHandleType()) - first_handle[operation];
        // Enter must confirm what was typed, not press whatever button last had the keyboard.
        ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
        constexpr const char *verbs[] = {"Move", "Rotate", "Scale"};
        if (const auto begun = session.begin_drag(verbs[operation], io.KeyAlt); !begun) log_error(begun.error().message);
    }
    if (using_now && session.dragging() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        gizmo_cancelled = true;
        gizmo_entry.clear();
        if (gizmo_following) session.camera = gizmo_camera_start;
        report(session.cancel_drag(), "Drag cancelled");
    }
    if (gizmo_cancelled) std::copy_n(gizmo_start.begin(), 16, gizmo_matrix);
    if (gizmo_confirmed) std::copy(gizmo_final.begin(), gizmo_final.end(), gizmo_matrix);
    // ---- Typing a number sets how far the drag goes, and the mouse has no more say until it is deleted ----
    const auto gizmo_operation = GizmoOperation(operation);
    if (using_now && session.dragging()) {
        for (const auto character : io.InputQueueCharacters) (void)gizmo_entry.feed(character);
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) gizmo_entry.backspace();
    }
    const int typed_axis = gizmo_entry.axis() >= 0 ? gizmo_entry.axis() : gizmo_handle_axis(gizmo_operation, gizmo_handle);
    std::optional<WorldTransform> typed_change;
    if (const auto typed_value = gizmo_entry.value()) typed_change = gizmo_typed_change(gizmo_operation, gizmo_start_world, coordinates == 0, typed_axis, *typed_value);
    if (using_now && session.dragging() && gizmo_entry.active()) {
        // What is not a number yet holds the object where the drag began.
        WorldTransform change;
        const glm::dmat4 identity(1.0);
        for (int i = 0; i < 16; ++i) change.matrix[std::size_t(i)] = glm::value_ptr(identity)[i];
        if (typed_change) change = *typed_change;
        if (const auto text = gizmo_entry.text(); text != gizmo_typed_applied) {
            gizmo_typed_applied = text;
            if (const auto updated = session.update_drag(change); !updated) drag_error = updated.error().message;
        }
        const glm::mat4 shown(glm::make_mat4(change.matrix.data()) * glm::make_mat4(gizmo_start_world.matrix.data()));
        std::copy_n(glm::value_ptr(shown), 16, gizmo_matrix);
        changed = false;
        if (typed_change && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
            std::copy_n(gizmo_matrix, 16, gizmo_final.begin());
            gizmo_confirmed = true;
            gizmo_entry.clear();
            if (drag_error.empty()) report(session.end_drag(), "Gizmo transform committed");
            else { (void)session.end_drag(); log_error(drag_error); }
        }
    } else if (!gizmo_typed_applied.empty()) {
        gizmo_typed_applied.clear(); // All of it was deleted: back to the mouse.
        changed = true;
    }
    // Hold V to land on the nearest vertex of something else.
    if (using_now && session.dragging() && operation == 0 && !gizmo_entry.active() && ImGui::IsKeyDown(ImGuiKey_V)) {
        if (const auto vertex = session.nearest_vertex(*frame, io.MousePos.x - origin.x, io.MousePos.y - origin.y, size.x, size.y, px(30))) {
            gizmo_matrix[12] = float(vertex->x); gizmo_matrix[13] = float(vertex->y); gizmo_matrix[14] = float(vertex->z);
            changed = true;
            if (const auto dot = project({vertex->x, vertex->y, vertex->z}, origin, size)) ImGui::GetWindowDrawList()->AddCircle(*dot, px(7), IM_COL32(255,255,120,255), 0, px(2));
        }
    }
    if (using_now && changed && session.dragging()) {
        // Relative to where the drag began, so each entity keeps its own place and double precision.
        const auto delta = glm::make_mat4(gizmo_matrix) * glm::inverse(glm::make_mat4(gizmo_start.data()));
        WorldTransform moved;
        const auto *values = glm::value_ptr(delta);
        for (int i = 0; i < 16; ++i) moved.matrix[std::size_t(i)] = values[i];
        if (const auto updated = session.update_drag(moved); !updated) drag_error = updated.error().message;
    }
    if (using_now && session.dragging()) {
        // The camera follows with Shift, so a long move does not lose sight of what is moving.
        if (gizmo_following && io.KeyShift)
            session.camera.move({gizmo_matrix[12] - gizmo_last_position[0], gizmo_matrix[13] - gizmo_last_position[1], gizmo_matrix[14] - gizmo_last_position[2]});
        std::copy_n(gizmo_matrix + 12, 3, gizmo_last_position.begin());
        // What the drag has done so far, beside the cursor.
        float start_t[3], start_r[3], start_s[3], now_t[3], now_r[3], now_s[3];
        ImGuizmo::DecomposeMatrixToComponents(gizmo_start.data(), start_t, start_r, start_s);
        ImGuizmo::DecomposeMatrixToComponents(gizmo_matrix, now_t, now_r, now_s);
        char text[128];
        if (gizmo_entry.active()) {
            constexpr const char *axis_names[] = {"X", "Y", "Z", "All"};
            constexpr const char *units[] = {"m", "deg", "times"};
            if (typed_axis < 0) std::snprintf(text, sizeof text, "%s  Type X, Y or Z", gizmo_entry.number().c_str());
            else if (!typed_change && gizmo_entry.value()) std::snprintf(text, sizeof text, "%s %s  Must be more than 0", axis_names[typed_axis], gizmo_entry.number().c_str());
            else std::snprintf(text, sizeof text, "%s %s %s  Enter confirms, Esc cancels", axis_names[typed_axis], gizmo_entry.number().c_str(), units[operation]);
        }
        else if (operation == 0) std::snprintf(text, sizeof text, "X %.2f  Y %.2f  Z %.2f", now_t[0] - start_t[0], now_t[1] - start_t[1], now_t[2] - start_t[2]);
        else if (operation == 1) std::snprintf(text, sizeof text, "X %.1f  Y %.1f  Z %.1f deg", now_r[0] - start_r[0], now_r[1] - start_r[1], now_r[2] - start_r[2]);
        else std::snprintf(text, sizeof text, "X %.2f  Y %.2f  Z %.2f times", now_s[0] / std::max(start_s[0], 1e-6f), now_s[1] / std::max(start_s[1], 1e-6f), now_s[2] / std::max(start_s[2], 1e-6f));
        auto *draw = ImGui::GetWindowDrawList();
        const ImVec2 at{io.MousePos.x + px(18), io.MousePos.y + px(14)};
        const auto extent = ImGui::CalcTextSize(text);
        draw->AddRectFilled({at.x - px(4), at.y - px(2)}, {at.x + extent.x + px(4), at.y + extent.y + px(2)}, IM_COL32(10,14,20,210), px(3));
        draw->AddText(at, IM_COL32(255,255,255,255), text);
    }
    if (gizmo_active && !using_now && session.dragging()) {
        const auto ended = session.end_drag();
        if (!drag_error.empty()) log_error(drag_error);
        else report(ended, "Gizmo transform committed");
    }
    if (!using_now) { gizmo_following = false; gizmo_entry.clear(); }
    if (gizmo_active && !using_now) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    gizmo_active = using_now;
}
void EditorUI::game_overlay(gameplay::Game &game, ImVec2 origin, ImVec2 size, bool hovered) {
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
void EditorUI::free_mouse() {
    game_input.release(window);
    // Whatever was held stays held in the game otherwise, and the player would walk on by itself.
    if (session.game()) session.set_game_input({});
}
void EditorUI::stop_play() {
    game_input.release(window);
    report(session.stop(), "Stopped; runtime copy discarded, editor scene shown");
}
std::optional<ImVec2> EditorUI::project(glm::dvec3 p, ImVec2 origin, ImVec2 size) const {
    const auto clip = glm::make_mat4(frame->camera.projection.matrix.data()) * glm::make_mat4(frame->camera.view.matrix.data()) * glm::dvec4(p, 1);
    if (clip.w <= 1e-6) return std::nullopt;
    return ImVec2(origin.x + float((clip.x / clip.w + 1) / 2) * size.x, origin.y + float((1 - clip.y / clip.w) / 2) * size.y);
}
void EditorUI::bone_overlay(ImVec2 origin, ImVec2 size) {
    auto *draw = ImGui::GetWindowDrawList();
    for (const auto &line : session.bone_lines()) {
        const auto a = project({line.from.x, line.from.y, line.from.z}, origin, size), b = project({line.to.x, line.to.y, line.to.z}, origin, size);
        if (!a || !b) continue;
        const bool selected = session.selected().contains(line.entity);
        const ImU32 color = selected ? IM_COL32(120, 230, 255, 255) : IM_COL32(150, 190, 210, 170);
        draw->AddLine(*a, *b, color, px(selected ? 2.0f : 1.5f));
        draw->AddCircleFilled(*b, px(selected ? 3.0f : 2.0f), color);
    }
}
void EditorUI::ragdoll_overlay(ImVec2 origin, ImVec2 size) {
    auto *draw = ImGui::GetWindowDrawList();
    const auto c = physics::default_color(physics::DebugCategory::Ragdoll);
    const ImU32 color = ImGui::ColorConvertFloat4ToU32({c[0], c[1], c[2], 0.75f}), chosen = ImGui::ColorConvertFloat4ToU32({c[0], c[1], c[2], 1});
    for (const auto &line : session.ragdoll_lines()) {
        const auto a = project({line.a.x, line.a.y, line.a.z}, origin, size), b = project({line.b.x, line.b.y, line.b.z}, origin, size);
        if (!a || !b) continue;
        const bool selected = session.selected().contains(line.entity);
        draw->AddLine(*a, *b, selected ? chosen : color, px(selected ? 1.5f : 1.0f));
    }
}
void EditorUI::collider_outlines(ImVec2 origin, ImVec2 size) {
    auto *draw = ImGui::GetWindowDrawList();
    for (const auto &line : session.physics_bridge().debug_geometry().lines) {
        const auto a = project({line.a.x, line.a.y, line.a.z}, origin, size), b = project({line.b.x, line.b.y, line.b.z}, origin, size);
        if (!a || !b) continue;
        const auto c = physics::default_color(line.category);
        draw->AddLine(*a, *b, ImGui::ColorConvertFloat4ToU32({c[0], c[1], c[2], c[3]}));
    }
}
EntityId EditorUI::markers(ImVec2 origin, ImVec2 size) {
    const auto project = [&](glm::dvec3 p) { return this->project(p, origin, size); };
    auto *draw = ImGui::GetWindowDrawList();
    const auto mouse = ImGui::GetIO().MousePos;
    EntityId hovered = 0;
    for (const auto &e : session.scene().snapshot().entities) {
        if (!e.components.light && !e.components.camera) continue;
        if (!playing() && session.hidden(e.id)) continue;
        if (e.id == session.view_camera()) continue; // The camera being looked through sits at the eye.
        const auto m = glm::make_mat4(session.drawn_world(e.id)->matrix.data());
        const glm::dvec3 at(m[3]);
        const auto screen = project(at);
        if (!screen) continue;
        const bool selected = session.selected().contains(e.id);
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
} // namespace engine::editor
