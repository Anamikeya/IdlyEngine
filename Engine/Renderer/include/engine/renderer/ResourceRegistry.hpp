#pragma once
#include <engine/assets/asset_types.h>
#include <engine/renderer/RenderSnapshot.hpp>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace engine::renderer {
// Maps (AssetId, revision) to the handles frames carry (contract C-5). CPU data only: a renderer uploads what a
// handle finds when it first draws it. An asset keeps its index; a new revision gets a new generation, so handles
// of the old revision find nothing. Owner thread only.
//
// Lifetime: an entry holds its CPU data until release_except() drops it. The handle stays well formed; it finds
// nothing until the host registers the asset again, which restores the data under the same generation.
class ResourceRegistry {
public:
    // Handles below this are built-in resources.
    static constexpr std::uint32_t first_index = 16;
private:
    template <class Handle, class Data> class Table {
        struct Entry {
            assets::AssetId id;
            std::uint32_t revision = 0, generation = 0;
            std::shared_ptr<const Data> data;
        };
        std::map<assets::AssetId, std::uint32_t> indices_;
        std::vector<Entry> entries_;

    public:
        Handle get(assets::AssetId id, std::uint32_t revision, std::shared_ptr<const Data> data) {
            const auto [it, added] = indices_.emplace(id, first_index + static_cast<std::uint32_t>(entries_.size()));
            if (added) entries_.push_back({id, 0, 0, nullptr});
            auto &entry = entries_[it->second - first_index];
            if (entry.revision != revision) {
                entry.revision = revision;
                ++entry.generation;
                entry.data = std::move(data);
            } else if (!entry.data) entry.data = std::move(data);
            return {it->second, entry.generation};
        }
        std::shared_ptr<const Data> find(Handle handle) const {
            if (handle.index < first_index || handle.index - first_index >= entries_.size()) return nullptr;
            const auto &entry = entries_[handle.index - first_index];
            return entry.generation == handle.generation ? entry.data : nullptr;
        }
        void release_except(const std::set<assets::AssetId> &keep) {
            for (auto &entry : entries_) if (!keep.contains(entry.id)) entry.data = nullptr;
        }
        std::size_t held() const {
            std::size_t count = 0;
            for (const auto &entry : entries_) count += entry.data != nullptr;
            return count;
        }
    };
    Table<MeshHandle, assets::MeshData> meshes_;
    Table<TextureHandle, assets::TextureData> textures_;
    Table<EnvironmentHandle, assets::EnvironmentData> environments_;
    // A material's description, by asset. It changes generation only when its values change.
    std::map<assets::AssetId, std::uint32_t> material_indices_;
    struct MaterialEntry { std::uint32_t generation = 0; MaterialDesc desc; };
    std::vector<MaterialEntry> materials_;

public:
    MeshHandle mesh(assets::AssetId id, std::uint32_t revision, std::shared_ptr<const assets::MeshData> data) {
        return meshes_.get(id, revision, std::move(data));
    }
    TextureHandle texture(assets::AssetId id, std::uint32_t revision, std::shared_ptr<const assets::TextureData> data) {
        return textures_.get(id, revision, std::move(data));
    }
    EnvironmentHandle environment(assets::AssetId id, std::uint32_t revision, std::shared_ptr<const assets::EnvironmentData> data) {
        return environments_.get(id, revision, std::move(data));
    }
    MaterialHandle material(assets::AssetId id, const MaterialDesc &desc) {
        const auto [it, added] = material_indices_.emplace(id, first_index + static_cast<std::uint32_t>(materials_.size()));
        if (added) materials_.push_back({0, desc});
        auto &entry = materials_[it->second - first_index];
        if (added || entry.desc != desc) { entry.desc = desc; ++entry.generation; }
        return {it->second, entry.generation};
    }
    // Null for a built-in, unknown or stale handle.
    std::shared_ptr<const assets::MeshData> find(MeshHandle handle) const { return meshes_.find(handle); }
    std::shared_ptr<const assets::TextureData> find(TextureHandle handle) const { return textures_.find(handle); }
    std::shared_ptr<const assets::EnvironmentData> find(EnvironmentHandle handle) const { return environments_.find(handle); }
    const MaterialDesc *find(MaterialHandle handle) const {
        if (handle.index < first_index || handle.index - first_index >= materials_.size()) return nullptr;
        const auto &entry = materials_[handle.index - first_index];
        return entry.generation == handle.generation ? &entry.desc : nullptr;
    }
    // Drops the CPU data of every asset not named, so the asset database can free it too. Material descriptions stay:
    // they are small and hold no pixels.
    void release_except(const std::set<assets::AssetId> &keep) {
        meshes_.release_except(keep); textures_.release_except(keep); environments_.release_except(keep);
    }
    std::size_t held_meshes() const { return meshes_.held(); }
    std::size_t held_textures() const { return textures_.held(); }
};
} // namespace engine::renderer
