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
using MaterialAsset = LoadedAsset<MaterialData>;
using EnvironmentAsset = LoadedAsset<EnvironmentData>;

// A file decoded away from the database, ready for AssetDatabase::commit_import. Holds no database state.
struct PreparedImport {
	std::filesystem::path source;
	AssetType type = AssetType::Unknown;
	std::shared_ptr<const MeshData> mesh;
	std::shared_ptr<const TextureData> texture;
	std::shared_ptr<const MaterialData> material;
	std::shared_ptr<const EnvironmentData> environment;
};

// Reads and decodes a file with default settings. Touches nothing shared, so any thread may call it.
Result<PreparedImport> prepare_import(const std::filesystem::path& source, const ImportLimits& limits = {});

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
	Result<MaterialAsset> load_material(AssetId id);
	Result<EnvironmentAsset> load_environment(AssetId id);

	// Writes a new .material under a free name based on `path`, with a .meta holding a new random id, and rescans.
	Result<AssetId> create_material(std::string_view path, const MaterialData& material);
	// Replaces the file atomically; the cache holds the new value and the revision increases.
	Result<void> save_material(AssetId id, const MaterialData& material);

	// Copies an outside file into the root under a free name, writes a .meta with a new random id, rescans and
	// loads it once. Nothing stays behind on failure. A file already under the root is just loaded.
	// InvalidArgument for unsupported file types, with the supported ones in the message.
	Result<AssetId> import_file(const std::filesystem::path& source);
	// The same for a file already decoded by prepare_import: the decoded data goes into the cache.
	Result<AssetId> commit_import(const PreparedImport& prepared);

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
	Result<AssetId> import_source(const std::filesystem::path& source, const PreparedImport* prepared);
	explicit AssetDatabase(std::unique_ptr<Impl> impl) noexcept;
	std::unique_ptr<Impl> impl_;
};

} // namespace engine::assets
