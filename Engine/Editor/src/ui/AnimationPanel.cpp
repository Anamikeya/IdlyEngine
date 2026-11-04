#include "EditorUI.hpp"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <utility>

// The Animation panel (Milestone 5): previews the selected entity's animator. Clips can be played, crossfaded and layered
// on top, the timeline scrubbed, and events marked on the clips. Playing and scrubbing are the preview's, or the run's in
// Play: they never change the scene. Events are saved in the model's .meta, one undo step each.
namespace engine::editor {
std::vector<std::string> EditorUI::name_suggestions(std::string_view kind, EntityId id) const {
    std::vector<std::string> names;
    const auto record = session.scene().inspect(id);
    if (!record) return names;
    if (kind == "clip") {
        if (const auto clips = session.clip_set(id)) {
            for (std::size_t i = 0; i < clips->size(); ++i) names.push_back(clips->name(i));
        } else if (record->components.mesh) {
            if (const auto data = session.model(record->components.mesh->mesh_asset))
                for (const auto &clip : data->clips) names.push_back(clip.name);
        }
    } else if (kind == "bone" && record->parent) {
        if (const auto rig = session.rig(record->parent))
            for (std::size_t j = 0; j < rig->joint_count(); ++j) names.push_back(rig->joint_name(j));
    }
    return names;
}
void EditorUI::animation_panel() {
    if (!show_animation) return;
    // First shown floating at the lower left, over the hierarchy's empty space, clear of the view's middle.
    const auto &display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x * 0.005f, display.y * 0.42f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({display.x * 0.27f, display.y * 0.55f}, ImGuiCond_FirstUseEver);
    if (std::exchange(focus_animation, false)) ImGui::SetNextWindowFocus();
    const bool open = begin_panel("Animation", "Animation", context::details, &show_animation);
    if (!open) { ImGui::End(); return; }
    const EntityId id = session.selection();
    const auto record = id ? session.scene().inspect(id) : std::nullopt;
    if (!record || !record->components.animator) {
        if (record && record->components.mesh && session.rig(id)) {
            ImGui::TextWrapped("%s has a skeleton. Give it an Animator to play its clips.", record->name.c_str());
            if (tip_button("Add Animator", "Play the model's first clip")) {
                const auto data = session.model(record->components.mesh->mesh_asset);
                report(session.assign_clip(id, data && !data->clips.empty() ? data->clips.front().name : std::string()), "Animator added");
            }
            mark("Animation.AddAnimator");
        } else {
            ImGui::TextWrapped("Select an entity with an Animator to preview its clips, scrub them and mark events on them.");
        }
        ImGui::End();
        return;
    }
    std::string problem;
    const auto clips = session.clip_set(id, &problem);
    if (!problem.empty()) ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "%s", problem.c_str());
    auto &system = session.animation();
    const auto status = clips ? system.status(id) : std::nullopt;
    if (!clips || !status) {
        if (clips) ImGui::TextDisabled("Starting...");
        ImGui::End();
        return;
    }
    const auto &authored = *record->components.animator;
    ImGui::TextDisabled("%s: %zu joints, %zu clips", record->name.c_str(), clips->rig()->joint_count(), clips->size());
    if (!playing() && (!session.view_flags.animations || !authored.preview))
        ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "%s", !session.view_flags.animations ? "Animations are off in the viewport (View > Play Animations). Scrubbing still works."
                                                                                       : "This animator's preview is off. Scrubbing still works.");
    const auto quiet = [this](const Result<void> &r) { if (!r) log_error(r.error().message); };
    // ---- Transport and timeline ----
    const bool running = !status->paused;
    if (tip_button(running ? "Pause" : "Play", running ? "Hold the animation where it is" : "Play on from here")) quiet(system.set_paused(id, running));
    mark("Animation.PlayPause");
    ImGui::SameLine();
    if (tip_button("Restart", "Play the authored clip again from its start time")) {
        if (!authored.clip.empty() && clips->find(authored.clip)) {
            quiet(system.play(id, authored.clip, 0, authored.loop));
            quiet(system.set_time(id, authored.start_time));
            quiet(system.set_paused(id, false));
        }
    }
    mark("Animation.Restart");
    ImGui::SameLine();
    ImGui::TextUnformatted(status->clip.empty() ? "(resting)" : status->clip.c_str());
    const auto events = session.clip_events(session.clip_source(id));
    if (!status->clip.empty()) {
        float t = float(status->time);
        char label[64];
        std::snprintf(label, sizeof label, "%.2f / %.2f s", status->time, status->duration);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##time", &t, 0, float(std::max(status->duration, 1e-3)), label, ImGuiSliderFlags_AlwaysClamp)) {
            // Scrubbing holds the animation where it is put.
            quiet(system.set_paused(id, true));
            quiet(system.set_time(id, t));
        }
        mark("Animation.Timeline");
        // The clip's events as ticks along the timeline.
        if (status->duration > 0) {
            const auto low = ImGui::GetItemRectMin(), high = ImGui::GetItemRectMax();
            auto *draw = ImGui::GetWindowDrawList();
            for (const auto &event : events)
                if (event.clip == status->clip) {
                    const float x = low.x + (high.x - low.x) * float(std::clamp(event.time / status->duration, 0.0, 1.0));
                    draw->AddTriangleFilled({x - px(4), low.y}, {x + px(4), low.y}, {x, low.y + px(5)}, IM_COL32(250, 200, 80, 255));
                }
        }
    }
    if (!status->fading_from.empty()) ImGui::TextDisabled("Crossfading from %s: %.0f%%", status->fading_from.c_str(), status->fade * 100);
    if (!status->layered.empty()) ImGui::TextDisabled("Layered on top: %s at %.0f%%", status->layered.c_str(), status->layered_weight * 100);
    // ---- Clips ----
    ImGui::SeparatorText("Clips");
    ImGui::SetNextItemWidth(px(110));
    ImGui::DragFloat("Crossfade (s)", &animation_blend, 0.01f, 0, 2, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    mark("Animation.Blend");
    if (ImGui::BeginTable("##clips", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (std::size_t i = 0; i < clips->size(); ++i) {
            const auto &clip_name = clips->name(i);
            ImGui::PushID(int(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            const bool current = clip_name == status->clip, chosen = clip_name == authored.clip;
            if (current) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 1, 1));
            ImGui::Text("%s%s", clip_name.c_str(), chosen ? "  (default)" : "");
            if (current) ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.2f s", clips->duration(i));
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Play")) {
                quiet(system.play(id, clip_name, animation_blend, authored.loop));
                quiet(system.set_paused(id, false));
            }
            ImGui::SetItemTooltip("Crossfade to this clip over the time above. Only the preview changes.");
            mark("Animation.Play." + std::to_string(i));
            ImGui::SameLine();
            if (ImGui::SmallButton("Layer")) quiet(system.play_layered(id, clip_name, 1, 0.1));
            ImGui::SetItemTooltip("Play it once on top of the current clip, as the difference from its first frame: a flinch over a walk.");
            mark("Animation.Layer." + std::to_string(i));
            ImGui::SameLine();
            ImGui::BeginDisabled(chosen);
            if (ImGui::SmallButton("Default")) report(session.assign_clip(id, clip_name), ("Animator plays " + clip_name).c_str());
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Make it the clip the animator starts with. This changes the scene: one undo step.");
            mark("Animation.Default." + std::to_string(i));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // ---- Events of the current clip, in the model's .meta ----
    ImGui::SeparatorText("Events");
    const auto source = session.clip_source(id);
    if (status->clip.empty()) ImGui::TextDisabled("Play a clip to see its events.");
    else {
        ImGui::BeginDisabled(playing());
        bool changed = false;
        for (std::size_t i = 0; i < events.size() && !changed; ++i) {
            if (events[i].clip != status->clip) continue;
            ImGui::PushID(int(i));
            float t = event_drag && event_drag->first == i ? event_drag->second : events[i].time;
            ImGui::SetNextItemWidth(px(90));
            if (ImGui::DragFloat("##time", &t, 0.005f, 0, float(std::max(status->duration, 0.0)), "%.3f s", ImGuiSliderFlags_AlwaysClamp)) event_drag = std::pair{i, t};
            if (ImGui::IsItemDeactivatedAfterEdit() && event_drag) {
                auto moved = events;
                moved[i].time = event_drag->second;
                event_drag.reset();
                report(session.set_clip_events(source, moved), "Event moved");
                changed = true;
            }
            mark("Animation.EventTime." + std::to_string(i));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(events[i].name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Go")) { quiet(system.set_paused(id, true)); quiet(system.set_time(id, events[i].time)); }
            ImGui::SetItemTooltip("Move the timeline to this event");
            ImGui::SameLine();
            if (!changed && ImGui::SmallButton("Delete")) {
                auto fewer = events;
                fewer.erase(fewer.begin() + std::ptrdiff_t(i));
                report(session.set_clip_events(source, fewer), "Event deleted");
                changed = true;
            }
            mark("Animation.EventDelete." + std::to_string(i));
            ImGui::PopID();
        }
        ImGui::SetNextItemWidth(px(110));
        ImGui::InputTextWithHint("##event", "event name", event_name.data(), event_name.size(), ImGuiInputTextFlags_CharsNoBlank);
        mark("Animation.EventName");
        ImGui::SameLine();
        char add[48];
        std::snprintf(add, sizeof add, "Add at %.2f s", status->time);
        if (!changed && tip_button(add, "Mark an event at the timeline's time; gameplay hears it each time playback crosses it")) {
            auto more = events;
            more.push_back({status->clip, float(status->time), event_name.data()});
            report(session.set_clip_events(source, more), "Event added");
        }
        mark("Animation.AddEvent");
        ImGui::EndDisabled();
        if (playing()) ImGui::TextDisabled("Events are edited while editing, not in Play.");
    }
    // ---- What fired lately ----
    int shown = 0;
    for (auto it = session.animation_events().rbegin(); it != session.animation_events().rend() && shown < 5; ++it) {
        if (it->entity != id) continue;
        if (shown++ == 0) ImGui::SeparatorText("Fired");
        ImGui::TextDisabled("%s in %s at %.2f s", it->name.c_str(), it->clip.c_str(), it->time);
    }
    ImGui::Spacing();
    if (ImGui::Checkbox("Show bones", &session.view_flags.bones)) save_settings();
    mark("Animation.ShowBones");
    ImGui::End();
}
} // namespace engine::editor
