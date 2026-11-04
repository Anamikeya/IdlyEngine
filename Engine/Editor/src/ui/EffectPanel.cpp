#include "EditorUI.hpp"
#include <algorithm>
#include <cstdio>

// The Effect Editor (Milestone 5): edits the particle effect chosen in the Asset Browser. Dragging a value shows it live
// on every emitter using the effect; releasing it saves the file as one undo step, as the Material Editor does.
namespace engine::editor {
// What New effect starts from: a warm spray that reads against a bright sky and a dark room alike, to be shaped from there.
assets::EffectData EditorUI::starter_effect() {
    assets::EffectData e;
    e.rate = 40;
    e.lifetime = {0.8, 1.4};
    e.speed = {2.5, 4};
    e.cone_degrees = 15;
    e.acceleration = {0, -4.9, 0};
    e.size = {{0, 0.14}, {1, 0.04}};
    e.color = {{0, {1, 0.8, 0.4, 1}}, {1, {1, 0.35, 0.05, 0}}};
    e.emissive_strength = 3;
    return e;
}
void EditorUI::effect_editor() {
    if (!show_effect) return;
    const bool open = begin_panel("Effect Editor", "Effect Editor", context::details, &show_effect);
    const auto catalogue = session.assets();
    const auto chosen = std::find_if(catalogue.begin(), catalogue.end(), [&](const assets::AssetMetadata &a) { return a.id == selected_asset && a.type == assets::AssetType::Effect; });
    if (chosen == catalogue.end() || !open) {
        if (open) ImGui::TextWrapped("Choose a particle effect in the Asset Browser, or make one there with New effect.");
        ImGui::End();
        return;
    }
    // Re-read unless a field is being dragged: undo and other sources may have changed the file.
    if (effect_edit_id != chosen->id || !ImGui::IsAnyItemActive()) {
        if (const auto current = session.effect(chosen->id)) { effect_edit = *current; effect_edit_id = chosen->id; }
        else { ImGui::TextColored({0.95f, 0.45f, 0.35f, 1}, "This effect cannot be read: its file is missing or invalid."); ImGui::End(); return; }
    }
    ImGui::TextDisabled("%s", chosen->path.c_str());
    const auto stats = session.effect_stats();
    ImGui::TextDisabled("%zu particles alive in %zu emitters and %zu one-shots", stats.particles, stats.emitters, stats.one_shots);
    if (tip_button("Restart", "Start every emitter again from its beginning")) session.restart_effects();
    mark("Effect.Restart");
    ImGui::SameLine();
    if (tip_button("Place in scene", "Add an emitter playing this effect on the surface in the middle of the view")) {
        // On what the view's center looks at, as a drop would place it, so the emitter is not buried inside an object.
        Vec3 at = session.camera.target;
        if (frame) if (const auto hit = session.ray_hit(*frame, 0, 0)) at = *hit;
        report(session.place_asset_at(chosen->id, at), "Emitter added to the scene");
    }
    mark("Effect.Place");
    if (!session.view_flags.effects && !playing()) ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "Effects are off in the viewport. Turn on View > Play Effects.");
    auto &e = effect_edit;
    bool live = false, commit = false;
    const auto changed_by = [&](bool changed) { live |= changed; commit |= committed(changed); };
    const auto number = [&](const char *label, double &value, float speed, double low, double high, const char *format = "%.3f") {
        float v = float(value);
        const bool changed = ImGui::DragFloat(label, &v, speed, float(low), float(high), format, ImGuiSliderFlags_AlwaysClamp);
        mark(std::string("Effect.") + label);
        if (changed) value = v;
        changed_by(changed);
    };
    // A min and max pair; the max never drops below the min.
    const auto range = [&](const char *label, std::array<double, 2> &value, float speed, double low, double high) {
        float v[2]{float(value[0]), float(value[1])};
        const bool changed = ImGui::DragFloat2(label, v, speed, float(low), float(high), "%.3f", ImGuiSliderFlags_AlwaysClamp);
        mark(std::string("Effect.") + label);
        if (changed) { value[0] = v[0]; value[1] = std::max(double(v[1]), double(v[0])); }
        changed_by(changed);
    };
    const auto whole = [&](const char *label, std::uint32_t &value, int low, int high) {
        int v = int(value);
        const bool changed = ImGui::DragInt(label, &v, 1, low, high, "%d", ImGuiSliderFlags_AlwaysClamp);
        mark(std::string("Effect.") + label);
        if (changed) value = std::uint32_t(std::clamp(v, low, high));
        changed_by(changed);
    };
    const auto flag = [&](const char *label, bool &value) {
        if (ImGui::Checkbox(label, &value)) commit = true;
        mark(std::string("Effect.") + label);
    };
    ImGui::SeparatorText("Emission");
    number("Duration (s)", e.duration, 0.01f, 0.01, 3600);
    number("Rate (per s)", e.rate, 0.5f, 0, 100000, "%.1f");
    whole("Burst", e.burst, 0, int(assets::max_effect_particles));
    whole("Max particles", e.max_particles, 1, int(assets::max_effect_particles));
    ImGui::SeparatorText("Motion");
    range("Lifetime (s)", e.lifetime, 0.01f, 0.01, 600);
    range("Speed (m/s)", e.speed, 0.05f, 0, 10000);
    number("Cone (deg)", e.cone_degrees, 0.5f, 0, 180, "%.1f");
    number("Spawn radius (m)", e.spawn_radius, 0.005f, 0, 1000);
    {
        float a[3]{float(e.acceleration[0]), float(e.acceleration[1]), float(e.acceleration[2])};
        const bool changed = ImGui::DragFloat3("Acceleration", a, 0.05f, -1000, 1000, "%.2f");
        mark("Effect.Acceleration");
        if (changed) e.acceleration = {a[0], a[1], a[2]};
        changed_by(changed);
    }
    number("Drag (1/s)", e.drag, 0.01f, 0, 100);
    range("Spin (deg/s)", e.spin_degrees, 1.0f, -3600, 3600);
    number("Stretch (s)", e.stretch, 0.001f, 0, 10);
    ImGui::TextDisabled("Stretch above 0 draws streaks along the velocity, for sparks.");
    flag("World space", e.world_space);
    ImGui::SetItemTooltip("On: particles stay where they were born when the emitter moves. Off: they move with it.");
    flag("Bounce on a floor", e.collide_floor);
    if (e.collide_floor) {
        number("Floor height (m)", e.floor_height, 0.01f, -10000, 10000);
        number("Bounce", e.bounce, 0.01f, 0, 1);
    }
    ImGui::SeparatorText("Look");
    int blend = int(e.blend);
    const bool blend_changed = ImGui::Combo("Blend", &blend, "Additive (light)\0Alpha (smoke, dust)\0");
    mark("Effect.Blend");
    if (blend_changed) { e.blend = assets::ParticleBlend(blend); commit = true; }
    number("Strength", e.emissive_strength, 0.05f, 0, 1000, "%.2f");
    ImGui::SetItemTooltip("Multiplies the color. Above 1 it blooms.");
    commit |= texture_slot("Image", e.texture);
    mark("Effect.Image");
    // ---- Curves over a particle's life: up to eight keys each ----
    ImGui::SeparatorText("Size over life (m)");
    for (std::size_t i = 0; i < e.size.size(); ++i) {
        ImGui::PushID(int(i));
        float key[2]{float(e.size[i].time), float(e.size[i].size)};
        ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
        const bool changed = ImGui::DragFloat2("##size", key, 0.005f, 0, 1000, "%.3f");
        mark("Effect.Size" + std::to_string(i));
        if (changed) { e.size[i].time = std::clamp(double(key[0]), 0.0, 1.0); e.size[i].size = std::max(double(key[1]), 0.0); }
        changed_by(changed);
        ImGui::SameLine();
        ImGui::BeginDisabled(e.size.size() <= 1);
        if (ImGui::SmallButton("x")) { e.size.erase(e.size.begin() + std::ptrdiff_t(i)); commit = true; ImGui::EndDisabled(); ImGui::PopID(); break; }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Remove this key");
        ImGui::PopID();
    }
    ImGui::BeginDisabled(e.size.size() >= assets::max_effect_keys);
    if (ImGui::SmallButton("Add size key")) { e.size.push_back({1, e.size.empty() ? 0.1 : e.size.back().size}); commit = true; }
    ImGui::EndDisabled();
    mark("Effect.AddSizeKey");
    ImGui::TextDisabled("Each key is a time of life from 0 to 1, then a width.");
    ImGui::SeparatorText("Color over life");
    for (std::size_t i = 0; i < e.color.size(); ++i) {
        ImGui::PushID(int(i) + 100);
        float time = float(e.color[i].time);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4);
        const bool time_changed = ImGui::DragFloat("##time", &time, 0.005f, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        mark("Effect.ColorTime" + std::to_string(i));
        if (time_changed) e.color[i].time = time;
        changed_by(time_changed);
        ImGui::SameLine();
        float c[4]{float(e.color[i].color[0]), float(e.color[i].color[1]), float(e.color[i].color[2]), float(e.color[i].color[3])};
        const bool color_changed = ImGui::ColorEdit4("##color", c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar);
        mark("Effect.Color" + std::to_string(i));
        if (color_changed) for (int k = 0; k < 4; ++k) e.color[i].color[std::size_t(k)] = std::clamp(double(c[k]), 0.0, 1.0);
        changed_by(color_changed);
        ImGui::SameLine();
        ImGui::BeginDisabled(e.color.size() <= 1);
        if (ImGui::SmallButton("x")) { e.color.erase(e.color.begin() + std::ptrdiff_t(i)); commit = true; ImGui::EndDisabled(); ImGui::PopID(); break; }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Remove this key");
        ImGui::PopID();
    }
    ImGui::BeginDisabled(e.color.size() >= assets::max_effect_keys);
    if (ImGui::SmallButton("Add color key")) { e.color.push_back({1, e.color.empty() ? std::array<double, 4>{1, 1, 1, 0} : e.color.back().color}); commit = true; }
    ImGui::EndDisabled();
    mark("Effect.AddColorKey");
    // Keys must run in time order to be valid; editing a time can cross a neighbour, so they are sorted, and keys at the
    // same time are nudged apart.
    const auto order = [](auto &keys) {
        std::stable_sort(keys.begin(), keys.end(), [](const auto &a, const auto &b) { return a.time < b.time; });
        for (std::size_t i = 1; i < keys.size(); ++i)
            if (keys[i].time <= keys[i - 1].time) keys[i].time = std::min(keys[i - 1].time + 0.001, 1.0);
        while (keys.size() > 1 && keys[keys.size() - 1].time <= keys[keys.size() - 2].time) keys.pop_back();
    };
    if (live || commit) { order(e.size); order(e.color); }
    if (commit) report(session.save_effect(chosen->id, e), "Effect saved");
    else if (live) { if (auto shown = session.preview_effect(chosen->id, e); !shown) log_error(shown.error().message); }
    ImGui::TextDisabled("Changes save to the file when a control is released; Ctrl+Z undoes them.");
    ImGui::End();
}
} // namespace engine::editor
