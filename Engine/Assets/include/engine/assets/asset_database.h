#pragma once

// Catalogue and cache of the assets under one root directory.
// Trusted-host service (C-5): it reads files, so it is never exposed to agents or remote commands.
// Not thread safe; use from one thread.

#include "engine/assets/asset_types.h"

#include <engine/core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

// A problem found by scan() or validate_all(). The asset (if any) is excluded or failed to import.
struct AssetIssue {
	std::string path;
	std::string message;
};

enum class AssetState : std::uint8_t {
	Unknown, // id not in the catalogue
	Unloaded,
	Loaded,
	Failed,  // last import failed; load() retries
	Missing, // catalogued, but the source file is gone
};

[[nodiscard]] const char* to_string(AssetState state) noexcept;

// A cached asset. `revision` starts at 1 and increments on every reload(); renderers re-upload when it changes.
template <typename T>
struct LoadedAsset {
	AssetId id;
	std::uint32_t revision = 0;
	std::shared_ptr<const T> data;
};

using MeshAsset = LoadedAsset<MeshData>;
using TextureAsset = LoadedAsset<TextureData>;

class AssetDatabase {
public:
	// root must be an existing directory. Nothing is read until scan().
	static Result<AssetDatabase> open(const std::filesystem::path& root, const ImportLimits& limits = {});

	~AssetDatabase();
	AssetDatabase(AssetDatabase&& other) noexcept;
	AssetDatabase& operator=(AssetDatabase&& other) noexcept;
	AssetDatabase(const AssetDatabase&) = delete;
	AssetDatabase& operator=(const AssetDatabase&) = delete;

	// Rebuilds the catalogue from disk and returns the number of catalogued assets. Path, sidecar,
	// id-collision and case-collision problems become issues() and exclude the affected assets.
	// Cache entries survive when their asset keeps its id, path and settings.
	Result<std::size_t> scan();
	[[nodiscard]] std::span<const AssetIssue> issues() const noexcept;

	// Ascending by path.
	[[nodiscard]] std::vector<AssetMetadata> assets() const;
	[[nodiscard]] std::optional<AssetMetadata> find(AssetId id) const;
	// The path is normalized first.
	[[nodiscard]] std::optional<AssetMetadata> find(std::string_view path) const;

	// Imports on first use, then returns the cached data. NotFound for unknown ids and missing files,
	// InvalidArgument for a type mismatch, ParseError for invalid content.
	Result<MeshAsset> load_mesh(AssetId id);
	Result<TextureAsset> load_texture(AssetId id);

	// Copies an outside file into the root under a free name, writes a .meta with a new random id, rescans and
	// loads it once. Nothing stays behind on failure. A file already under the root is just loaded.
	// InvalidArgument for unsupported file types, with the supported ones in the message.
	Result<AssetId> import_file(const std::filesystem::path& source);

	// Imports every catalogued asset without caching it; returns the failures.
	std::vector<AssetIssue> validate_all();

	[[nodiscard]] AssetState state(AssetId id) const;
	// Checks every catalogued source file still exists; marks and returns the missing ones.
	std::vector<AssetId> find_missing();
	// Drops the cached data and bumps the revision; the next load re-imports.
	Result<void> reload(AssetId id);
	// Drops cached data nobody outside the database references. Returns how many entries were freed.
	std::size_t unload_unused();
	[[nodiscard]] std::size_t cached_count() const noexcept;
	[[nodiscard]] const std::filesystem::path& root() const noexcept;

private:
	struct Impl;
	explicit AssetDatabase(std::unique_ptr<Impl> impl) noexcept;
	std::unique_ptr<Impl> impl_;
};

} // namespace engine::assets
