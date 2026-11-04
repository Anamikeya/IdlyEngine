// Milestone 5 slice 2: skins, skeletons and clips from GLB files built in memory, and the skeleton math they use.

#include "engine/assets/asset_database.h"
#include "engine/assets/importers.h"
#include "engine/assets/skeleton.h"
#include "engine/test/test.h"
#include "glb_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace engine::assets;
using engine::test::Glb;
namespace fs = std::filesystem;

namespace {

const std::vector<float> identity16(identity_matrix.begin(), identity_matrix.end());

// A bar from y = 0 to y = 2, x from 0 to 1, facing +Z. Joint "root" at the origin and "tip" a metre up, bound as they
// rest. The bottom row follows root, the top row tip, the middle both equally. The mesh node's own translation must
// not apply. `armature` puts both joints under a node that is not a joint.
struct Bar {
	bool armature = false;
	// Adds `,"animations":[...]` built over the same buffer.
	std::function<std::string(Glb&)> animations;
};

std::vector<std::uint8_t> bar_glb(const Bar& bar = {})
{
	Glb g;
	const int positions = g.floats({0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 0, 2, 0, 1, 2, 0}, 3, true);
	const int indices = g.u16({0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4}, 1);
	const int joints = g.u16({0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, 4);
	const int weights = g.floats({1, 0, 0, 0, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0, 0.5f, 0.5f, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, 4);
	std::vector<float> ibm = identity16;
	ibm.insert(ibm.end(), {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1});
	const int inverse_binds = g.floats(ibm, 16);
	const std::string animations = bar.animations ? bar.animations(g) : "";
	std::string nodes = R"({"name":"root","children":[1]},{"name":"tip","translation":[0,1,0]},{"name":"bar","mesh":0,"skin":0,"translation":[5,0,0]})";
	std::string roots = "[0,2]";
	if (bar.armature) {
		nodes += R"(,{"name":"Armature","children":[0],"translation":[0,0,-2],"rotation":[0,0.707106781,0,0.707106781]})";
		roots = "[3,2]";
	}
	return g.build(R"("scene":0,"scenes":[{"nodes":)" + roots + R"(}],"nodes":[)" + nodes +
		R"(],"meshes":[{"primitives":[{"attributes":{"POSITION":)" + std::to_string(positions) + R"(,"JOINTS_0":)" + std::to_string(joints) +
		R"(,"WEIGHTS_0":)" + std::to_string(weights) + R"(},"indices":)" + std::to_string(indices) + R"(}]}],"skins":[{"joints":[0,1],"inverseBindMatrices":)" +
		std::to_string(inverse_binds) + "}]" + animations);
}

// One triangle weighted by `joints` and `weights` (two VEC4 sets when 8 values each) to a skin of `skin_joints` joints, all roots.
std::vector<std::uint8_t> weighted_triangle(std::size_t skin_joints, const std::vector<std::uint16_t>& joints, const std::vector<float>& weights)
{
	Glb g;
	const int positions = g.floats({0, 0, 0, 1, 0, 0, 0, 1, 0}, 3, true);
	std::string attributes = R"("POSITION":)" + std::to_string(positions);
	const std::size_t sets = joints.size() / 12;
	for (std::size_t s = 0; s < sets; ++s) {
		std::vector<std::uint16_t> j;
		std::vector<float> w;
		for (int v = 0; v < 3; ++v) {
			j.insert(j.end(), joints.begin() + static_cast<std::ptrdiff_t>(s * 4 + v * sets * 4), joints.begin() + static_cast<std::ptrdiff_t>(s * 4 + v * sets * 4 + 4));
			w.insert(w.end(), weights.begin() + static_cast<std::ptrdiff_t>(s * 4 + v * sets * 4), weights.begin() + static_cast<std::ptrdiff_t>(s * 4 + v * sets * 4 + 4));
		}
		attributes += R"(,"JOINTS_)" + std::to_string(s) + R"(":)" + std::to_string(g.u16(j, 4));
		attributes += R"(,"WEIGHTS_)" + std::to_string(s) + R"(":)" + std::to_string(g.floats(w, 4));
	}
	std::string nodes, list, roots;
	for (std::size_t i = 0; i < skin_joints; ++i) {
		nodes += R"({"name":"j)" + std::to_string(i) + R"("},)";
		list += (i ? "," : "") + std::to_string(i);
		roots += std::to_string(i) + ",";
	}
	nodes += R"({"mesh":0,"skin":0})";
	roots += std::to_string(skin_joints);
	return g.build(R"("scene":0,"scenes":[{"nodes":[)" + roots + R"(]}],"nodes":[)" + nodes + R"(],"meshes":[{"primitives":[{"attributes":{)" +
		attributes + R"(}}]}],"skins":[{"joints":[)" + list + "]}]");
}

bool has_warning(const MeshData& mesh, std::string_view text)
{
	return std::any_of(mesh.warnings.begin(), mesh.warnings.end(), [&](const std::string& w) { return w.find(text) != std::string::npos; });
}

void check_point(const std::array<float, 3>& p, std::array<float, 3> expected, float tolerance = 1.0e-5f)
{
	ENGINE_CHECK_NEAR(p[0], expected[0], tolerance);
	ENGINE_CHECK_NEAR(p[1], expected[1], tolerance);
	ENGINE_CHECK_NEAR(p[2], expected[2], tolerance);
}

void check_error(const std::vector<std::uint8_t>& bytes, std::string_view text)
{
	const auto result = import_glb(bytes);
	ENGINE_CHECK(!result);
	if (!result) {
		ENGINE_CHECK(result.error().message.find(text) != std::string::npos);
	}
}

const std::array<float, 4> quarter_turn_z{0, 0, 0.707106781f, 0.707106781f};

} // namespace

ENGINE_TEST(skeleton_math_composes_inverts_and_poses)
{
	const Matrix4 m = compose({1, 2, 3}, {0.1f, 0.7f, -0.2f, 0.6f}, {2, 0.5f, 3});
	const Matrix4 round = multiply(inverse_affine(m), m);
	for (std::size_t i = 0; i < 16; ++i) {
		ENGINE_CHECK_NEAR(round[i], identity_matrix[i], 1.0e-5);
	}
	check_point(transform_point(compose({0, 1, 0}, quarter_turn_z, {1, 1, 1}), {1, 0, 0}), {0, 2, 0});
	// A box turned a quarter is the box around its turned corners.
	const Bounds turned = transform_bounds(compose({}, quarter_turn_z, {1, 1, 1}), {{0, 0, 0}, {2, 1, 1}});
	check_point(turned.min, {-1, 0, 0});
	check_point(turned.max, {0, 2, 1});
	ENGINE_CHECK(inverse_affine(Matrix4{}) == identity_matrix); // Singular: no inverse, no NaNs.
}

ENGINE_TEST(glb_skin_keeps_bind_space_vertices_and_their_weights)
{
	if (!gltf_import_available()) {
		return;
	}
	const auto result = import_glb(bar_glb());
	ENGINE_REQUIRE(static_cast<bool>(result));
	const MeshData& mesh = result.value();
	ENGINE_REQUIRE(mesh.skin.joints.size() == 2);
	ENGINE_CHECK_EQ(mesh.skin.joints[0].name, std::string("root"));
	ENGINE_CHECK_EQ(mesh.skin.joints[1].name, std::string("tip"));
	ENGINE_CHECK_EQ(mesh.skin.joints[0].parent, -1);
	ENGINE_CHECK_EQ(mesh.skin.joints[1].parent, 0);
	check_point(mesh.skin.joints[1].translation, {0, 1, 0});
	ENGINE_REQUIRE(mesh.skin.bindings.size() == 2);
	ENGINE_REQUIRE(mesh.skin.influences.size() == mesh.vertices.size());
	// The file's positions, not moved by the mesh node's translation.
	check_point(mesh.vertices[5].position, {1, 2, 0});
	check_point(mesh.bounds.min, {0, 0, 0});
	check_point(mesh.bounds.max, {1, 2, 0});
	const VertexInfluence bottom = mesh.skin.influences[0], middle = mesh.skin.influences[2], top = mesh.skin.influences[4];
	ENGINE_CHECK_EQ(mesh.skin.bindings[bottom.binding[0]].joint, 0);
	ENGINE_CHECK_NEAR(bottom.weight[0], 1.0, 1e-6);
	ENGINE_CHECK_NEAR(middle.weight[0], 0.5, 1e-6);
	ENGINE_CHECK_NEAR(middle.weight[1], 0.5, 1e-6);
	ENGINE_CHECK_EQ(mesh.skin.bindings[top.binding[0]].joint, 1);
	ENGINE_CHECK_NEAR(mesh.skin.bindings[top.binding[0]].inverse_bind[13], -1.0, 1e-6);
	// Each bone's box holds the vertices it moves.
	check_point(mesh.skin.bindings[top.binding[0]].bounds.min, {0, 1, 0});
	check_point(mesh.skin.bindings[top.binding[0]].bounds.max, {1, 2, 0});
	ENGINE_CHECK(mesh.clips.empty());
	ENGINE_CHECK(mesh.warnings.empty());

	// At rest the palette changes nothing; turning the tip a quarter about Z bends the top row round its joint.
	const auto rest = rest_pose(mesh.skin);
	const auto still = skinning_palette(mesh.skin, rest);
	for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
		check_point(skinned_position(mesh.skin, still, mesh.vertices[v], v), mesh.vertices[v].position);
	}
	std::vector<Matrix4> pose = rest;
	pose[1] = compose({0, 1, 0}, quarter_turn_z, {1, 1, 1});
	const auto bent = skinning_palette(mesh.skin, pose);
	check_point(skinned_position(mesh.skin, bent, mesh.vertices[5], 5), {-1, 2, 0});
	check_point(skinned_position(mesh.skin, bent, mesh.vertices[3], 3), {0.5f, 1.5f, 0});
	check_point(skinned_position(mesh.skin, bent, mesh.vertices[1], 1), {1, 0, 0});
	const Bounds box = skinned_bounds(mesh.skin, bent);
	for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
		const auto p = skinned_position(mesh.skin, bent, mesh.vertices[v], v);
		for (std::size_t k = 0; k < 3; ++k) {
			ENGINE_CHECK(p[k] >= box.min[k] - 1e-5f && p[k] <= box.max[k] + 1e-5f);
		}
	}
}

ENGINE_TEST(glb_skin_includes_the_nodes_above_its_joints_and_scales_with_the_model)
{
	if (!gltf_import_available()) {
		return;
	}
	Bar bar;
	bar.armature = true;
	const auto result = import_glb(bar_glb(bar));
	ENGINE_REQUIRE(static_cast<bool>(result));
	const MeshData& mesh = result.value();
	ENGINE_REQUIRE(mesh.skin.joints.size() == 3);
	ENGINE_CHECK_EQ(mesh.skin.joints[0].name, std::string("Armature"));
	ENGINE_CHECK_EQ(mesh.skin.joints[1].parent, 0);
	ENGINE_CHECK_EQ(mesh.skin.joints[2].parent, 1);
	// Bound without the armature, the bar rests turned a quarter about Y and moved 2 m back: the rest pose places it.
	check_point(mesh.vertices[5].position, {1, 2, 0});
	check_point(mesh.bounds.min, {0, 0, -3});
	check_point(mesh.bounds.max, {0, 2, -2});

	ImportSettings doubled;
	doubled.mesh_scale = 2;
	const auto scaled = import_glb(bar_glb(bar), doubled);
	ENGINE_REQUIRE(static_cast<bool>(scaled));
	const MeshData& big = scaled.value();
	check_point(big.vertices[5].position, {2, 4, 0});
	check_point(big.skin.joints[0].translation, {0, 0, -4});
	check_point(big.bounds.min, {0, 0, -6});
	check_point(big.bounds.max, {0, 4, -4});
	const auto palette = skinning_palette(big.skin, rest_pose(big.skin));
	check_point(skinned_position(big.skin, palette, big.vertices[5], 5), {0, 4, -6});
}

ENGINE_TEST(glb_skin_keeps_the_four_strongest_influences)
{
	if (!gltf_import_available()) {
		return;
	}
	// Per vertex: JOINTS_0 0 1 2 3 with 0.1 0.3 0.05 0.2, JOINTS_1 4 5 2 0 with 0.25 0.1 0.1 0. Joint 2 adds to 0.15.
	std::vector<std::uint16_t> joints;
	std::vector<float> weights;
	for (int v = 0; v < 3; ++v) {
		joints.insert(joints.end(), {0, 1, 2, 3, 4, 5, 2, 0});
		weights.insert(weights.end(), {0.1f, 0.3f, 0.05f, 0.2f, 0.25f, 0.1f, 0.1f, 0});
	}
	const auto result = import_glb(weighted_triangle(6, joints, weights));
	ENGINE_REQUIRE(static_cast<bool>(result));
	const MeshData& mesh = result.value();
	ENGINE_CHECK(has_warning(mesh, "4 strongest"));
	ENGINE_REQUIRE(mesh.skin.bindings.size() == 4); // The two dropped influences take no bone.
	const VertexInfluence& kept = mesh.skin.influences[0];
	const int expected_joints[4]{1, 4, 3, 2};
	const float expected_weights[4]{0.3f / 0.9f, 0.25f / 0.9f, 0.2f / 0.9f, 0.15f / 0.9f};
	float total = 0;
	for (std::size_t k = 0; k < 4; ++k) {
		ENGINE_CHECK_EQ(mesh.skin.bindings[kept.binding[k]].joint, expected_joints[k]);
		ENGINE_CHECK_NEAR(kept.weight[k], expected_weights[k], 1e-6);
		total += kept.weight[k];
	}
	ENGINE_CHECK_NEAR(total, 1.0, 1e-6);
	// No weights at all: the vertex stays where it was bound.
	const auto none = import_glb(weighted_triangle(1, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, std::vector<float>(12, 0.0f)));
	ENGINE_REQUIRE(static_cast<bool>(none));
	ENGINE_CHECK(has_warning(none.value(), "no joint weights"));
	ENGINE_CHECK_EQ(none.value().skin.bindings[0].joint, -1);
}

ENGINE_TEST(glb_parts_without_a_skin_follow_the_joint_above_them)
{
	if (!gltf_import_available()) {
		return;
	}
	// No skin: "tip" is animated, so it and "root" form a skeleton. "tri" hangs under tip; "still" is outside it.
	Glb g;
	const int positions = g.floats({0, 0, 0, 1, 0, 0, 0, 1, 0}, 3, true);
	const int times = g.floats({0, 1}, 1, true);
	const int moves = g.floats({0, 1, 0, 0, 2, 0}, 3);
	const auto bytes = g.build(R"("scene":0,"scenes":[{"nodes":[0,3]}],"nodes":[{"name":"root","children":[1]},)"
		R"({"name":"tip","translation":[0,1,0],"children":[2]},{"name":"tri","mesh":0,"translation":[1,0,0]},{"name":"still","mesh":0}],)"
		R"("meshes":[{"primitives":[{"attributes":{"POSITION":)" + std::to_string(positions) + R"(}}]}],)"
		R"("animations":[{"channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}],"samplers":[{"input":)" +
		std::to_string(times) + R"(,"output":)" + std::to_string(moves) + "}]}]");
	const auto result = import_glb(bytes);
	ENGINE_REQUIRE(static_cast<bool>(result));
	const MeshData& mesh = result.value();
	ENGINE_REQUIRE(mesh.skin.joints.size() == 2);
	ENGINE_REQUIRE(mesh.vertices.size() == 6);
	ENGINE_REQUIRE(mesh.skin.bindings.size() == 2);
	// Baked as they rest, as before skeletons existed. "still" is a scene root, so its vertices come first.
	check_point(mesh.vertices[0].position, {0, 0, 0});
	check_point(mesh.vertices[3].position, {1, 1, 0});
	ENGINE_CHECK_EQ(mesh.skin.bindings[mesh.skin.influences[0].binding[0]].joint, -1);
	ENGINE_CHECK_EQ(mesh.skin.bindings[mesh.skin.influences[3].binding[0]].joint, 1);
	ENGINE_REQUIRE(mesh.clips.size() == 1);
	ENGINE_CHECK_EQ(mesh.clips[0].name, std::string("Animation 1"));
	ENGINE_CHECK_NEAR(mesh.clips[0].duration, 1.0, 1e-6);
	ENGINE_REQUIRE(mesh.clips[0].tracks.size() == 2);
	ENGINE_CHECK_EQ(mesh.clips[0].tracks[1].translation.size(), std::size_t{2});
	ENGINE_CHECK(mesh.clips[0].tracks[0].translation.empty());
	// Raising the tip a metre raises the part under it, and only that part.
	auto pose = rest_pose(mesh.skin);
	pose[1] = compose({0, 2, 0}, {0, 0, 0, 1}, {1, 1, 1});
	const auto palette = skinning_palette(mesh.skin, pose);
	check_point(skinned_position(mesh.skin, palette, mesh.vertices[3], 3), {1, 2, 0});
	check_point(skinned_position(mesh.skin, palette, mesh.vertices[2], 2), {0, 1, 0});
}

ENGINE_TEST(glb_animations_become_clips_with_linear_keys)
{
	if (!gltf_import_available()) {
		return;
	}
	Bar bar;
	bar.animations = [](Glb& g) {
		const int t0 = g.floats({0, 1}, 1, true), t1 = g.floats({0, 0.5f, 1}, 1, true), t2 = g.floats({0, 0.2f}, 1, true);
		const int moves = g.floats({0, 1, 0, 0, 3, 0}, 3);
		const int turns = g.floats({0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 1}, 4); // The middle key is not unit length.
		const int grows = g.floats({0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 2, 2, 2, 0, 0, 0}, 3); // In-tangent, value, out-tangent.
		const int backwards = g.floats({1, 0.5f}, 1);
		const std::string walk = R"({"name":"Walk","channels":[{"sampler":0,"target":{"node":1,"path":"translation"}},)"
			R"({"sampler":1,"target":{"node":1,"path":"rotation"}},{"sampler":2,"target":{"node":0,"path":"scale"}}],"samplers":[)"
			R"({"input":)" + std::to_string(t0) + R"(,"output":)" + std::to_string(moves) + R"(},)"
			R"({"input":)" + std::to_string(t1) + R"(,"output":)" + std::to_string(turns) + R"(,"interpolation":"STEP"},)"
			R"({"input":)" + std::to_string(t2) + R"(,"output":)" + std::to_string(grows) + R"(,"interpolation":"CUBICSPLINE"}]})";
		const std::string unnamed = R"({"channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}],"samplers":[{"input":)" +
			std::to_string(t0) + R"(,"output":)" + std::to_string(moves) + "}]}";
		const std::string again = R"({"name":"Walk",)" + unnamed.substr(1);
		const std::string broken = R"({"name":"Broken","channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}],"samplers":[{"input":)" +
			std::to_string(backwards) + R"(,"output":)" + std::to_string(moves) + "}]}";
		return R"(,"animations":[)" + walk + "," + unnamed + "," + again + "," + broken + "]";
	};
	ImportSettings doubled;
	doubled.mesh_scale = 2;
	const auto result = import_glb(bar_glb(bar), doubled);
	ENGINE_REQUIRE(static_cast<bool>(result));
	const MeshData& mesh = result.value();
	ENGINE_REQUIRE(mesh.clips.size() == 3);
	ENGINE_CHECK_EQ(mesh.clips[0].name, std::string("Walk"));
	ENGINE_CHECK_EQ(mesh.clips[1].name, std::string("Animation 2"));
	ENGINE_CHECK_EQ(mesh.clips[2].name, std::string("Walk 2"));
	ENGINE_CHECK(has_warning(mesh, "\"Broken\" is skipped"));
	const AnimationClip& walk = mesh.clips[0];
	ENGINE_CHECK_NEAR(walk.duration, 1.0, 1e-6);
	ENGINE_REQUIRE(walk.tracks.size() == 2);
	// Linear: as stored, with translations scaled like the model.
	const JointTrack& tip = walk.tracks[1];
	ENGINE_REQUIRE(tip.translation.size() == 2);
	check_point(tip.translation[1].value, {0, 6, 0});
	// Step: each key held until a moment before the next, and rotations made unit length.
	ENGINE_REQUIRE(tip.rotation.size() == 5);
	ENGINE_CHECK_NEAR(tip.rotation[1].time, 0.499, 1e-6);
	ENGINE_CHECK(tip.rotation[1].value == tip.rotation[0].value);
	ENGINE_CHECK_NEAR(tip.rotation[2].time, 0.5, 1e-6);
	ENGINE_CHECK_NEAR(tip.rotation[2].value[2], 1.0, 1e-6);
	ENGINE_CHECK_NEAR(tip.rotation[3].time, 0.999, 1e-6);
	// Cubic spline: 0.2 s at 30 Hz is 6 steps; flat tangents ease in and out, so halfway is half the change.
	const JointTrack& root = walk.tracks[0];
	ENGINE_REQUIRE(root.scale.size() == 7);
	ENGINE_CHECK_NEAR(root.scale[3].time, 0.1, 1e-6);
	ENGINE_CHECK_NEAR(root.scale[3].value[0], 1.5, 1e-5);
	ENGINE_CHECK_NEAR(root.scale[1].value[0], 1.0 + (3.0 / 36 - 2.0 / 216), 1e-5);
	ENGINE_CHECK_NEAR(root.scale[6].value[1], 2.0, 1e-6);
	ENGINE_CHECK(root.translation.empty() && root.rotation.empty()); // Rest values, not keys.
}

ENGINE_TEST(glb_skins_refuse_what_cannot_be_drawn)
{
	if (!gltf_import_available()) {
		return;
	}
	check_error(weighted_triangle(2, {0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}), "joint its skin does not have");
	// 258 vertices, each wholly on a joint of its own: 258 bones.
	{
		Glb g;
		std::vector<float> p;
		std::vector<std::uint16_t> j, idx;
		std::vector<float> w;
		for (std::uint16_t v = 0; v < 258; ++v) {
			p.insert(p.end(), {float(v % 3), float(v / 3), 0});
			j.insert(j.end(), {v, 0, 0, 0});
			w.insert(w.end(), {1, 0, 0, 0});
			idx.push_back(v);
		}
		const int positions = g.floats(p, 3, true), joints = g.u16(j, 4), weights = g.floats(w, 4), indices = g.u16(idx, 1);
		std::string nodes, list, roots;
		for (int i = 0; i < 258; ++i) {
			nodes += R"({"name":"j)" + std::to_string(i) + R"("},)";
			list += (i ? "," : "") + std::to_string(i);
			roots += std::to_string(i) + ",";
		}
		check_error(g.build(R"("scene":0,"scenes":[{"nodes":[)" + roots + R"(258]}],"nodes":[)" + nodes +
			R"({"mesh":0,"skin":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":)" + std::to_string(positions) + R"(,"JOINTS_0":)" +
			std::to_string(joints) + R"(,"WEIGHTS_0":)" + std::to_string(weights) + R"(},"indices":)" + std::to_string(indices) +
			R"(}]}],"skins":[{"joints":[)" + list + "]}]"), "more than 256 bones");
	}
	check_error(weighted_triangle(1025, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}), "at most 1024");
	// A weight that is not a number cannot be drawn.
	check_error(weighted_triangle(1, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
		{std::numeric_limits<float>::quiet_NaN(), 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}), "non-finite joint weights");
}

ENGINE_TEST(glb_joint_matrices_are_split_into_translation_rotation_and_scale)
{
	if (!gltf_import_available()) {
		return;
	}
	// The tip as a matrix: a quarter turn about Z, scaled by 2, a metre up.
	Glb g;
	const int positions = g.floats({0, 0, 0, 1, 0, 0, 0, 1, 0}, 3, true);
	const int times = g.floats({0}, 1, true), still = g.floats({0, 0, 0, 1}, 4);
	const auto result = import_glb(g.build(R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"name":"tip","mesh":0,)"
		R"("matrix":[0,2,0,0,-2,0,0,0,0,0,2,0,0,1,0,1]}],"meshes":[{"primitives":[{"attributes":{"POSITION":)" + std::to_string(positions) +
		R"(}}]}],"animations":[{"channels":[{"sampler":0,"target":{"node":0,"path":"rotation"}}],"samplers":[{"input":)" +
		std::to_string(times) + R"(,"output":)" + std::to_string(still) + "}]}]"));
	ENGINE_REQUIRE(static_cast<bool>(result));
	const Joint& tip = result.value().skin.joints.at(0);
	check_point(tip.translation, {0, 1, 0});
	check_point(tip.scale, {2, 2, 2});
	ENGINE_CHECK_NEAR(tip.rotation[2], quarter_turn_z[2], 1e-6);
	ENGINE_CHECK_NEAR(tip.rotation[3], quarter_turn_z[3], 1e-6);
	ENGINE_CHECK_NEAR(result.value().clips.at(0).duration, 0.0, 1e-9); // A single pose.
}

ENGINE_TEST(glb_real_skinned_model_brings_its_skeleton_and_clips)
{
	if (!gltf_import_available()) {
		return;
	}
	const fs::path fox = fs::path(GAMEENGINE_TEST_RESOURCES_DIR) / "filament/third_party/models/Fox/Fox.glb";
	if (!fs::exists(fox)) {
		return;
	}
	std::ifstream in(fox, std::ios::binary);
	const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	const auto result = import_glb(bytes);
	ENGINE_REQUIRE(static_cast<bool>(result));
	const MeshData& mesh = result.value();
	// Its 24 skin joints and the "root" node above them.
	ENGINE_CHECK_EQ(mesh.skin.joints.size(), std::size_t{25});
	ENGINE_CHECK_EQ(mesh.skin.joints[0].name, std::string("root"));
	// A bone for each joint some vertex is weighted to: at most the skin's 24.
	ENGINE_CHECK(!mesh.skin.bindings.empty() && mesh.skin.bindings.size() <= 24);
	ENGINE_CHECK_EQ(mesh.skin.influences.size(), mesh.vertices.size());
	ENGINE_REQUIRE(mesh.clips.size() == 3);
	ENGINE_CHECK_EQ(mesh.clips[0].name, std::string("Survey"));
	ENGINE_CHECK_EQ(mesh.clips[1].name, std::string("Walk"));
	ENGINE_CHECK_EQ(mesh.clips[2].name, std::string("Run"));
	ENGINE_CHECK_NEAR(mesh.clips[1].duration, 0.7083333, 1e-5);
	ENGINE_CHECK(!has_warning(mesh, "skin"));
	ENGINE_CHECK(!has_warning(mesh, "animation"));
	// It stands on its feet, taller than it is wide.
	ENGINE_CHECK(mesh.bounds.max[1] - mesh.bounds.min[1] > 50.0f);
	ENGINE_CHECK(mesh.bounds.min[1] > -5.0f);
	// Every vertex is inside the box of its bones, at rest.
	const auto palette = skinning_palette(mesh.skin, rest_pose(mesh.skin));
	const Bounds box = skinned_bounds(mesh.skin, palette);
	for (std::size_t k = 0; k < 3; ++k) {
		ENGINE_CHECK(box.min[k] <= mesh.bounds.min[k] + 1e-3f && box.max[k] >= mesh.bounds.max[k] - 1e-3f);
	}
}

ENGINE_TEST(meta_files_keep_clip_events_sorted_and_checked)
{
	const auto parsed = parse_meta("version = 1\n"
		"event = 0.5 right \"Walk \\\"fast\\\"\"\n"
		"event = 0.25 left \"Walk \\\"fast\\\"\"\n"
		"# a comment\n"
		"event = 0 start \"Idle\"\n");
	ENGINE_REQUIRE(static_cast<bool>(parsed));
	const auto& events = parsed.value().events;
	ENGINE_REQUIRE(events.size() == 3);
	// By clip, then time.
	ENGINE_CHECK_EQ(events[0].clip, std::string("Idle"));
	ENGINE_CHECK_EQ(events[1].clip, std::string("Walk \"fast\""));
	ENGINE_CHECK_EQ(events[1].name, std::string("left"));
	ENGINE_CHECK_NEAR(events[2].time, 0.5, 1e-9);
	const std::string written = write_meta(parsed.value());
	ENGINE_CHECK(written.find("event = 0.25 left \"Walk \\\"fast\\\"\"\n") != std::string::npos);
	const auto again = parse_meta(written);
	ENGINE_REQUIRE(static_cast<bool>(again));
	ENGINE_CHECK(again.value().events == events);
	for (const char* bad : {"version = 1\nevent = -1 left \"Walk\"\n", "version = 1\nevent = 0.5 Left \"Walk\"\n",
			 "version = 1\nevent = 0.5 left \"Walk\n", "version = 1\nevent = 0.5 left\n", "version = 1\nevent = x left \"Walk\"\n",
			 "version = 1\nevent = 0.5 left \"\"\n", "version = 1\nevent = 0.5 left \"Walk\"\nevent = 0.5 left \"Walk\"\n",
			 "version = 1\nevent = 4000 left \"Walk\"\n"}) {
		ENGINE_CHECK(!parse_meta(bad));
	}
	ENGINE_CHECK(!normalize_clip_events({{"Walk", 0.1f, std::string(33, 'a')}}));
	ENGINE_CHECK(!normalize_clip_events(std::vector<ClipEvent>(max_clip_events + 1, ClipEvent{"Walk", 0, "a"})));
}

ENGINE_TEST(the_database_saves_clip_events_without_reimporting_the_model)
{
	const fs::path root = fs::current_path() / "asset_tests_tmp" / "clip_events";
	fs::remove_all(root);
	fs::create_directories(root / "chars");
	{
		const auto bytes = bar_glb();
		std::ofstream out(root / "chars/bar.glb", std::ios::binary);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		std::ofstream material(root / "plain.material", std::ios::binary);
		material << "ENGINE_MATERIAL 1\n";
	}
	auto opened = AssetDatabase::open(root);
	ENGINE_REQUIRE(static_cast<bool>(opened));
	auto& db = opened.value();
	ENGINE_REQUIRE(static_cast<bool>(db.scan()));
	const auto meta = db.find("chars/bar.glb");
	ENGINE_REQUIRE(meta.has_value());
	const AssetId id = meta->id;
	ENGINE_REQUIRE(static_cast<bool>(db.load_mesh(id)));
	const auto revision = db.revision(id);
	const std::vector<ClipEvent> events{{"Bend", 0.5f, "step"}, {"Bend", 0.25f, "lift"}};
	ENGINE_REQUIRE(static_cast<bool>(db.set_clip_events(id, events)));
	// Sorted, in the catalogue at once, and the model was not imported again.
	ENGINE_REQUIRE(db.find(id)->events.size() == 2);
	ENGINE_CHECK_EQ(std::string(db.find(id)->events[0].name), std::string("lift"));
	ENGINE_CHECK_EQ(db.revision(id), revision);
	ENGINE_CHECK(db.state(id) == AssetState::Loaded);
	// The id came from the path; it is written into the new .meta, so references keep working.
	ENGINE_CHECK(fs::exists(root / "chars/bar.glb.meta"));
	ENGINE_REQUIRE(static_cast<bool>(db.scan()));
	ENGINE_CHECK(db.find(id).has_value());
	ENGINE_CHECK_EQ(db.find(id)->events.size(), std::size_t{2});
	ENGINE_CHECK_EQ(db.revision(id), revision);
	// Moving the model takes its events along.
	ENGINE_REQUIRE(static_cast<bool>(db.move(id, "chars/moved.glb")));
	ENGINE_CHECK_EQ(db.find(id)->events.size(), std::size_t{2});
	// Only models have clip events, and bad events change nothing.
	ENGINE_CHECK(!db.set_clip_events(db.find("plain.material")->id, events));
	ENGINE_CHECK(!db.set_clip_events(id, {{"Bend", -1, "step"}}));
	ENGINE_CHECK_EQ(db.find(id)->events.size(), std::size_t{2});
	ENGINE_REQUIRE(static_cast<bool>(db.set_clip_events(id, {})));
	ENGINE_CHECK(db.find(id)->events.empty());
	fs::remove_all(root);
}
