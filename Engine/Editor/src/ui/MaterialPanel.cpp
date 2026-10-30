#include "EditorUI.hpp"
#include <algorithm>

namespace engine::editor {
bool EditorUI::texture_slot(const char *label, assets::MaterialTexture &slot) {
    bool changed = false;
    if (ImGui::BeginCombo(label, slot.empty() ? "(none)" : slot.path.c_str())) {
        if (ImGui::Selectable("(none)", slot.empty())) { slot = {}; changed = true; }
        for (const auto &meta : session.assets())
            if (meta.type == assets::AssetType::Texture && ImGui::Selectable(meta.path.c_str(), slot.id == meta.id)) { slot = {meta.id, meta.path}; changed = true; }
        ImGui::EndCombo();
    }
    return changed;
}
void EditorUI::material_editor() {
    const bool open = begin_panel("Material Editor", "Material Editor", context::details);
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
        // The four number fields alone: the whole control also has a swatch and its label.
        if (ui_map.enabled()) {
            const auto at = ImGui::GetCursorScreenPos();
            const auto width = ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x;
            ui_map.mark("Material.BaseColor.Fields", at, {at.x + width, at.y + ImGui::GetFrameHeight()});
        }
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
        mark(std::string("Material.") + label);
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
    const bool alpha_changed = ImGui::Combo("Alpha mode", &alpha, "Opaque\0Mask\0Blend\0");
    mark("Material.AlphaMode");
    if (alpha_changed) { m.alpha_mode = assets::AlphaMode(alpha); commit = true; }
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
} // namespace engine::editor
