#include <engine/renderer/DesktopRenderer.hpp>
#include <engine/renderer/ResourceRegistry.hpp>
#include "FilamentUtilities.hpp"
#include "ImGuiRenderer.hpp"
#include <filament/Camera.h>
#include <filament/Color.h>
#include <filament/ColorGrading.h>
#include <filament/IndexBuffer.h>
#include <filament/IndirectLight.h>
#include <filament/LightManager.h>
#include <filament/MaterialInstance.h>
#include <filament/Options.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/RenderTarget.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/ToneMapper.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/Viewport.h>
#include <filament-iblprefilter/IBLPrefilterContext.h>
#include <utils/EntityManager.h>
#include <math/mat3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <tuple>
#include <atomic>
#include <utility>

namespace engine::renderer {
using namespace filament;
using namespace filament::math;
namespace {
mat4 matrix(const WorldTransform &w) {
    mat4 m;
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) m[c][r] = w.matrix[c * 4 + r];
    return m;
}
float3 vec(const Vec3 &v) { return {float(v.x), float(v.y), float(v.z)}; }
struct Vertex { float3 position; quatf tangent; float4 color; float2 uv; };
struct Mesh { VertexBuffer *vertices = nullptr; IndexBuffer *indices = nullptr; uint32_t count = 0; Box bounds; };
enum Slot : std::size_t { BaseColorSlot, MetallicRoughnessSlot, NormalSlot, OcclusionSlot, EmissiveSlot, SlotCount };
constexpr std::array<const char *, SlotCount> slot_names{"baseColorMap", "metallicRoughnessMap", "normalMap", "occlusionMap", "emissiveMap"};
// Everything a material instance is set to; compared to skip writes when nothing changed.
struct Parameters {
    float4 base_color{1, 1, 1, 1};
    float roughness = 0.65f, metallic = 0;
    float3 emissive{0, 0, 0};
    float normal_scale = 1, occlusion_strength = 1, alpha_cutoff = 0.5f;
    float2 uv_scale{1, 1};
    AlphaMode alpha_mode = AlphaMode::Opaque;
    bool double_sided = false;
    std::array<Texture *, SlotCount> maps{}; // Null: the slot's fallback.
    bool operator==(const Parameters &o) const {
        return base_color == o.base_color && roughness == o.roughness && metallic == o.metallic && emissive == o.emissive &&
               normal_scale == o.normal_scale && occlusion_strength == o.occlusion_strength && alpha_cutoff == o.alpha_cutoff &&
               uv_scale == o.uv_scale && alpha_mode == o.alpha_mode && double_sided == o.double_sided && maps == o.maps;
    }
};
// A range of the index buffer with its own material. `own_material` parts (GLB) use the values here; others take
// roughness and metallic from the object's Surface.
struct Part {
    uint32_t first = 0, count = 0;
    bool own_material = false;
    Parameters own;
};
struct GpuMesh : Mesh {
    uint32_t generation = 1;
    std::vector<Part> parts;
    std::vector<Texture *> images;
};
// A standalone image, uploaded once per color space it is used in.
struct GpuTexture { Texture *srgb = nullptr, *linear = nullptr; uint32_t generation = 0; };
Mesh mesh(Engine &engine, const std::vector<Vertex> &vertices, const std::vector<uint16_t> &indices, Box bounds) {
    Mesh result;
    result.vertices = VertexBuffer::Builder().vertexCount(static_cast<uint32_t>(vertices.size())).bufferCount(1)
        .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3, offsetof(Vertex, position), sizeof(Vertex))
        .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::FLOAT4, offsetof(Vertex, tangent), sizeof(Vertex))
        .attribute(VertexAttribute::COLOR, 0, VertexBuffer::AttributeType::FLOAT4, offsetof(Vertex, color), sizeof(Vertex))
        .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2, offsetof(Vertex, uv), sizeof(Vertex)).build(engine);
    result.indices = IndexBuffer::Builder().indexCount(static_cast<uint32_t>(indices.size())).bufferType(IndexBuffer::IndexType::USHORT).build(engine);
    result.count = static_cast<uint32_t>(indices.size());
    result.bounds = bounds;
    result.vertices->setBufferAt(engine, 0, detail::upload(vertices.data(), vertices.size() * sizeof(Vertex)));
    result.indices->setBuffer(engine, detail::upload(indices.data(), indices.size() * sizeof(uint16_t)));
    return result;
}
// The shader's bitangent is cross(n, t) * sign(q.w), and it must point at the top of the image: a normal map's +Y (glTF's
// convention). packTangentFrame makes w positive when the matrix's second column is along cross(t, n), hence that column.
// Checked by readback: the "normal map +Y" case of --render-test.
quatf tangent_frame(float3 t, float3 n, float handedness = 1) { return mat3f::packTangentFrame(mat3f(t, cross(t, n) * handedness, n)); }
GpuMesh builtin(Mesh mesh) {
    GpuMesh result; static_cast<Mesh &>(result) = mesh;
    Part part; part.count = mesh.count; result.parts = {part};
    return result;
}
Mesh cube(Engine &engine) {
    std::vector<Vertex> vertices; std::vector<uint16_t> indices;
    for (float3 n : {float3{1,0,0}, float3{-1,0,0}, float3{0,1,0}, float3{0,-1,0}, float3{0,0,1}, float3{0,0,-1}}) {
        const float3 t = normalize(cross(std::abs(n.y) > 0.5f ? float3{0,0,1} : float3{0,1,0}, n));
        const float3 b = cross(n, t);
        const auto q = tangent_frame(t, n);
        const auto start = static_cast<uint16_t>(vertices.size());
        // The image's first row (v = 0) is at +b, so an image reads upright on the sides.
        for (float2 corner : {float2{-0.5f,-0.5f}, float2{0.5f,-0.5f}, float2{0.5f,0.5f}, float2{-0.5f,0.5f}})
            vertices.push_back({n * 0.5f + t * corner.x + b * corner.y, q, {1,1,1,1}, {corner.x + 0.5f, 0.5f - corner.y}});
        for (int index : {0,1,2,0,2,3}) indices.push_back(static_cast<uint16_t>(start + index));
    }
    return mesh(engine, vertices, indices, {{0,0,0},{0.5f,0.5f,0.5f}});
}
// UV sphere of radius 0.5; the tangent follows increasing longitude, which stays defined at the poles.
Mesh sphere(Engine &engine) {
    constexpr int slices = 32, stacks = 16;
    std::vector<Vertex> vertices; std::vector<uint16_t> indices;
    for (int i = 0; i <= stacks; ++i) {
        const float theta = float(std::numbers::pi) * float(i) / stacks;
        for (int j = 0; j <= slices; ++j) {
            const float phi = 2 * float(std::numbers::pi) * float(j) / slices;
            const float3 n{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
            // The first row is at the north pole; cross(n, t) points south, hence the handedness.
            vertices.push_back({n * 0.5f, tangent_frame(float3{-std::sin(phi), 0, std::cos(phi)}, n, -1), {1,1,1,1}, {float(j) / slices, float(i) / stacks}});
        }
    }
    for (int i = 0; i < stacks; ++i) for (int j = 0; j < slices; ++j) {
        const auto a = static_cast<uint16_t>(i * (slices + 1) + j), b = static_cast<uint16_t>(a + slices + 1);
        // Counter-clockwise seen from outside.
        for (int index : {int(a), a + 1, int(b), a + 1, b + 1, int(b)}) indices.push_back(static_cast<uint16_t>(index));
    }
    return mesh(engine, vertices, indices, {{0,0,0},{0.5f,0.5f,0.5f}});
}
Mesh plane(Engine &engine) {
    const auto q = tangent_frame({1,0,0}, {0,1,0});
    // v grows with +Z, so looking down with -Z up the screen shows an image upright.
    const std::vector<Vertex> vertices{{{-0.5f,0,-0.5f}, q, {1,1,1,1}, {0,0}}, {{0.5f,0,-0.5f}, q, {1,1,1,1}, {1,0}},
                                       {{0.5f,0,0.5f}, q, {1,1,1,1}, {1,1}}, {{-0.5f,0,0.5f}, q, {1,1,1,1}, {0,1}}};
    return mesh(engine, vertices, {0,2,1,0,3,2}, {{0,0,0},{0.5f,0.01f,0.5f}});
}
// Filament clamps each of these, so a wide EV range needs all three. EV100 = log2(aperture^2 / shutter * 100 / ISO).
void set_exposure(Camera &camera, double ev100) {
    const float aperture = ev100 > 14 ? 16.f : 1.f;
    double shutter = double(aperture) * aperture / std::pow(2.0, ev100);
    double iso = 100;
    if (shutter > 30) { iso = 100 * shutter / 30; shutter = 30; }
    camera.setExposure(aperture, float(shutter), float(iso));
}
// Equirectangular RGBA, rows top to bottom: a vertical gradient from sky through horizon to ground, sharper near the horizon.
std::vector<float> procedural_sky(const RenderEnvironment &e, uint32_t width, uint32_t height) {
    std::vector<float> pixels(std::size_t(width) * height * 4);
    const auto mix = [](const Vec3 &a, const Vec3 &b, double t) { return Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; };
    for (uint32_t y = 0; y < height; ++y) {
        const double elevation = 1 - 2 * (y + 0.5) / height; // +1 at the zenith, -1 at the nadir.
        const Vec3 c = elevation >= 0 ? mix(e.horizon_color, e.sky_color, std::pow(elevation, 0.55)) : mix(e.horizon_color, e.ground_color, std::pow(-elevation, 0.3));
        for (uint32_t x = 0; x < width; ++x) {
            float *p = pixels.data() + (std::size_t(y) * width + x) * 4;
            p[0] = float(c.x); p[1] = float(c.y); p[2] = float(c.z); p[3] = 1;
        }
    }
    return pixels;
}
const ToneMapper *new_tone_mapper(ToneMapping tone) {
    switch (tone) {
    case ToneMapping::Filmic: return new FilmicToneMapper;
    case ToneMapping::Agx: return new AgxToneMapper;
    case ToneMapping::PbrNeutral: return new PBRNeutralToneMapper;
    case ToneMapping::Linear: return new LinearToneMapper;
    default: return new ACESLegacyToneMapper;
    }
}
}
struct DesktopRenderer::Impl {
    struct PartState { MaterialInstance *instance = nullptr; Parameters applied; bool written = false; };
    struct Object { utils::Entity entity; std::vector<PartState> parts; MeshHandle mesh; bool shadows = true; };
    struct Light { utils::Entity entity; LightKind kind; bool shadows = false; };
    using Key = std::pair<std::uint64_t, EntityId>; // Frame scene identity, entity.
    Engine *engine = nullptr;
    Renderer *renderer = nullptr;
    SwapChain *swap = nullptr;
    filament::Scene *scene = nullptr;
    View *view = nullptr;
    Camera *camera = nullptr;
    Skybox *flat_sky = nullptr, *image_sky = nullptr;
    Material *grid_material = nullptr;
    std::array<Material *, 3> pbr{}; // By AlphaMode.
    Texture *color = nullptr, *depth = nullptr;
    std::array<Texture *, SlotCount> fallbacks{};
    RenderTarget *target = nullptr;
    utils::Entity camera_entity, grid, sun, fill;
    bool editor_lights = true, grid_shown = true;
    double grid_spacing = 1;
    Mesh grid_mesh;
    std::map<uint32_t, GpuMesh> meshes; // By handle index: the built-ins, then uploaded assets.
    std::map<uint32_t, GpuTexture> textures;
    std::vector<GpuMesh> retired_meshes; // Replaced this sync; freed once no object draws them.
    std::vector<Texture *> retired_textures;
    std::vector<std::string> issues;
    std::map<Key, Object> objects;
    std::map<Key, Light> lights;
    std::unique_ptr<detail::ImGuiRenderer> ui;
    unsigned viewport_width = 0, viewport_height = 0;
    // Image-based lighting, rebuilt when its source changes.
    std::unique_ptr<IBLPrefilterContext> ibl_context;
    Texture *sky_cubemap = nullptr, *reflections = nullptr, *irradiance = nullptr;
    IndirectLight *indirect = nullptr;
    std::optional<RenderEnvironment> applied_environment;
    std::uint64_t environment_builds = 0;
    ColorGrading *grading = nullptr;
    // The material preview: a sphere in its own scene and view, drawn into its own texture.
    static constexpr uint32_t preview_size = 256;
    filament::Scene *preview_scene = nullptr;
    View *preview_view = nullptr;
    Camera *preview_camera = nullptr;
    utils::Entity preview_camera_entity, preview_object, preview_light;
    std::array<MaterialInstance *, 3> preview_instances{};
    AlphaMode preview_mode = AlphaMode::Opaque;
    Texture *preview_color = nullptr, *preview_depth = nullptr;
    RenderTarget *preview_target = nullptr;
    Skybox *preview_sky = nullptr;
    MaterialHandle preview_material;
    std::optional<Parameters> preview_applied;
    // Thumbnails share one scene, view and depth buffer; each has its own color texture.
    static constexpr uint32_t thumbnail_size = 128;
    struct ThumbnailRequest { uint64_t key; MeshHandle mesh; MaterialHandle material; };
    struct ThumbnailImage { Texture *color = nullptr; RenderTarget *target = nullptr; bool flipped = false; };
    std::vector<ThumbnailRequest> thumbnail_requests;
    std::map<uint64_t, ThumbnailImage> thumbnails;
    std::vector<ThumbnailImage> retired_thumbnails;
    filament::Scene *thumbnail_scene = nullptr;
    View *thumbnail_view = nullptr;
    Camera *thumbnail_camera = nullptr;
    Skybox *thumbnail_sky = nullptr;
    Texture *thumbnail_depth = nullptr;
    utils::Entity thumbnail_camera_entity, thumbnail_light, thumbnail_object;
    std::vector<MaterialInstance *> thumbnail_instances;
    std::optional<ThumbnailRequest> thumbnail_drawing; // In case the frame that should draw it is skipped.

    // Null when the GPU cannot take the image; the caller falls back to white, so the asset still draws.
    Texture *upload(const assets::TextureData &data, bool srgb) {
        const auto limit = Texture::getMaxTextureSize(*engine, Texture::Sampler::SAMPLER_2D);
        if (data.width > limit || data.height > limit) {
            issues.push_back("A " + std::to_string(data.width) + "x" + std::to_string(data.height) + " image exceeds the GPU limit of " + std::to_string(limit) + " and is shown white");
            return nullptr;
        }
        uint8_t levels = 1;
        for (auto size = std::max(data.width, data.height); size > 1; size /= 2) ++levels;
        auto *texture = Texture::Builder().width(data.width).height(data.height).levels(levels).sampler(Texture::Sampler::SAMPLER_2D)
            .format(srgb ? Texture::InternalFormat::SRGB8_A8 : Texture::InternalFormat::RGBA8)
            .usage(Texture::Usage::DEFAULT | Texture::Usage::GEN_MIPMAPPABLE).build(*engine);
        texture->setImage(*engine, 0, Texture::PixelBufferDescriptor(detail::copy_bytes(data.pixels.data(), data.pixels.size()), data.pixels.size(),
            Texture::Format::RGBA, Texture::Type::UBYTE, detail::release));
        texture->generateMipmaps(*engine);
        return texture;
    }
    GpuMesh upload(const assets::MeshData &data, uint32_t generation) {
        std::vector<Vertex> vertices; vertices.reserve(data.vertices.size());
        for (const auto &v : data.vertices) {
            const float3 n{v.normal[0], v.normal[1], v.normal[2]};
            float3 t{v.tangent[0], v.tangent[1], v.tangent[2]};
            t = t - n * dot(n, t);
            if (const float len = length(t); len > 1e-5f) t /= len;
            else t = normalize(cross(std::abs(n.y) > 0.5f ? float3{0,0,1} : float3{0,1,0}, n));
            vertices.push_back({{v.position[0], v.position[1], v.position[2]}, tangent_frame(t, n, v.tangent[3] < 0 ? -1.f : 1.f), {1,1,1,1}, {v.uv[0], v.uv[1]}});
        }
        GpuMesh result;
        result.generation = generation;
        result.vertices = VertexBuffer::Builder().vertexCount(static_cast<uint32_t>(vertices.size())).bufferCount(1)
            .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3, offsetof(Vertex, position), sizeof(Vertex))
            .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::FLOAT4, offsetof(Vertex, tangent), sizeof(Vertex))
            .attribute(VertexAttribute::COLOR, 0, VertexBuffer::AttributeType::FLOAT4, offsetof(Vertex, color), sizeof(Vertex))
            .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2, offsetof(Vertex, uv), sizeof(Vertex)).build(*engine);
        result.indices = IndexBuffer::Builder().indexCount(static_cast<uint32_t>(data.indices.size())).bufferType(IndexBuffer::IndexType::UINT).build(*engine);
        result.count = static_cast<uint32_t>(data.indices.size());
        result.vertices->setBufferAt(*engine, 0, detail::upload(vertices.data(), vertices.size() * sizeof(Vertex)));
        result.indices->setBuffer(*engine, detail::upload(data.indices.data(), data.indices.size() * sizeof(uint32_t)));
        const auto &b = data.bounds;
        const float3 low{b.min[0], b.min[1], b.min[2]}, high{b.max[0], b.max[1], b.max[2]};
        result.bounds.set(low, high);
        result.bounds.halfExtent = max(result.bounds.halfExtent, float3(0.001f)); // A flat model still needs a volume to pass culling.
        for (const auto &image : data.images) result.images.push_back(upload(*image, image->srgb));
        const auto own_image = [&](int index) { return index >= 0 ? result.images.at(static_cast<std::size_t>(index)) : nullptr; };
        for (const auto &source : data.submeshes) {
            Part part{source.first_index, source.index_count, true, {}};
            const auto &m = source.material;
            part.own.base_color = {m.base_color[0], m.base_color[1], m.base_color[2], m.base_color[3]};
            part.own.roughness = m.roughness; part.own.metallic = m.metallic;
            part.own.emissive = {m.emissive[0], m.emissive[1], m.emissive[2]};
            part.own.normal_scale = m.normal_scale; part.own.occlusion_strength = m.occlusion_strength;
            part.own.alpha_cutoff = m.alpha_cutoff; part.own.double_sided = m.double_sided;
            part.own.alpha_mode = m.alpha_mode == assets::AlphaMode::Mask ? AlphaMode::Mask : m.alpha_mode == assets::AlphaMode::Blend ? AlphaMode::Blend : AlphaMode::Opaque;
            part.own.maps = {own_image(m.image), own_image(m.metallic_roughness_image), own_image(m.normal_image), own_image(m.occlusion_image), own_image(m.emissive_image)};
            result.parts.push_back(part);
        }
        if (result.parts.empty()) { Part whole; whole.count = result.count; result.parts = {whole}; }
        return result;
    }
    // The GPU mesh of a handle, uploaded if this is its first use or a new revision. Null for a stale handle.
    const GpuMesh *geometry(MeshHandle handle, const ResourceRegistry &registry) {
        const auto it = meshes.find(handle.index);
        if (it != meshes.end() && it->second.generation == handle.generation) return &it->second;
        const auto data = registry.find(handle);
        if (!data) return nullptr;
        if (it != meshes.end()) { retired_meshes.push_back(std::move(it->second)); meshes.erase(it); }
        return &meshes.emplace(handle.index, upload(*data, handle.generation)).first->second;
    }
    // The image of a handle in the color space a slot needs. Null for none, or for an image the registry no longer holds.
    Texture *image(TextureHandle handle, bool srgb, const ResourceRegistry &registry, std::set<uint32_t> &used) {
        if (!handle.index) return nullptr;
        used.insert(handle.index);
        auto it = textures.find(handle.index);
        if (it != textures.end() && it->second.generation != handle.generation) {
            for (auto *texture : {it->second.srgb, it->second.linear}) if (texture) retired_textures.push_back(texture);
            textures.erase(it); it = textures.end();
        }
        if (it == textures.end()) it = textures.emplace(handle.index, GpuTexture{nullptr, nullptr, handle.generation}).first;
        auto *&variant = srgb ? it->second.srgb : it->second.linear;
        if (!variant) if (const auto data = registry.find(handle)) variant = upload(*data, srgb);
        return variant;
    }
    // What a part of an object looks like: part slot > object material > the part's own > the Surface.
    Parameters parameters(const RenderObject &object, const Part &part, std::uint32_t index, const ResourceRegistry &registry, std::set<uint32_t> &used) {
        MaterialHandle chosen = object.material;
        for (const auto &slot : object.parts) if (slot.part == index) chosen = slot.material;
        const auto tint = vec(object.surface.linear_color);
        const float2 object_uv{float(object.surface.uv_scale_u), float(object.surface.uv_scale_v)};
        Parameters p;
        if (const MaterialDesc *desc = chosen == builtin_lit ? nullptr : registry.find(chosen)) {
            p.base_color = {vec(desc->base_color) * tint, float(desc->alpha)};
            p.roughness = float(desc->roughness); p.metallic = float(desc->metallic);
            p.emissive = vec(desc->emissive);
            p.normal_scale = float(desc->normal_scale); p.occlusion_strength = float(desc->occlusion_strength);
            p.alpha_cutoff = float(desc->alpha_cutoff); p.alpha_mode = desc->alpha_mode; p.double_sided = desc->double_sided;
            p.uv_scale = float2{float(desc->uv_scale_u), float(desc->uv_scale_v)} * object_uv;
            p.maps[BaseColorSlot] = image(desc->base_color_map, true, registry, used);
            p.maps[MetallicRoughnessSlot] = image(desc->metallic_roughness_map, false, registry, used);
            p.maps[NormalSlot] = image(desc->normal_map, false, registry, used);
            p.maps[OcclusionSlot] = image(desc->occlusion_map, false, registry, used);
            p.maps[EmissiveSlot] = image(desc->emissive_map, true, registry, used);
        } else if (part.own_material) {
            p = part.own;
            p.base_color = {p.base_color.rgb * tint, p.base_color.a};
            p.uv_scale = object_uv;
        } else {
            p.base_color = {tint, 1};
            p.roughness = float(object.surface.roughness); p.metallic = float(object.surface.metallic);
            p.double_sided = object.surface.double_sided;
            p.uv_scale = object_uv;
        }
        if (auto *override_image = image(object.surface.texture, true, registry, used)) p.maps[BaseColorSlot] = override_image;
        return p;
    }
    void apply(MaterialInstance &m, const Parameters &p, const Parameters *before) {
        if (!before || !(before->base_color == p.base_color)) m.setParameter("baseColor", p.base_color);
        if (!before || before->roughness != p.roughness) m.setParameter("roughness", p.roughness);
        if (!before || before->metallic != p.metallic) m.setParameter("metallic", p.metallic);
        if (!before || !(before->emissive == p.emissive)) m.setParameter("emissive", p.emissive);
        if (!before || before->normal_scale != p.normal_scale) m.setParameter("normalScale", p.normal_scale);
        if (!before || before->occlusion_strength != p.occlusion_strength) m.setParameter("occlusionStrength", p.occlusion_strength);
        if (!before || !(before->uv_scale == p.uv_scale)) m.setParameter("uvScale", p.uv_scale);
        if (!before || before->double_sided != p.double_sided) {
            m.setDoubleSided(p.double_sided);
            m.setCullingMode(p.double_sided ? MaterialInstance::CullingMode::NONE : MaterialInstance::CullingMode::BACK);
        }
        if (p.alpha_mode == AlphaMode::Mask && (!before || before->alpha_cutoff != p.alpha_cutoff)) m.setMaskThreshold(p.alpha_cutoff);
        TextureSampler sampler(TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR, TextureSampler::MagFilter::LINEAR, TextureSampler::WrapMode::REPEAT);
        sampler.setAnisotropy(8);
        for (std::size_t i = 0; i < SlotCount; ++i)
            if (!before || before->maps[i] != p.maps[i]) m.setParameter(slot_names[i], p.maps[i] ? p.maps[i] : fallbacks[i], sampler);
    }
    void init(void *window, const std::filesystem::path &materials) {
        engine = Engine::create(Engine::Backend::OPENGL);
        if (!engine) throw std::runtime_error("Filament OpenGL engine creation failed");
        swap = engine->createSwapChain(window, SwapChain::CONFIG_READABLE);
        renderer = engine->createRenderer(); scene = engine->createScene(); view = engine->createView();
        camera_entity = utils::EntityManager::get().create(); camera = engine->createCamera(camera_entity);
        set_exposure(*camera, 15);
        view->setScene(scene); view->setCamera(camera);
        view->setAntiAliasing(AntiAliasing::FXAA);
        view->setMultiSampleAntiAliasingOptions({.enabled = true, .sampleCount = 4});
        flat_sky = Skybox::Builder().color({0.055f, 0.07f, 0.095f, 1}).build(*engine); scene->setSkybox(flat_sky);
        pbr[size_t(AlphaMode::Opaque)] = detail::material(*engine, materials / "pbr_opaque.filamat");
        pbr[size_t(AlphaMode::Mask)] = detail::material(*engine, materials / "pbr_masked.filamat");
        pbr[size_t(AlphaMode::Blend)] = detail::material(*engine, materials / "pbr_transparent.filamat");
        grid_material = detail::material(*engine, materials / "grid.filamat");
        meshes.emplace(builtin_cube.index, builtin(cube(*engine)));
        meshes.emplace(builtin_sphere.index, builtin(sphere(*engine)));
        meshes.emplace(builtin_plane.index, builtin(plane(*engine)));
        // Fallbacks a missing map reads as "no change": white, and a normal pointing straight out.
        const auto flat = [&](uint8_t r, uint8_t g, uint8_t b, Texture::InternalFormat format) {
            auto *texture = Texture::Builder().width(1).height(1).levels(1).sampler(Texture::Sampler::SAMPLER_2D).format(format).build(*engine);
            const uint8_t pixel[4]{r, g, b, 255};
            texture->setImage(*engine, 0, Texture::PixelBufferDescriptor(detail::copy_bytes(pixel, 4), 4, Texture::Format::RGBA, Texture::Type::UBYTE, detail::release));
            return texture;
        };
        fallbacks = {flat(255, 255, 255, Texture::InternalFormat::SRGB8_A8), flat(255, 255, 255, Texture::InternalFormat::RGBA8), flat(128, 128, 255, Texture::InternalFormat::RGBA8),
                     flat(255, 255, 255, Texture::InternalFormat::RGBA8), flat(255, 255, 255, Texture::InternalFormat::SRGB8_A8)};
        std::vector<Vertex> vertices; std::vector<uint16_t> indices;
        for (int i = -20; i <= 20; ++i) {
            const float shade = i % 5 == 0 ? 0.26f : 0.13f;
            const float4 gray{shade, shade, shade, 1};
            const float4 xcolor = i == 0 ? float4{0.6f,0.12f,0.12f,1} : gray;
            const float4 zcolor = i == 0 ? float4{0.12f,0.3f,0.65f,1} : gray;
            vertices.push_back({{-20,0,float(i)}, {}, xcolor}); vertices.push_back({{20,0,float(i)}, {}, xcolor});
            vertices.push_back({{float(i),0,-20}, {}, zcolor}); vertices.push_back({{float(i),0,20}, {}, zcolor});
        }
        for (uint16_t i = 0; i < vertices.size(); ++i) indices.push_back(i);
        grid_mesh = mesh(*engine, vertices, indices, {{0,0,0},{20,0.01f,20}});
        grid = utils::EntityManager::get().create();
        RenderableManager::Builder(1).boundingBox(grid_mesh.bounds).culling(false)
            .geometry(0, RenderableManager::PrimitiveType::LINES, grid_mesh.vertices, grid_mesh.indices, 0, grid_mesh.count)
            .material(0, grid_material->getDefaultInstance()).castShadows(false).receiveShadows(false).build(*engine, grid);
        scene->addEntity(grid);
        // Editor lighting, used only while the frame has no scene lights.
        sun = utils::EntityManager::get().create(); fill = utils::EntityManager::get().create();
        LightManager::Builder(LightManager::Type::DIRECTIONAL).direction({-0.5f,-1,-0.4f}).intensity(100000).castShadows(true).build(*engine, sun);
        LightManager::Builder(LightManager::Type::DIRECTIONAL).direction({0.7f,-0.2f,0.8f}).color({0.55f,0.7f,1}).intensity(30000).castShadows(false).build(*engine, fill);
        scene->addEntity(sun); scene->addEntity(fill);
        init_preview();
        init_thumbnails();
        ui = std::make_unique<detail::ImGuiRenderer>(*engine, materials);
    }
    void init_preview() {
        preview_scene = engine->createScene(); preview_view = engine->createView();
        preview_camera_entity = utils::EntityManager::get().create(); preview_camera = engine->createCamera(preview_camera_entity);
        preview_camera->setProjection(35, 1, 0.1, 10);
        preview_camera->lookAt({0, 0, 2.1f}, {0, 0, 0});
        preview_view->setScene(preview_scene); preview_view->setCamera(preview_camera);
        preview_view->setAntiAliasing(AntiAliasing::FXAA);
        preview_view->setMultiSampleAntiAliasingOptions({.enabled = true, .sampleCount = 4});
        preview_sky = Skybox::Builder().color({0.09f, 0.1f, 0.12f, 1}).build(*engine);
        preview_scene->setSkybox(preview_sky);
        preview_color = Texture::Builder().width(preview_size).height(preview_size).levels(1).sampler(Texture::Sampler::SAMPLER_2D)
            .format(Texture::InternalFormat::RGBA8).usage(Texture::Usage::COLOR_ATTACHMENT | Texture::Usage::SAMPLEABLE).build(*engine);
        preview_depth = Texture::Builder().width(preview_size).height(preview_size).levels(1).sampler(Texture::Sampler::SAMPLER_2D)
            .format(Texture::InternalFormat::DEPTH24).usage(Texture::Usage::DEPTH_ATTACHMENT).build(*engine);
        preview_target = RenderTarget::Builder().texture(RenderTarget::AttachmentPoint::COLOR, preview_color)
            .texture(RenderTarget::AttachmentPoint::DEPTH, preview_depth).build(*engine);
        preview_view->setRenderTarget(preview_target); preview_view->setViewport({0, 0, preview_size, preview_size});
        for (std::size_t i = 0; i < pbr.size(); ++i) { preview_instances[i] = pbr[i]->createInstance(); apply(*preview_instances[i], Parameters{}, nullptr); }
        const auto &ball = meshes.at(builtin_sphere.index);
        preview_object = utils::EntityManager::get().create();
        RenderableManager::Builder(1).boundingBox(ball.bounds).castShadows(false).receiveShadows(false)
            .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, ball.vertices, ball.indices, 0, ball.count)
            .material(0, preview_instances[0]).build(*engine, preview_object);
        auto &tm = engine->getTransformManager();
        tm.setTransform(tm.getInstance(preview_object), mat4f::scaling(float3(1.25f)));
        preview_scene->addEntity(preview_object);
        preview_light = utils::EntityManager::get().create();
        LightManager::Builder(LightManager::Type::DIRECTIONAL).direction({-0.5f,-0.7f,-0.6f}).intensity(70000).castShadows(false).build(*engine, preview_light);
        preview_scene->addEntity(preview_light);
    }
    void init_thumbnails() {
        thumbnail_scene = engine->createScene(); thumbnail_view = engine->createView();
        thumbnail_camera_entity = utils::EntityManager::get().create(); thumbnail_camera = engine->createCamera(thumbnail_camera_entity);
        thumbnail_view->setScene(thumbnail_scene); thumbnail_view->setCamera(thumbnail_camera);
        thumbnail_view->setAntiAliasing(AntiAliasing::FXAA);
        thumbnail_view->setViewport({0, 0, thumbnail_size, thumbnail_size});
        thumbnail_sky = Skybox::Builder().color({0.09f, 0.1f, 0.12f, 1}).build(*engine);
        thumbnail_scene->setSkybox(thumbnail_sky);
        thumbnail_depth = Texture::Builder().width(thumbnail_size).height(thumbnail_size).levels(1).sampler(Texture::Sampler::SAMPLER_2D)
            .format(Texture::InternalFormat::DEPTH24).usage(Texture::Usage::DEPTH_ATTACHMENT).build(*engine);
        thumbnail_light = utils::EntityManager::get().create();
        LightManager::Builder(LightManager::Type::DIRECTIONAL).direction({-0.5f,-0.7f,-0.6f}).intensity(70000).castShadows(false).build(*engine, thumbnail_light);
        thumbnail_scene->addEntity(thumbnail_light);
    }
    void clear_thumbnail_object() {
        if (!thumbnail_object) return;
        thumbnail_scene->remove(thumbnail_object);
        remove(thumbnail_object);
        thumbnail_object = {};
        for (auto *instance : thumbnail_instances) engine->destroy(instance);
        thumbnail_instances.clear();
    }
    void retire(const ThumbnailImage &image) { retired_thumbnails.push_back(image); }
    void destroy_retired_thumbnails() {
        for (const auto &image : retired_thumbnails) {
            if (image.target) engine->destroy(image.target);
            if (image.color) engine->destroy(image.color);
        }
        retired_thumbnails.clear();
    }
    // Sets up the next thumbnail for this frame. True when there is one to draw.
    bool prepare_thumbnail(const ResourceRegistry &registry, std::set<uint32_t> &used) {
        while (!thumbnail_requests.empty()) {
            const auto request = thumbnail_requests.front();
            thumbnail_requests.erase(thumbnail_requests.begin());
            thumbnail_drawing = request;
            const GpuMesh *gpu = geometry(request.mesh, registry);
            if (!gpu) continue;
            thumbnail_object = utils::EntityManager::get().create();
            RenderableManager::Builder builder(gpu->parts.size());
            builder.boundingBox(gpu->bounds).castShadows(false).receiveShadows(false);
            const RenderObject probe{0, request.mesh, request.material, {}, Surface{}, {}};
            for (std::size_t i = 0; i < gpu->parts.size(); ++i) {
                const Parameters wanted = parameters(probe, gpu->parts[i], static_cast<std::uint32_t>(i), registry, used);
                auto *instance = pbr[size_t(wanted.alpha_mode)]->createInstance();
                apply(*instance, wanted, nullptr);
                thumbnail_instances.push_back(instance);
                builder.geometry(i, RenderableManager::PrimitiveType::TRIANGLES, gpu->vertices, gpu->indices, gpu->parts[i].first, gpu->parts[i].count).material(i, instance);
            }
            builder.build(*engine, thumbnail_object);
            thumbnail_scene->addEntity(thumbnail_object);
            // Frames the bounds from a three-quarter view.
            const float3 center = gpu->bounds.center, half = gpu->bounds.halfExtent;
            const float radius = std::max(length(half), 0.001f);
            const float distance = radius / std::sin(17.5f * 3.14159265f / 180) * 1.05f;
            const float3 eye = center + normalize(float3{0.6f, 0.45f, 1}) * distance;
            thumbnail_camera->setProjection(35, 1, std::max(distance - radius * 1.5f, distance * 0.01f), distance + radius * 1.5f);
            thumbnail_camera->lookAt(eye, center);
            ThumbnailImage image;
            image.flipped = true;
            image.color = Texture::Builder().width(thumbnail_size).height(thumbnail_size).levels(1).sampler(Texture::Sampler::SAMPLER_2D)
                .format(Texture::InternalFormat::RGBA8).usage(Texture::Usage::COLOR_ATTACHMENT | Texture::Usage::SAMPLEABLE).build(*engine);
            image.target = RenderTarget::Builder().texture(RenderTarget::AttachmentPoint::COLOR, image.color)
                .texture(RenderTarget::AttachmentPoint::DEPTH, thumbnail_depth).build(*engine);
            thumbnail_view->setRenderTarget(image.target);
            if (const auto old = thumbnails.find(request.key); old != thumbnails.end()) retire(old->second);
            thumbnails[request.key] = image;
            return true;
        }
        return false;
    }
    void remove(utils::Entity e) { engine->destroy(e); utils::EntityManager::get().destroy(e); }
    void remove(const Object &o) { scene->remove(o.entity); remove(o.entity); for (const auto &part : o.parts) engine->destroy(part.instance); }
    void remove(const Light &l) { scene->remove(l.entity); remove(l.entity); }
    void destroy(const Mesh &m) { engine->destroy(m.vertices); engine->destroy(m.indices); }
    void destroy(const GpuMesh &m) {
        destroy(static_cast<const Mesh &>(m));
        for (auto *texture : m.images) if (texture) engine->destroy(texture);
    }
    void destroy(Texture *texture) { if (texture) engine->destroy(texture); }
    void destroy(const GpuTexture &gpu) { destroy(gpu.srgb); destroy(gpu.linear); }
    void destroy_environment() {
        if (indirect) { scene->setIndirectLight(nullptr); preview_scene->setIndirectLight(nullptr); thumbnail_scene->setIndirectLight(nullptr); engine->destroy(indirect); indirect = nullptr; }
        if (image_sky) { scene->setSkybox(flat_sky); engine->destroy(image_sky); image_sky = nullptr; }
        destroy(sky_cubemap); destroy(reflections); destroy(irradiance);
        sky_cubemap = reflections = irradiance = nullptr;
    }
    ~Impl() {
        if (!engine) return;
        engine->flushAndWait();
        ui.reset();
        engine->destroy(view);
        clear_thumbnail_object();
        for (const auto &[key, image] : thumbnails) { (void)key; retire(image); }
        destroy_retired_thumbnails();
        thumbnail_scene->remove(thumbnail_light); remove(thumbnail_light);
        engine->destroy(thumbnail_view); engine->destroy(thumbnail_depth); engine->destroy(thumbnail_sky);
        engine->destroyCameraComponent(thumbnail_camera_entity); utils::EntityManager::get().destroy(thumbnail_camera_entity);
        engine->destroy(preview_view); preview_scene->remove(preview_object); preview_scene->remove(preview_light);
        remove(preview_object); remove(preview_light);
        for (auto *instance : preview_instances) engine->destroy(instance);
        engine->destroy(preview_target); engine->destroy(preview_color); engine->destroy(preview_depth); engine->destroy(preview_sky);
        engine->destroyCameraComponent(preview_camera_entity); utils::EntityManager::get().destroy(preview_camera_entity);
        for (const auto &[key, object] : objects) { (void)key; remove(object); }
        for (const auto &[key, light] : lights) { (void)key; remove(light); }
        destroy_environment();
        ibl_context.reset();
        // After destroy_environment(), which takes the indirect light back from all three scenes.
        engine->destroy(thumbnail_scene);
        engine->destroy(preview_scene);
        engine->destroy(scene);
        remove(grid); remove(sun); remove(fill);
        engine->destroyCameraComponent(camera_entity); utils::EntityManager::get().destroy(camera_entity);
        engine->destroy(target); engine->destroy(color); engine->destroy(depth);
        for (const auto &[index, gpu] : meshes) { (void)index; destroy(gpu); }
        for (const auto &[index, gpu] : textures) { (void)index; destroy(gpu); }
        destroy(grid_mesh);
        for (auto *texture : fallbacks) engine->destroy(texture);
        if (grading) engine->destroy(grading);
        for (auto *material : pbr) engine->destroy(material);
        engine->destroy(grid_material); engine->destroy(flat_sky);
        engine->destroy(renderer); engine->destroy(swap);
        engine->flushAndWait(); Engine::destroy(&engine);
    }
    void sync_objects(const FrameSnapshot &frame, const ResourceRegistry &registry) {
        std::set<Key> live;
        std::set<uint32_t> used_meshes, used_textures;
        auto &rm = engine->getRenderableManager();
        for (const auto &object : frame.objects) {
            const GpuMesh *gpu = geometry(object.mesh, registry);
            if (!gpu && object.mesh.index < ResourceRegistry::first_index) throw std::runtime_error("Unknown built-in mesh handle");
            if (!gpu) continue;
            used_meshes.insert(object.mesh.index);
            const Key key{frame.stamp.scene, object.entity}; live.insert(key);
            auto it = objects.find(key);
            if (it != objects.end() && it->second.mesh != object.mesh) { remove(it->second); objects.erase(it); it = objects.end(); }
            if (it == objects.end()) {
                Object fresh{utils::EntityManager::get().create(), {}, object.mesh, true};
                RenderableManager::Builder builder(gpu->parts.size());
                builder.boundingBox(gpu->bounds).castShadows(true).receiveShadows(true);
                fresh.parts.resize(gpu->parts.size());
                for (std::size_t i = 0; i < gpu->parts.size(); ++i) {
                    fresh.parts[i].instance = pbr[size_t(AlphaMode::Opaque)]->createInstance();
                    builder.geometry(i, RenderableManager::PrimitiveType::TRIANGLES, gpu->vertices, gpu->indices, gpu->parts[i].first, gpu->parts[i].count)
                        .material(i, fresh.parts[i].instance);
                }
                builder.build(*engine, fresh.entity);
                scene->addEntity(fresh.entity); it = objects.emplace(key, std::move(fresh)).first;
            }
            auto &record = it->second;
            const auto renderable = rm.getInstance(record.entity);
            bool shadows = false;
            for (std::size_t i = 0; i < gpu->parts.size(); ++i) {
                const Parameters wanted = parameters(object, gpu->parts[i], static_cast<std::uint32_t>(i), registry, used_textures);
                auto &part = record.parts[i];
                // Each blend mode is its own Filament material, so changing it means a new instance.
                if (!part.written || part.applied.alpha_mode != wanted.alpha_mode) {
                    auto *replacement = pbr[size_t(wanted.alpha_mode)]->createInstance();
                    rm.setMaterialInstanceAt(renderable, i, replacement);
                    engine->destroy(part.instance);
                    part.instance = replacement;
                    apply(*part.instance, wanted, nullptr);
                    part.applied = wanted; part.written = true;
                } else if (part.applied != wanted) {
                    apply(*part.instance, wanted, &part.applied);
                    part.applied = wanted;
                }
                shadows |= wanted.alpha_mode != AlphaMode::Blend; // Blended parts would cast solid shadows.
            }
            if (shadows != record.shadows) { rm.setCastShadows(renderable, shadows); record.shadows = shadows; }
            auto &tm = engine->getTransformManager();
            tm.setTransform(tm.getInstance(record.entity), matrix(object.world));
        }
        for (auto it = objects.begin(); it != objects.end();)
            if (!live.contains(it->first)) { remove(it->second); it = objects.erase(it); } else ++it;
        // No object draws these now.
        for (const auto &gpu : retired_meshes) destroy(gpu);
        retired_meshes.clear();
        for (auto *texture : retired_textures) destroy(texture);
        retired_textures.clear();
        if (preview_material.index) update_preview(registry, used_textures);
        for (auto it = meshes.begin(); it != meshes.end();)
            if (it->first >= ResourceRegistry::first_index && !used_meshes.contains(it->first)) { destroy(it->second); it = meshes.erase(it); } else ++it;
        for (auto it = textures.begin(); it != textures.end();)
            if (!used_textures.contains(it->first)) { destroy(it->second); it = textures.erase(it); } else ++it;
    }
    // The preview sphere uses the chosen material; its textures count as used so the sweep above keeps them.
    void update_preview(const ResourceRegistry &registry, std::set<uint32_t> &used) {
        const RenderObject probe{0, builtin_sphere, preview_material, {}, Surface{}, {}};
        const Parameters wanted = parameters(probe, Part{}, 0, registry, used);
        if (preview_applied && *preview_applied == wanted) return;
        auto *instance = preview_instances[size_t(wanted.alpha_mode)];
        apply(*instance, wanted, nullptr);
        if (preview_mode != wanted.alpha_mode) {
            auto &rm = engine->getRenderableManager();
            rm.setMaterialInstanceAt(rm.getInstance(preview_object), 0, instance);
            preview_mode = wanted.alpha_mode;
        }
        preview_applied = wanted;
    }
    void sync_lights(const FrameSnapshot &frame) {
        std::set<Key> live;
        auto &lm = engine->getLightManager();
        const auto &e = frame.environment;
        LightManager::ShadowOptions directional;
        directional.mapSize = e.shadow_quality == ShadowQuality::High ? 4096 : e.shadow_quality == ShadowQuality::Medium ? 2048 : 1024;
        directional.shadowCascades = e.shadow_quality == ShadowQuality::High ? 4 : e.shadow_quality == ShadowQuality::Medium ? 3 : 1;
        directional.shadowFar = float(e.shadow_distance);
        directional.stable = true; // A turning first-person camera would otherwise make shadow edges swim.
        directional.lispsm = false;
        LightManager::ShadowOptions punctual;
        punctual.mapSize = e.shadow_quality == ShadowQuality::High ? 1024 : 512;
        for (const auto &light : frame.lights) {
            const Key key{frame.stamp.scene, light.entity}; live.insert(key);
            auto it = lights.find(key);
            if (it != lights.end() && it->second.kind != light.kind) { remove(it->second); lights.erase(it); it = lights.end(); }
            if (it == lights.end()) {
                const Light created{utils::EntityManager::get().create(), light.kind, false};
                LightManager::Builder(light.kind == LightKind::Point ? LightManager::Type::POINT : light.kind == LightKind::Spot ? LightManager::Type::SPOT : LightManager::Type::DIRECTIONAL)
                    .castShadows(false).build(*engine, created.entity);
                scene->addEntity(created.entity); it = lights.emplace(key, created).first;
            }
            const auto instance = lm.getInstance(it->second.entity);
            lm.setColor(instance, LinearColor(vec(light.linear_color)));
            lm.setIntensity(instance, float(light.intensity));
            if (light.kind != LightKind::Point) lm.setDirection(instance, vec(light.direction));
            if (light.kind != LightKind::Directional) { lm.setPosition(instance, vec(light.position)); lm.setFalloff(instance, float(light.range)); }
            if (light.kind == LightKind::Spot) lm.setSpotLightCone(instance, float(light.inner_cone_radians), float(light.outer_cone_radians));
            if (light.cast_shadows != it->second.shadows) { lm.setShadowCaster(instance, light.cast_shadows); it->second.shadows = light.cast_shadows; }
            if (light.cast_shadows) lm.setShadowOptions(instance, light.kind == LightKind::Directional ? directional : punctual);
        }
        for (auto it = lights.begin(); it != lights.end();)
            if (!live.contains(it->first)) { remove(it->second); it = lights.erase(it); } else ++it;
        const bool wanted = frame.lights.empty();
        if (wanted != editor_lights) {
            if (wanted) { scene->addEntity(sun); scene->addEntity(fill); } else { scene->remove(sun); scene->remove(fill); }
            editor_lights = wanted;
        }
        if (wanted) {
            lm.setShadowOptions(lm.getInstance(sun), directional);
            // They exist to show the scene, so they follow its exposure: an indoor EV100 must not burn it out.
            const auto stops = float(std::pow(2.0, e.ev100 - 15));
            lm.setIntensity(lm.getInstance(sun), 100000 * stops);
            lm.setIntensity(lm.getInstance(fill), 30000 * stops);
        }
        if (frame.show_grid != grid_shown) {
            if (frame.show_grid) scene->addEntity(grid); else scene->remove(grid);
            grid_shown = frame.show_grid;
        }
        if (frame.grid_spacing != grid_spacing) {
            auto &transforms = engine->getTransformManager();
            if (!transforms.hasComponent(grid)) transforms.create(grid);
            const auto spacing = float(frame.grid_spacing);
            transforms.setTransform(transforms.getInstance(grid), filament::math::mat4f::scaling(filament::math::float3{spacing, 1.f, spacing}));
            grid_spacing = frame.grid_spacing;
        }
    }
    // Builds the cubemaps from an equirectangular image with Filament's GPU prefilter. Outside beginFrame/endFrame.
    void build_environment(const float *pixels, uint32_t width, uint32_t height, const RenderEnvironment &e) {
        uint8_t levels = 1;
        for (auto size = std::max(width, height); size > 1; size /= 2) ++levels;
        const std::size_t bytes = std::size_t(width) * height * 4 * sizeof(float);
        auto *equirect = Texture::Builder().width(width).height(height).levels(levels).sampler(Texture::Sampler::SAMPLER_2D)
            .format(Texture::InternalFormat::RGBA16F).usage(Texture::Usage::DEFAULT | Texture::Usage::GEN_MIPMAPPABLE).build(*engine);
        equirect->setImage(*engine, 0, Texture::PixelBufferDescriptor(detail::copy_bytes(pixels, bytes), bytes, Texture::Format::RGBA, Texture::Type::FLOAT, detail::release));
        equirect->generateMipmaps(*engine);
        if (!ibl_context) ibl_context = std::make_unique<IBLPrefilterContext>(*engine);
        IBLPrefilterContext::EquirectangularToCubemap to_cube(*ibl_context);
        constexpr uint32_t cube_size = 512;
        auto *cube = Texture::Builder().width(cube_size).height(cube_size).levels(10).sampler(Texture::Sampler::SAMPLER_CUBEMAP)
            .format(Texture::InternalFormat::R11F_G11F_B10F)
            .usage(Texture::Usage::COLOR_ATTACHMENT | Texture::Usage::SAMPLEABLE | Texture::Usage::GEN_MIPMAPPABLE).build(*engine);
        to_cube(equirect, cube);
        engine->destroy(equirect);
        IBLPrefilterContext::SpecularFilter specular(*ibl_context);
        IBLPrefilterContext::IrradianceFilter diffuse(*ibl_context);
        reflections = specular(cube);
        irradiance = diffuse(cube);
        sky_cubemap = cube;
        indirect = IndirectLight::Builder().reflections(reflections).irradiance(irradiance).intensity(float(e.intensity)).build(*engine);
        image_sky = Skybox::Builder().environment(sky_cubemap).build(*engine);
        ++environment_builds;
    }
    void sync_environment(const FrameSnapshot &frame, const ResourceRegistry &registry) {
        const auto &e = frame.environment;
        if (applied_environment && *applied_environment == e) return;
        const auto rebuild_key = [](const RenderEnvironment &v) { return std::make_tuple(v.source, v.image.index, v.image.generation, v.sky_color.x, v.sky_color.y, v.sky_color.z, v.horizon_color.x, v.horizon_color.y, v.horizon_color.z, v.ground_color.x, v.ground_color.y, v.ground_color.z, v.rotation_radians); };
        if (!applied_environment || rebuild_key(*applied_environment) != rebuild_key(e)) {
            // The flush keeps a cubemap the last frame used alive until the GPU is done with it.
            engine->flushAndWait();
            destroy_environment();
            std::vector<float> pixels;
            uint32_t width = 0, height = 0;
            if (e.source == EnvironmentSource::Procedural) {
                width = 256; height = 128;
                pixels = procedural_sky(e, width, height);
            } else if (e.source == EnvironmentSource::Image) {
                if (const auto data = registry.find(e.image)) {
                    width = data->width; height = data->height;
                    pixels.resize(std::size_t(width) * height * 4);
                    for (std::size_t i = 0; i < std::size_t(width) * height; ++i) {
                        pixels[i * 4] = data->pixels[i * 3]; pixels[i * 4 + 1] = data->pixels[i * 3 + 1]; pixels[i * 4 + 2] = data->pixels[i * 3 + 2]; pixels[i * 4 + 3] = 1;
                    }
                }
            }
            if (width) {
                // Rotation shifts columns before the conversion: unlike the indirect light, a skybox cannot rotate.
                const double turns = e.rotation_radians / (2 * std::numbers::pi);
                const auto shift = uint32_t(std::llround((turns - std::floor(turns)) * width)) % width;
                if (shift) for (uint32_t y = 0; y < height; ++y) {
                    auto *row = pixels.data() + std::size_t(y) * width * 4;
                    std::rotate(row, row + std::size_t(width - shift) * 4, row + std::size_t(width) * 4);
                }
                build_environment(pixels.data(), width, height, e);
                scene->setIndirectLight(indirect);
                preview_scene->setIndirectLight(indirect);
                thumbnail_scene->setIndirectLight(indirect);
            }
        }
        if (indirect) indirect->setIntensity(float(e.intensity));
        scene->setSkybox(e.show_skybox && image_sky ? image_sky : flat_sky);
        set_exposure(*camera, e.ev100);
        set_exposure(*preview_camera, e.ev100);
        set_exposure(*thumbnail_camera, e.ev100);
        if (!applied_environment || applied_environment->tone_mapping != e.tone_mapping) {
            if (grading) engine->destroy(grading);
            std::unique_ptr<const ToneMapper> tone(new_tone_mapper(e.tone_mapping));
            grading = ColorGrading::Builder().toneMapper(tone.get()).build(*engine);
            view->setColorGrading(grading); preview_view->setColorGrading(grading);
        }
        view->setShadowingEnabled(e.shadow_quality != ShadowQuality::Off);
        view->setShadowType(ShadowType::PCF); // DPCF drew no shadow in --render-test on this driver, so every quality uses PCF.
        View::AmbientOcclusionOptions occlusion; occlusion.enabled = e.ambient_occlusion;
        view->setAmbientOcclusionOptions(occlusion);
        View::BloomOptions bloom; bloom.enabled = e.bloom; bloom.strength = 0.08f;
        view->setBloomOptions(bloom);
        applied_environment = e;
    }
};
DesktopRenderer::DesktopRenderer(void *window, const std::filesystem::path &materials) : impl_(std::make_unique<Impl>()) { impl_->init(window, materials); }
DesktopRenderer::~DesktopRenderer() = default;
void DesktopRenderer::resize_viewport(unsigned width, unsigned height) {
    auto &p = *impl_; width = std::clamp(width, 1u, 4096u); height = std::clamp(height, 1u, 4096u);
    if (p.viewport_width == width && p.viewport_height == height) return;
    p.engine->flushAndWait();
    p.view->setRenderTarget(nullptr);
    p.engine->destroy(p.target); p.engine->destroy(p.color); p.engine->destroy(p.depth);
    p.color = Texture::Builder().width(width).height(height).levels(1).sampler(Texture::Sampler::SAMPLER_2D)
        .format(Texture::InternalFormat::RGBA8).usage(Texture::Usage::COLOR_ATTACHMENT | Texture::Usage::SAMPLEABLE | Texture::Usage::BLIT_SRC).build(*p.engine);
    p.depth = Texture::Builder().width(width).height(height).levels(1).sampler(Texture::Sampler::SAMPLER_2D)
        .format(Texture::InternalFormat::DEPTH24).usage(Texture::Usage::DEPTH_ATTACHMENT).build(*p.engine);
    p.target = RenderTarget::Builder().texture(RenderTarget::AttachmentPoint::COLOR, p.color)
        .texture(RenderTarget::AttachmentPoint::DEPTH, p.depth).build(*p.engine);
    p.view->setRenderTarget(p.target); p.view->setViewport({0,0,width,height});
    p.viewport_width = width; p.viewport_height = height;
}
std::uint64_t DesktopRenderer::viewport_texture() const { return reinterpret_cast<std::uint64_t>(impl_->color); }
void DesktopRenderer::set_material_preview(MaterialHandle handle) { impl_->preview_material = handle; }
std::uint64_t DesktopRenderer::preview_texture() const { return reinterpret_cast<std::uint64_t>(impl_->preview_color); }
void DesktopRenderer::request_thumbnail(std::uint64_t key, MeshHandle mesh, MaterialHandle material) {
    auto &requests = impl_->thumbnail_requests;
    std::erase_if(requests, [&](const Impl::ThumbnailRequest &request) { return request.key == key; });
    requests.push_back({key, mesh, material});
}
void DesktopRenderer::set_thumbnail_image(std::uint64_t key, const std::vector<std::uint8_t> &rgba, unsigned width, unsigned height) {
    auto &p = *impl_;
    if (!width || !height || rgba.size() != std::size_t(width) * height * 4) return;
    Impl::ThumbnailImage image;
    image.color = Texture::Builder().width(width).height(height).levels(1).sampler(Texture::Sampler::SAMPLER_2D).format(Texture::InternalFormat::RGBA8).build(*p.engine);
    image.color->setImage(*p.engine, 0, Texture::PixelBufferDescriptor(detail::copy_bytes(rgba.data(), rgba.size()), rgba.size(), Texture::Format::RGBA, Texture::Type::UBYTE, detail::release));
    if (const auto old = p.thumbnails.find(key); old != p.thumbnails.end()) p.retire(old->second);
    p.thumbnails[key] = image;
}
DesktopRenderer::Thumbnail DesktopRenderer::thumbnail(std::uint64_t key) const {
    const auto found = impl_->thumbnails.find(key);
    if (found == impl_->thumbnails.end()) return {};
    return {reinterpret_cast<std::uint64_t>(found->second.color), found->second.flipped};
}
void DesktopRenderer::forget_thumbnail(std::uint64_t key) {
    auto &p = *impl_;
    std::erase_if(p.thumbnail_requests, [&](const Impl::ThumbnailRequest &request) { return request.key == key; });
    if (const auto old = p.thumbnails.find(key); old != p.thumbnails.end()) { p.retire(old->second); p.thumbnails.erase(old); }
}
RendererStats DesktopRenderer::stats() const {
    RendererStats s;
    const auto &p = *impl_;
    s.objects = p.objects.size(); s.lights = p.lights.size();
    for (const auto &[key, object] : p.objects) { (void)key; s.parts += object.parts.size(); s.material_instances += object.parts.size(); }
    s.meshes = asset_mesh_count(); s.textures = p.textures.size();
    s.environment_builds = p.environment_builds;
    for (const auto &info : p.renderer->getFrameInfoHistory(4))
        if (info.gpuFrameDuration > 0) { s.gpu_frame_ms = double(info.gpuFrameDuration) / 1e6; break; }
    return s;
}
std::size_t DesktopRenderer::object_count() const { return impl_->objects.size(); }
std::size_t DesktopRenderer::light_count() const { return impl_->lights.size(); }
std::size_t DesktopRenderer::asset_mesh_count() const {
    return static_cast<std::size_t>(std::ranges::count_if(impl_->meshes, [](const auto &entry) { return entry.first >= ResourceRegistry::first_index; }));
}
std::size_t DesktopRenderer::asset_texture_count() const { return impl_->textures.size(); }
std::vector<std::string> DesktopRenderer::take_issues() { return std::exchange(impl_->issues, {}); }
bool DesktopRenderer::render(const FrameSnapshot &frame, const ResourceRegistry &registry, ImDrawData *data, unsigned width, unsigned height) {
    auto &p = *impl_;
    if (!width || !height || !p.target) return false;
    p.clear_thumbnail_object(); // Before the sweep in sync_objects, which would free what it draws.
    p.sync_environment(frame, registry);
    p.sync_objects(frame, registry);
    p.sync_lights(frame);
    std::set<uint32_t> thumbnail_textures;
    const bool draw_thumbnail = p.prepare_thumbnail(registry, thumbnail_textures);
    p.camera->setModelMatrix(matrix(frame.camera.world));
    p.camera->setCustomProjection(matrix(frame.camera.projection), frame.camera.lens.near_plane, frame.camera.lens.far_plane);
    p.ui->update(data, width, height);
    if (!p.renderer->beginFrame(p.swap)) {
        if (draw_thumbnail && p.thumbnail_drawing) p.thumbnail_requests.push_back(*p.thumbnail_drawing);
        return false;
    }
    if (draw_thumbnail) p.renderer->render(p.thumbnail_view);
    if (p.preview_material.index) p.renderer->render(p.preview_view);
    p.renderer->render(p.view);
    p.renderer->render(p.ui->view());
    p.renderer->endFrame();
    p.destroy_retired_thumbnails();
    return true;
}
std::vector<std::uint8_t> DesktopRenderer::readback(unsigned width, unsigned height) {
    auto &p = *impl_;
    std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    std::atomic<bool> done = false;
    p.engine->flushAndWait();
    if (!p.renderer->beginFrame(p.swap)) throw std::runtime_error("Capture frame skipped");
    p.renderer->render(p.view); p.renderer->render(p.ui->view());
    p.renderer->readPixels(0, 0, width, height, Texture::PixelBufferDescriptor(pixels.data(), pixels.size(),
        Texture::Format::RGBA, Texture::Type::UBYTE, [](void *, std::size_t, void *user) { static_cast<std::atomic<bool> *>(user)->store(true); }, &done));
    p.renderer->endFrame(); p.engine->flushAndWait();
    if (!done) throw std::runtime_error("Readback callback did not complete");
    return pixels;
}
void DesktopRenderer::capture(const std::filesystem::path &path, unsigned width, unsigned height) {
    const auto pixels = readback(width, height);
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << width << ' ' << height << "\n255\n";
    // Filament readPixels already returns rows from top to bottom, including
    // on OpenGL. Reversing them here would invert the exported image.
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x)
        file.write(reinterpret_cast<const char *>(pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4), 3);
    if (!file) throw std::runtime_error("Cannot save capture");
}
} // namespace engine::renderer
