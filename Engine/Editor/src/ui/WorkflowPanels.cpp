#include "EditorUI.hpp"
#include <engine/editor/FuzzyMatch.hpp>
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <sstream>

// The command palette, quick open and the console: the ways to reach anything by typing.
namespace engine::editor {
namespace {
int picker_keys(ImGuiInputTextCallbackData *data) {
    auto &index = *static_cast<int *>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) index += data->EventKey == ImGuiKey_DownArrow ? 1 : -1;
    return 0;
}
}
void EditorUI::open_picker(Picker kind) {
    picker = kind;
    picker_focus = true;
    picker_text[0] = 0;
    picker_index = 0;
}
void EditorUI::remember_recent(const std::string &id) {
    std::erase(recent_commands, id);
    recent_commands.insert(recent_commands.begin(), id);
    if (recent_commands.size() > 12) recent_commands.resize(12);
    std::string text;
    for (const auto &recent : recent_commands) text += (text.empty() ? "" : ",") + recent;
    settings.set_text("palette.recent", text);
    save_settings();
}
void EditorUI::picker_window() {
    if (picker == Picker::None) return;
    // ---- What can be picked ----
    struct Item { std::string search, label, detail, keys; bool enabled = true; std::function<void()> run; };
    std::vector<Item> items;
    if (picker == Picker::Commands) {
        // Recently used first, so the empty list is a shortlist.
        std::vector<const EditorCommand *> ordered;
        for (const auto &id : recent_commands) if (const auto *command = registry.find(id)) ordered.push_back(command);
        for (const auto &command : registry.commands())
            if (!read_directly(command.info) && std::ranges::find(ordered, &command) == ordered.end()) ordered.push_back(&command);
        for (const auto *command : ordered) {
            const auto &id = command->info.id;
            items.push_back({command->info.category + " " + command->info.label, command->info.label, command->info.category, registry.shortcut(id), registry.enabled(id),
                             [this, id] { remember_recent(id); registry.run(id); }});
        }
    } else {
        for (const auto &e : session.scene().snapshot().entities)
            items.push_back({e.name, e.name, "Entity", {}, true, [this, id = e.id] { session.select(id); registry.run("Viewport.FocusSelected"); }});
        for (const auto &meta : session.assets())
            items.push_back({meta.path, meta.path, assets::to_string(meta.type), {}, true, [this, id = meta.id, type = meta.type] {
                reveal_asset(id);
                if (type == assets::AssetType::Material) registry.run("Asset.Edit");
            }});
    }
    std::vector<std::string> searched;
    searched.reserve(items.size());
    for (const auto &item : items) searched.push_back(item.search);
    // A little weight for what was used lately, so it wins between otherwise equal matches.
    std::vector<int> bonus(items.size(), 0);
    if (picker == Picker::Commands) for (std::size_t i = 0; i < items.size() && i < recent_commands.size(); ++i) bonus[i] = int(recent_commands.size() - i);
    const auto ranked = fuzzy_rank(std::string_view(picker_text.data()), searched, bonus);
    // ---- The window ----
    const auto *main = ImGui::GetMainViewport();
    const float width = std::min(px(620), main->WorkSize.x * 0.9f);
    ImGui::SetNextWindowPos({main->WorkPos.x + main->WorkSize.x / 2, main->WorkPos.y + px(50)}, ImGuiCond_Always, {0.5f, 0});
    ImGui::SetNextWindowSize({width, 0});
    ImGui::SetNextWindowSizeConstraints({width, 0}, {width, main->WorkSize.y * 0.7f});
    if (picker_focus) ImGui::SetNextWindowFocus();
    begin_panel("##picker", "Picker", context::global, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    if (picker_focus) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool entered = ImGui::InputTextWithHint("##picker-text", picker == Picker::Commands ? "Type a command" : "Type the name of an entity or asset", picker_text.data(), picker_text.size(),
                                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, picker_keys, &picker_index);
    mark("Picker.Input");
    if (ImGui::IsItemEdited()) picker_index = 0;
    picker_index = ranked.empty() ? 0 : std::clamp(picker_index, 0, int(ranked.size()) - 1);
    std::function<void()> chosen;
    const float list_height = std::min(float(ranked.size()) * ImGui::GetFrameHeightWithSpacing(), px(360));
    if (ranked.empty()) ImGui::TextDisabled("Nothing matches.");
    else if (ImGui::BeginChild("##list", {0, list_height}, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav)) {
        for (int row = 0; row < int(ranked.size()); ++row) {
            const auto &item = items[ranked[std::size_t(row)]];
            ImGui::PushID(row);
            ImGui::BeginDisabled(!item.enabled);
            const bool clicked = ImGui::Selectable("##item", row == picker_index, ImGuiSelectableFlags_AllowOverlap);
            ImGui::EndDisabled();
            if (row == picker_index && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
            if (row == picker_index && picker_index != picker_scrolled) { ImGui::SetScrollHereY(); picker_scrolled = picker_index; }
            mark("Picker.Item." + std::to_string(row));
            if (clicked && item.enabled) chosen = item.run;
            ImGui::SameLine(px(6));
            if (!item.enabled) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextUnformatted(item.label.c_str());
            if (!item.enabled) ImGui::PopStyleColor();
            ImGui::SameLine(); ImGui::TextDisabled("%s", item.detail.c_str());
            if (!item.keys.empty()) {
                const float keys_width = ImGui::CalcTextSize(item.keys.c_str()).x;
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - keys_width - px(4));
                ImGui::TextColored({0.6f, 0.75f, 0.95f, 1}, "%s", item.keys.c_str());
            }
            ImGui::PopID();
        }
    }
    if (!ranked.empty()) ImGui::EndChild();
    if (entered && !ranked.empty() && items[ranked[std::size_t(picker_index)]].enabled) chosen = items[ranked[std::size_t(picker_index)]].run;
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool leave = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || (!focused && !picker_focus);
    picker_focus = false;
    ImGui::End();
    if (chosen || leave || entered) { picker = Picker::None; picker_scrolled = -1; }
    if (chosen) chosen();
}
// ---- The console ----
namespace {
int console_keys(ImGuiInputTextCallbackData *data) {
    auto &ui = *static_cast<EditorUI *>(data->UserData);
    const auto replace = [&](const std::string &text) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, text.c_str());
    };
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        // The key that opens the console also closes it, rather than typing itself.
        if (data->EventChar == '`') { ui.console_close = true; return 1; }
        return 0;
    }
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) replace(data->EventKey == ImGuiKey_UpArrow ? ui.console_history.previous() : ui.console_history.next());
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        const auto found = ui.console.suggest(std::string_view(data->Buf, std::size_t(data->BufTextLen)), 1);
        if (!found.empty()) replace(found.front().name + " ");
    }
    return 0;
}
}
void EditorUI::console_print(const std::string &text) {
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) console_line(Severity::Info, line);
}
void EditorUI::console_panel() {
    if (console_close) {
        console_close = false;
        console_input[0] = 0;
        if (std::exchange(console_opened_by_key, false)) show_console = false;
        ImGui::SetWindowFocus("###3D Viewport");
    }
    if (!show_console) return;
    if (!begin_panel("Console", "Console", context::global, &show_console)) { ImGui::End(); return; }
    const float footer = ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild("##messages", {0, -footer})) {
        const auto &entries = messages.entries();
        for (int i = 0; i < int(entries.size()); ++i) draw_log_entry(entries[std::size_t(i)], i, "Console");
        // The newest message, such as an import error, is what the user needs to see.
        if (std::exchange(scroll_console, false)) ImGui::SetScrollHereY(1);
    }
    ImGui::EndChild();
    if (std::exchange(console_focus, false)) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool entered = ImGui::InputTextWithHint("##console-input", "Type a command; Tab completes", console_input.data(), console_input.size(),
                                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackCharFilter,
                                                  console_keys, this);
    mark("Console.Input");
    const bool active = ImGui::IsItemActive();
    const auto input_at = ImGui::GetItemRectMin();
    if (entered) {
        const std::string line(console_input.data());
        console_input[0] = 0;
        console_focus = true; // Enter takes the focus; the next command needs it back.
        if (!line.empty()) {
            console_history.add(line);
            console_line(Severity::Info, "> " + line);
            const auto result = console.execute(line);
            if (result) console_print(result.value());
            else console_line(Severity::Error, result.error().message);
        }
    }
    // ---- What it could be, above the line ----
    if (active && console_input[0]) {
        const auto found = console.suggest(std::string_view(console_input.data()));
        if (!found.empty()) {
            ImGui::SetNextWindowPos({input_at.x, input_at.y - px(4)}, ImGuiCond_Always, {0, 1});
            ImGui::SetNextWindowSize({px(560), 0});
            ImGui::Begin("##console-suggestions", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
            ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
            ui_map.mark("Console.Suggestions", ImGui::GetWindowPos(), {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y});
            for (const auto &suggestion : found) {
                ImGui::TextUnformatted(suggestion.name.c_str());
                if (!suggestion.value.empty()) { ImGui::SameLine(); ImGui::TextColored({0.6f, 0.75f, 0.95f, 1}, "= %s", suggestion.value.c_str()); }
                ImGui::SameLine(); ImGui::TextDisabled("%s", suggestion.description.c_str());
            }
            ImGui::End();
        }
    }
    ImGui::End();
}
void EditorUI::bind_console() {
    // ---- Variables, which read the real value whenever they are asked ----
    const auto flag = [this](const char *variable, const char *description, bool &value) {
        console.add_bool(variable, description, [&value] { return value; }, [this, &value](bool on) { value = on; save_settings(); });
    };
    flag("r.shadows", "Draw shadows in the viewport.", session.view_flags.shadows);
    flag("r.ambient_occlusion", "Draw ambient occlusion in the viewport.", session.view_flags.ambient_occlusion);
    flag("r.bloom", "Draw bloom in the viewport.", session.view_flags.bloom);
    flag("r.skybox", "Draw the skybox in the viewport.", session.view_flags.skybox);
    flag("r.effects", "Play particle emitters while editing.", session.view_flags.effects);
    flag("r.animations", "Play animators whose preview is on while editing.", session.view_flags.animations);
    flag("view.bones", "Draw the skeletons of skinned and animated models.", session.view_flags.bones);
    flag("view.ragdolls", "Draw ragdolls' capsules: where the bodies would be, or are.", session.view_flags.ragdolls);
    flag("view.grid", "Draw the grid.", show_grid);
    flag("view.collision", "Draw collider outlines.", show_colliders);
    flag("view.markers", "Draw the markers for lights and cameras.", show_markers);
    flag("snap.grid", "Moves go in whole grid steps.", snap.grid);
    flag("snap.rotation", "Rotations go in whole steps.", snap.rotation);
    flag("snap.scale", "Scaling goes in whole steps.", snap.scale);
    console.add_number("snap.grid_size", "The grid's size, in meters.", [this] { return snap.grid_size; }, [this](double value) { snap.grid_size = value; save_settings(); }, 0.001, 1000);
    console.add_number("snap.rotation_step", "The rotation snap, in degrees.", [this] { return snap.rotation_step; }, [this](double value) { snap.rotation_step = value; save_settings(); }, 0.001, 360);
    console.add_number("snap.scale_step", "The scale snap.", [this] { return snap.scale_step; }, [this](double value) { snap.scale_step = value; save_settings(); }, 0.001, 100);
    console.add_number("camera.speed", "Flying speed, in meters a second.", [this] { return camera_control.speed(); }, [this](double value) { camera_control.set_speed(value); save_settings(); }, CameraController::min_speed, CameraController::max_speed);
    // ---- Actions ----
    const auto stat = [this](const char *action, const char *description, bool &value) {
        console.add_action(action, description, [&value, action](std::string_view) {
            value = !value;
            return std::string(action) + (value ? " on" : " off");
        });
    };
    stat("stat fps", "Show frames per second over the viewport.", stat_fps);
    stat("stat timings", "Show frame, GPU, physics and gameplay times over the viewport.", stat_timings);
    console.add_action("stat none", "Hide all stats.", [this](std::string_view) { stat_fps = stat_timings = false; return std::string("Stats off"); });
    console.add_action("show collision", "Toggle collider outlines.", [this](std::string_view) {
        show_colliders = !show_colliders;
        save_settings();
        return std::string("Collider outlines ") + (show_colliders ? "on" : "off");
    });
    console.add_action("asset.resolve", "Find the asset a /Game/... name means, through redirectors.", [this](std::string_view name) {
        const auto found = session.resolve_package(name);
        if (!found) return found.error().message;
        const auto &r = found.value();
        std::string text = std::string(name) + " is " + session.package_name_of(r.id) + ", " + r.asset_path + " (" + assets::to_string(r.type) + ")";
        for (const auto &step : r.redirects) text += "\n  through the redirector " + step;
        return text;
    });
    console.add_action("clear", "Empty the Console.", [this](std::string_view) { messages.clear(); return std::string{}; });
    console.add_action("help", "List everything the console knows, or what matches a word.", [this](std::string_view word) {
        std::string text;
        for (const auto &entry : word.empty() ? console.all() : console.suggest(word, 40)) {
            text += entry.name;
            if (!entry.value.empty()) text += " = " + entry.value;
            text += "  " + entry.description + "\n";
        }
        return text;
    });
    // ---- Every command, by its ID ----
    for (const auto &command : registry.commands()) {
        if (read_directly(command.info)) continue;
        console.add_action(command.info.id, command.info.tooltip, [this, id = command.info.id](std::string_view) {
            return registry.run(id) ? std::string{} : id + " cannot run right now";
        });
    }
}
void EditorUI::frame_drawn() {
    const auto now = std::chrono::steady_clock::now();
    if (frames_drawn++ > 0) stat_frame_ms += (std::chrono::duration<double, std::milli>(now - last_drawn).count() - stat_frame_ms) * 0.05;
    last_drawn = now;
}
// Stats over the viewport, below the snapping widgets.
void EditorUI::draw_stats(ImVec2 origin, ImVec2 size) {
    if (!stat_fps && !stat_timings) return;
    std::vector<std::string> lines;
    char text[96];
    if (stat_fps) {
        std::snprintf(text, sizeof text, "%.0f FPS  %.2f ms", stat_frame_ms > 0 ? 1000.0 / stat_frame_ms : 0.0, stat_frame_ms);
        lines.emplace_back(text);
    }
    if (stat_timings) {
        const auto stats = renderer.stats();
        std::snprintf(text, sizeof text, "GPU %.2f ms", stats.gpu_frame_ms);
        lines.emplace_back(text);
        const auto &timing = session.timing();
        const double ticks = double(std::max<std::uint64_t>(timing.ticks, 1));
        std::snprintf(text, sizeof text, "Physics %.3f ms  Gameplay %.3f ms a tick", std::chrono::duration<double, std::milli>(timing.physics).count() / ticks, std::chrono::duration<double, std::milli>(timing.gameplay).count() / ticks);
        lines.emplace_back(text);
        const double updates = double(std::max<std::uint64_t>(timing.effect_updates, 1));
        std::snprintf(text, sizeof text, "Animation %.3f ms a tick  Particles %.3f ms", std::chrono::duration<double, std::milli>(timing.animation).count() / ticks, std::chrono::duration<double, std::milli>(timing.effects).count() / updates);
        lines.emplace_back(text);
        std::snprintf(text, sizeof text, "Skinned %zu  Particles %zu", stats.skinned, stats.particles);
        lines.emplace_back(text);
        std::snprintf(text, sizeof text, "Objects %zu  Lights %zu  Parts %zu", stats.objects, stats.lights, stats.parts);
        lines.emplace_back(text);
    }
    auto *draw = ImGui::GetWindowDrawList();
    float y = origin.y + px(40);
    for (const auto &line : lines) {
        const float width = ImGui::CalcTextSize(line.c_str()).x;
        const ImVec2 at{origin.x + size.x - width - px(10), y};
        draw->AddRectFilled({at.x - px(4), at.y - px(1)}, {at.x + width + px(4), at.y + ImGui::GetTextLineHeight() + px(1)}, IM_COL32(10, 14, 20, 190), px(2));
        draw->AddText(at, IM_COL32(150, 240, 160, 255), line.c_str());
        y += ImGui::GetTextLineHeightWithSpacing();
    }
    ui_map.mark("Stats", {origin.x + size.x - px(200), origin.y + px(40)}, {origin.x + size.x, y});
}
} // namespace engine::editor
