#include "EditorUI.hpp"
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>

namespace engine::editor {
void EditorUI::inspector() {
    ImGui::Begin("Inspector");
    const auto record = session.scene().inspect(session.selection());
    if (!record) { ImGui::TextWrapped("Select an entity in the hierarchy, or click an object or a light/camera marker in the viewport."); ImGui::End(); return; }
    // Physics changes the revision every tick in Play mode; a field being edited keeps its value until released.
    if (draft_id != record->id || draft_scene != session.scene_identity() ||
        (draft_revision != session.scene_revision() && !ImGui::IsAnyItemActive())) {
        draft_id = record->id; draft_scene = session.scene_identity(); draft_revision = session.scene_revision();
        std::snprintf(name.data(), name.size(), "%s", record->name.c_str());
        const auto &t = record->transform;
        position[0] = float(t.position.x); position[1] = float(t.position.y); position[2] = float(t.position.z);
        const auto euler = glm::degrees(glm::eulerAngles(glm::dquat(t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z)));
        for (int i = 0; i < 3; ++i) rotation[i] = tidy_degrees(euler[i]);
        scale[0] = float(t.scale.x); scale[1] = float(t.scale.y); scale[2] = float(t.scale.z);
        components = record->components;
    }
    if (playing()) ImGui::TextColored({0.45f, 0.85f, 0.45f, 1}, "Play mode: edits affect the runtime copy only");
    ImGui::TextDisabled("Stable ID: %llu", static_cast<unsigned long long>(record->id));
    if (ImGui::InputText("Name", name.data(), name.size(), ImGuiInputTextFlags_EnterReturnsTrue))
        report(session.execute(commands::RenameEntity{record->id, name.data()}), "Renamed entity");
    ImGui::TextDisabled("Press Enter to rename");
    ImGui::SeparatorText("Local transform");
    bool commit = false;
    ImGui::DragFloat3("Position", position, 0.03f); commit |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::DragFloat3("Rotation", rotation, 0.5f); commit |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::DragFloat3("Scale", scale, 0.01f); commit |= ImGui::IsItemDeactivatedAfterEdit();
    if (commit) {
        const glm::dquat q(glm::radians(glm::dvec3(rotation[0], rotation[1], rotation[2])));
        Transform t{{position[0], position[1], position[2]}, {q.x,q.y,q.z,q.w}, {scale[0],scale[1],scale[2]}};
        report(session.execute(commands::SetTransform{record->id, t}), "Transform committed");
        draft_revision = ~uint64_t(0);
    }
    ImGui::TextDisabled("Rotation: XYZ Euler degrees");
    ImGui::TextWrapped("Each drag commits on release as one undo step.");
    if (ImGui::BeginCombo("Parent", record->parent ? session.scene().inspect(record->parent)->name.c_str() : "(root)")) {
        if (ImGui::Selectable("(root)", !record->parent)) report(session.reparent(record->id, 0), "Moved to root (world transform kept)");
        for (const auto &e : session.scene().snapshot().entities) {
            // The entity itself and its descendants would form a cycle.
            bool own = false;
            for (EntityId p = e.id; p && !own; p = session.scene().inspect(p)->parent) own = p == record->id;
            ImGui::PushID(reinterpret_cast<void *>(static_cast<uintptr_t>(e.id)));
            if (ImGui::Selectable(e.name.c_str(), record->parent == e.id, own ? ImGuiSelectableFlags_Disabled : 0))
                report(session.reparent(record->id, e.id), "Reparented (world transform kept)");
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SeparatorText("World position");
    const auto w = session.scene().world_transform(record->id);
    ImGui::Text("%.3f, %.3f, %.3f", w->matrix[12], w->matrix[13], w->matrix[14]);
    component_sections(*record);
    ImGui::End();
}
bool EditorUI::committed(bool changed) { return ImGui::IsItemDeactivatedAfterEdit() || (changed && !ImGui::IsItemActive()); }
bool EditorUI::color_edit(const char *label, Vec3 &c) {
    float v[3]{float(c.x), float(c.y), float(c.z)};
    const bool changed = ImGui::ColorEdit3(label, v, ImGuiColorEditFlags_Float);
    if (changed) c = {std::clamp(double(v[0]), 0.0, 1.0), std::clamp(double(v[1]), 0.0, 1.0), std::clamp(double(v[2]), 0.0, 1.0)};
    return committed(changed);
}
bool EditorUI::drag(const char *label, double &value, float speed, double low, double high, const char *format) {
    float v = float(value);
    const bool changed = ImGui::DragFloat(label, &v, speed, float(low), float(high), format, ImGuiSliderFlags_AlwaysClamp);
    if (changed) value = v;
    return committed(changed);
}
bool EditorUI::drag3(const char *label, Vec3 &value, float speed, float low, float high) {
    float v[3]{float(value.x), float(value.y), float(value.z)};
    const bool changed = ImGui::DragFloat3(label, v, speed, low, high, "%.3f", low < high ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None);
    if (changed) value = {v[0], v[1], v[2]};
    return committed(changed);
}
void EditorUI::apply(commands::Command command, const char *success) { report(session.execute(std::move(command)), success); draft_revision = ~uint64_t(0); }
bool EditorUI::asset_combo(const char *label, AssetRef &ref, assets::AssetType type, const char *none, bool whiten) {
    bool changed = false;
    if (ImGui::BeginCombo(label, ref.empty() ? none : ref.path.c_str())) {
        if (ImGui::Selectable(none, ref.empty())) { ref = {}; changed = true; }
        for (const auto &meta : session.assets()) {
            if (meta.type != type) continue;
            if (ImGui::Selectable(meta.path.c_str(), ref.id == meta.id.value)) {
                ref = {meta.id.value, meta.path}; changed = true;
                if (whiten) components.mesh->color = {1, 1, 1};
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}
bool EditorUI::remove_button(const char *id) {
    ImGui::PushID(id); const bool clicked = ImGui::SmallButton("Remove"); ImGui::PopID(); return clicked;
}
void EditorUI::component_sections(const EntityRecord &record) {
    auto &c = components;
    if (c.mesh) {
        ImGui::SeparatorText("Mesh Renderer");
        auto &m = *c.mesh; bool commit = false;
        commit |= asset_combo("Model", m.mesh_asset, assets::AssetType::Mesh, "(built-in mesh)");
        if (m.mesh_asset.empty()) {
            int shape = int(m.mesh) - 1;
            if (ImGui::Combo("Mesh", &shape, "Cube\0Sphere\0Plane\0")) { m.mesh = BuiltinMesh(shape + 1); commit = true; }
        }
        commit |= asset_combo("Material", m.material, assets::AssetType::Material, m.mesh_asset.empty() ? "(none)" : "(from the model)");
        commit |= asset_combo("Texture", m.texture, assets::AssetType::Texture, m.mesh_asset.empty() ? "(none)" : "(from the model)");
        commit |= color_edit("Color", m.color);
        const auto model = session.model(m.mesh_asset);
        if (!m.material.empty()) ImGui::TextDisabled("Roughness and metallic come from the material.");
        else if (model && !model->submeshes.empty()) ImGui::TextDisabled("Roughness and metallic come from the model.");
        else {
            commit |= drag("Roughness", m.roughness, 0.01f, 0, 1);
            commit |= drag("Metallic", m.metallic, 0.01f, 0, 1);
        }
        float uv[2]{float(m.uv_scale_u), float(m.uv_scale_v)};
        if (ImGui::DragFloat2("UV scale", uv, 0.01f, 0.001f, 1000, "%.3f", ImGuiSliderFlags_AlwaysClamp)) { m.uv_scale_u = uv[0]; m.uv_scale_v = uv[1]; }
        commit |= ImGui::IsItemDeactivatedAfterEdit();
        // A model's parts can each take a material.
        if (const auto parts = session.mesh_part_count(m); parts > 1) {
            ImGui::TextDisabled("Materials per part (%zu parts)", parts);
            for (std::uint32_t part = 0; part < parts && part < max_material_slots; ++part) {
                ImGui::PushID(int(part));
                AssetRef chosen;
                const auto slot = std::find_if(m.material_slots.begin(), m.material_slots.end(), [&](const MaterialSlot &s) { return s.slot == part; });
                if (slot != m.material_slots.end()) chosen = slot->material;
                if (asset_combo(("Part " + std::to_string(part)).c_str(), chosen, assets::AssetType::Material, "(default)", false)) {
                    report(session.assign_material(record.id, assets::AssetId{chosen.id}, part), "Part material changed");
                    draft_revision = ~uint64_t(0);
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
        }
        if (commit) apply(commands::SetMesh{record.id, m}, "Mesh renderer changed");
        if (remove_button("mesh")) apply(commands::SetMesh{record.id, std::nullopt}, "Mesh renderer removed");
    }
    if (c.camera) {
        ImGui::SeparatorText("Camera");
        auto &cam = *c.camera; bool commit = false;
        commit |= drag("Field of view", cam.vertical_fov_degrees, 0.2f, 1, 179, "%.1f deg");
        commit |= drag("Near", cam.near_plane, 0.01f, 0.001, cam.far_plane * 0.999);
        commit |= drag("Far", cam.far_plane, 1.0f, cam.near_plane * 1.001, 1e6, "%.1f");
        commit |= ImGui::Checkbox("Primary (Play mode view)", &cam.primary);
        if (commit) apply(commands::SetCamera{record.id, cam}, "Camera changed");
        ImGui::TextDisabled("Looks along local -Z; must not be scaled.");
        if (remove_button("camera")) apply(commands::SetCamera{record.id, std::nullopt}, "Camera removed");
    }
    if (c.light) {
        ImGui::SeparatorText("Light");
        auto &l = *c.light; bool commit = false;
        int type = int(l.type) - 1;
        if (ImGui::Combo("Type", &type, "Directional\0Point\0Spot\0")) { l.type = LightType(type + 1); commit = true; }
        commit |= color_edit("Light color", l.color);
        commit |= drag(l.type == LightType::Directional ? "Intensity (lux)" : "Intensity (lm)", l.intensity, 500.0f, 0, 1e7, "%.0f");
        if (l.type != LightType::Directional) commit |= drag("Range (m)", l.range, 0.05f, 0.01, 1000);
        if (l.type == LightType::Spot) {
            commit |= drag("Inner cone (deg)", l.inner_cone_degrees, 0.2f, 0, l.outer_cone_degrees, "%.1f");
            commit |= drag("Outer cone (deg)", l.outer_cone_degrees, 0.2f, l.inner_cone_degrees, 90, "%.1f");
        }
        if (l.type != LightType::Point) ImGui::TextDisabled("Shines along local -Z.");
        commit |= ImGui::Checkbox("Cast shadows", &l.cast_shadows);
        if (commit) apply(commands::SetLight{record.id, l}, "Light changed");
        if (remove_button("light")) apply(commands::SetLight{record.id, std::nullopt}, "Light removed");
    }
    if (c.environment) {
        ImGui::SeparatorText("Environment");
        ImGui::TextDisabled("Edited in the Lighting panel.");
        if (remove_button("environment")) apply(commands::SetEnvironment{record.id, std::nullopt}, "Environment removed");
    }
    physics_sections(record);
    behavior_sections(record);
    ImGui::Spacing();
    if (ImGui::Button("Add Component")) ImGui::OpenPopup("Add Component");
    if (ImGui::BeginPopup("Add Component")) {
        if (ImGui::MenuItem("Mesh Renderer", nullptr, false, !c.mesh)) apply(commands::SetMesh{record.id, MeshComponent{}}, "Mesh renderer added");
        if (ImGui::MenuItem("Camera", nullptr, false, !c.camera)) apply(commands::SetCamera{record.id, CameraComponent{}}, "Camera added");
        if (ImGui::MenuItem("Light", nullptr, false, !c.light)) apply(commands::SetLight{record.id, LightComponent{}}, "Light added");
        if (ImGui::MenuItem("Rigid Body", nullptr, false, !c.body)) apply(commands::SetRigidBody{record.id, RigidBodyComponent{}}, "Rigid body added");
        if (ImGui::MenuItem("Collider (fitted to mesh)", nullptr, false, c.colliders.size() < max_colliders_per_entity)) {
            auto list = c.colliders;
            list.push_back(fitted_collider(c.mesh));
            apply(commands::SetColliders{record.id, std::move(list)}, "Collider added");
        }
        if (ImGui::BeginMenu("Behavior")) {
            for (const auto &behavior : gameplay::behavior_templates()) {
                const bool present = std::ranges::any_of(c.behaviors, [&](const BehaviorComponent &b) { return b.type == behavior.type; });
                if (ImGui::MenuItem(behavior.type.c_str(), nullptr, false, !present && c.behaviors.size() < max_behaviors_per_entity)) {
                    auto list = c.behaviors;
                    list.push_back(behavior);
                    apply(commands::SetBehaviors{record.id, std::move(list)}, "Behavior added");
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
}
void EditorUI::physics_sections(const EntityRecord &record) {
    auto &c = components;
    if (c.body) {
        ImGui::SeparatorText("Rigid Body");
        auto &b = *c.body; bool commit = false;
        int kind = int(b.kind) - 1;
        if (ImGui::Combo("Body type", &kind, "Static\0Kinematic\0Dynamic\0")) { b.kind = BodyKind(kind + 1); commit = true; }
        commit |= drag3("Initial velocity", b.linear_velocity, 0.05f);
        commit |= drag3("Initial spin", b.angular_velocity, 0.05f);
        commit |= drag("Linear damping", b.linear_damping, 0.01f, 0, 100);
        commit |= drag("Angular damping", b.angular_damping, 0.01f, 0, 100);
        commit |= drag("Gravity scale", b.gravity_scale, 0.01f, -100, 100);
        commit |= ImGui::Checkbox("Can sleep", &b.enable_sleep);
        commit |= ImGui::Checkbox("Continuous collision", &b.is_bullet);
        commit |= ImGui::Checkbox("Lock rotation", &b.lock_rotation);
        if (commit) apply(commands::SetRigidBody{record.id, b}, "Rigid body changed");
        if (c.colliders.empty()) ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "No colliders: this body will not collide.");
        if (playing())
            if (const auto state = session.physics_bridge().body_state(record.id)) {
                const auto &v = state->linear_velocity;
                ImGui::TextDisabled("Speed %.2f m/s, mass %.1f kg, %s", std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z), state->mass, state->awake ? "awake" : "asleep");
            }
        if (remove_button("body")) apply(commands::SetRigidBody{record.id, std::nullopt}, "Rigid body removed");
    }
    for (std::size_t i = 0; i < c.colliders.size(); ++i) {
        ImGui::PushID(int(i));
        ImGui::SeparatorText(("Collider " + std::to_string(i + 1) + (c.body ? "" : " (static)")).c_str());
        auto &k = c.colliders[i]; bool commit = false;
        int shape = int(k.shape) - 1;
        if (ImGui::Combo("Shape", &shape, "Sphere\0Box\0Capsule\0")) { k.shape = ColliderShape(shape + 1); commit = true; }
        commit |= drag3("Center", k.center, 0.01f);
        if (k.shape == ColliderShape::Box) commit |= drag3("Half extents", k.half_extents, 0.01f, 0.001f, 10000);
        else commit |= drag("Radius", k.radius, 0.01f, 0.001, 10000);
        if (k.shape == ColliderShape::Capsule) commit |= drag("Half height", k.half_height, 0.01f, 0, 10000);
        if (k.shape != ColliderShape::Sphere) {
            const auto euler = glm::degrees(glm::eulerAngles(glm::dquat(k.rotation.w, k.rotation.x, k.rotation.y, k.rotation.z)));
            float r[3]{tidy_degrees(euler.x), tidy_degrees(euler.y), tidy_degrees(euler.z)};
            if (ImGui::DragFloat3("Local rotation", r, 0.5f)) {
                const glm::dquat q(glm::radians(glm::dvec3(r[0], r[1], r[2])));
                k.rotation = {q.x, q.y, q.z, q.w};
            }
            commit |= ImGui::IsItemDeactivatedAfterEdit();
        }
        commit |= drag("Density", k.density, 10.0f, 0, 100000, "%.0f kg/m3");
        commit |= drag("Friction", k.friction, 0.01f, 0, 10);
        commit |= drag("Restitution", k.restitution, 0.01f, 0, 1);
        commit |= ImGui::Checkbox("Sensor", &k.sensor);
        ImGui::SameLine(); commit |= ImGui::Checkbox("Report events", &k.events);
        if (commit) apply(commands::SetColliders{record.id, c.colliders}, "Collider changed");
        const bool removed = remove_button("collider");
        ImGui::PopID();
        if (removed) {
            auto list = c.colliders;
            list.erase(list.begin() + std::ptrdiff_t(i));
            apply(commands::SetColliders{record.id, std::move(list)}, "Collider removed");
            break;
        }
    }
    for (const auto &issue : session.physics_bridge().diagnostics().issues)
        if (issue.entity == record.id) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.35f, 1));
            ImGui::TextWrapped("Physics: %s", issue.message.c_str());
            ImGui::PopStyleColor();
        }
}
void EditorUI::behavior_sections(const EntityRecord &record) {
    auto &c = components;
    for (std::size_t i = 0; i < c.behaviors.size(); ++i) {
        ImGui::PushID(int(1000 + i));
        ImGui::SeparatorText(("Behavior: " + c.behaviors[i].type).c_str());
        bool commit = false;
        for (auto &[key, value] : c.behaviors[i].values) commit |= drag(key.c_str(), value, 0.05f, -1e9, 1e9);
        if (commit) apply(commands::SetBehaviors{record.id, c.behaviors}, "Behavior changed");
        const bool removed = remove_button("behavior");
        ImGui::PopID();
        if (removed) {
            auto list = c.behaviors;
            list.erase(list.begin() + std::ptrdiff_t(i));
            apply(commands::SetBehaviors{record.id, std::move(list)}, "Behavior removed");
            break;
        }
    }
}
} // namespace engine::editor
