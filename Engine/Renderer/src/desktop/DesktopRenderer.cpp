#include <engine/renderer/DesktopRenderer.hpp>
#include <engine/renderer/ResourceRegistry.hpp>
#include "FilamentUtilities.hpp"
#include "ImGuiRenderer.hpp"
#include <filament/Camera.h>
#include <filament/Color.h>
#include <filament/IndexBuffer.h>
#include <filament/LightManager.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/RenderTarget.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/Viewport.h>
#include <utils/EntityManager.h>
#include <math/mat3.h>
#include <algorithm>
#include <map>
#include <numbers>
#include <set>
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
// A range of the index buffer with its own material. `own_material` parts (GLB) use the values here; others take
// roughness and metallic from the object's Surface.
struct Part {
    uint32_t first = 0, count = 0;
    bool own_material = false;
    float3 color{1, 1, 1};
    float roughness = 0, metallic = 0;
    Texture *image = nullptr;
};
struct GpuMesh : Mesh {
    uint32_t generation = 1;
    std::vector<Part> parts;
    std::vector<Texture *> images;
};
struct GpuTexture { Texture *texture = nullptr; uint32_t generation = 0; };
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
quatf tangent_frame(float3 t, float3 n) { return mat3f::packTangentFrame(mat3f(t, cross(n, t), n)); }
// Any frame around the normal: no material here uses a tangent-space map.
quatf tangent_frame(float3 n) { return tangent_frame(normalize(cross(std::abs(n.y) > 0.5f ? float3{0,0,1} : float3{0,1,0}, n)), n); }
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
        for (float2 corner : {float2{-0.5f,-0.5f}, float2{0.5f,-0.5f}, float2{0.5f,0.5f}, float2{-0.5f,0.5f}})
            vertices.push_back({n * 0.5f + t * corner.x + b * corner.y, q, {1,1,1,1}, corner + 0.5f});
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
            vertices.push_back({n * 0.5f, tangent_frame(float3{-std::sin(phi), 0, std::cos(phi)}, n), {1,1,1,1}, {float(j) / slices, float(i) / stacks}});
        }
    }
    for (int i = 0; i < stacks; ++i) for (int j = 0; j < slices; ++j) {
        const auto a = static_cast<uint16_t>(i * (slices + 1) + j), b = static_cast<uint16_t>(a + slices + 1);
        for (int index : {int(a), int(b), a + 1, a + 1, int(b), b + 1}) indices.push_back(static_cast<uint16_t>(index));
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
}
struct DesktopRenderer::Impl {
    struct Object { utils::Entity entity; std::vector<MaterialInstance *> materials; MeshHandle mesh; TextureHandle texture; };
    struct Light { utils::Entity entity; LightKind kind; };
    using Key = std::pair<std::uint64_t, EntityId>; // Frame scene identity, entity.
    Engine *engine = nullptr;
    Renderer *renderer = nullptr;
    SwapChain *swap = nullptr;
    filament::Scene *scene = nullptr;
    View *view = nullptr;
    Camera *camera = nullptr;
    Skybox *sky = nullptr;
    Material *lit_material = nullptr, *grid_material = nullptr;
    Texture *color = nullptr, *depth = nullptr, *white = nullptr;
    RenderTarget *target = nullptr;
    utils::Entity camera_entity, grid, sun, fill;
    bool editor_lights = true;
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
    // Null when the GPU cannot take the image; the caller falls back to white, so the asset still draws.
    Texture *upload(const assets::TextureData &data) {
        const auto limit = Texture::getMaxTextureSize(*engine, Texture::Sampler::SAMPLER_2D);
        if (data.width > limit || data.height > limit) {
            issues.push_back("A " + std::to_string(data.width) + "x" + std::to_string(data.height) + " image exceeds the GPU limit of " + std::to_string(limit) + " and is shown white");
            return nullptr;
        }
        uint8_t levels = 1;
        for (auto size = std::max(data.width, data.height); size > 1; size /= 2) ++levels;
        auto *texture = Texture::Builder().width(data.width).height(data.height).levels(levels).sampler(Texture::Sampler::SAMPLER_2D)
            .format(data.srgb ? Texture::InternalFormat::SRGB8_A8 : Texture::InternalFormat::RGBA8)
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
            vertices.push_back({{v.position[0], v.position[1], v.position[2]}, tangent_frame(n), {1,1,1,1}, {v.uv[0], v.uv[1]}});
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
        for (const auto &image : data.images) result.images.push_back(upload(*image));
        for (const auto &source : data.submeshes) {
            Part part{source.first_index, source.index_count, true};
            const auto &m = source.material;
            part.color = {m.base_color[0], m.base_color[1], m.base_color[2]};
            part.roughness = m.roughness; part.metallic = m.metallic;
            if (m.image >= 0) part.image = result.images.at(static_cast<std::size_t>(m.image));
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
    Texture *image(TextureHandle handle, const ResourceRegistry &registry) {
        if (!handle.index) return nullptr;
        const auto it = textures.find(handle.index);
        if (it != textures.end() && it->second.generation == handle.generation) return it->second.texture;
        const auto data = registry.find(handle);
        if (!data) return nullptr;
        if (it != textures.end()) { retired_textures.push_back(it->second.texture); textures.erase(it); }
        return textures.emplace(handle.index, GpuTexture{upload(*data), handle.generation}).first->second.texture;
    }
    void init(void *window, const std::filesystem::path &materials) {
        engine = Engine::create(Engine::Backend::OPENGL);
        if (!engine) throw std::runtime_error("Filament OpenGL engine creation failed");
        swap = engine->createSwapChain(window, SwapChain::CONFIG_READABLE);
        renderer = engine->createRenderer(); scene = engine->createScene(); view = engine->createView();
        camera_entity = utils::EntityManager::get().create(); camera = engine->createCamera(camera_entity);
        camera->setExposure(16, 1.0f / 125.0f, 100);
        view->setScene(scene); view->setCamera(camera);
        view->setPostProcessingEnabled(false);
        view->setShadowingEnabled(false);
        sky = Skybox::Builder().color({0.055f, 0.07f, 0.095f, 1}).build(*engine); scene->setSkybox(sky);
        lit_material = detail::material(*engine, materials / "lit.filamat");
        grid_material = detail::material(*engine, materials / "grid.filamat");
        meshes.emplace(builtin_cube.index, builtin(cube(*engine)));
        meshes.emplace(builtin_sphere.index, builtin(sphere(*engine)));
        meshes.emplace(builtin_plane.index, builtin(plane(*engine)));
        white = Texture::Builder().width(1).height(1).levels(1).sampler(Texture::Sampler::SAMPLER_2D).format(Texture::InternalFormat::RGBA8).build(*engine);
        const uint8_t opaque_white[4]{255, 255, 255, 255};
        white->setImage(*engine, 0, Texture::PixelBufferDescriptor(detail::copy_bytes(opaque_white, 4), 4, Texture::Format::RGBA, Texture::Type::UBYTE, detail::release));
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
        LightManager::Builder(LightManager::Type::DIRECTIONAL).direction({-0.5f,-1,-0.4f}).intensity(100000).castShadows(false).build(*engine, sun);
        LightManager::Builder(LightManager::Type::DIRECTIONAL).direction({0.7f,-0.2f,0.8f}).color({0.55f,0.7f,1}).intensity(30000).build(*engine, fill);
        scene->addEntity(sun); scene->addEntity(fill);
        ui = std::make_unique<detail::ImGuiRenderer>(*engine, materials);
    }
    void remove(utils::Entity e) { engine->destroy(e); utils::EntityManager::get().destroy(e); }
    void remove(const Object &o) { scene->remove(o.entity); remove(o.entity); for (auto *material : o.materials) engine->destroy(material); }
    void remove(const Light &l) { scene->remove(l.entity); remove(l.entity); }
    void destroy(const Mesh &m) { engine->destroy(m.vertices); engine->destroy(m.indices); }
    void destroy(const GpuMesh &m) {
        destroy(static_cast<const Mesh &>(m));
        for (auto *texture : m.images) if (texture) engine->destroy(texture);
    }
    void destroy(Texture *texture) { if (texture) engine->destroy(texture); }
    ~Impl() {
        if (!engine) return;
        engine->flushAndWait();
        ui.reset();
        engine->destroy(view);
        for (const auto &[key, object] : objects) { (void)key; remove(object); }
        for (const auto &[key, light] : lights) { (void)key; remove(light); }
        engine->destroy(scene);
        remove(grid); remove(sun); remove(fill);
        engine->destroyCameraComponent(camera_entity); utils::EntityManager::get().destroy(camera_entity);
        engine->destroy(target); engine->destroy(color); engine->destroy(depth);
        for (const auto &[index, gpu] : meshes) { (void)index; destroy(gpu); }
        for (const auto &[index, gpu] : textures) { (void)index; destroy(gpu.texture); }
        destroy(grid_mesh); engine->destroy(white);
        engine->destroy(lit_material); engine->destroy(grid_material); engine->destroy(sky);
        engine->destroy(renderer); engine->destroy(swap);
        engine->flushAndWait(); Engine::destroy(&engine);
    }
    void sync_objects(const FrameSnapshot &frame, const ResourceRegistry &registry) {
        std::set<Key> live;
        std::set<uint32_t> used_meshes, used_textures;
        for (const auto &object : frame.objects) {
            if (object.material != builtin_lit) throw std::runtime_error("Unknown or stale material handle");
            const GpuMesh *gpu = geometry(object.mesh, registry);
            if (!gpu && object.mesh.index < ResourceRegistry::first_index) throw std::runtime_error("Unknown built-in mesh handle");
            if (!gpu) continue;
            used_meshes.insert(object.mesh.index);
            if (object.surface.texture.index) used_textures.insert(object.surface.texture.index);
            const Key key{frame.stamp.scene, object.entity}; live.insert(key);
            auto it = objects.find(key);
            if (it != objects.end() && it->second.mesh != object.mesh) { remove(it->second); objects.erase(it); it = objects.end(); }
            const bool created = it == objects.end();
            if (created) {
                Object fresh{utils::EntityManager::get().create(), {}, object.mesh, {}};
                RenderableManager::Builder builder(gpu->parts.size());
                builder.boundingBox(gpu->bounds).castShadows(false).receiveShadows(false);
                for (std::size_t i = 0; i < gpu->parts.size(); ++i) {
                    fresh.materials.push_back(lit_material->createInstance());
                    builder.geometry(i, RenderableManager::PrimitiveType::TRIANGLES, gpu->vertices, gpu->indices, gpu->parts[i].first, gpu->parts[i].count)
                        .material(i, fresh.materials[i]);
                }
                builder.build(*engine, fresh.entity);
                scene->addEntity(fresh.entity); it = objects.emplace(key, std::move(fresh)).first;
            }
            auto &record = it->second;
            auto *override_image = image(object.surface.texture, registry);
            const bool rebind = created || record.texture != object.surface.texture;
            record.texture = object.surface.texture;
            for (std::size_t i = 0; i < gpu->parts.size(); ++i) {
                const auto &part = gpu->parts[i];
                auto *material = record.materials[i];
                material->setParameter("baseColor", vec(object.surface.linear_color) * part.color);
                material->setParameter("roughness", part.own_material ? part.roughness : float(object.surface.roughness));
                material->setParameter("metallic", part.own_material ? part.metallic : float(object.surface.metallic));
                if (rebind) {
                    const TextureSampler sampler(TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR, TextureSampler::MagFilter::LINEAR, TextureSampler::WrapMode::REPEAT);
                    material->setParameter("baseColorMap", override_image ? override_image : part.image ? part.image : white, sampler);
                }
            }
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
        for (auto it = meshes.begin(); it != meshes.end();)
            if (it->first >= ResourceRegistry::first_index && !used_meshes.contains(it->first)) { destroy(it->second); it = meshes.erase(it); } else ++it;
        for (auto it = textures.begin(); it != textures.end();)
            if (!used_textures.contains(it->first)) { destroy(it->second.texture); it = textures.erase(it); } else ++it;
    }
    void sync_lights(const FrameSnapshot &frame) {
        std::set<Key> live;
        auto &lm = engine->getLightManager();
        for (const auto &light : frame.lights) {
            const Key key{frame.stamp.scene, light.entity}; live.insert(key);
            auto it = lights.find(key);
            if (it != lights.end() && it->second.kind != light.kind) { remove(it->second); lights.erase(it); it = lights.end(); }
            if (it == lights.end()) {
                const Light created{utils::EntityManager::get().create(), light.kind};
                LightManager::Builder(light.kind == LightKind::Point ? LightManager::Type::POINT : LightManager::Type::DIRECTIONAL)
                    .castShadows(false).build(*engine, created.entity);
                scene->addEntity(created.entity); it = lights.emplace(key, created).first;
            }
            const auto instance = lm.getInstance(it->second.entity);
            lm.setColor(instance, LinearColor(vec(light.linear_color)));
            lm.setIntensity(instance, float(light.intensity));
            if (light.kind == LightKind::Point) { lm.setPosition(instance, vec(light.position)); lm.setFalloff(instance, float(light.range)); }
            else lm.setDirection(instance, vec(light.direction));
        }
        for (auto it = lights.begin(); it != lights.end();)
            if (!live.contains(it->first)) { remove(it->second); it = lights.erase(it); } else ++it;
        const bool wanted = frame.lights.empty();
        if (wanted != editor_lights) {
            if (wanted) { scene->addEntity(sun); scene->addEntity(fill); } else { scene->remove(sun); scene->remove(fill); }
            editor_lights = wanted;
        }
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
    p.sync_objects(frame, registry);
    p.sync_lights(frame);
    p.camera->setModelMatrix(matrix(frame.camera.world));
    p.camera->setCustomProjection(matrix(frame.camera.projection), frame.camera.lens.near_plane, frame.camera.lens.far_plane);
    p.ui->update(data, width, height);
    if (!p.renderer->beginFrame(p.swap)) return false;
    p.renderer->render(p.view);
    p.renderer->render(p.ui->view());
    p.renderer->endFrame();
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
