#include "EditorUI.hpp"
#include <algorithm>
#include <chrono>

namespace engine::editor {
void EditorUI::import_file(const std::filesystem::path &file) {
    import_started[file.string()] = std::chrono::steady_clock::now();
    session.begin_import(file);
    log("Importing " + file.filename().string() + "...");
}
void EditorUI::finish_imports() {
    for (const auto &finished : session.poll_imports()) {
        const auto started = import_started.find(finished.source.string());
        const auto took = started == import_started.end() ? 0 : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started->second).count();
        if (started != import_started.end()) import_started.erase(started);
        if (!finished.result) { log("Import failed: " + finished.result.error().message); continue; }
        const auto &asset = finished.result.value();
        log("Imported " + asset.path + " as a " + assets::to_string(asset.type) + " in " + std::to_string(took) + " ms");
        import_times.emplace_back(asset.path, static_cast<long long>(took));
        for (const auto &warning : asset.warnings) log("Warning: " + asset.path + ": " + warning);
        selected_asset = asset.id;
    }
}
void EditorUI::assets() {
    begin_panel("Asset Browser", "Asset Browser", context::asset_browser);
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - px(150), px(80)));
    const bool entered = ImGui::InputText("##import", import_path.data(), import_path.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    mark("Asset.ImportPath");
    ImGui::SameLine();
    if (ImGui::Button("Import") || entered) {
        if (import_path[0]) import_file(utf8_path(import_path.data()));
        else log("Error: Enter a file to import, or drop one on the window");
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) report(session.rescan_assets(), "Asset folder rescanned");
    ImGui::TextDisabled("Assets in %s (.glb .obj .png .jpg .tga .ppm .hdr .material; or drop files on the window)", session.asset_folder().generic_string().c_str());
    if (session.imports_pending()) ImGui::TextColored({0.95f, 0.75f, 0.3f, 1}, "Importing %zu file(s) in the background...", session.imports_pending());
    ImGui::SetNextItemWidth(px(160)); ImGui::InputText("##material-name", new_material_name.data(), new_material_name.size());
    ImGui::SameLine();
    if (ImGui::Button("New material")) {
        const auto created = session.create_material(new_material_name.data());
        if (report(created, "Material created")) selected_asset = created.value();
    }
    mark("Asset.NewMaterial");
    const auto catalogue = session.assets();
    const auto chosen = std::find_if(catalogue.begin(), catalogue.end(), [&](const assets::AssetMetadata &a) { return a.id == selected_asset; });
    const bool have = chosen != catalogue.end();
    const bool have_material = have && chosen->type == assets::AssetType::Material;
    ImGui::SameLine(); ImGui::BeginDisabled(!have_material);
    if (ImGui::Button("Duplicate material")) {
        const auto copied = session.duplicate_material(selected_asset);
        if (report(copied, "Material duplicated")) selected_asset = copied.value();
    }
    ImGui::EndDisabled();
    mark("Asset.DuplicateMaterial");
    ImGui::BeginDisabled(!have || !session.selection() || chosen->type == assets::AssetType::Environment);
    if (ImGui::Button("Assign to selection"))
        report(have_material ? session.assign_material(session.selection(), selected_asset) : session.assign_asset(session.selection(), selected_asset), "Asset assigned to the selected entity");
    ImGui::EndDisabled();
    mark("Asset.Assign");
    ImGui::SameLine(); ImGui::BeginDisabled(!have || chosen->type != assets::AssetType::Mesh);
    if (ImGui::Button("Add to scene")) report(session.place_asset(selected_asset), "Model added to the scene");
    ImGui::EndDisabled();
    mark("Asset.AddToScene");
    ImGui::SameLine(); ImGui::BeginDisabled(!have);
    if (ImGui::Button("Reload")) report(session.reload_asset(selected_asset), "Asset re-imported from disk");
    ImGui::EndDisabled();
    for (const auto &meta : catalogue) {
        const auto state = session.asset_state(meta.id);
        const bool failed = state == assets::AssetState::Failed || state == assets::AssetState::Missing;
        if (failed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.35f, 1));
        const char *kind = meta.type == assets::AssetType::Mesh ? "[model]    " : meta.type == assets::AssetType::Material ? "[material] " : meta.type == assets::AssetType::Environment ? "[hdr]      " : "[texture]  ";
        const auto label = std::string(kind) + meta.path + "  (" + assets::to_string(state) + ")##" + assets::to_string(meta.id);
        if (ImGui::Selectable(label.c_str(), meta.id == selected_asset)) selected_asset = meta.id;
        mark("Asset." + meta.path);
        if (meta.type == assets::AssetType::Mesh && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(asset_payload, &meta.id.value, sizeof meta.id.value);
            ImGui::Text("Place '%s'", meta.path.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && meta.type == assets::AssetType::Mesh)
            report(session.place_asset(meta.id), "Model added to the scene");
        if (failed) ImGui::PopStyleColor();
    }
    if (catalogue.empty()) ImGui::TextDisabled("No assets yet.");
    ImGui::Separator();
    ImGui::TextDisabled("Built-in meshes");
    if (ImGui::Button("Cube")) spawn("Cube", {MeshComponent{BuiltinMesh::Cube}, {}, {}});
    ImGui::SameLine(); if (ImGui::Button("Sphere")) spawn("Sphere", {MeshComponent{BuiltinMesh::Sphere}, {}, {}});
    ImGui::SameLine(); if (ImGui::Button("Plane")) spawn("Plane", {MeshComponent{BuiltinMesh::Plane}, {}, {}}, {{0,0,0}, {}, {10,1,10}});
    ImGui::TextDisabled("Scene files in current scene folder");
    auto directory = std::filesystem::path(scene_path.data()).parent_path();
    if (directory.empty()) directory = ".";
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (it->path().extension() != ".scene") continue;
        const auto path = it->path().string();
        if (ImGui::Selectable(it->path().filename().string().c_str())) std::snprintf(scene_path.data(), scene_path.size(), "%s", path.c_str());
    }
    if (error) ImGui::TextWrapped("Cannot browse folder: %s", error.message().c_str());
    ImGui::End();
}
} // namespace engine::editor
