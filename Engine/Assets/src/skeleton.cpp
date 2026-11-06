#include "engine/assets/skeleton.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::assets {

Matrix4 multiply(const Matrix4& a, const Matrix4& b) noexcept
{
	Matrix4 out{};
	for (std::size_t c = 0; c < 4; ++c) {
		for (std::size_t r = 0; r < 4; ++r) {
			out[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
		}
	}
	return out;
}

Matrix4 compose(const std::array<float, 3>& t, const std::array<float, 4>& rotation, const std::array<float, 3>& s) noexcept
{
	float x = rotation[0], y = rotation[1], z = rotation[2], w = rotation[3];
	const float length = std::sqrt(x * x + y * y + z * z + w * w);
	if (std::isfinite(length) && length > 1.0e-12f) {
		x /= length;
		y /= length;
		z /= length;
		w /= length;
	} else {
		x = y = z = 0.0f;
		w = 1.0f;
	}
	return {(1 - 2 * (y * y + z * z)) * s[0], 2 * (x * y + z * w) * s[0], 2 * (x * z - y * w) * s[0], 0,
		2 * (x * y - z * w) * s[1], (1 - 2 * (x * x + z * z)) * s[1], 2 * (y * z + x * w) * s[1], 0,
		2 * (x * z + y * w) * s[2], 2 * (y * z - x * w) * s[2], (1 - 2 * (x * x + y * y)) * s[2], 0, t[0], t[1], t[2], 1};
}

Matrix4 inverse_affine(const Matrix4& m) noexcept
{
	// Cofactors of the upper 3x3, transposed, over the determinant.
	const float c00 = m[5] * m[10] - m[6] * m[9], c01 = m[6] * m[8] - m[4] * m[10], c02 = m[4] * m[9] - m[5] * m[8];
	const float det = m[0] * c00 + m[1] * c01 + m[2] * c02;
	if (!std::isfinite(det) || std::abs(det) < 1.0e-30f) {
		return identity_matrix;
	}
	const float k = 1.0f / det;
	Matrix4 out{};
	out[0] = c00 * k;
	out[4] = c01 * k;
	out[8] = c02 * k;
	out[1] = (m[2] * m[9] - m[1] * m[10]) * k;
	out[5] = (m[0] * m[10] - m[2] * m[8]) * k;
	out[9] = (m[1] * m[8] - m[0] * m[9]) * k;
	out[2] = (m[1] * m[6] - m[2] * m[5]) * k;
	out[6] = (m[2] * m[4] - m[0] * m[6]) * k;
	out[10] = (m[0] * m[5] - m[1] * m[4]) * k;
	for (std::size_t r = 0; r < 3; ++r) {
		out[12 + r] = -(out[r] * m[12] + out[4 + r] * m[13] + out[8 + r] * m[14]);
	}
	out[15] = 1.0f;
	return out;
}

std::array<float, 3> transform_point(const Matrix4& m, const std::array<float, 3>& p) noexcept
{
	return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
		m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}

Bounds transform_bounds(const Matrix4& m, const Bounds& b) noexcept
{
	Bounds out;
	for (int i = 0; i < 8; ++i) {
		const std::array<float, 3> corner{i & 1 ? b.max[0] : b.min[0], i & 2 ? b.max[1] : b.min[1], i & 4 ? b.max[2] : b.min[2]};
		const auto p = transform_point(m, corner);
		for (std::size_t k = 0; k < 3; ++k) {
			out.min[k] = i == 0 ? p[k] : std::min(out.min[k], p[k]);
			out.max[k] = i == 0 ? p[k] : std::max(out.max[k], p[k]);
		}
	}
	return out;
}

std::vector<Matrix4> model_pose(const MeshSkin& skin, std::span<const Matrix4> local)
{
	std::vector<Matrix4> out(skin.joints.size(), identity_matrix);
	for (std::size_t i = 0; i < skin.joints.size() && i < local.size(); ++i) {
		const int parent = skin.joints[i].parent;
		out[i] = parent >= 0 && static_cast<std::size_t>(parent) < i ? multiply(out[static_cast<std::size_t>(parent)], local[i]) : local[i];
	}
	return out;
}

std::vector<Matrix4> rest_pose(const MeshSkin& skin)
{
	std::vector<Matrix4> local;
	local.reserve(skin.joints.size());
	for (const Joint& joint : skin.joints) {
		local.push_back(compose(joint.translation, joint.rotation, joint.scale));
	}
	return model_pose(skin, local);
}

std::vector<Matrix4> skinning_palette(const MeshSkin& skin, std::span<const Matrix4> pose)
{
	std::vector<Matrix4> palette;
	palette.reserve(skin.bindings.size());
	for (const SkinBinding& binding : skin.bindings) {
		const bool follows = binding.joint >= 0 && static_cast<std::size_t>(binding.joint) < pose.size();
		palette.push_back(follows ? multiply(pose[static_cast<std::size_t>(binding.joint)], binding.inverse_bind) : binding.inverse_bind);
	}
	return palette;
}

std::array<float, 3> skinned_position(const MeshSkin& skin, std::span<const Matrix4> palette, const MeshVertex& vertex,
	std::size_t index) noexcept
{
	if (index >= skin.influences.size()) {
		return vertex.position;
	}
	const VertexInfluence& influence = skin.influences[index];
	std::array<float, 3> out{};
	for (std::size_t k = 0; k < 4; ++k) {
		if (influence.weight[k] <= 0.0f || influence.binding[k] >= palette.size()) {
			continue;
		}
		const auto p = transform_point(palette[influence.binding[k]], vertex.position);
		for (std::size_t c = 0; c < 3; ++c) {
			out[c] += influence.weight[k] * p[c];
		}
	}
	return out;
}

Bounds skinned_bounds(const MeshSkin& skin, std::span<const Matrix4> palette) noexcept
{
	// Each skinned vertex is a weighted mean of its bindings' transforms of it, so it lies in the box around their boxes.
	Bounds out;
	bool any = false;
	for (std::size_t i = 0; i < skin.bindings.size() && i < palette.size(); ++i) {
		const Bounds b = transform_bounds(palette[i], skin.bindings[i].bounds);
		for (std::size_t k = 0; k < 3; ++k) {
			out.min[k] = any ? std::min(out.min[k], b.min[k]) : b.min[k];
			out.max[k] = any ? std::max(out.max[k], b.max[k]) : b.max[k];
		}
		any = true;
	}
	return out;
}

} // namespace engine::assets
