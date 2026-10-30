#include "EditorUI.hpp"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>

namespace engine::editor {
namespace {
bool has_text(std::string_view text, std::string_view wanted) {
    if (wanted.empty()) return true;
    return !std::ranges::search(text, wanted, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }).empty();
}
// A small toggle at the row's right edge, drawn rather than a character, because the font has no icons.
bool row_icon(const char *id, float right_offset, bool active, bool eye, const char *tooltip) {
    const float size = ImGui::GetFrameHeight() * 0.8f;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - right_offset);
    const auto at = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, {size, ImGui::GetTextLineHeight()});
    ImGui::SetItemTooltip("%s", tooltip);
    const ImU32 color = ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, 255) : active ? IM_COL32(170, 180, 195, 255) : IM_COL32(90, 95, 105, 255);
    auto *draw = ImGui::GetWindowDrawList();
    const ImVec2 middle{at.x + size / 2, at.y + ImGui::GetTextLineHeight() / 2};
    const float r = px(5);
    if (eye) {
        draw->AddCircle(middle, r, color, 12, px(1.2f));
        if (active) draw->AddCircleFilled(middle, r * 0.45f, color);
        else draw->AddLine({middle.x - r, middle.y + r}, {middle.x + r, middle.y - r}, color, px(1.5f));
    } else {
        draw->AddRect({middle.x - r * 0.8f, middle.y - r * 0.1f}, {middle.x + r * 0.8f, middle.y + r}, color, 0, 0, px(1.2f));
        if (active) draw->AddRectFilled({middle.x - r * 0.8f, middle.y - r * 0.1f}, {middle.x + r * 0.8f, middle.y + r}, color);
        draw->PathArcTo({middle.x, middle.y - r * 0.2f}, r * 0.55f, 3.14159f, 6.28318f, 8);
        draw->PathStroke(color, 0, px(1.2f));
    }
    return clicked;
}
}
bool EditorUI::entity_shown(const EntityRecord &e) const {
    const auto &c = e.components;
    const bool any_type = outliner_show_mesh || outliner_show_light || outliner_show_camera || outliner_show_physics;
    const bool type_fits = !any_type || (outliner_show_mesh && c.mesh) || (outliner_show_light && c.light) || (outliner_show_camera && c.camera) ||
                           (outliner_show_physics && (c.body || !c.colliders.empty()));
    return type_fits && has_text(e.name, std::string_view(outliner_search.data()));
}
void EditorUI::drop_onto(EntityId parent) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const auto *payload = ImGui::AcceptDragDropPayload(entity_payload)) {
        EntityId dragged = 0; std::memcpy(&dragged, payload->Data, sizeof dragged);
        const char *done = parent ? "Reparented (world transform kept)" : "Moved to root (world transform kept)";
        if (dragged == parent) {}
        else if (session.selected().contains(dragged) && session.selected().size() > 1) {
            // Dragging one row of a multi-selection moves all of it, or nothing if one of them cannot go there.
            const auto roots = session.selection_roots();
            ScopedTransaction transaction(session, "Reparent " + std::to_string(roots.size()) + " Entities");
            bool moved = true;
            for (const auto id : roots) {
                std::string problem;
                if (parent) { if (const auto result = session.reparent(id, parent); !result) problem = result.error().message; }
                else if (const auto filed = session.file_entity(id, 0); !filed) problem = filed.error().message;
                if (!problem.empty()) { log("Error: " + problem); transaction.cancel(); moved = false; break; }
            }
            if (moved) log(done);
        } else if (parent) report(session.reparent(dragged, parent), done);
        else report(session.file_entity(dragged, 0), done); // The root: out of any parent and any folder.
    }
    if (const auto *payload = ImGui::AcceptDragDropPayload(folder_payload); payload && !parent) {
        std::uint64_t dragged = 0; std::memcpy(&dragged, payload->Data, sizeof dragged);
        report(session.move_folder(dragged, 0), "Folder moved");
    }
    ImGui::EndDragDropTarget();
}
// A plain click selects the row. Ctrl toggles it; Shift selects the rows from the last clicked one to this one.
void EditorUI::click_row(EntityId id) {
    const auto &io = ImGui::GetIO();
    selected_folder = 0;
    if (io.KeyCtrl) session.selection_set().toggle(id);
    else if (io.KeyShift && selection_anchor && selection_anchor != id) {
        const auto from = std::ranges::find(shown_rows, selection_anchor), to = std::ranges::find(shown_rows, id);
        if (from != shown_rows.end() && to != shown_rows.end()) {
            std::vector<EntityId> range = from < to ? std::vector<EntityId>(from, to + 1) : std::vector<EntityId>(to, from + 1);
            // The clicked row last: it becomes the primary one.
            std::erase(range, id);
            range.push_back(id);
            session.selection_set().set(std::span<const EntityId>(range));
            return;
        }
    }
    else session.select(id);
    selection_anchor = id;
}
void EditorUI::tree_node(EntityId id, const std::set<EntityId> &open_path, const std::set<EntityId> &passes) {
    if (!passes.empty() && !passes.contains(id)) return;
    visible_rows.push_back(id);
    const auto &e = records.at(id);
    const auto kids = children.find(id);
    const bool leaf = kids == children.end();
    const bool renaming = rename_entity == id;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (session.selected().contains(id)) flags |= ImGuiTreeNodeFlags_Selected;
    if (open_path.contains(id) || !passes.empty()) ImGui::SetNextItemOpen(true);
    const bool hidden_row = !playing() && session.hidden(id);
    if (hidden_row) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void *>(static_cast<uintptr_t>(id)), flags, "%s", renaming ? "" : e.name.c_str());
    if (hidden_row) ImGui::PopStyleColor();
    mark("Row." + e.name);
    mark("RowAt." + std::to_string(visible_rows.size() - 1)); // Names repeat; a row's place in the tree does not.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        // A row already in a multi-selection may be the start of a drag of all of it, so a plain click waits for the release.
        if (session.selected().contains(id) && session.selected().size() > 1 && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) click_row_on_release = id;
        else click_row(id);
    }
    if (click_row_on_release == id && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !ImGui::GetDragDropPayload()) click_row(id);
        click_row_on_release = 0;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { session.select(id); registry.run("Viewport.FocusSelected"); }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(entity_payload, &id, sizeof id);
        ImGui::Text("Move '%s'", e.name.c_str());
        ImGui::EndDragDropSource();
    }
    drop_onto(id);
    if (ImGui::BeginPopupContextItem()) {
        selected_folder = 0;
        if (!session.selected().contains(id)) session.select(id);
        if (ImGui::BeginMenu("Create child")) {
            command_item("Create.ChildEmpty", "Empty");
            command_item("Create.ChildCube", "Cube");
            command_item("Create.ChildSphere", "Sphere");
            command_item("Create.ChildPointLight", "Point Light");
            ImGui::EndMenu();
        }
        for (const auto *command : {"Edit.Rename", "Edit.Duplicate", "Edit.Copy", "Edit.Paste", "Edit.Group", "Edit.MoveToRoot", "Viewport.FocusSelected", "Assets.FindInBrowser"}) command_item(command);
        ImGui::Separator();
        for (const auto *command : {"Edit.Hide", "Edit.UnhideAll"}) command_item(command);
        ImGui::Separator();
        command_item("Edit.Delete");
        ImGui::EndPopup();
    }
    if (renaming) {
        ImGui::SameLine();
        if (rename_focus) { ImGui::SetKeyboardFocusHere(); rename_focus = false; }
        ImGui::SetNextItemWidth(-px(44));
        const bool entered = ImGui::InputText("##rename", rename_text.data(), rename_text.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        mark("Rename.Field");
        const bool left = ImGui::IsItemDeactivated();
        if (entered || (left && !ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
            if (rename_text[0] && e.name != rename_text.data()) report(session.execute(commands::RenameEntity{id, rename_text.data()}, "Rename " + e.name), "Renamed");
            rename_entity = 0;
        } else if (left) rename_entity = 0;
    } else {
        const auto &c = e.components;
        if (c.light || c.camera || c.body || !c.colliders.empty()) {
            ImGui::SameLine();
            // A behavior-driven entity's tag is left off: the Inspector lists them.
            ImGui::TextDisabled("%s%s%s", c.light ? (c.light->type == LightType::Point ? " point light" : c.light->type == LightType::Spot ? " spot light" : " directional light") : "",
                                c.camera ? (c.camera->primary ? " camera (primary)" : " camera") : "",
                                c.body ? (c.body->kind == BodyKind::Dynamic ? " dynamic" : c.body->kind == BodyKind::Kinematic ? " kinematic" : " static")
                                       : !c.colliders.empty() ? " collider" : "");
        }
    }
    // Editor-only, so they have no meaning for a running copy.
    if (!playing()) {
        const float icon = ImGui::GetFrameHeight() * 0.8f;
        ImGui::PushID(reinterpret_cast<void *>(static_cast<uintptr_t>(id)));
        if (row_icon("##eye", icon * 2 + px(2), !session.hidden(id), true, session.hidden(id) ? "Hidden in the editor. Click to show." : "Click to hide in the editor.")) session.set_hidden(id, !session.hidden(id));
        mark("Eye." + e.name);
        if (row_icon("##lock", icon + px(2), session.locked(id), false, session.locked(id) ? "Locked: cannot be clicked in the viewport. Click to unlock." : "Click to stop it being clicked in the viewport.")) session.set_locked(id, !session.locked(id));
        mark("Lock." + e.name);
        ImGui::PopID();
    }
    if (open && !leaf) {
        for (const auto child : kids->second) tree_node(child, open_path, passes);
        ImGui::TreePop();
    }
}
void EditorUI::folder_node(const SceneFolder &folder, const std::set<EntityId> &open_path, const std::set<EntityId> &passes) {
    const auto &folders = session.view_state().folders;
    std::vector<EntityId> contents;
    for (const auto &[entity, in_folder] : folders.members)
        if (in_folder == folder.id && records.contains(entity) && !records.at(entity).parent && (passes.empty() || passes.contains(entity))) contents.push_back(entity);
    std::vector<const SceneFolder *> inner;
    for (const auto &other : folders.folders) if (other.parent == folder.id) inner.push_back(&other);
    if (!passes.empty() && contents.empty() && inner.empty()) return;
    const bool renaming = rename_folder_id == folder.id;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap;
    if (selected_folder == folder.id) flags |= ImGuiTreeNodeFlags_Selected;
    if (!passes.empty()) ImGui::SetNextItemOpen(true);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.8f, 0.45f, 1));
    const auto node_id = reinterpret_cast<void *>(static_cast<uintptr_t>(folder.id + 0x4000000000000000ull));
    const bool open = ImGui::TreeNodeEx(node_id, flags, "%s", renaming ? "" : folder.name.c_str());
    ImGui::PopStyleColor();
    mark("Folder." + folder.name);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) { selected_folder = folder.id; session.select(0); }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(folder_payload, &folder.id, sizeof folder.id);
        ImGui::Text("Move folder '%s'", folder.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const auto *payload = ImGui::AcceptDragDropPayload(entity_payload)) {
            EntityId dragged = 0; std::memcpy(&dragged, payload->Data, sizeof dragged);
            const auto moving = session.selected().contains(dragged) ? session.selection_roots() : std::vector<EntityId>{dragged};
            ScopedTransaction transaction(session, moving.size() == 1 ? "Move to Folder " + folder.name : "Move " + std::to_string(moving.size()) + " Entities to Folder " + folder.name);
            for (const auto entity : moving) {
                const auto filed = session.file_entity(entity, folder.id);
                if (!filed) { log("Error: " + filed.error().message); transaction.cancel(); break; }
            }
        }
        if (const auto *payload = ImGui::AcceptDragDropPayload(folder_payload)) {
            std::uint64_t dragged = 0; std::memcpy(&dragged, payload->Data, sizeof dragged);
            report(session.move_folder(dragged, folder.id), "Folder moved");
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::BeginPopupContextItem()) {
        selected_folder = folder.id;
        for (const auto *command : {"Outliner.NewFolder", "Edit.Rename", "Edit.Delete"}) command_item(command);
        ImGui::EndPopup();
    }
    if (renaming) {
        ImGui::SameLine();
        if (rename_focus) { ImGui::SetKeyboardFocusHere(); rename_focus = false; }
        ImGui::SetNextItemWidth(-px(44));
        const bool entered = ImGui::InputText("##rename", rename_text.data(), rename_text.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        mark("Rename.Field");
        const bool left = ImGui::IsItemDeactivated();
        if (entered || (left && !ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
            if (rename_text[0] && folder.name != rename_text.data()) report(session.rename_folder(folder.id, rename_text.data()), "Folder renamed");
            rename_folder_id = 0;
        } else if (left) rename_folder_id = 0;
    }
    // The eye and lock act on everything inside.
    if (!playing()) {
        const float icon = ImGui::GetFrameHeight() * 0.8f;
        const bool any_shown = contents.empty() || std::ranges::any_of(contents, [&](EntityId entity) { return !session.hidden(entity); });
        const bool all_locked = !contents.empty() && std::ranges::all_of(contents, [&](EntityId entity) { return session.locked(entity); });
        ImGui::PushID(node_id);
        if (row_icon("##eye", icon * 2 + px(2), any_shown, true, "Hide or show what is in this folder")) for (const auto entity : contents) session.set_hidden(entity, any_shown);
        if (row_icon("##lock", icon + px(2), all_locked, false, "Lock or unlock what is in this folder")) for (const auto entity : contents) session.set_locked(entity, !all_locked);
        ImGui::PopID();
    }
    if (open) {
        for (const auto *other : inner) folder_node(*other, open_path, passes);
        for (const auto entity : contents) tree_node(entity, open_path, passes);
        ImGui::TreePop();
    }
}
void EditorUI::hierarchy() {
    if (!show_hierarchy) return;
    if (!begin_panel("Scene Hierarchy", "Scene Hierarchy", context::outliner, &show_hierarchy)) { ImGui::End(); return; }
    shown_rows = std::exchange(visible_rows, {});
    records.clear(); children.clear();
    for (auto &e : session.scene().snapshot().entities) { children[e.parent].push_back(e.id); records.emplace(e.id, std::move(e)); }
    children.erase(EntityId{0});
    // ---- Search and filter ----
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", "Search entities", outliner_search.data(), outliner_search.size());
    mark("Outliner.Search");
    const auto chip = [this](const char *label, bool &on) {
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.5f, 0.85f, 1));
        if (ImGui::SmallButton(label)) on = !on;
        if (on) ImGui::PopStyleColor();
        mark(std::string("Outliner.Chip.") + label);
        ImGui::SameLine();
    };
    chip("Mesh", outliner_show_mesh); chip("Light", outliner_show_light); chip("Camera", outliner_show_camera); chip("Physics", outliner_show_physics);
    ImGui::TextDisabled("%zu", records.size());
    ImGui::SetItemTooltip("Entities in the scene. Drag a row to reparent it, Ctrl or Shift selects several, right-click for actions.");
    // ---- What passes: matches and the ancestors that lead to them ----
    std::set<EntityId> passes;
    const bool filtering = outliner_search[0] || outliner_show_mesh || outliner_show_light || outliner_show_camera || outliner_show_physics;
    if (filtering) {
        for (const auto &[id, e] : records)
            if (entity_shown(e)) {
                passes.insert(id);
                for (EntityId p = e.parent; p && records.contains(p); p = records.at(p).parent) passes.insert(p);
            }
        if (passes.empty()) ImGui::TextDisabled("Nothing matches.");
    }
    std::set<EntityId> open_path;
    if (session.selection() != revealed) {
        revealed = session.selection();
        if (records.contains(revealed))
            for (EntityId p = records.at(revealed).parent; p && records.contains(p); p = records.at(p).parent) open_path.insert(p);
    }
    ImGui::BeginChild("##tree");
    const auto &folders = session.view_state().folders;
    if (!filtering || !passes.empty()) {
        for (const auto &folder : folders.folders) if (!folder.parent) folder_node(folder, open_path, passes);
        for (const auto &[id, e] : records) {
            if (e.parent) continue;
            // Filed in a folder that still exists: drawn there.
            const auto in_folder = session.folder_of(id);
            if (in_folder && folders.find(in_folder)) continue;
            tree_node(id, open_path, passes);
        }
    }
    // The rest of the panel is a drop zone for moving entities back to the root.
    const auto room = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##hierarchy-root", {std::max(room.x, 1.f), std::max(room.y, px(24))});
    mark("Hierarchy.Empty");
    drop_onto(0);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) { session.select(0); selected_folder = 0; }
    if (ImGui::BeginPopupContextItem("##empty-menu")) {
        command_item("Outliner.NewFolder");
        ImGui::Separator();
        create_items();
        ImGui::Separator();
        command_item("Edit.Paste");
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::End();
}
} // namespace engine::editor
