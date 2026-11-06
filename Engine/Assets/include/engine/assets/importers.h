#pragma once

// Bounded, strict, locale-independent importers. Pure functions over bytes: no filesystem access.

#include "engine/assets/asset_types.h"

#include <engine/core/Result.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace engine::assets {

// Virtual asset path rules (C-5): relative, '/'-separated, case preserved, '.' removed, '..' resolved
// but never above the root. Rejects absolute/drive/UNC paths, control and reserved characters,
// Windows reserved device names and segments ending in '.' or ' '. At most 1024 bytes.
Result<std::string> normalize_asset_path(std::string_view raw);

// Wavefront OBJ subset: v, vt, vn, f (v, v/vt, v//vn, v/vt/vn; negative indices; convex polygons are
// fan-triangulated). o, g, s, usemtl and mtllib are accepted and ignored. Any other statement is an error.
Result<MeshData> import_obj(std::string_view text, const ImportSettings& settings = {}, const ImportLimits& limits = {});

// Truecolor 24/32-bit (types 2 and 10, RLE) and 8-bit grayscale (type 3). No colour maps.
Result<TextureData> import_tga(std::span<const std::uint8_t> bytes, const ImportSettings& settings = {},
	const ImportLimits& limits = {});

// Binary PPM (P6) with maxval 255.
Result<TextureData> import_ppm(std::span<const std::uint8_t> bytes, const ImportSettings& settings = {},
	const ImportLimits& limits = {});

// False when this build was configured without the cgltf and stb_image sources; import_glb and import_image
// then return an error saying so.
[[nodiscard]] bool gltf_import_available() noexcept;

// Self-contained binary glTF 2.0. All primitives of the default scene become one mesh with node transforms
// baked in and one SubMesh per primitive. Factors, the five metallic-roughness maps, alpha mode and
// double-sidedness are kept. Ignored features (morph targets, secondary UV sets, texture transforms,
// non-triangle primitives, unused skins) and unusable secondary maps are listed in MeshData::warnings.
// External files, Draco and other required extensions are errors.
// Skins and animated nodes give the mesh a skeleton (MeshData::skin): skinned primitives keep their bind-space
// vertices, every other part is baked as it rests and follows the nearest joint above it. Each animation becomes
// an AnimationClip. More than 4 influences keep the strongest 4; a model needing more than max_skin_bindings bones
// is refused. ImportSettings::mesh_scale scales joint, key and inverse bind translations along with the vertices.
Result<MeshData> import_glb(std::span<const std::uint8_t> bytes, const ImportSettings& settings = {},
	const ImportLimits& limits = {});

// PNG or JPEG, decoded to RGBA8.
Result<TextureData> import_image(std::span<const std::uint8_t> bytes, const ImportSettings& settings = {},
	const ImportLimits& limits = {});

// Radiance .hdr, equirectangular (width == 2 * height).
Result<EnvironmentData> import_hdr(std::span<const std::uint8_t> bytes, const ImportLimits& limits = {});

// Fills MeshVertex::tangent of the given index range from positions and UVs (Lengyel's method). Vertices without
// usable UVs get an arbitrary frame around the normal.
void compute_tangents(MeshData& mesh, std::uint32_t first_index, std::uint32_t index_count);

// Material asset text, version 1: "ENGINE_MATERIAL 1", then one "key values" line for each of the fields below.
// Every key is optional and at most once; unknown keys and out-of-range values are errors.
//   base_color r g b a | metallic v | roughness v | emissive r g b | emissive_strength v | normal_scale v
//   occlusion_strength v | alpha_mode opaque|mask|blend | alpha_cutoff v | double_sided 0|1 | uv_scale u v
//   base_color_map, metallic_roughness_map, normal_map, occlusion_map, emissive_map: - | <16 hex digits> "<path>"
Result<MaterialData> parse_material(std::string_view text);
[[nodiscard]] std::string write_material(const MaterialData& material);
// The same range checks parse_material applies.
Result<void> validate_material(const MaterialData& material);

// Particle effects (.effect): a versioned key/value text file, like materials. Unknown or repeated keys are errors;
// missing keys keep their defaults.
Result<EffectData> parse_effect(std::string_view text);
[[nodiscard]] std::string write_effect(const EffectData& effect);
// The same range checks parse_effect applies.
Result<void> validate_effect(const EffectData& effect);

// Sidecar "<asset>.meta", version 1:
//   version = 1
//   id = 0123456789abcdef      (optional)
//   mesh.scale = 1             (optional)
//   texture.srgb = true        (optional)
// '#' starts a comment line. Unknown keys and duplicate keys are errors.
struct MetaFile {
	std::optional<AssetId> id;
	ImportSettings settings;
	std::vector<ClipEvent> events; // One line each: event = <time> <name> "<clip>".
};

// Checks names, times and the count, and sorts by clip, time and name. Two events of one clip at one time with one
// name are refused.
Result<std::vector<ClipEvent>> normalize_clip_events(std::vector<ClipEvent> events);

Result<MetaFile> parse_meta(std::string_view text);
[[nodiscard]] std::string write_meta(const MetaFile& meta);

} // namespace engine::assets
