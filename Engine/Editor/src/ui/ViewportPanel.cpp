#include "EditorUI.hpp"
#include "../GameHud.hpp"
#include <ImGuizmo.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>

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
} // namespace engine::editor
