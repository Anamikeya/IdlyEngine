#include "GameHud.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

namespace engine::editor {
namespace {
using gameplay::GamePhase;
void text(ImDrawList *draw, ImVec2 at, float size, ImU32 color, const char *value) {
    // A soft shadow keeps text readable over a bright sky.
    draw->AddText(ImGui::GetFont(), size, {at.x + 1, at.y + 1}, IM_COL32(0, 0, 0, 200), value);
    draw->AddText(ImGui::GetFont(), size, at, color, value);
}
ImVec2 measure(float size, const char *value) { return ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, value); }
// Shrinks a line that would not fit, so a narrow viewport still shows all of it.
void centered(ImDrawList *draw, ImVec2 center, float size, ImU32 color, const char *value, float width = FLT_MAX) {
    auto extent = measure(size, value);
    if (extent.x > width) { size *= width / extent.x; extent = measure(size, value); }
    text(draw, {center.x - extent.x / 2, center.y - extent.y / 2}, size, color, value);
}
}
HudAction draw_game_hud(const gameplay::HudState &hud, ImVec2 origin, ImVec2 size, bool captured) {
    auto *draw = ImGui::GetWindowDrawList();
    const float unit = ImGui::GetFontSize() / 13, line = 20 * unit, big = 30 * unit, huge = 54 * unit;
    const ImVec2 center{origin.x + size.x / 2, origin.y + size.y / 2};
    const ImVec2 far_corner{origin.x + size.x, origin.y + size.y};
    const float margin = 18 * unit;
    constexpr ImU32 white = IM_COL32(240, 244, 250, 255), dim = IM_COL32(190, 200, 215, 255), warm = IM_COL32(255, 205, 90, 255), danger = IM_COL32(255, 95, 80, 255);
    char buffer[128];

    // Crosshair, with a hit marker that fades.
    if (hud.phase == GamePhase::Playing) {
        const float gap = 5 * unit, arm = 9 * unit, thick = 1.6f * unit;
        const ImU32 cross = IM_COL32(255, 255, 255, 220);
        for (const ImVec2 d : {ImVec2{1, 0}, ImVec2{-1, 0}, ImVec2{0, 1}, ImVec2{0, -1}})
            draw->AddLine({center.x + d.x * gap, center.y + d.y * gap}, {center.x + d.x * (gap + arm), center.y + d.y * (gap + arm)}, cross, thick);
        if (hud.hit_marker > 0) {
            const auto alpha = static_cast<int>(255 * hud.hit_marker);
            const float a = 7 * unit, b = 14 * unit;
            for (const ImVec2 d : {ImVec2{1, 1}, ImVec2{-1, 1}, ImVec2{1, -1}, ImVec2{-1, -1}})
                draw->AddLine({center.x + d.x * a, center.y + d.y * a}, {center.x + d.x * b, center.y + d.y * b}, IM_COL32(255, 80, 60, alpha), 2.4f * unit);
        }
    }
    // Score and targets, top left. Timer, top center.
    std::snprintf(buffer, sizeof buffer, "SCORE  %d", hud.score);
    text(draw, {origin.x + margin, origin.y + margin}, big, white, buffer);
    std::snprintf(buffer, sizeof buffer, "TARGETS  %d / %d", hud.targets_total - hud.targets_remaining, hud.targets_total);
    text(draw, {origin.x + margin, origin.y + margin + 1.3f * big}, line, dim, buffer);
    const int seconds = static_cast<int>(std::ceil(hud.time_remaining));
    std::snprintf(buffer, sizeof buffer, "%d:%02d", seconds / 60, seconds % 60);
    centered(draw, {center.x, origin.y + margin + big / 2}, big * 1.2f, hud.time_remaining <= 10 && hud.phase == GamePhase::Playing ? danger : white, buffer);
    // Ammo, bottom right.
    if (hud.magazine > 0) {
        if (hud.reserve < 0) std::snprintf(buffer, sizeof buffer, "%d", hud.ammo);
        else std::snprintf(buffer, sizeof buffer, "%d / %d", hud.ammo, hud.reserve);
        const auto extent = measure(big * 1.2f, buffer);
        text(draw, {far_corner.x - margin - extent.x, far_corner.y - margin - extent.y}, big * 1.2f, hud.ammo == 0 ? danger : white, buffer);
        if (hud.reloading) {
            const float width = 160 * unit, height = 6 * unit, top = far_corner.y - margin - extent.y - 2 * height;
            draw->AddRectFilled({far_corner.x - margin - width, top}, {far_corner.x - margin, top + height}, IM_COL32(0, 0, 0, 140));
            draw->AddRectFilled({far_corner.x - margin - width, top}, {far_corner.x - margin - width + width * float(hud.reload_progress), top + height}, warm);
            const auto label = measure(line, "RELOADING");
            text(draw, {far_corner.x - margin - label.x, top - label.y - 2 * unit}, line, warm, "RELOADING");
        } else if (hud.ammo == 0 && hud.phase == GamePhase::Playing) {
            const auto label = measure(line, "R TO RELOAD");
            text(draw, {far_corner.x - margin - label.x, far_corner.y - margin - extent.y - label.y - 4 * unit}, line, danger, "R TO RELOAD");
        }
    }
    // The latest scoring event.
    if (hud.phase == GamePhase::Playing && !hud.last_event.empty() && hud.last_event_age < 1.6)
        centered(draw, {center.x, center.y + 60 * unit}, line * 1.1f, warm, hud.last_event.c_str());
    if (!hud.fall_notice.empty()) centered(draw, {center.x, center.y + 90 * unit}, line, danger, hud.fall_notice.c_str());
    if (hud.phase == GamePhase::Playing) return HudAction::None;

    // Overlays with choices, once the mouse is free.
    draw->AddRectFilled(origin, far_corner, IM_COL32(6, 9, 14, 150));
    const char *title = "PHYSICS SHOOTER";
    ImU32 color = white;
    std::string detail;
    if (hud.phase == GamePhase::Ready) {
        std::snprintf(buffer, sizeof buffer, "Destroy all %d targets in %d seconds. Shots push what they hit: try the crates.", hud.targets_total, static_cast<int>(hud.time_limit));
        detail = buffer;
    } else if (hud.phase == GamePhase::Paused) {
        title = "PAUSED";
        detail = "Click the view to resume. Esc again stops.";
    } else if (hud.phase == GamePhase::Won) {
        title = "YOU WIN";
        color = IM_COL32(120, 235, 140, 255);
        std::snprintf(buffer, sizeof buffer, "Final score %d, with %d seconds left.", hud.score, seconds);
        detail = buffer;
    } else {
        title = "TIME'S UP";
        color = danger;
        std::snprintf(buffer, sizeof buffer, "Final score %d. %d of %d targets destroyed.", hud.score, hud.targets_total - hud.targets_remaining, hud.targets_total);
        detail = buffer;
    }
    const float top = center.y - 90 * unit;
    centered(draw, {center.x, top}, huge, color, title);
    centered(draw, {center.x, top + 50 * unit}, line, white, detail.c_str(), size.x * 0.94f);
    if (captured) return HudAction::None;
    float y = top + 84 * unit;
    if (hud.phase == GamePhase::Ready) {
        centered(draw, {center.x, y}, line, dim, "W A S D move   |   Mouse look   |   Left mouse fire   |   Space jump   |   Shift sprint   |   R reload   |   Esc pause   |   F8 look around", size.x * 0.94f);
        y += 34 * unit;
    }
    HudAction action = HudAction::None;
    const auto button = [&](const char *label, float x, HudAction chosen) {
        ImGui::SetCursorScreenPos({x, y});
        if (ImGui::Button(label, {120 * unit, 32 * unit})) action = chosen;
    };
    if (hud.phase == GamePhase::Ready) {
        button("Start", center.x - 60 * unit, HudAction::Start);
    } else {
        if (hud.phase == GamePhase::Paused) button("Resume", center.x - 195 * unit, HudAction::Resume);
        button("Restart", hud.phase == GamePhase::Paused ? center.x - 60 * unit : center.x - 125 * unit, HudAction::Restart);
        button("Stop", hud.phase == GamePhase::Paused ? center.x + 75 * unit : center.x + 5 * unit, HudAction::Stop);
    }
    return action;
}
} // namespace engine::editor
