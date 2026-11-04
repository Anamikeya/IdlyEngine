#include "EditorUI.hpp"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace engine::editor {
void EditorUI::import_file(const std::filesystem::path &file) {
    import_started[file.string()] = std::chrono::steady_clock::now();
    const auto task_name = "Importing " + file.filename().string();
    import_tasks[tasks.begin(task_name, true)] = session.begin_import(file);
    log(task_name + "...");
}
void EditorUI::finish_imports() {
    for (const auto &finished : session.poll_imports()) {
        const auto started = import_started.find(finished.source.string());
        const auto took = started == import_started.end() ? 0 : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started->second).count();
        if (started != import_started.end()) import_started.erase(started);
        for (auto task = import_tasks.begin(); task != import_tasks.end();) {
            if (task->second == finished.id) { tasks.finish(task->first); task = import_tasks.erase(task); }
            else ++task;
        }
        if (finished.cancelled) continue; // Said so when it was cancelled.
        if (!finished.result) { log_error("Import failed: " + finished.result.error().message); continue; }
        const auto &asset = finished.result.value();
        const auto link = LogLink::asset(asset.id.value, asset.path);
        notify(Severity::Success, "Imported as a " + std::string(assets::to_string(asset.type)) + " in " + std::to_string(took) + " ms", link);
        import_times.emplace_back(asset.path, static_cast<long long>(took));
        for (const auto &warning : asset.warnings) notify(Severity::Warning, warning, link);
        selected_asset = asset.id;
    }
}
namespace {
ImU32 type_color(assets::AssetType type) {
    switch (type) {
    case assets::AssetType::Mesh: return IM_COL32(70, 135, 225, 255);
    case assets::AssetType::Texture: return IM_COL32(85, 180, 95, 255);
    case assets::AssetType::Material: return IM_COL32(230, 150, 60, 255);
    case assets::AssetType::Environment: return IM_COL32(165, 105, 215, 255);
    case assets::AssetType::Effect: return IM_COL32(55, 185, 200, 255);
    default: return IM_COL32(120, 120, 120, 255);
    }
}
const char *type_label(assets::AssetType type) {
    switch (type) {
    case assets::AssetType::Mesh: return "MODEL";
    case assets::AssetType::Texture: return "IMAGE";
    case assets::AssetType::Material: return "MATERIAL";
    case assets::AssetType::Environment: return "HDR";
    case assets::AssetType::Effect: return "EFFECT";
    default: return "?";
    }
}
assets::AssetType type_from_name(std::string_view name) {
    if (name == "mesh") return assets::AssetType::Mesh;
    if (name == "texture") return assets::AssetType::Texture;
    if (name == "material") return assets::AssetType::Material;
    if (name == "environment") return assets::AssetType::Environment;
    if (name == "effect") return assets::AssetType::Effect;
    return assets::AssetType::Unknown;
}
bool has_text(std::string_view text, std::string_view wanted) {
    if (wanted.empty()) return true;
    return !std::ranges::search(text, wanted, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }).empty();
}
std::string file_name_of(const std::string &path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
std::string readable_size(std::uint64_t bytes) {
    char text[32];
    if (bytes >= 1024 * 1024) std::snprintf(text, sizeof text, "%.1f MB", double(bytes) / (1024 * 1024));
    else if (bytes >= 1024) std::snprintf(text, sizeof text, "%.1f KB", double(bytes) / 1024);
    else std::snprintf(text, sizeof text, "%llu bytes", static_cast<unsigned long long>(bytes));
    return text;
}
}
// ---- Thumbnails. Decoded on a worker; the GPU work happens one a frame in the renderer ----
void EditorUI::update_thumbnails() {
    // The browser asks for a few at a time, so "no jobs" for a moment does not mean it has finished.
    thumbnails_idle = session.thumbnail_jobs() ? 0 : thumbnails_idle + ImGui::GetIO().DeltaTime;
    if (session.thumbnail_jobs() && !thumbnail_task) { thumbnail_task = tasks.begin("Making thumbnails"); thumbnails_made = 0; }
    if (thumbnail_task && thumbnails_idle > 0.5) { tasks.finish(thumbnail_task); thumbnail_task = 0; }
    for (auto &data : session.poll_thumbnail_data()) {
        if (thumbnail_task) tasks.set_name(thumbnail_task, "Making thumbnails: " + std::to_string(++thumbnails_made) + " done");
        auto &state = thumbnails[data.id];
        state.vertices = data.vertices; state.triangles = data.triangles;
        state.width = data.source_width; state.height = data.source_height;
        if (data.failed) { state.failed = true; continue; }
        if (data.type == assets::AssetType::Mesh) renderer.request_thumbnail(data.id.value, data.mesh, renderer::builtin_lit);
        else renderer.set_thumbnail_image(data.id.value, data.pixels, data.width, data.height);
    }
    if (thumbnail_cooldown > 0) --thumbnail_cooldown;
}
void EditorUI::want_thumbnail(const assets::AssetMetadata &meta) {
    auto &state = thumbnails[meta.id];
    if (state.failed) return;
    if (meta.type == assets::AssetType::Material) {
        // Reading a material's maps happens here, so these are spaced out. Editing one gives it a new handle, and a new picture.
        if (thumbnail_cooldown > 0) return;
        const auto handle = session.material_handle(meta.id);
        if (!handle.index) { state.failed = true; return; }
        if (handle == state.material) return;
        state.material = handle;
        renderer.request_thumbnail(meta.id.value, renderer::builtin_sphere, handle);
        thumbnail_cooldown = 6;
    } else if ((meta.type == assets::AssetType::Mesh || meta.type == assets::AssetType::Texture) && !state.requested && session.thumbnail_jobs() < 2) {
        state.requested = true;
        session.request_thumbnail_data(meta.id);
    }
}
// Its picture when there is one, otherwise a tile in its type's color. Always a strip of that color along the bottom.
void EditorUI::draw_asset_picture(const assets::AssetMetadata &meta, ImVec2 at, float size) {
    auto *draw = ImGui::GetWindowDrawList();
    const ImVec2 end{at.x + size, at.y + size};
    const auto color = type_color(meta.type);
    const auto picture = renderer.thumbnail(meta.id.value);
    if (picture.texture) {
        if (picture.flipped) draw->AddImage(ImTextureRef(picture.texture), at, end, {0, 1}, {1, 0});
        else draw->AddImage(ImTextureRef(picture.texture), at, end);
    } else {
        draw->AddRectFilled(at, end, (color & 0x00FFFFFF) | 0x50000000, px(2));
        if (size >= px(40)) {
            const char *label = type_label(meta.type);
            const auto extent = ImGui::CalcTextSize(label);
            draw->AddText({at.x + (size - extent.x) / 2, at.y + (size - extent.y) / 2}, IM_COL32(235, 235, 235, 255), label);
        }
    }
    draw->AddRectFilled({at.x, end.y - std::max(px(3), size * 0.04f)}, end, color);
}
void EditorUI::asset_tooltip(const assets::AssetMetadata &meta) {
    if (!ImGui::BeginItemTooltip()) return;
    ImGui::TextUnformatted(meta.path.c_str());
    ImGui::TextDisabled("%s, %s, %s", assets::to_string(meta.type), readable_size(meta.source_bytes).c_str(), assets::to_string(session.asset_state(meta.id)));
    const auto found = thumbnails.find(meta.id);
    if (found != thumbnails.end()) {
        if (found->second.width) ImGui::TextDisabled("%u x %u pixels", found->second.width, found->second.height);
        if (found->second.triangles) ImGui::TextDisabled("%zu triangles, %zu vertices", found->second.triangles, found->second.vertices);
    }
    ImGui::EndTooltip();
}
bool EditorUI::asset_is_favorite(assets::AssetId id) const { return settings.flag("asset.favorite." + assets::to_string(id), false); }
void EditorUI::asset_menu(const assets::AssetMetadata &meta) {
    if (!ImGui::BeginPopupContextItem()) return;
    selected_asset = meta.id;
    for (const auto *id : {"Asset.Assign", "Asset.AddToScene", "Asset.Edit", "Asset.Reload", "Asset.Rename", "Asset.ToggleFavorite"}) command_item(id);
    ImGui::EndPopup();
}
void EditorUI::asset_item_input(const assets::AssetMetadata &meta) {
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) selected_asset = meta.id;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (meta.type == assets::AssetType::Mesh) report(session.place_asset(meta.id), "Model added to the scene");
        else if (meta.type == assets::AssetType::Material || meta.type == assets::AssetType::Effect) registry.run("Asset.Edit");
    }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(asset_payload, &meta.id.value, sizeof meta.id.value);
        ImGui::Text("%s", file_name_of(meta.path).c_str());
        ImGui::TextDisabled(meta.type == assets::AssetType::Mesh || meta.type == assets::AssetType::Effect ? "Drop on the view to place it, or on a field" : "Drop on a matching field");
        ImGui::EndDragDropSource();
    }
    asset_tooltip(meta);
    asset_menu(meta);
    if (asset_scroll_to == meta.id) { ImGui::SetScrollHereY(); asset_scroll_to = {}; }
    mark("Asset." + meta.path);
}
void EditorUI::reveal_asset(assets::AssetId id) {
    show_assets = true;
    selected_asset = id;
    asset_scroll_to = id;
    asset_source = AssetSource::All;
    asset_search[0] = 0;
    for (auto &chip : asset_chips) chip = false;
    if (asset_drawer_open) ImGui::SetWindowFocus("Asset Drawer");
    else ImGui::SetWindowFocus("Asset Browser");
}
// The field of an asset reference in the Details panel: a picture, a searchable list, and buttons to use or find.
bool EditorUI::asset_field(std::string_view type_name, AssetRef &ref, bool mixed) {
    bool changed = false;
    const auto type = type_from_name(type_name);
    const auto catalogue = session.assets();
    const auto current = std::ranges::find_if(catalogue, [&](const assets::AssetMetadata &meta) { return meta.id.value == ref.id; });
    const float picture = ImGui::GetFrameHeight();
    if (current != catalogue.end() && !mixed) {
        want_thumbnail(*current);
        draw_asset_picture(*current, ImGui::GetCursorScreenPos(), picture);
        ImGui::Dummy({picture, picture});
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    }
    const float small = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - 2 * (small + ImGui::GetStyle().ItemInnerSpacing.x), px(40)));
    const std::string preview = mixed ? "Multiple Values" : ref.empty() ? "(none)" : file_name_of(ref.path);
    if (ImGui::BeginCombo("##pick", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::IsWindowAppearing()) { asset_field_search[0] = 0; ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##search", "Search", asset_field_search.data(), asset_field_search.size());
        mark("Field.Search");
        if (ImGui::Selectable("(none)", ref.empty() && !mixed)) { ref = {}; changed = true; }
        const std::string_view wanted(asset_field_search.data());
        for (const auto &meta : catalogue) {
            if (meta.type != type || !has_text(meta.path, wanted)) continue;
            if (ImGui::Selectable(meta.path.c_str(), !mixed && ref.id == meta.id.value)) { ref = {meta.id.value, meta.path}; changed = true; }
            mark("Field.Item." + meta.path);
        }
        ImGui::EndCombo();
    }
    mark("Field.Combo");
    if (ImGui::BeginDragDropTarget()) {
        if (const auto *payload = ImGui::AcceptDragDropPayload(asset_payload)) {
            assets::AssetId id;
            std::memcpy(&id.value, payload->Data, sizeof id.value);
            const auto dropped = std::ranges::find_if(catalogue, [&](const assets::AssetMetadata &meta) { return meta.id == id; });
            if (dropped != catalogue.end() && dropped->type == type) { ref = {dropped->id.value, dropped->path}; changed = true; }
            else log_error(std::string("That field takes a ") + std::string(type_name));
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    const auto chosen = std::ranges::find_if(catalogue, [&](const assets::AssetMetadata &meta) { return meta.id == selected_asset; });
    const bool usable = chosen != catalogue.end() && chosen->type == type;
    ImGui::BeginDisabled(!usable);
    if (ImGui::ArrowButton("##use", ImGuiDir_Left) && usable) { ref = {chosen->id.value, chosen->path}; changed = true; }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Use the asset selected in the Asset Browser");
    mark("Field.Use");
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(ref.empty() || mixed);
    if (ImGui::ArrowButton("##browse", ImGuiDir_Right)) reveal_asset(assets::AssetId{ref.id});
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Find this asset in the Asset Browser");
    mark("Field.Browse");
    return changed;
}
void EditorUI::asset_folder_tree(const std::map<std::string, std::vector<std::string>> &tree, const std::string &folder) {
    const auto kids = tree.find(folder);
    if (kids == tree.end()) return;
    for (const auto &child : kids->second) {
        const auto path = folder.empty() ? child : folder + "/" + child;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
        if (!tree.contains(path)) flags |= ImGuiTreeNodeFlags_Leaf;
        if (asset_source == AssetSource::Folder && asset_folder == path) flags |= ImGuiTreeNodeFlags_Selected;
        const bool open = ImGui::TreeNodeEx(path.c_str(), flags, "%s", child.c_str());
        mark("Source." + path);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) { asset_source = AssetSource::Folder; asset_folder = path; }
        if (open) {
            asset_folder_tree(tree, path);
            ImGui::TreePop();
        }
    }
}
void EditorUI::asset_browser_contents() {
    const auto catalogue = session.assets();
    const auto chosen = std::find_if(catalogue.begin(), catalogue.end(), [&](const assets::AssetMetadata &a) { return a.id == selected_asset; });
    const bool have = chosen != catalogue.end();
    const bool have_material = have && chosen->type == assets::AssetType::Material;
    // ---- Tools. They wrap to fit: this panel can be any width, and a drawer is wider than a dock ----
    const float right_edge = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    const auto flow = [&](float next_width) {
        ImGui::SameLine();
        if (ImGui::GetCursorScreenPos().x + next_width > right_edge) ImGui::NewLine();
    };
    const auto button_width = [](const char *label) { return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2; };
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - button_width("Import") - button_width("Rescan") - ImGui::GetStyle().ItemSpacing.x * 2, px(80)));
    const bool entered = ImGui::InputTextWithHint("##import", "File to import, or drop one on the window", import_path.data(), import_path.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    mark("Asset.ImportPath");
    ImGui::SameLine();
    if (tip_button("Import", "Bring this file into the project. It runs in the background.") || entered) {
        if (import_path[0]) import_file(utf8_path(import_path.data()));
        else log_error("Enter a file to import, or drop one on the window");
    }
    ImGui::SameLine();
    if (tip_button("Rescan", "Look through the asset folder again for changed files")) report(session.rescan_assets(), "Asset folder rescanned");
    ImGui::SetNextItemWidth(px(110)); ImGui::InputTextWithHint("##material-name", "Name", new_material_name.data(), new_material_name.size());
    mark("Asset.NewName");
    flow(button_width("New material"));
    if (tip_button("New material", "Make a material with the name to the left")) {
        const auto created = session.create_material(new_material_name.data());
        if (report(created, "Material created")) { selected_asset = created.value(); asset_scroll_to = selected_asset; }
    }
    mark("Asset.NewMaterial");
    flow(button_width("New effect"));
    if (tip_button("New effect", "Make a particle effect with the name to the left")) {
        const auto created = session.create_effect(new_material_name.data(), starter_effect());
        if (report(created, "Effect created")) { selected_asset = created.value(); asset_scroll_to = selected_asset; }
    }
    mark("Asset.NewEffect");
    flow(button_width("Duplicate"));
    ImGui::BeginDisabled(!have_material);
    if (ImGui::Button("Duplicate")) {
        const auto copied = session.duplicate_material(selected_asset);
        if (report(copied, "Material duplicated")) selected_asset = copied.value();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Copy the selected material");
    mark("Asset.DuplicateMaterial");
    flow(button_width("Assign to selection"));
    command_button("Asset.Assign", "Assign to selection");
    mark("Asset.Assign");
    flow(button_width("Add to scene"));
    command_button("Asset.AddToScene", "Add to scene");
    mark("Asset.AddToScene");
    flow(button_width("Reload"));
    command_button("Asset.Reload", "Reload");
    ImGui::SetNextItemWidth(px(140));
    ImGui::InputTextWithHint("##asset-search", "Search assets", asset_search.data(), asset_search.size());
    mark("Asset.Search");
    constexpr const char *chip_names[] = {"Model", "Image", "Material", "HDR", "Effect"};
    constexpr assets::AssetType chip_types[] = {assets::AssetType::Mesh, assets::AssetType::Texture, assets::AssetType::Material, assets::AssetType::Environment, assets::AssetType::Effect};
    for (std::size_t i = 0; i < std::size(chip_names); ++i) {
        flow(button_width(chip_names[i]));
        if (asset_chips[i]) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(type_color(chip_types[i])));
        if (tip_small_button(chip_names[i], "Show only assets of the kinds chosen here")) asset_chips[i] = !asset_chips[i];
        if (asset_chips[i]) ImGui::PopStyleColor();
        mark(std::string("Asset.Chip.") + chip_names[i]);
    }
    flow(button_width("Grid"));
    if (ImGui::SmallButton(asset_list_view ? "Grid" : "List")) asset_list_view = !asset_list_view;
    ImGui::SetItemTooltip("Switch between thumbnails and a list");
    mark("Asset.ViewToggle");
    if (session.imports_pending()) { ImGui::SameLine(); ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "Importing %zu...", session.imports_pending()); }
    // ---- Sources ----
    ImGui::BeginChild("##sources", {px(140), 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    const auto source = [this](const char *label, AssetSource kind) {
        if (ImGui::Selectable(label, asset_source == kind)) asset_source = kind;
        mark(std::string("Source.") + label);
    };
    source("All", AssetSource::All);
    source("Favorites", AssetSource::Favorites);
    std::map<std::string, std::vector<std::string>> tree;
    for (const auto &meta : catalogue) {
        std::string parent;
        std::string_view rest(meta.path);
        for (auto slash = rest.find('/'); slash != std::string_view::npos; slash = rest.find('/')) {
            const std::string part(rest.substr(0, slash));
            auto &siblings = tree[parent];
            if (std::ranges::find(siblings, part) == siblings.end()) siblings.push_back(part);
            parent = parent.empty() ? part : parent + "/" + part;
            rest.remove_prefix(slash + 1);
        }
    }
    asset_folder_tree(tree, "");
    ImGui::Separator();
    source("Built-in", AssetSource::Builtin);
    source("Scenes", AssetSource::Scenes);
    ImGui::EndChild();
    ImGui::SameLine();
    // ---- What is in it ----
    ImGui::BeginChild("##items", {0, 0}, ImGuiChildFlags_Borders);
    if (asset_source == AssetSource::Builtin) {
        ImGui::TextDisabled("Built-in meshes");
        for (const auto *id : {"Create.Cube", "Create.Sphere", "Create.Plane"}) { command_button(id); ImGui::SameLine(); }
        ImGui::NewLine();
    } else if (asset_source == AssetSource::Scenes) {
        ImGui::TextDisabled("Scene files beside the current scene. Click one to choose it, then Load.");
        auto directory = std::filesystem::path(scene_path.data()).parent_path();
        if (directory.empty()) directory = ".";
        std::error_code error;
        for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
            if (it->path().extension() != ".scene") continue;
            const auto path = it->path().string();
            if (ImGui::Selectable(it->path().filename().string().c_str())) std::snprintf(scene_path.data(), scene_path.size(), "%s", path.c_str());
        }
        if (error) ImGui::TextWrapped("Cannot browse folder: %s", error.message().c_str());
    } else {
        const std::string_view wanted(asset_search.data());
        const bool any_chip = std::ranges::any_of(asset_chips, [](bool on) { return on; });
        std::vector<const assets::AssetMetadata *> shown;
        for (const auto &meta : catalogue) {
            if (!has_text(meta.path, wanted)) continue;
            if (any_chip) {
                bool fits = false;
                for (std::size_t i = 0; i < std::size(chip_types); ++i) fits |= asset_chips[i] && chip_types[i] == meta.type;
                if (!fits) continue;
            }
            if (asset_source == AssetSource::Favorites && !asset_is_favorite(meta.id)) continue;
            if (asset_source == AssetSource::Folder && !meta.path.starts_with(asset_folder + "/")) continue;
            shown.push_back(&meta);
        }
        if (catalogue.empty()) ImGui::TextDisabled("No assets yet. Assets in %s (.glb .obj .png .jpg .tga .ppm .hdr .material), or drop files on the window.", session.asset_folder().generic_string().c_str());
        else if (shown.empty()) ImGui::TextDisabled("Nothing here matches.");
        if (asset_list_view) {
            const float picture = ImGui::GetTextLineHeight() + px(2);
            for (const auto *meta : shown) {
                ImGui::PushID(int(std::distance(catalogue.data(), meta)));
                const auto state = session.asset_state(meta->id);
                const bool failed = state == assets::AssetState::Failed || state == assets::AssetState::Missing;
                const auto at = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable(("##row"), meta->id == selected_asset, ImGuiSelectableFlags_AllowOverlap)) selected_asset = meta->id;
                asset_item_input(*meta);
                if (ImGui::IsItemVisible()) { want_thumbnail(*meta); draw_asset_picture(*meta, at, picture); }
                if (failed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.35f, 1));
                ImGui::SameLine(picture + px(12));
                ImGui::Text("%s%s", meta->path.c_str(), asset_is_favorite(meta->id) ? "  *" : "");
                ImGui::SameLine();
                ImGui::TextDisabled("%s", assets::to_string(state));
                if (failed) ImGui::PopStyleColor();
                ImGui::PopID();
            }
        } else {
            const float tile = px(82), gap = ImGui::GetStyle().ItemSpacing.x;
            const float line = ImGui::GetTextLineHeight();
            float used = 0;
            const float room = ImGui::GetContentRegionAvail().x;
            for (const auto *meta : shown) {
                ImGui::PushID(int(std::distance(catalogue.data(), meta)));
                if (used > 0 && used + tile <= room) ImGui::SameLine();
                else used = 0;
                used += tile + gap;
                const auto at = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton("##tile", {tile, tile + line + px(4)});
                const bool visible = ImGui::IsItemVisible();
                const bool hovered = ImGui::IsItemHovered();
                asset_item_input(*meta);
                if (visible) {
                    want_thumbnail(*meta);
                    auto *draw = ImGui::GetWindowDrawList();
                    const ImVec2 end{at.x + tile, at.y + tile + line + px(4)};
                    if (meta->id == selected_asset) draw->AddRectFilled(at, end, IM_COL32(50, 95, 150, 255), px(3));
                    else if (hovered) draw->AddRectFilled(at, end, IM_COL32(60, 65, 75, 255), px(3));
                    draw_asset_picture(*meta, {at.x + px(3), at.y + px(3)}, tile - px(6));
                    const auto state = session.asset_state(meta->id);
                    const bool failed = state == assets::AssetState::Failed || state == assets::AssetState::Missing;
                    const auto label = file_name_of(meta->path);
                    draw->PushClipRect(at, end, true);
                    draw->AddText({at.x + px(3), at.y + tile}, failed ? IM_COL32(245, 115, 90, 255) : IM_COL32(225, 225, 225, 255), label.c_str());
                    draw->PopClipRect();
                    if (asset_is_favorite(meta->id)) draw->AddCircleFilled({end.x - px(8), at.y + px(8)}, px(3.5f), IM_COL32(245, 200, 70, 255));
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
    rename_asset_dialog();
}
void EditorUI::rename_asset_dialog() {
    if (std::exchange(rename_asset_prompt, false)) ImGui::OpenPopup("Rename asset");
    if (!ImGui::BeginPopupModal("Rename asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("New path inside the asset folder. Slashes move it to another folder.");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(px(420));
    const bool entered = ImGui::InputText("##path", rename_asset_path.data(), rename_asset_path.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    mark("Asset.RenamePath");
    ImGui::BeginDisabled(!rename_asset_path[0]);
    if ((ImGui::Button("Rename") || entered) && rename_asset_path[0]) {
        report(session.rename_asset(selected_asset, rename_asset_path.data()), "Asset renamed");
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
void EditorUI::assets() {
    update_thumbnails();
    if (show_assets) {
        if (begin_panel("Asset Browser", "Asset Browser", context::asset_browser, &show_assets) && !asset_drawer_open) asset_browser_contents();
        else if (asset_drawer_open) ImGui::TextDisabled("Showing in the drawer.");
        ImGui::End();
    }
    // ---- The drawer: the same browser, sliding up over the editor. Gets out of the way of a drag ----
    if (asset_drawer_open && ImGui::GetDragDropPayload()) asset_drawer_open = false;
    const float wanted = asset_drawer_open ? 1.f : 0.f;
    const auto &io = ImGui::GetIO();
    asset_drawer_amount += std::clamp(wanted - asset_drawer_amount, -io.DeltaTime * 7, io.DeltaTime * 7);
    if (asset_drawer_amount <= 0) return;
    const auto *main = ImGui::GetMainViewport();
    const float height = main->WorkSize.y * 0.42f * asset_drawer_amount;
    ImGui::SetNextWindowPos({main->WorkPos.x, main->WorkPos.y + main->WorkSize.y - height});
    ImGui::SetNextWindowSize({main->WorkSize.x, height});
    if (std::exchange(asset_drawer_focus, false)) ImGui::SetNextWindowFocus();
    const bool drawn = begin_panel("Asset Drawer", "Asset Drawer", context::asset_browser, nullptr,
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    if (drawn && asset_drawer_open) asset_browser_contents();
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::End();
}
} // namespace engine::editor
