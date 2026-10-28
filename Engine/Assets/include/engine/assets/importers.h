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

// Sidecar "<asset>.meta", version 1:
//   version = 1
//   id = 0123456789abcdef      (optional)
//   mesh.scale = 1             (optional)
//   texture.srgb = true        (optional)
// '#' starts a comment line. Unknown keys and duplicate keys are errors.
struct MetaFile {
	std::optional<AssetId> id;
	ImportSettings settings;
};

Result<MetaFile> parse_meta(std::string_view text);
[[nodiscard]] std::string write_meta(const MetaFile& meta);

} // namespace engine::assets
