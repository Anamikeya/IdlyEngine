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
	Effect, // .effect: text, see parse_effect
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
	std::size_t max_animation_keys = std::size_t{1} << 22; // All clips of a model together, after resampling.
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

// Column-major 4x4, as glTF and the renderer store them.
using Matrix4 = std::array<float, 16>;
inline constexpr Matrix4 identity_matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// A joint of a model's skeleton: a node of the source file. The rest transform is relative to the parent joint.
struct Joint {
	std::string name;                          // Unique within the skeleton.
	int parent = -1;                           // An earlier joint, or -1 for a root.
	std::array<float, 3> translation{};
	std::array<float, 4> rotation{0, 0, 0, 1}; // Unit quaternion x, y, z, w.
	std::array<float, 3> scale{1, 1, 1};

	friend bool operator==(const Joint&, const Joint&) = default;
};

// One bone of the palette a skinned mesh is drawn with: the joint it follows, and the inverse of that joint's
// model-space transform when the vertices were bound. Joint -1 is the model itself: vertices that follow no joint.
// Its skinning matrix is joint_model[joint] * inverse_bind.
struct SkinBinding {
	int joint = -1;
	Matrix4 inverse_bind = identity_matrix;
	Bounds bounds; // Of the vertices it moves, as stored: a conservative box for any pose.

	friend bool operator==(const SkinBinding&, const SkinBinding&) = default;
};

// Up to 4 bindings per vertex. Weights are positive and sum to 1, strongest first; unused slots have weight 0.
struct VertexInfluence {
	std::array<std::uint16_t, 4> binding{};
	std::array<float, 4> weight{1, 0, 0, 0};

	friend bool operator==(const VertexInfluence&, const VertexInfluence&) = default;
};

// Filament draws at most this many bones per renderable.
inline constexpr std::size_t max_skin_bindings = 256;
inline constexpr std::size_t max_skeleton_joints = 1024;

// The skeleton of a model and how its vertices follow it. Empty for a model without skins or animations.
// Skinned vertices keep the bind-space positions of the file; unskinned parts are baked as they rest and follow the
// nearest joint above them rigidly. Linear blend skinning: position = sum of weight * skinning matrix * vertex.
struct MeshSkin {
	std::vector<Joint> joints;               // Parents before children.
	std::vector<SkinBinding> bindings;       // At most max_skin_bindings.
	std::vector<VertexInfluence> influences; // One per vertex of MeshData::vertices.

	[[nodiscard]] bool empty() const noexcept { return joints.empty(); }
	friend bool operator==(const MeshSkin&, const MeshSkin&) = default;
};

struct Vec3Key {
	float time = 0;
	std::array<float, 3> value{};
	friend bool operator==(const Vec3Key&, const Vec3Key&) = default;
};
struct RotationKey {
	float time = 0;
	std::array<float, 4> value{0, 0, 0, 1}; // Unit quaternion x, y, z, w.
	friend bool operator==(const RotationKey&, const RotationKey&) = default;
};
// Keys of one joint, linearly interpolated, times ascending. A channel without keys holds the joint's rest value.
struct JointTrack {
	std::vector<Vec3Key> translation;
	std::vector<RotationKey> rotation;
	std::vector<Vec3Key> scale;
	friend bool operator==(const JointTrack&, const JointTrack&) = default;
};
// An animation of the skeleton. STEP keys become pairs of keys a moment apart; CUBICSPLINE is sampled at 30 Hz.
struct AnimationClip {
	std::string name;              // Unique within the model.
	float duration = 0;            // Seconds: the last key's time. 0 for a single pose.
	std::vector<JointTrack> tracks; // One per joint of the skeleton.
	friend bool operator==(const AnimationClip&, const AnimationClip&) = default;
};

// Triangle list, counter-clockwise front faces, 32-bit indices.
struct MeshData {
	std::vector<MeshVertex> vertices;
	std::vector<std::uint32_t> indices;
	Bounds bounds; // As the model rests: skinned vertices are placed by the skeleton's rest pose.
	// GLB models: the parts and their embedded images. Empty for sources without materials (OBJ).
	std::vector<SubMesh> submeshes;
	std::vector<std::shared_ptr<const TextureData>> images;
	// GLB models with skins or animated nodes.
	MeshSkin skin;
	std::vector<AnimationClip> clips;
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

enum class ParticleBlend : std::uint8_t {
	Additive, // Adds light: sparks, flashes, glows. Needs no sorting.
	Alpha,    // Covers what is behind: smoke, dust. Sorted back to front within an emitter.
};

// A key of a curve over a particle's life; time is 0 at birth and 1 at death.
struct ParticleSizeKey {
	double time = 0.0;
	double size = 0.1; // Quad width in metres.
	friend bool operator==(const ParticleSizeKey&, const ParticleSizeKey&) = default;
};
struct ParticleColorKey {
	double time = 0.0;
	std::array<double, 4> color{1.0, 1.0, 1.0, 1.0}; // sRGB in [0, 1]; alpha is linear.
	friend bool operator==(const ParticleColorKey&, const ParticleColorKey&) = default;
};
inline constexpr std::size_t max_effect_keys = 8;
inline constexpr std::uint32_t max_effect_particles = 20000;

// A particle effect asset (.effect): how one emitter spawns, moves and draws its particles. Visual only.
// Directions are around the emitter's +Y axis, or the direction a one-shot effect is fired along.
struct EffectData {
	double duration = 1.0;                   // Seconds of one cycle; > 0.
	double rate = 30.0;                      // Particles per second while a cycle runs; >= 0.
	std::uint32_t burst = 0;                 // Particles at the start of each cycle.
	std::array<double, 2> lifetime{0.6, 1.0}; // Seconds, min and max; 0 < min <= max.
	std::array<double, 2> speed{1.0, 2.0};    // Metres per second, min and max; 0 <= min <= max.
	double cone_degrees = 20.0;              // Half angle of the spread; [0, 180]. 180 is every direction.
	double spawn_radius = 0.0;               // Particles start within a sphere this big; >= 0.
	std::array<double, 3> acceleration{0.0, -9.81, 0.0}; // World space, m/s^2. Gravity by default.
	double drag = 0.0;                       // Fraction of velocity lost per second, as 1/s; >= 0.
	std::array<double, 2> spin_degrees{0.0, 0.0}; // Degrees per second, min and max.
	double stretch = 0.0;                    // 0: square quads facing the camera. Above 0: stretched along the velocity, speed times this many seconds long.
	std::vector<ParticleSizeKey> size{{0.0, 0.1}, {1.0, 0.1}};
	std::vector<ParticleColorKey> color{{0.0, {1.0, 1.0, 1.0, 1.0}}, {1.0, {1.0, 1.0, 1.0, 0.0}}};
	double emissive_strength = 1.0;          // Multiplies the color; above 1 it blooms. >= 0.
	ParticleBlend blend = ParticleBlend::Additive;
	MaterialTexture texture;                 // Empty: a soft round dot.
	bool collide_floor = false;              // Particles bounce on a horizontal plane.
	double floor_height = 0.0;               // World Y of that plane.
	double bounce = 0.3;                     // Fraction of speed kept by a bounce; [0, 1].
	bool world_space = true;                 // Particles stay where they were born when the emitter moves. Off: they move with it.
	std::uint32_t max_particles = 500;       // Alive at once; 1..max_effect_particles.

	friend bool operator==(const EffectData&, const EffectData&) = default;
};

} // namespace engine::assets
