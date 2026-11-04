// Binary glTF and PNG/JPEG import. cgltf and stb_image are the parsers Filament's own gltfio uses; they are
// compiled into this one file from the read-only dependency tree. Filament's AssetLoader is deliberately not
// used: it builds its own entity tree outside the scene, while this keeps assets as plain CPU data (C-5).

#include "engine/assets/importers.h"
#include "engine/assets/skeleton.h"

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

using Mat4 = Matrix4; // Column-major.

std::array<float, 3> transform_point(const Mat4& m, const float* p) noexcept
{
	return assets::transform_point(m, {p[0], p[1], p[2]});
}

// A unit quaternion (x, y, z, w) from the columns of a rotation matrix.
std::array<float, 4> quaternion_from(const std::array<float, 3>& x, const std::array<float, 3>& y, const std::array<float, 3>& z) noexcept
{
	const float trace = x[0] + y[1] + z[2];
	std::array<float, 4> q{};
	if (trace > 0.0f) {
		const float s = std::sqrt(trace + 1.0f) * 2.0f;
		q = {(y[2] - z[1]) / s, (z[0] - x[2]) / s, (x[1] - y[0]) / s, 0.25f * s};
	} else if (x[0] > y[1] && x[0] > z[2]) {
		const float s = std::sqrt(1.0f + x[0] - y[1] - z[2]) * 2.0f;
		q = {0.25f * s, (y[0] + x[1]) / s, (z[0] + x[2]) / s, (y[2] - z[1]) / s};
	} else if (y[1] > z[2]) {
		const float s = std::sqrt(1.0f + y[1] - x[0] - z[2]) * 2.0f;
		q = {(y[0] + x[1]) / s, 0.25f * s, (z[1] + y[2]) / s, (z[0] - x[2]) / s};
	} else {
		const float s = std::sqrt(1.0f + z[2] - x[0] - y[1]) * 2.0f;
		q = {(z[0] + x[2]) / s, (z[1] + y[2]) / s, 0.25f * s, (x[1] - y[0]) / s};
	}
	const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (!std::isfinite(length) || length < 1.0e-12f) {
		return {0, 0, 0, 1};
	}
	return {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
}

std::array<float, 4> normalized_quaternion(std::array<float, 4> q) noexcept
{
	const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (!std::isfinite(length) || length < 1.0e-12f) {
		return {0, 0, 0, 1};
	}
	return {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
}

// A node's local transform as translation, rotation and scale. A matrix is split; any shear in it is lost.
void local_transform(const cgltf_node& node, Joint& joint)
{
	if (!node.has_matrix) {
		if (node.has_translation) {
			joint.translation = {node.translation[0], node.translation[1], node.translation[2]};
		}
		if (node.has_rotation) {
			joint.rotation = normalized_quaternion({node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]});
		}
		if (node.has_scale) {
			joint.scale = {node.scale[0], node.scale[1], node.scale[2]};
		}
		return;
	}
	const float* m = node.matrix;
	std::array<float, 3> x{m[0], m[1], m[2]}, y{m[4], m[5], m[6]}, z{m[8], m[9], m[10]};
	joint.translation = {m[12], m[13], m[14]};
	const auto length = [](const std::array<float, 3>& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
	std::array<float, 3> scale{length(x), length(y), length(z)};
	const float det = x[0] * (y[1] * z[2] - y[2] * z[1]) - y[0] * (x[1] * z[2] - x[2] * z[1]) + z[0] * (x[1] * y[2] - x[2] * y[1]);
	if (det < 0.0f) {
		scale[0] = -scale[0];
	}
	for (std::size_t k = 0; k < 3; ++k) {
		x[k] = scale[0] != 0.0f ? x[k] / scale[0] : 0.0f;
		y[k] = scale[1] != 0.0f ? y[k] / scale[1] : 0.0f;
		z[k] = scale[2] != 0.0f ? z[k] / scale[2] : 0.0f;
	}
	joint.rotation = quaternion_from(x, y, z);
	joint.scale = scale;
}

// One channel's keys at linear interpolation.
template <std::size_t N> struct Sampled {
	std::vector<float> times;
	std::vector<std::array<float, N>> values;
};

// glTF cubic spline segment: values v0, v1 with out-tangent b0 and in-tangent a1, over a segment of `span` seconds.
template <std::size_t N>
std::array<float, N> hermite(const std::array<float, N>& v0, const std::array<float, N>& b0, const std::array<float, N>& a1,
	const std::array<float, N>& v1, float span, float s) noexcept
{
	const float s2 = s * s, s3 = s2 * s;
	std::array<float, N> out{};
	for (std::size_t k = 0; k < N; ++k) {
		out[k] = (2 * s3 - 3 * s2 + 1) * v0[k] + span * (s3 - 2 * s2 + s) * b0[k] + (-2 * s3 + 3 * s2) * v1[k] + span * (s3 - s2) * a1[k];
	}
	return out;
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
		const std::vector<const cgltf_node*> nodes = nodes_in_scene();
		if (auto built = build_skeleton(nodes); !built) {
			return built.error();
		}
		for (const cgltf_node* node : nodes) {
			if (node->mesh) {
				if (auto added = add_mesh(*node); !added) {
					return added.error();
				}
			}
		}
		if (mesh_.indices.empty()) {
			return fail(ErrorCode::ParseError, "the model has no triangles in its scene");
		}
		if (!mesh_.skin.empty()) {
			bind_bounds();
			if (auto clips = add_clips(); !clips) {
				return clips.error();
			}
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
	// The skeleton: which joint each node is, and every joint's model-space transform as it rests.
	std::map<const cgltf_node*, int> joint_of_;
	std::vector<Mat4> rest_model_;
	std::set<const cgltf_skin*> used_skins_;
	// Bones of the palette: by skin and joint of that skin, and by joint for rigid parts (-1: the model itself).
	std::map<std::pair<const cgltf_skin*, cgltf_size>, std::uint16_t> skin_bindings_;
	std::map<int, std::uint16_t> rigid_bindings_;

	// A skeleton exists when a mesh in the scene is skinned or a node in it is animated. It holds those joints and
	// nodes and every node above them, so each joint's model transform is the product of its chain.
	Result<void> build_skeleton(const std::vector<const cgltf_node*>& nodes)
	{
		const std::set<const cgltf_node*> in_scene(nodes.begin(), nodes.end());
		std::set<const cgltf_node*> members;
		for (const cgltf_node* node : nodes) {
			if (node->mesh && node->skin && used_skins_.insert(node->skin).second) {
				members.insert(node->skin->joints, node->skin->joints + node->skin->joints_count);
			}
		}
		for (cgltf_size a = 0; a < data_.animations_count; ++a) {
			const cgltf_animation& animation = data_.animations[a];
			for (cgltf_size c = 0; c < animation.channels_count; ++c) {
				const cgltf_animation_channel& channel = animation.channels[c];
				if (channel.target_node && in_scene.contains(channel.target_node) && channel.target_path != cgltf_animation_path_type_weights &&
					channel.target_path != cgltf_animation_path_type_invalid) {
					members.insert(channel.target_node);
				}
			}
		}
		if (members.empty()) {
			return {};
		}
		for (const cgltf_node* node : std::vector<const cgltf_node*>(members.begin(), members.end())) {
			for (const cgltf_node* above = node->parent; above; above = above->parent) {
				members.insert(above);
			}
		}
		if (members.size() > max_skeleton_joints) {
			return fail(ErrorCode::InvalidArgument, "the model's skeleton has " + std::to_string(members.size()) + " joints; at most " +
				std::to_string(max_skeleton_joints) + " are supported");
		}
		// Depth first from the roots, in scene order, then any joints outside the scene.
		std::vector<const cgltf_node*> roots;
		for (const cgltf_node* node : nodes) {
			if (!node->parent && members.contains(node)) {
				roots.push_back(node);
			}
		}
		for (cgltf_size i = 0; i < data_.nodes_count; ++i) {
			const cgltf_node* node = &data_.nodes[i];
			if (!node->parent && members.contains(node) && std::find(roots.begin(), roots.end(), node) == roots.end()) {
				roots.push_back(node);
			}
		}
		std::set<std::string> names;
		bool renamed = false;
		std::vector<const cgltf_node*> stack(roots.rbegin(), roots.rend());
		while (!stack.empty()) {
			const cgltf_node* node = stack.back();
			stack.pop_back();
			if (joint_of_.contains(node)) {
				continue;
			}
			Joint joint;
			const std::string base = node->name && *node->name ? node->name : "joint " + std::to_string(node - data_.nodes);
			joint.name = base;
			for (int n = 2; !names.insert(joint.name).second; ++n) {
				joint.name = base + " " + std::to_string(n);
				renamed = true;
			}
			joint.parent = node->parent ? joint_of_.at(node->parent) : -1;
			local_transform(*node, joint);
			for (float& t : joint.translation) {
				t *= settings_.mesh_scale;
			}
			joint_of_.emplace(node, static_cast<int>(mesh_.skin.joints.size()));
			mesh_.skin.joints.push_back(std::move(joint));
			for (cgltf_size i = node->children_count; i > 0; --i) {
				if (members.contains(node->children[i - 1])) {
					stack.push_back(node->children[i - 1]);
				}
			}
		}
		if (renamed) {
			warnings_.insert("some joints share a name; the repeats were numbered");
		}
		rest_model_ = rest_pose(mesh_.skin);
		return {};
	}

	// The nearest joint at or above a node, or -1.
	int joint_above(const cgltf_node* node) const
	{
		for (; node; node = node->parent) {
			if (const auto found = joint_of_.find(node); found != joint_of_.end()) {
				return found->second;
			}
		}
		return -1;
	}

	Result<std::uint16_t> add_binding(SkinBinding binding)
	{
		if (mesh_.skin.bindings.size() >= max_skin_bindings) {
			return fail(ErrorCode::InvalidArgument, "the model needs more than " + std::to_string(max_skin_bindings) +
				" bones to draw, which is more than the renderer supports");
		}
		mesh_.skin.bindings.push_back(binding);
		return static_cast<std::uint16_t>(mesh_.skin.bindings.size() - 1);
	}

	Result<std::uint16_t> skin_binding(const cgltf_skin& skin, cgltf_size joint)
	{
		const auto key = std::make_pair(&skin, joint);
		if (const auto found = skin_bindings_.find(key); found != skin_bindings_.end()) {
			return found->second;
		}
		SkinBinding binding;
		binding.joint = joint_of_.at(skin.joints[joint]);
		if (skin.inverse_bind_matrices) {
			Mat4 m{};
			if (!cgltf_accessor_read_float(skin.inverse_bind_matrices, joint, m.data(), 16) ||
				!std::all_of(m.begin(), m.end(), [](float v) { return std::isfinite(v); })) {
				return fail(ErrorCode::ParseError, "a skin's inverse bind matrices cannot be read");
			}
			// Uniform scaling commutes with everything but translations, so scaling the model scales those alone.
			for (std::size_t k = 12; k < 15; ++k) {
				m[k] *= settings_.mesh_scale;
			}
			binding.inverse_bind = m;
		}
		auto index = add_binding(binding);
		if (index) {
			skin_bindings_.emplace(key, index.value());
		}
		return index;
	}

	// A bone for vertices baked as they rest that follow one joint rigidly: it undoes the joint's rest transform.
	Result<std::uint16_t> rigid_binding(int joint)
	{
		if (const auto found = rigid_bindings_.find(joint); found != rigid_bindings_.end()) {
			return found->second;
		}
		SkinBinding binding;
		binding.joint = joint;
		if (joint >= 0) {
			binding.inverse_bind = inverse_affine(rest_model_[static_cast<std::size_t>(joint)]);
		}
		auto index = add_binding(binding);
		if (index) {
			rigid_bindings_.emplace(joint, index.value());
		}
		return index;
	}

	Result<void> add_rigid(int joint, std::size_t base, std::size_t count)
	{
		auto binding = rigid_binding(joint);
		if (!binding) {
			return binding.error();
		}
		VertexInfluence influence;
		influence.binding = {binding.value(), 0, 0, 0};
		mesh_.skin.influences.resize(base + count, influence);
		return {};
	}

	// JOINTS_n and WEIGHTS_n, every set, merged into the 4 strongest influences per vertex.
	Result<void> add_weights(const cgltf_primitive& primitive, const cgltf_skin& skin, std::size_t base, std::size_t count)
	{
		std::vector<std::pair<const cgltf_accessor*, const cgltf_accessor*>> sets;
		for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
			const cgltf_attribute& joints = primitive.attributes[i];
			if (joints.type != cgltf_attribute_type_joints) {
				continue;
			}
			for (cgltf_size j = 0; j < primitive.attributes_count; ++j) {
				const cgltf_attribute& weights = primitive.attributes[j];
				if (weights.type == cgltf_attribute_type_weights && weights.index == joints.index) {
					sets.emplace_back(joints.data, weights.data);
				}
			}
		}
		for (const auto& [joints, weights] : sets) {
			if (joints->type != cgltf_type_vec4 || weights->type != cgltf_type_vec4 || joints->count != count || weights->count != count) {
				return fail(ErrorCode::ParseError, "a skinned primitive's JOINTS or WEIGHTS does not match its POSITION");
			}
		}
		mesh_.skin.influences.resize(base + count);
		bool trimmed = false, unweighted = false;
		std::vector<std::pair<float, cgltf_uint>> found; // Weight, joint of the skin.
		for (std::size_t v = 0; v < count; ++v) {
			found.clear();
			for (const auto& [joints, weights] : sets) {
				cgltf_uint j[4]{};
				float w[4]{};
				if (!cgltf_accessor_read_uint(joints, v, j, 4) || !cgltf_accessor_read_float(weights, v, w, 4)) {
					return fail(ErrorCode::ParseError, "a skinned primitive's joint weights cannot be read");
				}
				for (std::size_t k = 0; k < 4; ++k) {
					if (!std::isfinite(w[k])) {
						return fail(ErrorCode::ParseError, "a skinned primitive has non-finite joint weights");
					}
					if (w[k] <= 0.0f) {
						continue;
					}
					if (j[k] >= skin.joints_count) {
						return fail(ErrorCode::ParseError, "a skinned primitive names a joint its skin does not have");
					}
					const auto same = std::find_if(found.begin(), found.end(), [&](const auto& f) { return f.second == j[k]; });
					if (same != found.end()) {
						same->first += w[k];
					} else {
						found.emplace_back(w[k], j[k]);
					}
				}
			}
			std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			if (found.size() > 4) {
				found.resize(4);
				trimmed = true;
			}
			float total = 0.0f;
			for (const auto& f : found) {
				total += f.first;
			}
			VertexInfluence& influence = mesh_.skin.influences[base + v];
			if (found.empty() || !(total > 0.0f) || !std::isfinite(total)) {
				auto root = rigid_binding(-1);
				if (!root) {
					return root.error();
				}
				influence.binding = {root.value(), 0, 0, 0};
				influence.weight = {1, 0, 0, 0};
				unweighted = true;
				continue;
			}
			// Bones exist only for the influences kept, so dropped ones take no place in the palette.
			for (std::size_t k = 0; k < 4; ++k) {
				std::uint16_t binding = 0;
				if (k < found.size()) {
					auto made = skin_binding(skin, found[k].second);
					if (!made) {
						return made.error();
					}
					binding = made.value();
				}
				influence.binding[k] = binding;
				influence.weight[k] = k < found.size() ? found[k].first / total : 0.0f;
			}
		}
		if (trimmed) {
			warnings_.insert("vertices with more than 4 joint influences keep their 4 strongest");
		}
		if (unweighted) {
			warnings_.insert("some skinned vertices have no joint weights; they stay where they were bound");
		}
		return {};
	}

	// Each bone's box around the vertices it moves, as stored, for culling any pose.
	void bind_bounds()
	{
		std::vector<bool> seen(mesh_.skin.bindings.size(), false);
		for (std::size_t v = 0; v < mesh_.vertices.size() && v < mesh_.skin.influences.size(); ++v) {
			const VertexInfluence& influence = mesh_.skin.influences[v];
			for (std::size_t k = 0; k < 4; ++k) {
				if (influence.weight[k] <= 0.0f) {
					continue;
				}
				const std::size_t b = influence.binding[k];
				Bounds& box = mesh_.skin.bindings[b].bounds;
				const auto& p = mesh_.vertices[v].position;
				for (std::size_t c = 0; c < 3; ++c) {
					box.min[c] = seen[b] ? std::min(box.min[c], p[c]) : p[c];
					box.max[c] = seen[b] ? std::max(box.max[c], p[c]) : p[c];
				}
				seen[b] = true;
			}
		}
	}

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

	static bool has_attribute(const cgltf_primitive& primitive, cgltf_attribute_type type) noexcept
	{
		for (cgltf_size i = 0; i < primitive.attributes_count; ++i) {
			if (primitive.attributes[i].type == type && primitive.attributes[i].index == 0) {
				return true;
			}
		}
		return false;
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
			// glTF places a skinned mesh by its joints alone, so its own node's transform does not apply.
			const bool skinned = node.skin && !mesh_.skin.empty() && has_attribute(primitive, cgltf_attribute_type_joints) &&
				has_attribute(primitive, cgltf_attribute_type_weights);
			if (auto added = add_primitive(primitive, skinned ? identity_matrix : world, skinned ? node.skin : nullptr, joint_above(&node)); !added) {
				return added;
			}
		}
		return {};
	}

	// `skin`: the primitive's vertices are in bind space and weighted to its joints. Otherwise, in a model with a
	// skeleton, they are baked with `world` and follow `rigid_joint` (-1: none).
	Result<void> add_primitive(const cgltf_primitive& primitive, const Mat4& world, const cgltf_skin* skin, int rigid_joint)
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
		if (!mesh_.skin.empty()) {
			auto bound = skin ? add_weights(primitive, *skin, base, count) : add_rigid(rigid_joint, base, count);
			if (!bound) {
				return bound;
			}
		}

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

	// Times and values of one sampler as linear keys. False, with a reason, when the sampler cannot be used.
	template <std::size_t N>
	bool sample(const cgltf_animation_sampler& sampler, bool rotation, Sampled<N>& out, std::string& reason)
	{
		(void)rotation; // Only rotations, which have 4 components, are normalized.
		const cgltf_accessor* input = sampler.input;
		const cgltf_accessor* output = sampler.output;
		const cgltf_type wanted = N == 4 ? cgltf_type_vec4 : cgltf_type_vec3;
		const bool cubic = sampler.interpolation == cgltf_interpolation_type_cubic_spline;
		if (!input || !output || input->type != cgltf_type_scalar || output->type != wanted || input->count == 0 ||
			output->count != input->count * (cubic ? 3 : 1)) {
			reason = "a channel's keys do not match its times";
			return false;
		}
		std::vector<float> times(input->count), values(output->count * N);
		if (cgltf_accessor_unpack_floats(input, times.data(), times.size()) != times.size() ||
			cgltf_accessor_unpack_floats(output, values.data(), values.size()) != values.size()) {
			reason = "its keys cannot be read";
			return false;
		}
		for (std::size_t i = 0; i < times.size(); ++i) {
			if (!std::isfinite(times[i]) || times[i] < 0.0f || (i > 0 && !(times[i] > times[i - 1]))) {
				reason = "its key times are not increasing";
				return false;
			}
		}
		if (!std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); })) {
			reason = "it has non-finite keys";
			return false;
		}
		const auto at = [&](std::size_t element) {
			std::array<float, N> v{};
			std::copy_n(values.begin() + static_cast<std::ptrdiff_t>(element * N), N, v.begin());
			return v;
		};
		const auto finish = [&](std::array<float, N> v) {
			if constexpr (N == 4) {
				if (rotation) {
					v = normalized_quaternion(v);
				}
			}
			return v;
		};
		const std::size_t n = times.size();
		if (sampler.interpolation == cgltf_interpolation_type_step) {
			// Held until a moment before the next key, where linear interpolation then jumps.
			for (std::size_t i = 0; i < n; ++i) {
				out.times.push_back(times[i]);
				out.values.push_back(finish(at(i)));
				if (i + 1 < n) {
					const float gap = times[i + 1] - times[i];
					out.times.push_back(times[i + 1] - std::min(0.001f, 0.25f * gap));
					out.values.push_back(finish(at(i)));
				}
			}
		} else if (cubic) {
			// Each key is (in-tangent, value, out-tangent). Every segment is sampled at 30 Hz or finer, keeping the keys.
			for (std::size_t i = 0; i < n; ++i) {
				out.times.push_back(times[i]);
				out.values.push_back(finish(at(i * 3 + 1)));
				if (i + 1 == n) {
					break;
				}
				const float span = times[i + 1] - times[i];
				const auto steps = static_cast<std::size_t>(std::ceil(span * 30.0f - 1.0e-4f));
				for (std::size_t s = 1; s < steps; ++s) {
					const float f = static_cast<float>(s) / static_cast<float>(steps);
					out.times.push_back(times[i] + span * f);
					out.values.push_back(finish(hermite<N>(at(i * 3 + 1), at(i * 3 + 2), at((i + 1) * 3), at((i + 1) * 3 + 1), span, f)));
				}
			}
		} else {
			for (std::size_t i = 0; i < n; ++i) {
				out.times.push_back(times[i]);
				out.values.push_back(finish(at(i)));
			}
		}
		return true;
	}

	// Every glTF animation that moves the skeleton becomes a clip. One that cannot be read is skipped with a warning.
	Result<void> add_clips()
	{
		std::set<std::string> names;
		std::size_t keys = 0;
		for (cgltf_size a = 0; a < data_.animations_count; ++a) {
			const cgltf_animation& animation = data_.animations[a];
			AnimationClip clip;
			const std::string base = animation.name && *animation.name ? animation.name : "Animation " + std::to_string(a + 1);
			clip.name = base;
			for (int n = 2; names.contains(clip.name); ++n) {
				clip.name = base + " " + std::to_string(n);
			}
			clip.tracks.resize(mesh_.skin.joints.size());
			bool usable = false;
			std::string reason;
			for (cgltf_size c = 0; c < animation.channels_count && reason.empty(); ++c) {
				const cgltf_animation_channel& channel = animation.channels[c];
				if (channel.target_path == cgltf_animation_path_type_weights) {
					warnings_.insert("morph target animations are ignored");
					continue;
				}
				const auto joint = channel.target_node ? joint_of_.find(channel.target_node) : joint_of_.end();
				if (joint == joint_of_.end() || !channel.sampler || channel.target_path == cgltf_animation_path_type_invalid) {
					continue;
				}
				JointTrack& track = clip.tracks[static_cast<std::size_t>(joint->second)];
				if (channel.target_path == cgltf_animation_path_type_rotation) {
					Sampled<4> sampled;
					if (!sample(*channel.sampler, true, sampled, reason)) {
						break;
					}
					track.rotation.clear();
					for (std::size_t i = 0; i < sampled.times.size(); ++i) {
						track.rotation.push_back({sampled.times[i], sampled.values[i]});
					}
				} else {
					Sampled<3> sampled;
					if (!sample(*channel.sampler, false, sampled, reason)) {
						break;
					}
					const bool translation = channel.target_path == cgltf_animation_path_type_translation;
					auto& keys_out = translation ? track.translation : track.scale;
					keys_out.clear();
					for (std::size_t i = 0; i < sampled.times.size(); ++i) {
						auto value = sampled.values[i];
						if (translation) {
							for (float& v : value) {
								v *= settings_.mesh_scale;
							}
						}
						keys_out.push_back({sampled.times[i], value});
					}
				}
				usable = true;
			}
			if (!reason.empty()) {
				warnings_.insert("animation \"" + clip.name + "\" is skipped: " + reason);
				continue;
			}
			if (!usable) {
				warnings_.insert("animation \"" + clip.name + "\" moves no joint and is skipped");
				continue;
			}
			for (const JointTrack& track : clip.tracks) {
				keys += track.translation.size() + track.rotation.size() + track.scale.size();
				for (const auto* channel : {&track.translation, &track.scale}) {
					if (!channel->empty()) {
						clip.duration = std::max(clip.duration, channel->back().time);
					}
				}
				if (!track.rotation.empty()) {
					clip.duration = std::max(clip.duration, track.rotation.back().time);
				}
			}
			if (keys > limits_.max_animation_keys) {
				return fail(ErrorCode::InvalidArgument, "the model's animations exceed the key limit");
			}
			names.insert(clip.name);
			mesh_.clips.push_back(std::move(clip));
		}
		return {};
	}

	void report_ignored_features()
	{
		if (data_.skins_count > used_skins_.size()) {
			warnings_.insert("skins that no mesh in the scene uses are ignored");
		}
		if (mesh_.skin.empty() && data_.animations_count > 0) {
			warnings_.insert("animations that move nothing in the scene are ignored");
		}
		if (data_.cameras_count > 0 || data_.lights_count > 0) {
			warnings_.insert("cameras and lights in the file are ignored");
		}
	}

	// As the model rests: skinned vertices where the skeleton's rest pose puts them.
	void compute_bounds()
	{
		const std::vector<Mat4> palette = mesh_.skin.empty() ? std::vector<Mat4>{} : skinning_palette(mesh_.skin, rest_model_);
		Bounds& b = mesh_.bounds;
		for (std::size_t i = 0; i < mesh_.vertices.size(); ++i) {
			const auto p = palette.empty() ? mesh_.vertices[i].position : skinned_position(mesh_.skin, palette, mesh_.vertices[i], i);
			for (std::size_t k = 0; k < 3; ++k) {
				b.min[k] = i == 0 ? p[k] : std::min(b.min[k], p[k]);
				b.max[k] = i == 0 ? p[k] : std::max(b.max[k], p[k]);
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
