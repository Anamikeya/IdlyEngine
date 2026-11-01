#include "EditorUI.hpp"
#include <algorithm>
#include <cfloat>
#include <optional>
#include <string>

// What the editor tells the user: the Message Log, toasts in the corner, and tasks still running.
namespace engine::editor {
namespace {
ImVec4 severity_color(Severity severity) {
    switch (severity) {
    case Severity::Success: return {0.5f, 0.85f, 0.5f, 1};
    case Severity::Warning: return {0.95f, 0.78f, 0.35f, 1};
    case Severity::Error: return {0.98f, 0.45f, 0.42f, 1};
    case Severity::Info: break;
    }
    return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}
const char *severity_prefix(Severity severity) {
    return severity == Severity::Error ? "Error: " : severity == Severity::Warning ? "Warning: " : "";
}
}
void EditorUI::log(Severity severity, std::string text, LogLink link) {
    messages.add({severity, std::move(text), std::move(link)});
    scroll_console = scroll_message_log = true;
}
void EditorUI::log(std::string text, LogLink link) { log(Severity::Info, std::move(text), std::move(link)); }
void EditorUI::notify(Severity severity, std::string text, LogLink link) {
    toasts.show(severity, text, link);
    log(severity, std::move(text), std::move(link));
}
void EditorUI::log_error(std::string text) { notify(Severity::Error, std::move(text)); }
void EditorUI::console_line(Severity severity, std::string text) {
    messages.add({severity, std::move(text), {}, true});
    scroll_console = true;
}
void EditorUI::open_link(const LogLink &link) {
    if (link.kind == LogLink::Kind::Entity) {
        if (!session.scene().inspect(EntityId(link.id))) { log(Severity::Warning, link.label + " is no longer in this scene"); return; }
        session.select(EntityId(link.id));
        registry.run("Viewport.FocusSelected");
    } else if (link.kind == LogLink::Kind::Asset) {
        reveal_asset(assets::AssetId{link.id});
    }
}
void EditorUI::draw_log_entry(const LogEntry &entry, int index, const char *where) {
    ImGui::PushID(index);
    ImGui::PushStyleColor(ImGuiCol_Text, severity_color(entry.severity));
    ImGui::TextWrapped("%s%s", severity_prefix(entry.severity), entry.text.c_str());
    ImGui::PopStyleColor();
    if (entry.link) {
        const float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        ImGui::SameLine();
        if (ImGui::GetCursorScreenPos().x + ImGui::CalcTextSize(entry.link.label.c_str()).x > right_edge) ImGui::NewLine();
        if (ImGui::TextLink(entry.link.label.c_str())) link_clicked = entry.link;
        mark(std::string(where) + ".Link." + std::to_string(index));
    }
    ImGui::PopID();
}
void EditorUI::message_log_panel() {
    if (!show_message_log) return;
    // A saved layout from before this panel has no place for it: beside the Console.
    if (const auto *console_window = ImGui::FindWindowByName("Console"); console_window && console_window->DockId) ImGui::SetNextWindowDockID(console_window->DockId, ImGuiCond_FirstUseEver);
    if (!begin_panel("Message Log", "Message Log", context::global, &show_message_log)) { ImGui::End(); return; }
    // They wrap, because this panel is often narrow.
    const float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const auto flow = [&](const char *label) {
        ImGui::SameLine();
        if (ImGui::GetCursorScreenPos().x + ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2 > right_edge) ImGui::NewLine();
    };
    bool first_chip = true;
    const auto chip = [&](const char *name, Severity severity, bool &on) {
        const auto label = std::string(name) + " " + std::to_string(messages.count(severity)) + "###" + name;
        if (!std::exchange(first_chip, false)) flow(label.c_str());
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.5f, 0.85f, 1));
        if (tip_small_button(label.c_str(), "Show or hide messages of this kind")) { on = !on; save_settings(); }
        if (on) ImGui::PopStyleColor();
        mark(std::string("Log.Filter.") + to_string(severity));
    };
    chip("Info", Severity::Info, log_filter.info);
    chip("Done", Severity::Success, log_filter.success);
    chip("Warnings", Severity::Warning, log_filter.warning);
    chip("Errors", Severity::Error, log_filter.error);
    flow("Clear");
    if (tip_small_button("Clear", "Empty the log, and the Console with it")) messages.clear();
    mark("Log.Clear");
    if (ImGui::BeginChild("##entries", {0, 0}, ImGuiChildFlags_Borders)) {
        if (!messages.shown(log_filter)) ImGui::TextDisabled(messages.entries().empty() ? "Nothing to show yet." : "Nothing of the kinds chosen above.");
        const auto &entries = messages.entries();
        for (int i = 0; i < int(entries.size()); ++i)
            if (log_filter.allows(entries[std::size_t(i)])) draw_log_entry(entries[std::size_t(i)], i, "Log");
        if (std::exchange(scroll_message_log, false)) ImGui::SetScrollHereY(1);
    }
    ImGui::EndChild();
    ImGui::End();
}
void EditorUI::cancel_task(std::uint64_t id) {
    const auto found = import_tasks.find(id);
    if (found == import_tasks.end()) return;
    (void)session.cancel_import(found->second);
    const auto task = std::ranges::find(tasks.tasks(), id, &BackgroundTask::id);
    notify(Severity::Info, (task == tasks.tasks().end() ? std::string("Import") : task->name) + ": cancelled");
    import_tasks.erase(found);
    tasks.finish(id);
}
void EditorUI::toast_windows() {
    const auto &io = ImGui::GetIO();
    tasks.update(io.DeltaTime);
    const auto *main = ImGui::GetMainViewport();
    const float right = main->WorkPos.x + main->WorkSize.x - px(12), width = px(360);
    float bottom = main->WorkPos.y + main->WorkSize.y - px(12);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize;
    // Each is a small window of its own, stacked up from the corner, with the newest at the bottom.
    const auto open = [&](const std::string &id, ImVec4 accent) {
        ImGui::SetNextWindowPos({right, bottom}, ImGuiCond_Always, {1, 1});
        ImGui::SetNextWindowSize({width, 0});
        ImGui::SetNextWindowBgAlpha(0.95f);
        ImGui::Begin(id.c_str(), nullptr, flags);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        const auto at = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(at, {at.x + px(4), at.y + ImGui::GetWindowHeight()}, ImGui::ColorConvertFloat4ToU32(accent));
    };
    const auto close = [&]() {
        bottom -= ImGui::GetWindowHeight() + px(6);
        ImGui::End();
    };
    std::uint64_t hovered = 0, dismissed = 0;
    std::optional<LogLink> followed;
    int row = 0;
    const auto &queue = toasts.toasts();
    for (auto toast = queue.rbegin(); toast != queue.rend(); ++toast, ++row) {
        open("##toast" + std::to_string(toast->id), severity_color(toast->severity));
        ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - px(28));
        ImGui::PushStyleColor(ImGuiCol_Text, severity_color(toast->severity));
        ImGui::TextUnformatted(toast->text.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        const auto control = "Toast." + std::to_string(row);
        ui_map.mark(control, ImGui::GetWindowPos(), {ImGui::GetWindowPos().x + ImGui::GetWindowSize().x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y});
        if (toast->count > 1) ImGui::TextDisabled("%d times", toast->count);
        if (toast->link) {
            if (ImGui::TextLink(toast->link.label.c_str())) followed = toast->link;
            mark(control + ".Link");
        }
        ImGui::SetCursorPos({ImGui::GetContentRegionMax().x - px(20), ImGui::GetStyle().WindowPadding.y});
        if (ImGui::SmallButton("x")) dismissed = toast->id;
        mark(control + ".Close");
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) hovered = toast->id;
        close();
    }
    std::uint64_t cancelled = 0;
    row = 0;
    for (const auto &task : tasks.tasks()) {
        if (task.age < TaskList::show_after) continue;
        open("##task" + std::to_string(task.id), {0.45f, 0.7f, 0.95f, 1});
        ImGui::TextUnformatted(task.name.c_str());
        // A negative fraction is ImGui's way of saying it does not know how far along it is.
        ImGui::ProgressBar(task.progress < 0 ? float(-ImGui::GetTime()) : float(task.progress), {-FLT_MIN, px(6)}, "");
        if (task.can_cancel) {
            const auto control = "Task." + std::to_string(row++);
            if (tip_small_button("Cancel", "Stop waiting for this and discard what it makes")) cancelled = task.id;
            mark(control + ".Cancel");
        }
        close();
    }
    toasts.update(io.DeltaTime, hovered);
    if (dismissed) toasts.dismiss(dismissed);
    if (followed) open_link(*followed);
    if (cancelled) cancel_task(cancelled);
}
} // namespace engine::editor
