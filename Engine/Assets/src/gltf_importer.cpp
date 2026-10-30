// Binary glTF and PNG/JPEG import. cgltf and stb_image are the parsers Filament's own gltfio uses; they are
// compiled into this one file from the read-only dependency tree. Filament's AssetLoader is deliberately not
// used: it builds its own entity tree outside the scene, while this keeps assets as plain CPU data (C-5).

#include "engine/assets/importers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef ENGINE_ASSETS_HAVE_GLTF
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_HDR
#define STBI_NO_STDIO
#include <stb_image.h>
#endif

namespace engine::assets {

#ifndef ENGINE_ASSETS_HAVE_GLTF

bool gltf_import_available() noexcept { return false; }

Result<MeshData> import_glb(std::span<const std::uint8_t>, const ImportSettings&, const ImportLimits&)
{
	return fail(ErrorCode::InvalidState, "this build has no GLB support (cgltf was not found in the dependency tree)");
}

Result<TextureData> import_image(std::span<const std::uint8_t>, const ImportSettings&, const ImportLimits&)
{
	return fail(ErrorCode::InvalidState, "this build has no PNG/JPEG support (stb_image was not found in the dependency tree)");
}

Result<EnvironmentData> import_hdr(std::span<const std::uint8_t>, const ImportLimits&)
{
	return fail(ErrorCode::InvalidState, "this build has no .hdr support (stb_image was not found in the dependency tree)");
}

#else

namespace {

struct DataDeleter {
	void operator()(cgltf_data* data) const noexcept { cgltf_free(data); }
};

const char* describe(cgltf_result result) noexcept
{
	switch (result) {
	case cgltf_result_data_too_short:
		return "the file is truncated";
	case cgltf_result_unknown_format:
		return "the file is not binary glTF";
	case cgltf_result_invalid_json:
		return "its JSON is invalid";
	case cgltf_result_invalid_gltf:
		return "its contents are not valid glTF";
	case cgltf_result_out_of_memory:
		return "it needs too much memory";
	case cgltf_result_legacy_gltf:
		return "it is glTF 1.0, which is not supported";
	default:
		return "it could not be parsed";
	}
}

using Mat4 = std::array<float, 16>; // Column-major.

std::array<float, 3> transform_point(const Mat4& m, const float* p) noexcept
{
	return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
		m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}

// Cofactor matrix of the upper 3x3, which transforms normals correctly under non-uniform scale.
std::array<float, 9> cofactors(const Mat4& m) noexcept
{
	return {m[5] * m[10] - m[6] * m[9], m[6] * m[8] - m[4] * m[10], m[4] * m[9] - m[5] * m[8],
		m[9] * m[2] - m[10] * m[1], m[10] * m[0] - m[8] * m[2], m[8] * m[1] - m[9] * m[0],
		m[1] * m[6] - m[2] * m[5], m[2] * m[4] - m[0] * m[6], m[0] * m[5] - m[1] * m[4]};
}

float determinant(const Mat4& m) noexcept
{
	return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) + m[8] * (m[1] * m[6] - m[2] * m[5]);
}

bool normalize(std::array<float, 3>& v) noexcept
{
	const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (!std::isfinite(length) || length < 1.0e-12f) {
		return false;
	}
	v = {v[0] / length, v[1] / length, v[2] / length};
	return true;
}

bool required_extension_supported(std::string_view name) noexcept
{
	// Quantized attributes are decoded by cgltf; the others only affect features reported as ignored.
	return name == "KHR_mesh_quantization" || name == "KHR_texture_transform" || name == "KHR_lights_punctual" ||
		name.rfind("KHR_materials_", 0) == 0;
}

class GlbImporter {
public:
	GlbImporter(cgltf_data& data, const ImportSettings& settings, const ImportLimits& limits) noexcept
		: data_(data), settings_(settings), limits_(limits)
	{
	}

	Result<MeshData> run()
	{
		for (const cgltf_node* node : nodes_in_scene()) {
			if (node->mesh) {
				if (auto added = add_mesh(*node); !added) {
					return added.error();
				}
			}
		}
		if (mesh_.indices.empty()) {
			return fail(ErrorCode::ParseError, "the model has no triangles in its scene");
		}
		report_ignored_features();
		compute_bounds();
		mesh_.warnings.assign(warnings_.begin(), warnings_.end());
		return std::move(mesh_);
	}

private:
	cgltf_data& data_;
	const ImportSettings& settings_;
	const ImportLimits& limits_;
	MeshData mesh_;
	std::map<std::pair<const cgltf_image*, bool>, int> images_; // By image and color space.
	std::set<std::string> warnings_;

	// Every node of the default scene (or of all root nodes when the file names no scene), parents first.
	std::vector<const cgltf_node*> nodes_in_scene() const
	{
		std::vector<const cgltf_node*> order;
		if (const cgltf_scene* scene = data_.scene ? data_.scene : (data_.scenes_count ? &data_.scenes[0] : nullptr)) {
			order.assign(scene->nodes, scene->nodes + scene->nodes_count);
		} else {
			for (cgltf_size i = 0; i < data_.nodes_count; ++i) {
				if (!data_.nodes[i].parent) {
					order.push_back(&data_.nodes[i]);
				}
			}
		}
		std::set<const cgltf_node*> seen;
		std::vector<const cgltf_node*> unique;
		for (std::size_t i = 0; i < order.size(); ++i) {
			if (seen.insert(order[i]).second) {
				unique.push_back(order[i]);
				order.insert(order.end(), order[i]->children, order[i]->children + order[i]->children_count);
			}
		}
		return unique;
	}

	Result<void> add_mesh(const cgltf_node& node)
	{
		Mat4 world{};
		cgltf_node_transform_world(&node, world.data());
		for (cgltf_size i = 0; i < node.mesh->primitives_count; ++i) {
			const cgltf_primitive& primitive = node.mesh->primitives[i];
			if (primitive.type != cgltf_primitive_type_triangles) {
				warnings_.insert("primitives that are not triangle lists were skipped");
				continue;
			}
			if (primitive.has_draco_mesh_compression) {
				return fail(ErrorCode::ParseError, "Draco mesh compression is not supported");
			}
			if (primitive.targets_count > 0) {
				warnings_.insert("morph targets are ignored; the base shape is imported");
			}
			if (auto added = add_primitive(primitive, world); !added) {
				return added;
			}
		}
		return {};
	}

	Result<void> add_primitive(const cgltf_primitive& primitive, const Mat4& world)
	{
		const cgltf_accessor* positions = nullptr;
		const cgltf_accessor* normals = nullptr;
		const cgltf_accessor* uvs = nullptr;
		const cgltf_accessor* tangents = nullptr;
		for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
			const cgltf_attribute& attribute = primitive.attributes[i];
			if (attribute.type == cgltf_attribute_type_position) {
				positions = attribute.data;
			} else if (attribute.type == cgltf_attribute_type_normal) {
				normals = attribute.data;
			} else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0) {
				uvs = attribute.data;
			} else if (attribute.type == cgltf_attribute_type_tangent) {
				tangents = attribute.data;
			}
		}
		// A malformed TANGENT is not worth refusing a model over: derive the tangents instead.
		if (tangents && (tangents->type != cgltf_type_vec4 || tangents->count != positions->count)) {
			tangents = nullptr;
		}
		if (!normals) {
			tangents = nullptr; // Generated normals would not match the file's tangents.
		}
		if (!positions || positions->type != cgltf_type_vec3) {
			return fail(ErrorCode::ParseError, "a primitive has no usable POSITION attribute");
		}
		if ((normals && (normals->type != cgltf_type_vec3 || normals->count != positions->count)) ||
			(uvs && (uvs->type != cgltf_type_vec2 || uvs->count != positions->count))) {
			return fail(ErrorCode::ParseError, "a primitive's NORMAL or TEXCOORD_0 does not match its POSITION");
		}
		const std::size_t count = positions->count;
		const std::size_t index_count = primitive.indices ? primitive.indices->count : count;
		if (mesh_.vertices.size() + count > limits_.max_vertices || mesh_.indices.size() + index_count > limits_.max_indices) {
			return fail(ErrorCode::InvalidArgument, "the model exceeds the vertex or index limit");
		}
		if (index_count % 3 != 0) {
			return fail(ErrorCode::ParseError, "a triangle list has an index count that is not a multiple of 3");
		}
		if (primitive.indices && (primitive.indices->type != cgltf_type_scalar || primitive.indices->is_sparse)) {
			return fail(ErrorCode::ParseError, "a primitive has an unusable index accessor");
		}
		if (index_count == 0) {
			return {};
		}
		bool readable = true;
		const auto read = [&](const cgltf_accessor* accessor, std::size_t components) {
			std::vector<float> values(accessor ? count * components : 0);
			if (accessor) {
				readable = readable && cgltf_accessor_unpack_floats(accessor, values.data(), values.size()) == values.size();
			}
			return values;
		};
		const std::vector<float> p = read(positions, 3), n = read(normals, 3), t = read(uvs, 2), g = read(tangents, 4);
		if (!readable) {
			return fail(ErrorCode::ParseError, "a primitive's vertex data cannot be read");
		}

		const float det = determinant(world);
		const std::array<float, 9> cof = cofactors(world);
		const float flip = det < 0.0f ? -1.0f : 1.0f;
		const auto base = static_cast<std::uint32_t>(mesh_.vertices.size());
		mesh_.vertices.resize(mesh_.vertices.size() + count);
		for (std::size_t i = 0; i < count; ++i) {
			MeshVertex& v = mesh_.vertices[base + i];
			v.position = transform_point(world, &p[i * 3]);
			for (float& c : v.position) {
				c *= settings_.mesh_scale;
				if (!std::isfinite(c)) {
					return fail(ErrorCode::ParseError, "a primitive has non-finite positions");
				}
			}
			if (normals) {
				const float* s = &n[i * 3];
				v.normal = {flip * (cof[0] * s[0] + cof[3] * s[1] + cof[6] * s[2]), flip * (cof[1] * s[0] + cof[4] * s[1] + cof[7] * s[2]),
					flip * (cof[2] * s[0] + cof[5] * s[1] + cof[8] * s[2])};
				if (!normalize(v.normal)) {
					v.normal = {0.0f, 1.0f, 0.0f};
				}
			}
			if (uvs) {
				// glTF and the engine both put the UV origin at the first (top) row of the image.
				v.uv = {t[i * 2], t[i * 2 + 1]};
				if (!std::isfinite(v.uv[0]) || !std::isfinite(v.uv[1])) {
					return fail(ErrorCode::ParseError, "a primitive has non-finite texture coordinates");
				}
			}
			if (tangents) {
				const float* s = &g[i * 4];
				std::array<float, 3> direction{world[0] * s[0] + world[4] * s[1] + world[8] * s[2],
					world[1] * s[0] + world[5] * s[1] + world[9] * s[2], world[2] * s[0] + world[6] * s[1] + world[10] * s[2]};
				if (normalize(direction)) {
					v.tangent = {direction[0], direction[1], direction[2], s[3] < 0.0f ? -flip : flip};
				}
			}
		}
		mesh_.missing_uvs = mesh_.missing_uvs || !uvs;

		SubMesh part;
		part.first_index = static_cast<std::uint32_t>(mesh_.indices.size());
		part.index_count = static_cast<std::uint32_t>(index_count);
		mesh_.indices.resize(mesh_.indices.size() + index_count);
		std::uint32_t* out = mesh_.indices.data() + part.first_index;
		for (std::size_t i = 0; i < index_count; ++i) {
			const std::size_t index = primitive.indices ? cgltf_accessor_read_index(primitive.indices, i) : i;
			if (index >= count) {
				return fail(ErrorCode::ParseError, "a primitive has an index beyond its vertices");
			}
			out[i] = base + static_cast<std::uint32_t>(index);
		}
		if (det < 0.0f) { // A mirroring node reverses the winding; restore counter-clockwise front faces.
			for (std::size_t i = 0; i + 2 < index_count; i += 3) {
				std::swap(out[i + 1], out[i + 2]);
			}
		}
		if (!normals) {
			generate_normals(part, base, count);
		}
		if (!tangents) {
			compute_tangents(mesh_, part.first_index, part.index_count);
		}
		auto material = convert_material(primitive.material);
		if (!material) {
			return material.error();
		}
		part.material = material.value();
		mesh_.submeshes.push_back(part);
		return {};
	}

	void generate_normals(const SubMesh& part, std::uint32_t base, std::size_t count)
	{
		mesh_.generated_normals = true;
		const std::uint32_t* indices = mesh_.indices.data() + part.first_index;
		for (std::uint32_t i = 0; i + 2 < part.index_count; i += 3) {
			MeshVertex& a = mesh_.vertices[indices[i]];
			MeshVertex& b = mesh_.vertices[indices[i + 1]];
			MeshVertex& c = mesh_.vertices[indices[i + 2]];
			const std::array<float, 3> u{b.position[0] - a.position[0], b.position[1] - a.position[1], b.position[2] - a.position[2]};
			const std::array<float, 3> v{c.position[0] - a.position[0], c.position[1] - a.position[1], c.position[2] - a.position[2]};
			const std::array<float, 3> face{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
			for (MeshVertex* vertex : {&a, &b, &c}) {
				for (int k = 0; k < 3; ++k) {
					vertex->normal[static_cast<std::size_t>(k)] += face[static_cast<std::size_t>(k)]; // Weighted by area.
				}
			}
		}
		for (std::size_t i = 0; i < count; ++i) {
			if (!normalize(mesh_.vertices[base + i].normal)) {
				mesh_.vertices[base + i].normal = {0.0f, 1.0f, 0.0f};
			}
		}
	}

	Result<MeshMaterial> convert_material(const cgltf_material* material)
	{
		MeshMaterial out;
		if (!material) {
			return out;
		}
		out.alpha_mode = material->alpha_mode == cgltf_alpha_mode_mask    ? AlphaMode::Mask
			: material->alpha_mode == cgltf_alpha_mode_blend ? AlphaMode::Blend
															 : AlphaMode::Opaque;
		out.alpha_cutoff = std::clamp(material->alpha_cutoff, 0.0f, 1.0f);
		out.double_sided = material->double_sided != 0;
		const float emissive_strength = material->has_emissive_strength ? material->emissive_strength.emissive_strength : 1.0f;
		for (std::size_t i = 0; i < 3; ++i) {
			out.emissive[i] = std::max(material->emissive_factor[i] * emissive_strength, 0.0f);
		}
		out.normal_scale = std::max(material->normal_texture.scale, 0.0f);
		out.occlusion_strength = std::clamp(material->occlusion_texture.scale, 0.0f, 1.0f);
		out.normal_image = secondary_map(material->normal_texture, false, "normal");
		out.occlusion_image = secondary_map(material->occlusion_texture, false, "occlusion");
		out.emissive_image = secondary_map(material->emissive_texture, true, "emissive");
		if (material->has_pbr_specular_glossiness) {
			warnings_.insert("specular-glossiness materials are not supported; default values are used");
		}
		if (!material->has_pbr_metallic_roughness) {
			return out;
		}
		const cgltf_pbr_metallic_roughness& pbr = material->pbr_metallic_roughness;
		for (std::size_t i = 0; i < 4; ++i) {
			out.base_color[i] = std::clamp(pbr.base_color_factor[i], 0.0f, 1.0f);
		}
		out.roughness = std::clamp(pbr.roughness_factor, 0.0f, 1.0f);
		out.metallic = std::clamp(pbr.metallic_factor, 0.0f, 1.0f);
		out.metallic_roughness_image = secondary_map(pbr.metallic_roughness_texture, false, "metallic-roughness");
		if (const cgltf_texture* texture = pbr.base_color_texture.texture) {
			check_texture_view(pbr.base_color_texture);
			if (!texture->image) {
				return fail(ErrorCode::ParseError, "a base color texture has no PNG or JPEG image (WebP and KTX2 are not supported)");
			}
			auto image = image_index(*texture->image, true);
			if (!image) {
				return image.error();
			}
			out.image = image.value();
		}
		return out;
	}

	void check_texture_view(const cgltf_texture_view& view)
	{
		if (view.texcoord != 0 || view.has_transform) {
			warnings_.insert("texture transforms and secondary UV sets are ignored");
		}
	}

	// An image for a map the material can do without: anything wrong with it is a warning, and the part still imports.
	int secondary_map(const cgltf_texture_view& view, bool srgb, const char* role)
	{
		if (!view.texture) {
			return -1;
		}
		check_texture_view(view);
		if (!view.texture->image) {
			warnings_.insert(std::string(role) + " map skipped: it has no PNG or JPEG image (WebP and KTX2 are not supported)");
			return -1;
		}
		auto image = image_index(*view.texture->image, srgb);
		if (!image) {
			warnings_.insert(std::string(role) + " map skipped: " + image.error().message);
			return -1;
		}
		return image.value();
	}

	Result<int> image_index(const cgltf_image& image, bool srgb)
	{
		if (const auto found = images_.find({&image, srgb}); found != images_.end()) {
			return found->second;
		}
		ImportSettings settings = settings_;
		settings.texture_srgb = srgb;
		Result<TextureData> decoded = fail(ErrorCode::ParseError, "unset");
		if (image.buffer_view) {
			const auto* bytes = cgltf_buffer_view_data(image.buffer_view);
			if (!bytes) {
				return fail(ErrorCode::ParseError, "an embedded image has no data");
			}
			decoded = import_image({bytes, image.buffer_view->size}, settings, limits_);
		} else if (image.uri && std::strncmp(image.uri, "data:", 5) == 0) {
			const char* comma = std::strchr(image.uri, ',');
			if (!comma) {
				return fail(ErrorCode::ParseError, "an embedded image has a malformed data URI");
			}
			const std::size_t length = std::strlen(comma + 1);
			std::size_t size = length / 4 * 3;
			for (std::size_t i = length; i > 0 && comma[i] == '='; --i) {
				--size;
			}
			void* bytes = nullptr;
			const cgltf_options options{};
			if (cgltf_load_buffer_base64(&options, size, comma + 1, &bytes) != cgltf_result_success) {
				return fail(ErrorCode::ParseError, "an embedded image has invalid base64 data");
			}
			decoded = import_image({static_cast<const std::uint8_t*>(bytes), size}, settings, limits_);
			std::free(bytes);
		} else {
			return fail(ErrorCode::ParseError, "an image is stored in an external file; export the model with embedded images");
		}
		if (!decoded) {
			return fail(decoded.error().code, "an embedded image failed to load: " + decoded.error().message);
		}
		mesh_.images.push_back(std::make_shared<const TextureData>(std::move(decoded.value())));
		const int index = static_cast<int>(mesh_.images.size()) - 1;
		images_.emplace(std::make_pair(&image, srgb), index);
		return index;
	}

	void report_ignored_features()
	{
		if (data_.skins_count > 0) {
			warnings_.insert("skinning is ignored; the model is imported in its bind pose");
		}
		if (data_.animations_count > 0) {
			warnings_.insert("animations are ignored");
		}
		if (data_.cameras_count > 0 || data_.lights_count > 0) {
			warnings_.insert("cameras and lights in the file are ignored");
		}
	}

	void compute_bounds()
	{
		Bounds& b = mesh_.bounds;
		b.min = mesh_.vertices.front().position;
		b.max = b.min;
		for (const MeshVertex& v : mesh_.vertices) {
			for (std::size_t k = 0; k < 3; ++k) {
				b.min[k] = std::min(b.min[k], v.position[k]);
				b.max[k] = std::max(b.max[k], v.position[k]);
			}
		}
	}
};

} // namespace

bool gltf_import_available() noexcept { return true; }

Result<MeshData> import_glb(std::span<const std::uint8_t> bytes, const ImportSettings& settings, const ImportLimits& limits)
{
	if (bytes.size() > limits.max_source_bytes) {
		return fail(ErrorCode::InvalidArgument, "the model exceeds the size limit");
	}
	if (!(settings.mesh_scale > 0.0f) || !std::isfinite(settings.mesh_scale)) {
		return fail(ErrorCode::InvalidArgument, "mesh scale must be finite and positive");
	}
	cgltf_options options{};
	options.type = cgltf_file_type_glb;
	cgltf_data* raw = nullptr;
	if (const cgltf_result parsed = cgltf_parse(&options, bytes.data(), bytes.size(), &raw); parsed != cgltf_result_success) {
		return fail(ErrorCode::ParseError, std::string("Invalid GLB: ") + describe(parsed));
	}
	const std::unique_ptr<cgltf_data, DataDeleter> data(raw);
	for (cgltf_size i = 0; i < data->extensions_required_count; ++i) {
		if (!required_extension_supported(data->extensions_required[i])) {
			return fail(ErrorCode::ParseError, std::string("the model requires unsupported extension ") + data->extensions_required[i]);
		}
	}
	// Only self-contained files: nothing here may make cgltf open a path named by the file's content.
	for (cgltf_size i = 0; i < data->buffers_count; ++i) {
		const char* uri = data->buffers[i].uri;
		if (uri && std::strncmp(uri, "data:", 5) != 0) {
			return fail(ErrorCode::ParseError, "the model stores its data in an external file; export it as a self-contained .glb");
		}
	}
	if (const cgltf_result loaded = cgltf_load_buffers(&options, data.get(), nullptr); loaded != cgltf_result_success) {
		return fail(ErrorCode::ParseError, std::string("Invalid GLB: ") + describe(loaded));
	}
	if (const cgltf_result valid = cgltf_validate(data.get()); valid != cgltf_result_success) {
		return fail(ErrorCode::ParseError, std::string("Invalid GLB: ") + describe(valid));
	}
	return GlbImporter(*data, settings, limits).run();
}

Result<EnvironmentData> import_hdr(std::span<const std::uint8_t> bytes, const ImportLimits& limits)
{
	if (bytes.size() > limits.max_source_bytes || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		return fail(ErrorCode::InvalidArgument, "the image exceeds the size limit");
	}
	const auto size = static_cast<int>(bytes.size());
	if (!stbi_is_hdr_from_memory(bytes.data(), size)) {
		return fail(ErrorCode::ParseError, "not a Radiance .hdr image");
	}
	int width = 0, height = 0, channels = 0;
	if (!stbi_info_from_memory(bytes.data(), size, &width, &height, &channels)) {
		return fail(ErrorCode::ParseError, "not a valid .hdr image");
	}
	if (width <= 0 || height <= 0 || static_cast<std::uint32_t>(width) > limits.max_texture_dimension ||
		static_cast<std::uint32_t>(height) > limits.max_texture_dimension) {
		return fail(ErrorCode::InvalidArgument, "the image exceeds the dimension limit");
	}
	if (width != 2 * height) {
		return fail(ErrorCode::InvalidArgument, "an environment image must be equirectangular: twice as wide as it is tall");
	}
	const std::unique_ptr<float, decltype(&stbi_image_free)> pixels(
		stbi_loadf_from_memory(bytes.data(), size, &width, &height, &channels, 3), stbi_image_free);
	if (!pixels) {
		return fail(ErrorCode::ParseError, std::string("the image could not be decoded: ") + stbi_failure_reason());
	}
	EnvironmentData out;
	out.width = static_cast<std::uint32_t>(width);
	out.height = static_cast<std::uint32_t>(height);
	const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
	out.pixels.assign(pixels.get(), pixels.get() + count);
	for (float& value : out.pixels) {
		if (!std::isfinite(value) || value < 0.0f) {
			value = 0.0f;
		}
	}
	return out;
}

Result<TextureData> import_image(std::span<const std::uint8_t> bytes, const ImportSettings& settings, const ImportLimits& limits)
{
	if (bytes.size() > limits.max_source_bytes || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		return fail(ErrorCode::InvalidArgument, "the image exceeds the size limit");
	}
	const auto size = static_cast<int>(bytes.size());
	int width = 0, height = 0, channels = 0;
	if (!stbi_info_from_memory(bytes.data(), size, &width, &height, &channels)) {
		return fail(ErrorCode::ParseError, "not a valid PNG or JPEG image");
	}
	if (width <= 0 || height <= 0 || static_cast<std::uint32_t>(width) > limits.max_texture_dimension ||
		static_cast<std::uint32_t>(height) > limits.max_texture_dimension) {
		return fail(ErrorCode::InvalidArgument, "the image exceeds the dimension limit");
	}
	const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
		stbi_load_from_memory(bytes.data(), size, &width, &height, &channels, 4), stbi_image_free);
	if (!pixels) {
		return fail(ErrorCode::ParseError, std::string("the image could not be decoded: ") + stbi_failure_reason());
	}
	TextureData texture;
	texture.width = static_cast<std::uint32_t>(width);
	texture.height = static_cast<std::uint32_t>(height);
	texture.srgb = settings.texture_srgb;
	texture.pixels.assign(pixels.get(), pixels.get() + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
	return texture;
}

#endif

} // namespace engine::assets
