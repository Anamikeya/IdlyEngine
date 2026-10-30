#include "EditorUI.hpp"
#include <algorithm>
#include <cstring>

namespace engine::editor {
void EditorUI::drop_onto(EntityId parent) {
    if (!ImGui::BeginDragDropTarget()) return;
    if (const auto *payload = ImGui::AcceptDragDropPayload(entity_payload)) {
        EntityId dragged = 0; std::memcpy(&dragged, payload->Data, sizeof dragged);
        if (dragged != parent) report(session.reparent(dragged, parent), parent ? "Reparented (world transform kept)" : "Moved to root (world transform kept)");
    }
    ImGui::EndDragDropTarget();
}
void EditorUI::tree_node(EntityId id, const std::set<EntityId> &open_path) {
    const auto &e = records.at(id);
    const auto kids = children.find(id);
    const bool leaf = kids == children.end();
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (session.selection() == id) flags |= ImGuiTreeNodeFlags_Selected;
    if (open_path.contains(id)) ImGui::SetNextItemOpen(true);
    const bool open = ImGui::TreeNodeEx(reinterpret_cast<void *>(static_cast<uintptr_t>(id)), flags, "%s", e.name.c_str());
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) session.select(id);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { session.select(id); session.focus(); }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(entity_payload, &id, sizeof id);
        ImGui::Text("Move '%s'", e.name.c_str());
        ImGui::EndDragDropSource();
    }
    drop_onto(id);
    if (ImGui::BeginPopupContextItem()) {
        session.select(id);
        if (ImGui::BeginMenu("Create child")) {
            const Transform above{{0,1,0}, {}, {1,1,1}};
            if (ImGui::MenuItem("Empty")) spawn("Empty", {}, above, id);
            if (ImGui::MenuItem("Cube")) spawn("Cube", {MeshComponent{BuiltinMesh::Cube}, {}, {}}, above, id);
            if (ImGui::MenuItem("Sphere")) spawn("Sphere", {MeshComponent{BuiltinMesh::Sphere}, {}, {}}, above, id);
            if (ImGui::MenuItem("Point Light")) spawn("Point Light", {{}, {}, point_light()}, above, id);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate_selection();
        if (ImGui::MenuItem("Move to root", nullptr, false, e.parent != 0)) report(session.reparent(id, 0), "Moved to root (world transform kept)");
        if (ImGui::MenuItem("Focus", "F")) session.focus();
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", "Del")) delete_selection();
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
    ImGui::Begin("Scene Hierarchy");
    records.clear(); children.clear();
    for (auto &e : session.scene().snapshot().entities) { children[e.parent].push_back(e.id); records.emplace(e.id, std::move(e)); }
    children.erase(EntityId{0});
    ImGui::TextDisabled("%zu entities  (drag to reparent, right-click for actions)", records.size());
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
    drop_onto(0);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) session.select(0);
    ImGui::End();
}
} // namespace engine::editor
