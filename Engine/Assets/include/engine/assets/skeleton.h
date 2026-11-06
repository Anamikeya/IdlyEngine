#pragma once

// Skeleton math on the plain asset types, shared by the importer, the renderer and the editor. Matrices are
// column-major. Poses are model-space joint transforms, one per joint of MeshSkin::joints, in the same order.

#include "engine/assets/asset_types.h"

#include <span>
#include <vector>

namespace engine::assets {

[[nodiscard]] Matrix4 multiply(const Matrix4& a, const Matrix4& b) noexcept;
// Translation * rotation * scale.
[[nodiscard]] Matrix4 compose(const std::array<float, 3>& translation, const std::array<float, 4>& rotation,
	const std::array<float, 3>& scale) noexcept;
// The inverse of an affine matrix (bottom row 0 0 0 1). A singular matrix gives the identity.
[[nodiscard]] Matrix4 inverse_affine(const Matrix4& m) noexcept;
[[nodiscard]] std::array<float, 3> transform_point(const Matrix4& m, const std::array<float, 3>& p) noexcept;
// The box around a box after transforming it.
[[nodiscard]] Bounds transform_bounds(const Matrix4& m, const Bounds& b) noexcept;

// Model-space transform of every joint as it rests.
[[nodiscard]] std::vector<Matrix4> rest_pose(const MeshSkin& skin);
// Model-space transforms from local ones, one per joint: parents first, as MeshSkin::joints are ordered.
[[nodiscard]] std::vector<Matrix4> model_pose(const MeshSkin& skin, std::span<const Matrix4> local);
// The bone matrices to draw with: joint_model[binding.joint] * binding.inverse_bind. The pose must have one
// matrix per joint; a binding to joint -1 gives its inverse bind matrix alone.
[[nodiscard]] std::vector<Matrix4> skinning_palette(const MeshSkin& skin, std::span<const Matrix4> pose);
// Where a vertex lands with that palette.
[[nodiscard]] std::array<float, 3> skinned_position(const MeshSkin& skin, std::span<const Matrix4> palette,
	const MeshVertex& vertex, std::size_t index) noexcept;
// A box holding every vertex for that palette, from each binding's own box. Never smaller than the exact one.
[[nodiscard]] Bounds skinned_bounds(const MeshSkin& skin, std::span<const Matrix4> palette) noexcept;

} // namespace engine::assets
