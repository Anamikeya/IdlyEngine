#pragma once
#include <engine/assets/asset_types.h>
#include <engine/renderer/RenderSnapshot.hpp>
#include <map>
#include <memory>
#include <vector>

namespace engine::renderer {
// Maps (AssetId, revision) to the handles frames carry (contract C-5). CPU data only: a renderer uploads what a
// handle finds when it first draws it. An asset keeps its index; a new revision gets a new generation, so handles
// of the old revision find nothing. Owner thread only.
class ResourceRegistry {
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
            }
            return {it->second, entry.generation};
        }
        std::shared_ptr<const Data> find(Handle handle) const {
            if (handle.index < first_index || handle.index - first_index >= entries_.size()) return nullptr;
            const auto &entry = entries_[handle.index - first_index];
            return entry.generation == handle.generation ? entry.data : nullptr;
        }
    };
    Table<MeshHandle, assets::MeshData> meshes_;
    Table<TextureHandle, assets::TextureData> textures_;

public:
    // Handles below this are built-in resources.
    static constexpr std::uint32_t first_index = 16;
    MeshHandle mesh(assets::AssetId id, std::uint32_t revision, std::shared_ptr<const assets::MeshData> data) {
        return meshes_.get(id, revision, std::move(data));
    }
    TextureHandle texture(assets::AssetId id, std::uint32_t revision, std::shared_ptr<const assets::TextureData> data) {
        return textures_.get(id, revision, std::move(data));
    }
    // Null for a built-in, unknown or stale handle.
    std::shared_ptr<const assets::MeshData> find(MeshHandle handle) const { return meshes_.find(handle); }
    std::shared_ptr<const assets::TextureData> find(TextureHandle handle) const { return textures_.find(handle); }
};
} // namespace engine::renderer
