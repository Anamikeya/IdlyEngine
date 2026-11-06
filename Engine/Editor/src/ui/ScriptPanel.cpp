#include "EditorUI.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <system_error>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// The Script Editor (Milestone 6, D9): one script at a time, in Lua colors, checked as it is typed, saved with Ctrl+S.
// In Play, saving reloads the running scripts at once (D8). What is typed stays here until it is saved: the file, and
// what Details and Play read, change only then.
namespace engine::editor {
namespace {
std::filesystem::file_time_type written_at(const std::filesystem::path &file) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(file, ec);
    return ec ? std::filesystem::file_time_type{} : time;
}
// "/Game/Scripts/Gate:4: unexpected symbol" -> 4, when the message names the script.
int line_in(const std::string &message, const std::string &script) {
    const auto at = message.find(script + ":");
    if (at == std::string::npos) return 0;
    std::size_t digit = at + script.size() + 1;
    int line = 0;
    while (digit < message.size() && std::isdigit(static_cast<unsigned char>(message[digit]))) line = line * 10 + (message[digit++] - '0');
    return line;
}
// Where the script defines a function: "function Gate:tick(" or "function Gate.tick(", 1-based; 0 when not found.
int definition_line(const TextEditor &text, const std::string &function) {
    for (std::size_t line = 0; line < text.GetLineCount(); ++line) {
        const auto source = text.GetLineText(line);
        const auto at = source.find("function");
        if (at == std::string::npos) continue;
        for (const char separator : {':', '.'})
            if (source.find(std::string(1, separator) + function + "(", at) != std::string::npos) return int(line) + 1;
    }
    return 0;
}
// Runs a command line without a console window and without waiting for it.
bool launch(const std::string &command) {
    std::string line = "cmd.exe /c " + command;
    const int length = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
    if (length <= 0) return false;
    std::wstring wide(std::size_t(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wide.data(), length);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, wide.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
} // namespace

bool EditorUI::script_unsaved() const { return script_edit.text && script_edit.text->GetUndoIndex() != script_edit.saved_undo; }

bool EditorUI::load_script_text(assets::AssetId id) {
    const auto text = session.script_text(id);
    if (!text) { log_error("The script cannot be opened: " + text.error().message); return false; }
    auto &e = script_edit;
    e = ScriptEdit{};
    e.id = id;
    e.text = std::make_unique<TextEditor>();
    e.text->SetLanguage(TextEditor::Language::Lua());
    e.text->SetTabSize(4);
    // The widget skips a closing bracket it added only when it is typed at once: typing "f(x)" would give "f(x))".
    e.text->SetCompletePairedGlyphs(false);
    e.text->SetText(text.value());
    e.saved_undo = e.seen_undo = e.text->GetUndoIndex();
    e.file_time = written_at(session.script_file(id));
    return true;
}

void EditorUI::open_script(assets::AssetId id, int line) {
    if (id != script_edit.id && script_unsaved()) {
        notify(Severity::Warning, session.package_name_of(script_edit.id) + " has unsaved changes: save it, or revert it, before opening another script");
        show_script = focus_script = true;
        return;
    }
    if ((id != script_edit.id || !script_edit.text) && !load_script_text(id)) return;
    script_edit.go_to = line;
    show_script = focus_script = true;
}

void EditorUI::save_open_script() {
    auto &e = script_edit;
    if (!e.text) return;
    const auto script_name = session.package_name_of(e.id);
    if (!report(session.save_script(e.id, e.text->GetText()), playing() ? "Script saved: the running scripts use it now" : "Script saved")) return;
    e.saved_undo = e.text->GetUndoIndex();
    e.file_time = written_at(session.script_file(e.id));
    e.changed_on_disk = false;
    // Errors Play reported were about the old text.
    script_problems.erase(script_name);
    e.markers_stale = true;
}

void EditorUI::open_script_externally() {
    const auto &e = script_edit;
    const auto file = session.script_file(e.id);
    if (file.empty()) { log_error("No script is open in the Script Editor"); return; }
    const int line = e.text ? int(e.text->GetMainCursorPosition().line) + 1 : 1;
    const auto command = EditorSession::external_command(settings.text_of("script.external_editor"), file, line);
    if (launch(command)) log("Opened in the external editor: " + command);
    else log_error("The external editor could not be started: " + command);
}

void EditorUI::script_editor() {
    if (!show_script) return;
    // A saved layout from before this panel has no place for it: beside the Asset Browser, where code has room.
    if (const auto *browser = ImGui::FindWindowByName("Asset Browser"); browser && browser->DockId) ImGui::SetNextWindowDockID(browser->DockId, ImGuiCond_FirstUseEver);
    if (std::exchange(focus_script, false)) ImGui::SetNextWindowFocus();
    auto &e = script_edit;
    e.focused = false;
    const bool open = begin_panel("Script Editor", "Script Editor", context::script_editor, &show_script);
    const auto catalogue = session.assets();
    const auto meta = std::ranges::find_if(catalogue, [&](const assets::AssetMetadata &m) { return m.id == e.id && m.type == assets::AssetType::Script; });
    if (!open || meta == catalogue.end() || !e.text) {
        if (open) ImGui::TextWrapped("Open a script to edit it: double-click one in the Asset Browser, or press Edit Script in Details. New script there makes one.");
        ImGui::End();
        return;
    }
    const std::string script_name = session.package_name_of(e.id);
    // ---- Another editor changed the file ----
    if (const auto now = written_at(session.script_file(e.id)); now != e.file_time) {
        e.file_time = now;
        if (!script_unsaved()) {
            const int line = int(e.text->GetMainCursorPosition().line) + 1;
            if (load_script_text(e.id)) {
                script_edit.go_to = line;
                log("Reloaded " + script_name + ": its file changed");
            }
            ImGui::End();
            return;
        }
        e.changed_on_disk = true;
    }
    // ---- The tools. They wrap, as the dock can be narrow and "(unsaved)" makes the row longer ----
    const bool unsaved = script_unsaved();
    const float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const auto flow = [&](const char *label, bool compact = false) {
        ImGui::SameLine();
        const float padding = compact ? ImGui::GetStyle().FramePadding.x : ImGui::GetStyle().FramePadding.x * 2;
        if (ImGui::GetCursorScreenPos().x + ImGui::CalcTextSize(label).x + padding > right_edge) ImGui::NewLine();
    };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(script_name.c_str());
    if (unsaved) { flow("(unsaved)"); ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "(unsaved)"); }
    flow("Save");
    command_button("Script.Save", "Save");
    mark("Script.Save");
    flow("Revert");
    ImGui::BeginDisabled(!unsaved);
    if (tip_button("Revert", "Go back to the text as last saved")) {
        const int line = int(e.text->GetMainCursorPosition().line) + 1;
        if (load_script_text(e.id)) script_edit.go_to = line;
    }
    ImGui::EndDisabled();
    mark("Script.Revert");
    flow("Find");
    if (tip_button("Find", "Find and replace in the script (Ctrl+F in the text)")) e.text->OpenFindReplaceWindow();
    mark("Script.Find");
    flow("Open in External Editor");
    command_button("Script.OpenExternal", "Open in External Editor");
    mark("Script.OpenExternal");
    flow("...", true);
    if (ImGui::SmallButton("...")) ImGui::OpenPopup("##script-settings");
    ImGui::SetItemTooltip("Which external editor to use");
    mark("Script.Settings");
    if (ImGui::BeginPopup("##script-settings")) {
        static std::array<char, 512> pattern{};
        if (ImGui::IsWindowAppearing()) {
            const auto saved = settings.text_of("script.external_editor");
            std::snprintf(pattern.data(), pattern.size(), "%s", saved.empty() ? std::string(EditorSession::default_external_editor).c_str() : saved.c_str());
        }
        ImGui::TextUnformatted("External editor command: {file} and {line} are filled in.");
        ImGui::SetNextItemWidth(px(360));
        if (ImGui::InputText("##pattern", pattern.data(), pattern.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
            settings.set_text("script.external_editor", pattern.data());
            save_settings();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (e.changed_on_disk) {
        ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "The file changed on disk while this was unsaved.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Use the file")) { if (load_script_text(e.id)) log("Reloaded " + script_name + " from its file"); ImGui::End(); return; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Keep mine")) e.changed_on_disk = false;
        mark("Script.DiskChanged");
    }
    // ---- What it declares, read from what is typed a moment after typing stops ----
    const auto undo = e.text->GetUndoIndex();
    if (undo != e.seen_undo) { e.seen_undo = undo; e.edited_at = ImGui::GetTime(); }
    if (undo != e.checked_undo && ImGui::GetTime() - e.edited_at > 0.4) {
        e.checked_undo = undo;
        const auto checked = session.check_script(e.id, e.text->GetText());
        if (checked) { e.declares = checked.value(); e.problem.clear(); e.problem_line = 0; }
        else { e.problem = checked.error().message; e.problem_line = line_in(e.problem, script_name); }
        e.markers_stale = true;
    }
    if (e.declares) {
        ImGui::TextDisabled("Properties:");
        if (e.declares->properties.empty()) { ImGui::SameLine(); ImGui::TextDisabled("none"); }
        for (const auto &p : e.declares->properties) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s (%s)", p.name.c_str(), scripting::to_string(scripting::type_of(p.default_value)));
        }
        ImGui::TextDisabled("Functions:");
        if (e.declares->callbacks.empty()) { ImGui::SameLine(); ImGui::TextDisabled("none the engine calls"); }
        for (const auto &f : e.declares->callbacks) {
            ImGui::SameLine();
            if (ImGui::TextLink(f.c_str())) if (const int line = definition_line(*e.text, f)) e.go_to = line;
            mark("Script.Function." + f);
        }
    }
    if (!e.problem.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.35f, 1));
        ImGui::TextWrapped("%s", e.problem.c_str());
        ImGui::PopStyleColor();
        mark("Script.Problem");
    }
    // ---- Markers: the typed text's problem, and what Play reported since the last save ----
    if (e.markers_stale) {
        e.markers_stale = false;
        e.text->ClearMarkers();
        const ImU32 number = IM_COL32(240, 90, 80, 255), line_color = IM_COL32(240, 90, 80, 50);
        if (e.problem_line > 0) e.text->AddMarker(std::size_t(e.problem_line - 1), number, line_color, "Line " + std::to_string(e.problem_line), e.problem);
        if (const auto found = script_problems.find(script_name); found != script_problems.end())
            for (const auto &error : found->second)
                if (error.line > 0)
                    e.text->AddMarker(std::size_t(error.line - 1), number, line_color, "Line " + std::to_string(error.line),
                                      error.message + (error.callback.empty() ? std::string() : "  (in " + error.callback + ", while playing)"));
    }
    // ---- The text ----
    if (e.go_to > 0) {
        const auto line = std::size_t(e.go_to - 1);
        e.text->SetCursor({line, 0});
        e.text->ScrollToLine(line);
        e.text->SetFocus();
        e.go_to = 0;
    }
    e.text->Render("##script-text", ImGui::GetContentRegionAvail());
    mark("Script.Text");
    // The text area is a child window: focused while it is and the panel itself is not.
    e.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !ImGui::IsWindowFocused();
    ImGui::End();
}
} // namespace engine::editor
