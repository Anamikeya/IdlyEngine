#include "EditorUI.hpp"
#include <engine/editor/PropertyMeta.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <functional>
#include <span>

// The Details panel, generated from the property tables. Nothing here knows what a light or a collider is, beyond which
// table and command go with it: a new field needs a row in its table and no code in this file.
namespace engine::editor {
namespace {
bool has_text(std::string_view text, std::string_view wanted) {
    if (wanted.empty()) return true;
    return !std::ranges::search(text, wanted, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }).empty();
}
// One section's values: one per target, the primary one last.
template<class T> struct Section {
    std::string scope; // Names what is remembered about it: "Light", "Collider 2".
    const PropertyTable<T> &table;
    std::vector<EntityId> ids;
    std::vector<T> values;
    std::function<commands::Command(EntityId, const T &)> command;
};
glm::dvec3 euler_degrees(const Quaternion &q) { return glm::degrees(glm::eulerAngles(glm::dquat(q.w, q.x, q.y, q.z))); }
Quaternion from_degrees(const glm::dvec3 &degrees) {
    const glm::dquat q(glm::radians(degrees));
    return {q.x, q.y, q.z, q.w};
}
// A number field: drag to scrub, Shift fine, Ctrl coarse, click to type.
bool scrub(const char *id, double &value, double step, double low, double high, const char *format, bool mixed, const char *mixed_text = "Multiple Values") {
    auto &io = ImGui::GetIO();
    const double speed = step * (io.KeyShift ? 0.1 : 1) * (io.KeyCtrl ? 10 : 1);
    ImGuiSliderFlags flags = ImGuiSliderFlags_NoSpeedTweaks;
    if (low < high) flags |= ImGuiSliderFlags_AlwaysClamp;
    // ImGui takes Ctrl+click to mean "type". Here Ctrl means coarse, and a plain click types, so it does not see Ctrl
    // on these fields, unless one is being typed in: there Ctrl+A must still work.
    const bool typing = ImGui::TempInputIsActive(ImGui::GetID(id));
    const bool ctrl = io.KeyCtrl;
    if (!typing) io.KeyCtrl = false;
    const bool changed = ImGui::DragScalar(id, ImGuiDataType_Double, &value, float(speed), low < high ? &low : nullptr, low < high ? &high : nullptr, mixed ? mixed_text : format, flags);
    io.KeyCtrl = ctrl;
    return changed;
}
// Three number fields, marked red, green and blue. Returns the axis that changed, or -1.
int axis_fields(std::array<double, 3> &values, double step, double low, double high, const char *format, const std::array<bool, 3> &mixed) {
    constexpr ImU32 colors[] = {IM_COL32(205, 60, 60, 255), IM_COL32(70, 180, 70, 255), IM_COL32(70, 110, 215, 255)};
    constexpr const char *ids[] = {"##x", "##y", "##z"};
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x, bar = px(3);
    const float width = (ImGui::GetContentRegionAvail().x - 2 * spacing) / 3;
    int changed = -1;
    for (int axis = 0; axis < 3; ++axis) {
        if (axis) ImGui::SameLine(0, spacing);
        ImGui::SetNextItemWidth(width);
        const auto at = ImGui::GetCursorScreenPos();
        if (scrub(ids[axis], values[std::size_t(axis)], step, low, high, format, mixed[std::size_t(axis)], "Mixed")) changed = axis;
        if (mixed[std::size_t(axis)]) ImGui::SetItemTooltip("Multiple Values");
        ImGui::GetWindowDrawList()->AddRectFilled(at, {at.x + bar, at.y + ImGui::GetFrameHeight()}, colors[axis], px(1));
    }
    return changed;
}
void draw_star(ImVec2 center, float radius, ImU32 color) {
    auto *draw = ImGui::GetWindowDrawList();
    ImVec2 points[10];
    for (int i = 0; i < 10; ++i) {
        const float angle = -1.5707963f + float(i) * 0.6283185f, r = i % 2 ? radius * 0.42f : radius;
        points[i] = {center.x + std::cos(angle) * r, center.y + std::sin(angle) * r};
    }
    for (int i = 0; i < 10; ++i) draw->AddTriangleFilled(center, points[i], points[(i + 1) % 10], color);
}
template<class T> bool same_values(const Section<T> &s, const Property<T> &p) {
    const auto first = p.get(s.values.back());
    return std::ranges::all_of(s.values, [&](const T &v) { return p.get(v) == first; });
}
template<class T> bool is_modified(const Section<T> &s, const Property<T> &p) {
    const auto standard = p.get(T{});
    return std::ranges::any_of(s.values, [&](const T &v) { return p.get(v) != standard; });
}
template<class T> void edit_all(EditorUI &ui, const Section<T> &s, std::string_view name, const std::function<void(T &)> &change) {
    std::vector<commands::Command> edits;
    for (std::size_t i = 0; i < s.values.size(); ++i) {
        T value = s.values[i];
        change(value);
        if (!(value == s.values[i])) edits.push_back(s.command(s.ids[i], value));
    }
    ui.details_apply(std::string(name), std::move(edits));
}
std::string settings_key(std::string_view group, std::string_view scope, std::string_view id = {}) {
    auto key = "details." + std::string(group) + '.' + std::string(scope);
    if (!id.empty()) key += '.' + std::string(id);
    std::ranges::replace(key, ' ', '_');
    return key;
}
template<class T> void property_row(EditorUI &ui, Section<T> &s, const Property<T> &p) {
    const T &primary = s.values.back();
    const bool applies = !p.condition || p.condition(primary);
    const auto value = p.get(primary);
    const bool mixed = !same_values(s, p);
    const bool modified = is_modified(s, p);
    const auto favorite_key = settings_key("favorite", s.table.name, p.id);
    const bool favorite = ui.settings.flag(favorite_key, false);
    ImGui::PushID(p.id.data(), p.id.data() + p.id.size());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    // The star, then the name.
    const float star_size = ImGui::GetFrameHeight();
    const auto star_at = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##star", {star_size * 0.7f, star_size})) { ui.settings.set(favorite_key, !favorite); ui.save_settings(); }
    const bool star_hovered = ImGui::IsItemHovered();
    ImGui::SetItemTooltip(favorite ? "Remove from favorites" : "Add to favorites");
    ui.mark("Star." + std::string(s.table.name) + "." + std::string(p.id));
    draw_star({star_at.x + star_size * 0.35f, star_at.y + star_size / 2}, px(5.5f), favorite ? IM_COL32(245, 200, 70, 255) : star_hovered ? IM_COL32(170, 170, 170, 255) : IM_COL32(70, 75, 85, 255));
    ImGui::SameLine(0, 0);
    ImGui::AlignTextToFramePadding();
    if (!applies) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(p.display_name.data(), p.display_name.data() + p.display_name.size());
    if (!applies) ImGui::PopStyleColor();
    if (!p.tooltip.empty()) ImGui::SetItemTooltip("%.*s", int(p.tooltip.size()), p.tooltip.data());
    ImGui::OpenPopupOnItemClick("##property", ImGuiPopupFlags_MouseButtonRight);
    // ---- The value ----
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::BeginDisabled(!applies || p.read_only);
    ImGui::BeginGroup(); // So the menu and the UI map see the whole value, however many fields it has.
    const auto name = "Change " + std::string(p.display_name);
    std::visit([&](const auto &current) {
        using V = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<V, bool>) {
            bool on = current;
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, mixed);
            if (ImGui::Checkbox("##value", &on)) edit_all<T>(ui, s, name, [&](T &t) { p.set(t, PropertyValue(on)); });
            ImGui::PopItemFlag();
        } else if constexpr (std::is_same_v<V, double>) {
            double number = current;
            if (scrub("##value", number, p.step, p.min, p.max, p.format.data(), mixed)) edit_all<T>(ui, s, name, [&](T &t) { p.set(t, PropertyValue(number)); });
        } else if constexpr (std::is_same_v<V, Vec3>) {
            if (p.type == PropertyType::Color) {
                float color[3]{float(current.x), float(current.y), float(current.z)};
                if (mixed) {
                    // There is no one color to show. A click gives them all the primary entity's.
                    if (ImGui::Button("Multiple Values", {-FLT_MIN, 0})) edit_all<T>(ui, s, name, [&](T &t) { p.set(t, value); });
                } else if (ImGui::ColorEdit3("##value", color, ImGuiColorEditFlags_Float))
                    edit_all<T>(ui, s, name, [&](T &t) { p.set(t, PropertyValue(Vec3{std::clamp(double(color[0]), 0.0, 1.0), std::clamp(double(color[1]), 0.0, 1.0), std::clamp(double(color[2]), 0.0, 1.0)})); });
            } else {
                std::array<double, 3> values{current.x, current.y, current.z};
                std::array<bool, 3> differ{};
                for (const auto &v : s.values) {
                    const auto other = std::get<Vec3>(p.get(v));
                    differ[0] |= other.x != current.x; differ[1] |= other.y != current.y; differ[2] |= other.z != current.z;
                }
                if (const int axis = axis_fields(values, p.step, p.min, p.max, p.format.data(), differ); axis >= 0)
                    edit_all<T>(ui, s, name, [&](T &t) {
                        auto vector = std::get<Vec3>(p.get(t));
                        (axis == 0 ? vector.x : axis == 1 ? vector.y : vector.z) = values[std::size_t(axis)];
                        p.set(t, PropertyValue(vector));
                    });
            }
        } else if constexpr (std::is_same_v<V, Quaternion>) {
            // Degrees held while dragging: read back from a quaternion they would jump at the poles.
            const auto key = s.scope + '.' + std::string(p.id);
            if (ui.details_euler_key != key || !ImGui::IsAnyItemActive()) {
                const auto now = euler_degrees(current);
                for (int i = 0; i < 3; ++i) ui.details_euler[std::size_t(i)] = tidy_degrees(now[i]);
                ui.details_euler_key = key;
            }
            std::array<bool, 3> differ{};
            for (const auto &v : s.values) differ[0] = differ[1] = differ[2] = differ[0] || std::get<Quaternion>(p.get(v)) != current;
            if (const int axis = axis_fields(ui.details_euler, p.step, p.min, p.max, p.format.data(), differ); axis >= 0)
                edit_all<T>(ui, s, name, [&](T &t) {
                    // Each entity keeps its other two angles; the primary one's come from the held degrees.
                    auto degrees = t == s.values.back() ? glm::dvec3(ui.details_euler[0], ui.details_euler[1], ui.details_euler[2]) : euler_degrees(std::get<Quaternion>(p.get(t)));
                    degrees[axis] = ui.details_euler[std::size_t(axis)];
                    p.set(t, PropertyValue(from_degrees(degrees)));
                });
        } else if constexpr (std::is_same_v<V, int>) {
            const auto label = mixed ? std::string("Multiple Values") : std::string(p.choices[std::size_t(current)]);
            if (ImGui::BeginCombo("##value", label.c_str())) {
                for (int i = 0; i < int(p.choices.size()); ++i) {
                    const std::string choice(p.choices[std::size_t(i)]);
                    if (ImGui::Selectable(choice.c_str(), !mixed && i == current)) edit_all<T>(ui, s, name, [&](T &t) { p.set(t, PropertyValue(i)); });
                }
                ImGui::EndCombo();
            }
        } else if constexpr (std::is_same_v<V, AssetRef>) {
            AssetRef chosen = current;
            if (ui.asset_field(p.asset_type, chosen, mixed)) edit_all<T>(ui, s, name, [&](T &t) { p.set(t, PropertyValue(chosen)); });
        } else {
            std::uint64_t bits = current;
            if (ImGui::InputScalar("##value", ImGuiDataType_U64, &bits, nullptr, nullptr, "%016llX", ImGuiInputTextFlags_CharsHexadecimal)) edit_all<T>(ui, s, name, [&](T &t) { p.set(t, PropertyValue(bits)); });
        }
    }, value);
    ImGui::EndGroup();
    ImGui::EndDisabled();
    ui.mark("Details." + s.scope + "." + std::string(p.id));
    ImGui::OpenPopupOnItemClick("##property", ImGuiPopupFlags_MouseButtonRight);
    // ---- The menu: copy, paste, reset ----
    if (ImGui::BeginPopup("##property")) {
        if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(property_to_text(value).c_str());
        ui.mark("Details.Copy");
        const auto *clipboard = ImGui::GetClipboardText();
        const auto pasted = property_from_text(clipboard ? clipboard : "");
        const bool fits = pasted && pasted->index() == value.index() && (p.type != PropertyType::Enum || std::size_t(std::get<int>(*pasted)) < p.choices.size());
        if (ImGui::MenuItem("Paste", nullptr, false, fits && applies && !p.read_only)) edit_all<T>(ui, s, "Paste " + std::string(p.display_name), [&](T &t) { p.set(t, *pasted); });
        ui.mark("Details.Paste");
        if (ImGui::MenuItem("Reset to Default", nullptr, false, modified)) edit_all<T>(ui, s, "Reset " + std::string(p.display_name), [&](T &t) { p.set(t, p.get(T{})); });
        ui.mark("Details.Reset");
        ImGui::Separator();
        if (ImGui::MenuItem(favorite ? "Remove from Favorites" : "Add to Favorites")) { ui.settings.set(favorite_key, !favorite); ui.save_settings(); }
        ImGui::EndPopup();
    }
    // ---- The arrow, only when it has somewhere to go ----
    ImGui::TableNextColumn();
    if (modified && !p.read_only) {
        if (ImGui::ArrowButton("##reset", ImGuiDir_Left)) edit_all<T>(ui, s, "Reset " + std::string(p.display_name), [&](T &t) { p.set(t, p.get(T{})); });
        ImGui::SetItemTooltip("Reset to default");
        ui.mark("Arrow." + s.scope + "." + std::string(p.id));
    }
    ImGui::PopID();
}
template<class T> bool row_wanted(const EditorUI &ui, const Section<T> &s, const Property<T> &p) {
    if (p.hidden) return false;
    if (p.condition_hides && p.condition && !p.condition(s.values.back())) return false;
    if (ui.details_only_modified && !is_modified(s, p)) return false;
    const std::string_view search(ui.details_search.data());
    return has_text(p.display_name, search) || has_text(p.category, search) || has_text(s.table.name, search);
}
bool begin_rows(const char *id) {
    if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch, 0.36f);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.64f);
    ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
    return true;
}
template<class T> void favorite_rows(EditorUI &ui, Section<T> &s) {
    if (s.values.empty()) return;
    for (const auto &p : s.table.properties) {
        if (!ui.settings.flag(settings_key("favorite", s.table.name, p.id), false) || !row_wanted(ui, s, p)) continue;
        // Tables are per-section so columns line up within one; the scope keeps two colliders' rows apart.
        ImGui::PushID(s.scope.c_str());
        property_row(ui, s, p);
        ImGui::PopID();
    }
}
// A heading that collapses and remembers it. While searching there is nothing to remember: everything found is shown.
bool remembered_header(EditorUI &ui, const std::string &title, const std::string &key, bool top_level, bool allow_overlap) {
    if (ui.details_search[0]) {
        if (top_level) ImGui::SeparatorText(title.c_str());
        return true;
    }
    ImGui::SetNextItemOpen(ui.settings.flag(key, true), ImGuiCond_Once);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (allow_overlap) flags |= ImGuiTreeNodeFlags_AllowOverlap;
    if (top_level) flags |= ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    const bool open = ImGui::TreeNodeEx(title.c_str(), flags);
    if (ImGui::IsItemToggledOpen()) { ui.settings.set(key, open); ui.save_settings(); }
    return open;
}
template<class T> void section_rows(EditorUI &ui, Section<T> &s) {
    std::vector<std::string_view> categories;
    for (const auto &p : s.table.properties)
        if (row_wanted(ui, s, p) && std::ranges::find(categories, p.category) == categories.end()) categories.push_back(p.category);
    for (const auto category : categories) {
        // A category named after its section needs no heading of its own.
        const bool heading = categories.size() > 1 || category != s.table.name;
        ImGui::PushID(category.data(), category.data() + category.size());
        const bool open = !heading || remembered_header(ui, std::string(category), settings_key("open", s.scope, category), false, false);
        if (open) {
            if (begin_rows("##rows")) {
                for (const auto &p : s.table.properties) if (p.category == category && row_wanted(ui, s, p)) property_row(ui, s, p);
                ImGui::EndTable();
            }
            if (heading && !ui.details_search[0]) ImGui::TreePop();
        }
        ImGui::PopID();
    }
}
// A component's heading, with the actions on the whole of it. False when collapsed or when nothing in it is wanted.
template<class T> bool section_header(EditorUI &ui, Section<T> &s, const std::function<void()> &remove) {
    if (!std::ranges::any_of(s.table.properties, [&](const Property<T> &p) { return row_wanted(ui, s, p); })) return false;
    ImGui::PushID(s.scope.c_str());
    const bool open = remembered_header(ui, s.scope, settings_key("open", s.scope), true, true);
    ui.mark("Header." + s.scope);
    if (!ui.details_search[0] && ImGui::BeginPopupContextItem("##component")) {
        if (ImGui::MenuItem("Copy Component")) ImGui::SetClipboardText(component_to_text(s.table, s.values.back()).c_str());
        const auto *clipboard = ImGui::GetClipboardText();
        T probe = s.values.back();
        const bool fits = clipboard && component_from_text(s.table, clipboard, probe);
        if (ImGui::MenuItem("Paste Component", nullptr, false, fits)) {
            const std::string text(clipboard);
            edit_all<T>(ui, s, "Paste " + std::string(s.table.name), [&](T &t) { component_from_text(s.table, text, t); });
        }
        ImGui::EndPopup();
    }
    if (remove) {
        const float width = ImGui::CalcTextSize("Remove").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - width);
        if (ui.tip_small_button("Remove", "Take this component off everything selected")) remove();
        ui.mark("Remove." + s.scope);
    }
    ImGui::PopID();
    return open;
}
template<class T> void section(EditorUI &ui, Section<T> s, const std::function<void()> &remove = {}) {
    if (s.values.empty()) return;
    if (!section_header(ui, s, remove)) return;
    ImGui::PushID(s.scope.c_str());
    section_rows(ui, s);
    ImGui::PopID();
}
}
void EditorUI::details_apply(std::string label, std::vector<commands::Command> edits) {
    if (edits.empty()) return;
    if (!details_transaction) {
        if (edits.size() > 1) label += " on " + std::to_string(edits.size()) + " Entities";
        (void)session.begin_transaction(std::move(label));
        details_transaction = true;
    }
    for (std::size_t at = 0; at < edits.size(); at += commands::CommandBus::max_transaction) {
        const auto count = std::min(edits.size() - at, commands::CommandBus::max_transaction);
        if (const auto done = session.execute(std::span<const commands::Command>(edits).subspan(at, count), {}); !done) {
            // A value the scene refuses leaves the field where it was. Saying so once per drag is enough.
            if (done.error().message != details_error) { details_error = done.error().message; log_error(details_error); }
            return;
        }
    }
}
void EditorUI::details_add(const char *what, const std::function<std::optional<commands::Command>(const EntityRecord &)> &make) {
    std::vector<commands::Command> edits;
    for (const auto &record : details_targets) if (auto edit = make(record)) edits.push_back(std::move(*edit));
    details_apply(std::string("Add ") + what, std::move(edits));
}
void EditorUI::inspector() {
    if (!show_inspector) return;
    const bool visible = begin_panel("Inspector", "Inspector", context::details, &show_inspector);
    // ---- Whose details ----
    if (!details_locked) details_pinned = session.selected().items();
    details_targets.clear();
    for (const auto id : details_pinned) if (auto record = session.scene().inspect(id)) details_targets.push_back(std::move(*record));
    if (!visible) { finish_details_edit(); ImGui::End(); return; }
    if (details_locked) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.45f, 0.15f, 1));
    if (tip_button(details_locked ? "Locked" : "Lock", "Keep showing this entity when the selection changes")) details_locked = !details_locked;
    if (details_locked) ImGui::PopStyleColor();
    ImGui::SetItemTooltip(details_locked ? "These details stay on the same entities while the selection changes. Click to follow the selection again." : "Keep showing these entities while the selection changes.");
    mark("Details.Lock");
    if (details_targets.empty()) {
        ImGui::TextWrapped("Select an entity in the hierarchy, or click an object or a light/camera marker in the viewport.");
        finish_details_edit();
        ImGui::End();
        return;
    }
    const auto &primary = details_targets.back();
    const bool many = details_targets.size() > 1;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Modified").x - px(34), px(60)));
    ImGui::InputTextWithHint("##search", "Search properties", details_search.data(), details_search.size());
    mark("Details.Search");
    ImGui::SameLine();
    ImGui::Checkbox("Modified", &details_only_modified);
    ImGui::SetItemTooltip("Show only properties that differ from their default");
    mark("Details.OnlyModified");
    if (playing()) ImGui::TextColored({0.45f, 0.85f, 0.45f, 1}, "Play mode: edits affect the runtime copy only");
    if (many) ImGui::TextColored({0.45f, 0.75f, 0.95f, 1}, "%zu entities selected. Edits apply to all of them.", details_targets.size());
    // ---- Name ----
    if (details_name_id != primary.id || !ImGui::IsAnyItemActive()) {
        std::snprintf(name.data(), name.size(), "%s", many ? "" : primary.name.c_str());
        details_name_id = primary.id;
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##name", many ? "Multiple Values" : "Name", name.data(), name.size(), ImGuiInputTextFlags_EnterReturnsTrue) && name[0]) {
        std::vector<commands::Command> edits;
        for (const auto &record : details_targets) edits.emplace_back(commands::RenameEntity{record.id, name.data()});
        details_apply("Rename", std::move(edits));
    }
    mark("Details.Name");
    if (!many) {
        ImGui::TextDisabled("ID %llu", static_cast<unsigned long long>(primary.id));
        ImGui::SameLine();
        const auto w = session.scene().world_transform(primary.id);
        ImGui::TextDisabled("World %.2f, %.2f, %.2f", w->matrix[12], w->matrix[13], w->matrix[14]);
    }
    // ---- Parent, for one entity ----
    if (!many && !details_search[0] && !details_only_modified) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##parent", (std::string("Parent: ") + (primary.parent ? session.scene().inspect(primary.parent)->name : "(root)")).c_str())) {
            if (ImGui::Selectable("(root)", !primary.parent)) report(session.reparent(primary.id, 0), "Moved to root (world transform kept)");
            for (const auto &e : session.scene().snapshot().entities) {
                // The entity itself and its descendants would form a cycle.
                bool own = false;
                for (EntityId p = e.id; p && !own; p = session.scene().inspect(p)->parent) own = p == primary.id;
                ImGui::PushID(reinterpret_cast<void *>(static_cast<uintptr_t>(e.id)));
                if (ImGui::Selectable(e.name.c_str(), primary.parent == e.id, own ? ImGuiSelectableFlags_Disabled : 0))
                    report(session.reparent(primary.id, e.id), "Reparented (world transform kept)");
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }
    // ---- Sections: a component shows when every target has it ----
    const auto all_have = [&](auto has) { return std::ranges::all_of(details_targets, has); };
    std::vector<EntityId> ids;
    for (const auto &record : details_targets) ids.push_back(record.id);
    const auto gather = [&](auto get) {
        std::vector<std::decay_t<decltype(get(primary))>> values;
        for (const auto &record : details_targets) values.push_back(get(record));
        return values;
    };
    const auto remover = [&](const char *what, auto make) {
        return std::function<void()>([this, what, make] {
            std::vector<commands::Command> edits;
            for (const auto &record : details_targets) edits.emplace_back(make(record.id));
            details_apply(std::string("Remove ") + what, std::move(edits));
        });
    };
    Section<Transform> transform{"Transform", transform_properties(), ids, gather([](const EntityRecord &r) { return r.transform; }),
                                 [](EntityId id, const Transform &v) { return commands::Command(commands::SetTransform{id, v}); }};
    const bool has_mesh = all_have([](const EntityRecord &r) { return r.components.mesh.has_value(); });
    const bool has_camera = all_have([](const EntityRecord &r) { return r.components.camera.has_value(); });
    const bool has_light = all_have([](const EntityRecord &r) { return r.components.light.has_value(); });
    const bool has_body = all_have([](const EntityRecord &r) { return r.components.body.has_value(); });
    const bool has_environment = all_have([](const EntityRecord &r) { return r.components.environment.has_value(); });
    auto mesh = has_mesh ? Section<MeshComponent>{"Mesh Renderer", mesh_properties(), ids, gather([](const EntityRecord &r) { return *r.components.mesh; }),
                                                  [](EntityId id, const MeshComponent &v) { return commands::Command(commands::SetMesh{id, v}); }}
                         : Section<MeshComponent>{"Mesh Renderer", mesh_properties(), {}, {}, {}};
    auto camera = has_camera ? Section<CameraComponent>{"Camera", camera_properties(), ids, gather([](const EntityRecord &r) { return *r.components.camera; }),
                                                        [](EntityId id, const CameraComponent &v) { return commands::Command(commands::SetCamera{id, v}); }}
                             : Section<CameraComponent>{"Camera", camera_properties(), {}, {}, {}};
    auto light = has_light ? Section<LightComponent>{"Light", light_properties(), ids, gather([](const EntityRecord &r) { return *r.components.light; }),
                                                     [](EntityId id, const LightComponent &v) { return commands::Command(commands::SetLight{id, v}); }}
                           : Section<LightComponent>{"Light", light_properties(), {}, {}, {}};
    auto environment = has_environment ? Section<EnvironmentComponent>{"Environment", environment_properties(), ids, gather([](const EntityRecord &r) { return *r.components.environment; }),
                                                                       [](EntityId id, const EnvironmentComponent &v) { return commands::Command(commands::SetEnvironment{id, v}); }}
                                       : Section<EnvironmentComponent>{"Environment", environment_properties(), {}, {}, {}};
    auto body = has_body ? Section<RigidBodyComponent>{"Rigid Body", body_properties(), ids, gather([](const EntityRecord &r) { return *r.components.body; }),
                                                       [](EntityId id, const RigidBodyComponent &v) { return commands::Command(commands::SetRigidBody{id, v}); }}
                         : Section<RigidBodyComponent>{"Rigid Body", body_properties(), {}, {}, {}};
    // Colliders by position in the list, while every target has that many.
    std::vector<Section<ColliderComponent>> colliders;
    std::size_t shared_colliders = primary.components.colliders.size();
    for (const auto &record : details_targets) shared_colliders = std::min(shared_colliders, record.components.colliders.size());
    for (std::size_t i = 0; i < shared_colliders; ++i)
        colliders.push_back({"Collider " + std::to_string(i + 1), collider_properties(), ids, gather([i](const EntityRecord &r) { return r.components.colliders[i]; }),
                             [this, i](EntityId id, const ColliderComponent &v) {
                                 auto list = session.scene().inspect(id)->components.colliders;
                                 list[i] = v;
                                 return commands::Command(commands::SetColliders{id, std::move(list)});
                             }});
    // ---- Favorites ----
    const auto favorites_wanted = [&](const auto &s) {
        return !s.values.empty() && std::ranges::any_of(s.table.properties, [&](const auto &p) { return settings.flag(settings_key("favorite", s.table.name, p.id), false) && row_wanted(*this, s, p); });
    };
    bool any_favorite = favorites_wanted(transform) || favorites_wanted(mesh) || favorites_wanted(camera) || favorites_wanted(light) || favorites_wanted(environment) || favorites_wanted(body);
    for (const auto &s : colliders) any_favorite |= favorites_wanted(s);
    ImGui::BeginChild("##details", {0, 0});
    if (any_favorite) {
        ImGui::SeparatorText("Favorites");
        if (begin_rows("##favorites")) {
            favorite_rows(*this, transform); favorite_rows(*this, mesh); favorite_rows(*this, camera); favorite_rows(*this, light); favorite_rows(*this, environment); favorite_rows(*this, body);
            for (auto &s : colliders) favorite_rows(*this, s);
            ImGui::EndTable();
        }
    }
    // ---- Everything ----
    section(*this, transform);
    if (has_mesh) {
        section(*this, mesh, remover("Mesh Renderer", [](EntityId id) { return commands::SetMesh{id, std::nullopt}; }));
        // A model's parts can each take a material. One entity at a time: they differ per model.
        if (!many && !details_search[0] && !details_only_modified) {
            const auto &m = *primary.components.mesh;
            const auto model = session.model(m.mesh_asset);
            if (m.material.empty() && model && !model->submeshes.empty()) ImGui::TextDisabled("Roughness and metallic come from the model.");
            else if (!m.material.empty()) ImGui::TextDisabled("Roughness and metallic come from the material.");
            if (const auto parts = session.mesh_part_count(m); parts > 1) {
                ImGui::TextDisabled("Materials per part (%zu parts)", parts);
                for (std::uint32_t part = 0; part < parts && part < max_material_slots; ++part) {
                    ImGui::PushID(int(part));
                    AssetRef chosen;
                    const auto slot = std::find_if(m.material_slots.begin(), m.material_slots.end(), [&](const MaterialSlot &s) { return s.slot == part; });
                    if (slot != m.material_slots.end()) chosen = slot->material;
                    const bool part_changed = asset_combo(("Part " + std::to_string(part)).c_str(), chosen, assets::AssetType::Material, "(default)");
                    mark("Inspector.Part" + std::to_string(part));
                    if (part_changed) {
                        report(session.assign_material(primary.id, assets::AssetId{chosen.id}, part), "Part material changed");
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
            }
        }
    }
    if (has_camera) {
        section(*this, camera, remover("Camera", [](EntityId id) { return commands::SetCamera{id, std::nullopt}; }));
        if (!details_search[0] && !details_only_modified) ImGui::TextDisabled("Looks along local -Z; must not be scaled.");
    }
    if (has_light) section(*this, light, remover("Light", [](EntityId id) { return commands::SetLight{id, std::nullopt}; }));
    if (has_environment) section(*this, environment, remover("Environment", [](EntityId id) { return commands::SetEnvironment{id, std::nullopt}; }));
    if (has_body) {
        section(*this, body, remover("Rigid Body", [](EntityId id) { return commands::SetRigidBody{id, std::nullopt}; }));
        if (primary.components.colliders.empty()) ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "No colliders: this body will not collide.");
        if (playing())
            if (const auto state = session.physics_bridge().body_state(primary.id)) {
                const auto &v = state->linear_velocity;
                ImGui::TextDisabled("Speed %.2f m/s, mass %.1f kg, %s", std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z), state->mass, state->awake ? "awake" : "asleep");
            }
    }
    for (std::size_t i = 0; i < colliders.size(); ++i)
        section(*this, colliders[i], [this, i] {
            std::vector<commands::Command> edits;
            for (const auto &record : details_targets) {
                auto list = record.components.colliders;
                list.erase(list.begin() + std::ptrdiff_t(i));
                edits.emplace_back(commands::SetColliders{record.id, std::move(list)});
            }
            details_apply("Remove Collider", std::move(edits));
        });
    for (const auto &issue : session.physics_bridge().diagnostics().issues)
        if (std::ranges::any_of(details_targets, [&](const EntityRecord &r) { return r.id == issue.entity; })) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.35f, 1));
            ImGui::TextWrapped("Physics: %s", issue.message.c_str());
            ImGui::PopStyleColor();
        }
    behavior_sections();
    // ---- Adding ----
    ImGui::Spacing();
    if (tip_button("Add Component", "Add a component to everything selected")) ImGui::OpenPopup("Add Component");
    mark("Details.AddComponent");
    if (ImGui::BeginPopup("Add Component")) {
        const auto any_missing = [&](auto has) { return !all_have(has); };
        if (ImGui::MenuItem("Mesh Renderer", nullptr, false, any_missing([](const EntityRecord &r) { return r.components.mesh.has_value(); })))
            details_add("Mesh Renderer", [](const EntityRecord &r) -> std::optional<commands::Command> { if (r.components.mesh) return std::nullopt; return commands::SetMesh{r.id, MeshComponent{}}; });
        if (ImGui::MenuItem("Camera", nullptr, false, any_missing([](const EntityRecord &r) { return r.components.camera.has_value(); })))
            details_add("Camera", [](const EntityRecord &r) -> std::optional<commands::Command> { if (r.components.camera) return std::nullopt; return commands::SetCamera{r.id, CameraComponent{}}; });
        const bool add_light = ImGui::MenuItem("Light", nullptr, false, any_missing([](const EntityRecord &r) { return r.components.light.has_value(); }));
        mark("Details.Add.Light");
        if (add_light)
            details_add("Light", [](const EntityRecord &r) -> std::optional<commands::Command> { if (r.components.light) return std::nullopt; return commands::SetLight{r.id, LightComponent{}}; });
        if (ImGui::MenuItem("Rigid Body", nullptr, false, any_missing([](const EntityRecord &r) { return r.components.body.has_value(); })))
            details_add("Rigid Body", [](const EntityRecord &r) -> std::optional<commands::Command> { if (r.components.body) return std::nullopt; return commands::SetRigidBody{r.id, RigidBodyComponent{}}; });
        if (ImGui::MenuItem("Collider (fitted to mesh)", nullptr, false, any_missing([](const EntityRecord &r) { return r.components.colliders.size() >= max_colliders_per_entity; }) || !many))
            details_add("Collider", [this](const EntityRecord &r) -> std::optional<commands::Command> {
                if (r.components.colliders.size() >= max_colliders_per_entity) return std::nullopt;
                auto list = r.components.colliders;
                list.push_back(fitted_collider(r.components.mesh));
                return commands::SetColliders{r.id, std::move(list)};
            });
        if (ImGui::BeginMenu("Behavior")) {
            for (const auto &behavior : gameplay::behavior_templates()) {
                const auto present = [&](const EntityRecord &r) { return std::ranges::any_of(r.components.behaviors, [&](const BehaviorComponent &b) { return b.type == behavior.type; }); };
                if (ImGui::MenuItem(behavior.type.c_str(), nullptr, false, !all_have(present)))
                    details_add("Behavior", [&](const EntityRecord &r) -> std::optional<commands::Command> {
                        if (present(r) || r.components.behaviors.size() >= max_behaviors_per_entity) return std::nullopt;
                        auto list = r.components.behaviors;
                        list.push_back(behavior);
                        return commands::SetBehaviors{r.id, std::move(list)};
                    });
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    finish_details_edit();
    ImGui::End();
}
void EditorUI::finish_details_edit() {
    // The drag, or the typing, is over: everything since it began is one undo step.
    if (details_transaction && !ImGui::IsAnyItemActive()) {
        (void)session.end_transaction();
        details_transaction = false;
        details_error.clear();
    }
}
// Named numbers read by runtime systems when Play starts; the scene gives them no meaning, so they edit generically.
void EditorUI::behavior_sections() {
    const auto &primary = details_targets.back();
    for (const auto &behavior : primary.components.behaviors) {
        const auto of = [&](const EntityRecord &r) -> const BehaviorComponent * {
            const auto at = std::ranges::find(r.components.behaviors, behavior.type, &BehaviorComponent::type);
            return at == r.components.behaviors.end() ? nullptr : &*at;
        };
        if (!std::ranges::all_of(details_targets, [&](const EntityRecord &r) { return of(r) != nullptr; })) continue;
        const std::string_view search(details_search.data());
        const auto wanted = [&](const std::string &key) { return has_text(key, search) || has_text(behavior.type, search) || has_text("behavior", search); };
        const auto standard = [&](const std::string &key) -> std::optional<double> {
            for (const auto &item : gameplay::behavior_templates())
                if (item.type == behavior.type)
                    for (const auto &[name_of_key, value] : item.values) if (name_of_key == key) return value;
            return std::nullopt;
        };
        const auto modified = [&](const std::string &key, double value) { const auto d = standard(key); return d && *d != value; };
        if (!std::ranges::any_of(behavior.values, [&](const auto &entry) { return wanted(entry.first) && (!details_only_modified || modified(entry.first, entry.second)); })) continue;
        ImGui::PushID(behavior.type.c_str());
        const auto scope = "Behavior: " + behavior.type;
        const bool open = remembered_header(*this, scope, settings_key("open", scope), true, true);
        // Sets one key on every target, or takes the behavior off them all.
        const auto set_key = [&](const std::string &edit_name, const std::string &key, double value) {
            std::vector<commands::Command> edits;
            for (const auto &record : details_targets) {
                auto list = record.components.behaviors;
                for (auto &item : list)
                    if (item.type == behavior.type)
                        for (auto &[name_of_key, number] : item.values) if (name_of_key == key) number = value;
                if (list != record.components.behaviors) edits.emplace_back(commands::SetBehaviors{record.id, std::move(list)});
            }
            details_apply(edit_name, std::move(edits));
        };
        const float width = ImGui::CalcTextSize("Remove").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - width);
        if (tip_small_button("Remove", "Take this off everything selected")) {
            std::vector<commands::Command> edits;
            for (const auto &record : details_targets) {
                auto list = record.components.behaviors;
                std::erase_if(list, [&](const BehaviorComponent &b) { return b.type == behavior.type; });
                edits.emplace_back(commands::SetBehaviors{record.id, std::move(list)});
            }
            details_apply("Remove Behavior", std::move(edits));
        }
        if (open && begin_rows("##rows")) {
            for (const auto &[key, number] : behavior.values) {
                if (!wanted(key) || (details_only_modified && !modified(key, number))) continue;
                ImGui::PushID(key.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::Indent(ImGui::GetFrameHeight() * 0.7f);
                ImGui::TextUnformatted(key.c_str());
                ImGui::Unindent(ImGui::GetFrameHeight() * 0.7f);
                ImGui::OpenPopupOnItemClick("##property", ImGuiPopupFlags_MouseButtonRight);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                bool mixed = false;
                for (const auto &record : details_targets) {
                    const auto *other = of(record);
                    for (const auto &[other_key, other_value] : other->values) if (other_key == key && other_value != number) mixed = true;
                }
                double value = number;
                if (scrub("##value", value, 0.05, -1e9, 1e9, "%.3f", mixed)) set_key("Change " + key, key, value);
                mark("Details." + scope + "." + key);
                ImGui::OpenPopupOnItemClick("##property", ImGuiPopupFlags_MouseButtonRight);
                if (ImGui::BeginPopup("##property")) {
                    if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText(property_to_text(PropertyValue(number)).c_str());
                    const auto *clipboard = ImGui::GetClipboardText();
                    const auto pasted = property_from_text(clipboard ? clipboard : "");
                    const bool fits = pasted && std::holds_alternative<double>(*pasted);
                    if (ImGui::MenuItem("Paste", nullptr, false, fits)) set_key("Paste " + key, key, std::get<double>(*pasted));
                    if (ImGui::MenuItem("Reset to Default", nullptr, false, modified(key, number))) set_key("Reset " + key, key, *standard(key));
                    ImGui::EndPopup();
                }
                ImGui::TableNextColumn();
                if (modified(key, number)) {
                    if (ImGui::ArrowButton("##reset", ImGuiDir_Left)) set_key("Reset " + key, key, *standard(key));
                    ImGui::SetItemTooltip("Reset to default");
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
}
// ---- Field helpers the Lighting panel also uses: drags commit on release; combos and checkboxes at once ----
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
bool EditorUI::asset_combo(const char *label, AssetRef &ref, assets::AssetType type, const char *none) {
    bool changed = false;
    if (ImGui::BeginCombo(label, ref.empty() ? none : ref.path.c_str())) {
        if (ImGui::Selectable(none, ref.empty())) { ref = {}; changed = true; }
        for (const auto &meta : session.assets()) {
            if (meta.type != type) continue;
            if (ImGui::Selectable(meta.path.c_str(), ref.id == meta.id.value)) { ref = {meta.id.value, meta.path}; changed = true; }
        }
        ImGui::EndCombo();
    }
    return changed;
}
} // namespace engine::editor
