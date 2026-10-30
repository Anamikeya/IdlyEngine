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
	Material, // .material: text, see parse_material
	Environment, // .hdr: equirectangular radiance
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
	// xyz points along +u. w is the handedness: cross(normal, xyz) * w is the bitangent, which points at the top of
	// the image (glTF's convention), so a normal map's +Y leans towards the image's first row.
	std::array<float, 4> tangent{1.0f, 0.0f, 0.0f, 1.0f};

	friend bool operator==(const MeshVertex&, const MeshVertex&) = default;
};

struct Bounds {
	std::array<float, 3> min{};
	std::array<float, 3> max{};
};

enum class AlphaMode : std::uint8_t {
	Opaque,
	Mask,  // Fragments below the cutoff are discarded.
	Blend, // Alpha blended; does not cast shadows.
};

// Metallic-roughness material of one part of a model. Colors are linear, as glTF stores them.
struct MeshMaterial {
	std::array<float, 4> base_color{1.0f, 1.0f, 1.0f, 1.0f};
	float roughness = 1.0f;
	float metallic = 1.0f;
	std::array<float, 3> emissive{0.0f, 0.0f, 0.0f}; // Emissive factor times KHR_materials_emissive_strength.
	float normal_scale = 1.0f;
	float occlusion_strength = 1.0f;
	AlphaMode alpha_mode = AlphaMode::Opaque;
	float alpha_cutoff = 0.5f;
	bool double_sided = false;
	// Indices into MeshData::images, or -1. Base color and emissive images are sRGB, the others linear.
	int image = -1; // Base color.
	int metallic_roughness_image = -1;
	int normal_image = -1;
	int occlusion_image = -1;
	int emissive_image = -1;
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
	// GLB models: the parts and their embedded images. Empty for sources without materials (OBJ).
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

// Equirectangular radiance, width == 2 * height, rows top to bottom, 3 linear floats per pixel.
struct EnvironmentData {
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::vector<float> pixels;
};

// A texture a material names: stable id, and the path for when the id is gone. Both empty, or both set.
struct MaterialTexture {
	AssetId id;
	std::string path;

	[[nodiscard]] bool empty() const noexcept { return id.is_null(); }
	friend bool operator==(const MaterialTexture&, const MaterialTexture&) = default;
};

// A reusable material asset (.material). Colors are sRGB in [0, 1] as picked in the editor; hosts convert them.
struct MaterialData {
	std::array<double, 4> base_color{1.0, 1.0, 1.0, 1.0}; // Alpha is linear.
	double metallic = 0.0;
	double roughness = 0.65;
	std::array<double, 3> emissive{0.0, 0.0, 0.0};
	double emissive_strength = 1.0; // Multiplies emissive; above 1 it blooms.
	double normal_scale = 1.0;
	double occlusion_strength = 1.0;
	AlphaMode alpha_mode = AlphaMode::Opaque;
	double alpha_cutoff = 0.5;
	bool double_sided = false;
	std::array<double, 2> uv_scale{1.0, 1.0};
	MaterialTexture base_color_map;
	MaterialTexture metallic_roughness_map;
	MaterialTexture normal_map;
	MaterialTexture occlusion_map;
	MaterialTexture emissive_map;

	friend bool operator==(const MaterialData&, const MaterialData&) = default;
};

} // namespace engine::assets
