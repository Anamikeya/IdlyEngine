#include "EditorUI.hpp"

namespace engine::editor {
void EditorUI::lighting() {
    begin_panel("Lighting", "Lighting", context::details);
    auto e = session.environment();
    const auto owner = session.environment_entity();
    if (owner) ImGui::TextDisabled("On entity '%s'", session.scene().inspect(owner)->name.c_str());
    else ImGui::TextWrapped("This scene has no environment, so the defaults below apply. Changing anything adds an Environment entity.");
    if (playing()) ImGui::TextColored({0.45f, 0.85f, 0.45f, 1}, "Play mode: edits affect the runtime copy only");
    ImGui::SeparatorText("Presets");
    const bool outdoor = ImGui::Button("Outdoor");
    mark("Lighting.Outdoor");
    if (outdoor) { report(session.set_environment(editor::EditorSession::outdoor_environment()), "Outdoor lighting applied"); return ImGui::End(); }
    ImGui::SameLine();
    const bool indoor = ImGui::Button("Indoor");
    mark("Lighting.Indoor");
    if (indoor) { report(session.set_environment(editor::EditorSession::indoor_environment()), "Indoor lighting applied"); return ImGui::End(); }
    ImGui::SameLine();
    if (ImGui::Button("Add sun")) report(session.create_preset_lights(true), "Sun created");
    mark("Lighting.AddSun");
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
} // namespace engine::editor
