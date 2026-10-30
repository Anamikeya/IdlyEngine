#pragma once

// CPU-side asset model (contract C-5 in docs/integration/m2-contracts.md).
// No GPU object, renderer handle or material model lives here: the renderer maps (AssetId, revision)
// to its own MeshHandle/MaterialHandle and uploads from these structures.

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::assets {

// Stable 64-bit identifier. From the asset's .meta sidecar when present (survives renames),
// otherwise derived from the normalized virtual path. Zero is the null id.
struct AssetId {
	std::uint64_t value = 0;

	[[nodiscard]] bool is_null() const noexcept { return value == 0; }
	friend auto operator<=>(const AssetId&, const AssetId&) = default;
};

// 16 lowercase hex digits.
[[nodiscard]] std::string to_string(AssetId id);
// Accepts exactly 16 hex digits (either case) encoding a nonzero value.
[[nodiscard]] std::optional<AssetId> parse_asset_id(std::string_view text) noexcept;
// FNV-1a 64 of the normalized path bytes. Never returns the null id.
[[nodiscard]] AssetId asset_id_from_path(std::string_view normalized_path) noexcept;

enum class AssetType : std::uint8_t {
	Unknown,
	Mesh,    // .glb, .obj
	Texture, // .png, .jpg, .jpeg, .tga, .ppm
};

[[nodiscard]] const char* to_string(AssetType type) noexcept;
// By file extension, case-insensitive.
[[nodiscard]] AssetType asset_type_for(std::string_view path) noexcept;

struct ImportSettings {
	// Uniform factor applied to OBJ positions (e.g. 0.01 for centimetre exports). Finite and > 0.
	float mesh_scale = 1.0f;
	// Texture colour data is sRGB-encoded. Set false for data textures (normal maps, masks).
	bool texture_srgb = true;

	friend bool operator==(const ImportSettings&, const ImportSettings&) = default;
};

struct ImportLimits {
	std::uint64_t max_source_bytes = 64ull * 1024 * 1024;
	std::size_t max_vertices = std::size_t{1} << 22;
	std::size_t max_indices = std::size_t{1} << 24;
	std::uint32_t max_texture_dimension = 16384;
};

struct AssetMetadata {
	AssetId id;
	// Normalized virtual path relative to the database root, '/'-separated.
	std::string path;
	AssetType type = AssetType::Unknown;
	// True when the id came from a .meta sidecar rather than the path.
	bool has_meta = false;
	ImportSettings settings;
	std::uint64_t source_bytes = 0;
};

// Interleaved vertex. Right-handed, metres; UV origin top-left (v' = 1 - v_obj) to match texture rows.
struct MeshVertex {
	std::array<float, 3> position{};
	std::array<float, 3> normal{};
	std::array<float, 2> uv{};

	friend bool operator==(const MeshVertex&, const MeshVertex&) = default;
};

struct Bounds {
	std::array<float, 3> min{};
	std::array<float, 3> max{};
};

// Metallic-roughness values of one part of a model. Colors are linear, as glTF stores them.
struct MeshMaterial {
	std::array<float, 4> base_color{1.0f, 1.0f, 1.0f, 1.0f};
	float roughness = 1.0f;
	float metallic = 1.0f;
	// Index into MeshData::images, or -1.
	int image = -1;
};

// A run of the index list drawn with one material.
struct SubMesh {
	std::uint32_t first_index = 0;
	std::uint32_t index_count = 0;
	MeshMaterial material;
};

struct TextureData;

// Triangle list, counter-clockwise front faces, 32-bit indices.
struct MeshData {
	std::vector<MeshVertex> vertices;
	std::vector<std::uint32_t> indices;
	Bounds bounds;
	// GLB models: the parts and their embedded base color images. Empty for sources without materials (OBJ).
	std::vector<SubMesh> submeshes;
	std::vector<std::shared_ptr<const TextureData>> images;
	// Features of the source that were ignored, for the host to show when the asset is imported.
	std::vector<std::string> warnings;
	// The source had no normals for some corners; area-weighted normals were generated for the mesh.
	bool generated_normals = false;
	// The source had no texture coordinates for some corners; those corners use (0, 0).
	bool missing_uvs = false;
};

enum class TextureFormat : std::uint8_t {
	Rgba8,
};

// Rows top to bottom, 4 bytes per pixel, no padding.
struct TextureData {
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	TextureFormat format = TextureFormat::Rgba8;
	bool srgb = true;
	std::vector<std::uint8_t> pixels;
};

} // namespace engine::assets
