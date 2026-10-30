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
        current.insert("Physics: " + (record ? record->name : std::to_string(issue.entity)) + ": " + issue.message);
    }
    for (const auto &message : current) if (!reported_issues.contains(message)) log(message);
    reported_issues = std::move(current);
    if (session.physics_error() != reported_physics_error) {
        reported_physics_error = session.physics_error();
        if (!reported_physics_error.empty()) log("Error: " + reported_physics_error);
    }
}
void EditorUI::report_assets() {
    std::set<std::string> current(session.asset_problems().begin(), session.asset_problems().end());
    for (const auto &issue : session.asset_issues()) current.insert("Asset folder: " + issue.path + ": " + issue.message);
    for (const auto &issue : renderer.take_issues()) log("Renderer: " + issue);
    for (const auto &message : current) if (!reported_asset_problems.contains(message)) log(message);
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
    auto *game = session.game();
    if (game && game_input.captured) session.select(0); // The view belongs to the player: no gizmo, no picking.
    update_camera(hovered, size.y);
    session.view_flags.grid = show_grid && !game_view;
    session.grid_spacing = snap.grid_size;
    auto snapshot = session.frame(double(size.x) / size.y);
    if (!snapshot) { log(snapshot.error().message); ImGui::End(); return; }
    frame = snapshot.value();
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList()); ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
    ImGuizmo::SetOrthographic(frame->camera.lens.orthographic_half_height > 0);
    gizmo(origin, size);
    if (show_colliders && !game && !game_view) collider_outlines(origin, size);
    if (!game && !game_view) selection_outlines(origin, size);
    report_physics();
    report_assets();
    if (const auto problem = session.take_view_state_problem(); !problem.empty()) log("Error: " + problem);
    const EntityId marker = game || game_view || !show_markers ? 0 : markers(origin, size);
    if (camera_control.clicked() && !game) {
        const auto hit = marker ? marker : session.pick(*frame, (io.MousePos.x-origin.x) / size.x * 2-1, 1-(io.MousePos.y-origin.y) / size.y * 2);
        if (!io.KeyCtrl) session.select(hit);
        else session.selection_set().toggle(hit);
    }
    auto *draw = ImGui::GetWindowDrawList();
    const float margin = px(12), line = ImGui::GetTextLineHeightWithSpacing();
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
        draw->AddRect(origin, ImVec2(origin.x+size.x, origin.y+size.y), IM_COL32(115,215,115,255), 0.0f, ImDrawFlags_None, px(3));
        draw->AddText({origin.x+margin,below_overlay+line}, IM_COL32(115,215,115,255),
            session.play_state() == editor::PlayState::Paused ? "PLAY MODE (paused) - changes are discarded on Stop"
                                                              : "PLAY MODE - changes are discarded on Stop");
        std::string view = "Viewing through the editor camera (no primary camera)";
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
    if (session.game() || (camera_holding && !glfwGetWindowAttrib(window, GLFW_FOCUSED))) {
        camera_control.reset();
        if (camera_holding) hold_cursor(false);
        if (session.game()) return;
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
    if (ImGui::Button(view_names[view_kind])) ImGui::OpenPopup("##view-kind");
    mark("View.Kind");
    if (ImGui::BeginPopup("##view-kind")) {
        for (const auto *id : {"Viewport.Perspective", "Viewport.Front", "Viewport.Top", "Viewport.Side"}) command_item(id);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Lit")) ImGui::OpenPopup("##view-mode");
    mark("View.Mode");
    if (ImGui::BeginPopup("##view-mode")) {
        for (const auto *id : {"Viewport.Lit", "Viewport.Wireframe", "Viewport.Unlit"}) command_item(id);
        ImGui::SeparatorText("Show");
        for (const auto *id : {"Viewport.ShowGrid", "Viewport.ToggleColliders", "Viewport.ShowMarkers", "Viewport.ShowSkybox", "Viewport.ShowShadows", "Viewport.ShowAmbientOcclusion", "Viewport.ShowBloom"}) command_item(id);
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
        ImGui::SetItemTooltip("%s", registry.tooltip(item.command).c_str());
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
    ImGui::SetItemTooltip("Camera speed in meters a second. RMB + Wheel changes it while flying.");
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
    const auto *game = session.game();
    const auto world = session.scene().world_transform(session.selection());
    if (!world || game || game_view) {
        if (gizmo_active && session.dragging()) (void)session.end_drag();
        gizmo_active = false;
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
        constexpr const char *verbs[] = {"Move", "Rotate", "Scale"};
        if (const auto begun = session.begin_drag(verbs[operation], io.KeyAlt); !begun) log("Error: " + begun.error().message);
    }
    if (using_now && session.dragging() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        gizmo_cancelled = true;
        if (gizmo_following) session.camera = gizmo_camera_start;
        report(session.cancel_drag(), "Drag cancelled");
    }
    if (gizmo_cancelled) std::copy_n(gizmo_start.begin(), 16, gizmo_matrix);
    // Hold V to land on the nearest vertex of something else.
    if (using_now && session.dragging() && operation == 0 && ImGui::IsKeyDown(ImGuiKey_V)) {
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
        char text[96];
        if (operation == 0) std::snprintf(text, sizeof text, "X %.2f  Y %.2f  Z %.2f", now_t[0] - start_t[0], now_t[1] - start_t[1], now_t[2] - start_t[2]);
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
        if (!drag_error.empty()) log("Error: " + drag_error);
        else report(ended, "Gizmo transform committed");
    }
    if (!using_now) gizmo_following = false;
    gizmo_active = using_now;
}
void EditorUI::selection_outlines(ImVec2 origin, ImVec2 size) {
    auto *draw = ImGui::GetWindowDrawList();
    for (const auto id : session.selected().items()) {
        const auto bounds = session.local_bounds(id);
        const auto world = session.scene().world_transform(id);
        if (!bounds || !world) continue;
        const auto m = glm::make_mat4(world->matrix.data());
        const auto &[low, high] = *bounds;
        std::array<std::optional<ImVec2>, 8> corners;
        for (int i = 0; i < 8; ++i)
            corners[std::size_t(i)] = project(glm::dvec3(m * glm::dvec4(i & 1 ? high.x : low.x, i & 2 ? high.y : low.y, i & 4 ? high.z : low.z, 1)), origin, size);
        const ImU32 color = id == session.selection() ? IM_COL32(255, 170, 40, 255) : IM_COL32(255, 210, 120, 200);
        for (int a = 0; a < 8; ++a)
            for (const int bit : {1, 2, 4})
                if (!(a & bit) && corners[std::size_t(a)] && corners[std::size_t(a | bit)]) draw->AddLine(*corners[std::size_t(a)], *corners[std::size_t(a | bit)], color, px(1.5f));
    }
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
void EditorUI::stop_play() {
    game_input.release(window);
    report(session.stop(), "Stopped; runtime copy discarded, editor scene shown");
}
std::optional<ImVec2> EditorUI::project(glm::dvec3 p, ImVec2 origin, ImVec2 size) const {
    const auto clip = glm::make_mat4(frame->camera.projection.matrix.data()) * glm::make_mat4(frame->camera.view.matrix.data()) * glm::dvec4(p, 1);
    if (clip.w <= 1e-6) return std::nullopt;
    return ImVec2(origin.x + float((clip.x / clip.w + 1) / 2) * size.x, origin.y + float((1 - clip.y / clip.w) / 2) * size.y);
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
        const auto m = glm::make_mat4(session.scene().world_transform(e.id)->matrix.data());
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
