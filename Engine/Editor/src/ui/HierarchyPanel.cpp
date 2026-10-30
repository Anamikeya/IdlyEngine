#include "EditorUI.hpp"
#include <algorithm>
#include <cstring>

namespace engine::editor {
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
                const auto result = session.reparent(id, parent);
                if (!result) { log("Error: " + result.error().message); transaction.cancel(); moved = false; break; }
            }
            if (moved) log(done);
        } else report(session.reparent(dragged, parent), done);
    }
    ImGui::EndDragDropTarget();
}
// A plain click selects the row. Ctrl toggles it; Shift selects the rows from the last clicked one to this one.
void EditorUI::click_row(EntityId id) {
    const auto &io = ImGui::GetIO();
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
void EditorUI::tree_node(EntityId id, const std::set<EntityId> &open_path) {
    visible_rows.push_back(id);
    const auto &e = records.at(id);
    const auto kids = children.find(id);
    const bool leaf = kids == children.end();
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (session.selected().contains(id)) flags |= ImGuiTreeNodeFlags_Selected;
    if (open_path.contains(id)) ImGui::SetNextItemOpen(true);
    const bool hidden_row = !playing() && session.hidden(id);
    if (hidden_row) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void *>(static_cast<uintptr_t>(id)), flags, hidden_row ? "%s (hidden)" : "%s", e.name.c_str());
    if (hidden_row) ImGui::PopStyleColor();
    mark("Row." + e.name);
    mark("RowAt." + std::to_string(visible_rows.size() - 1)); // Names repeat; a row's place in the tree does not.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        // A row already in a multi-selection may be the start of a drag of all of it, so a plain click waits for the release.
        if (session.selected().contains(id) && session.selected().size() > 1 && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) click_on_release = id;
        else click_row(id);
    }
    if (click_on_release == id && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !ImGui::GetDragDropPayload()) click_row(id);
        click_on_release = 0;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { session.select(id); session.focus(); }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(entity_payload, &id, sizeof id);
        ImGui::Text("Move '%s'", e.name.c_str());
        ImGui::EndDragDropSource();
    }
    drop_onto(id);
    if (ImGui::BeginPopupContextItem()) {
        if (!session.selected().contains(id)) session.select(id);
        if (ImGui::BeginMenu("Create child")) {
            command_item("Create.ChildEmpty", "Empty");
            command_item("Create.ChildCube", "Cube");
            command_item("Create.ChildSphere", "Sphere");
            command_item("Create.ChildPointLight", "Point Light");
            ImGui::EndMenu();
        }
        command_item("Edit.Duplicate");
        command_item("Edit.MoveToRoot");
        command_item("Viewport.FocusSelected");
        ImGui::Separator();
        command_item("Edit.Delete");
        ImGui::EndPopup();
    }
    const auto &c = e.components;
    if (c.light || c.camera || c.body || !c.colliders.empty()) {
        ImGui::SameLine();
        // A behavior-driven entity's tag is left off: the Inspector lists them.
        ImGui::TextDisabled("%s%s%s", c.light ? (c.light->type == LightType::Point ? " point light" : c.light->type == LightType::Spot ? " spot light" : " directional light") : "",
                            c.camera ? (c.camera->primary ? " camera (primary)" : " camera") : "",
                            c.body ? (c.body->kind == BodyKind::Dynamic ? " dynamic" : c.body->kind == BodyKind::Kinematic ? " kinematic" : " static")
                                   : !c.colliders.empty() ? " collider" : "");
    }
    if (open && !leaf) {
        for (const auto child : kids->second) tree_node(child, open_path);
        ImGui::TreePop();
    }
}
void EditorUI::hierarchy() {
    begin_panel("Scene Hierarchy", "Scene Hierarchy", context::outliner);
    shown_rows = std::exchange(visible_rows, {});
    records.clear(); children.clear();
    for (auto &e : session.scene().snapshot().entities) { children[e.parent].push_back(e.id); records.emplace(e.id, std::move(e)); }
    children.erase(EntityId{0});
    ImGui::TextDisabled("%zu entities", records.size());
    ImGui::SetItemTooltip("Drag a row to reparent it. Right-click for actions. Ctrl or Shift selects several.");
    std::set<EntityId> open_path;
    if (session.selection() != revealed) {
        revealed = session.selection();
        if (records.contains(revealed))
            for (EntityId p = records.at(revealed).parent; p && records.contains(p); p = records.at(p).parent) open_path.insert(p);
    }
    for (const auto &[id, e] : records) if (!e.parent) tree_node(id, open_path);
    // The rest of the panel is a drop zone for moving entities back to the root.
    const auto room = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##hierarchy-root", {std::max(room.x, 1.f), std::max(room.y, px(24))});
    mark("Hierarchy.Empty");
    drop_onto(0);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) session.select(0);
    ImGui::End();
}
} // namespace engine::editor
