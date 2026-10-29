#include <engine/renderer/DesktopRenderer.hpp>
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
#include <filament/TransformManager.h>
#include <filament/VertexBuffer.h>
#include <filament/Viewport.h>
#include <utils/EntityManager.h>
#include <math/mat3.h>
#include <map>
#include <numbers>
#include <set>
#include <atomic>

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
struct Vertex { float3 position; quatf tangent; float4 color; };
struct Mesh { VertexBuffer *vertices = nullptr; IndexBuffer *indices = nullptr; uint32_t count = 0; Box bounds; };
Mesh mesh(Engine &engine, const std::vector<Vertex> &vertices, const std::vector<uint16_t> &indices, Box bounds) {
    Mesh result;
    result.vertices = VertexBuffer::Builder().vertexCount(static_cast<uint32_t>(vertices.size())).bufferCount(1)
        .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3, offsetof(Vertex, position), sizeof(Vertex))
        .attribute(VertexAttribute::TANGENTS, 0, VertexBuffer::AttributeType::FLOAT4, offsetof(Vertex, tangent), sizeof(Vertex))
        .attribute(VertexAttribute::COLOR, 0, VertexBuffer::AttributeType::FLOAT4, offsetof(Vertex, color), sizeof(Vertex)).build(engine);
    result.indices = IndexBuffer::Builder().indexCount(static_cast<uint32_t>(indices.size())).bufferType(IndexBuffer::IndexType::USHORT).build(engine);
    result.count = static_cast<uint32_t>(indices.size());
    result.bounds = bounds;
    result.vertices->setBufferAt(engine, 0, detail::upload(vertices.data(), vertices.size() * sizeof(Vertex)));
    result.indices->setBuffer(engine, detail::upload(indices.data(), indices.size() * sizeof(uint16_t)));
    return result;
}
quatf tangent_frame(float3 t, float3 n) { return mat3f::packTangentFrame(mat3f(t, cross(n, t), n)); }
Mesh cube(Engine &engine) {
    std::vector<Vertex> vertices; std::vector<uint16_t> indices;
    for (float3 n : {float3{1,0,0}, float3{-1,0,0}, float3{0,1,0}, float3{0,-1,0}, float3{0,0,1}, float3{0,0,-1}}) {
        const float3 t = normalize(cross(std::abs(n.y) > 0.5f ? float3{0,0,1} : float3{0,1,0}, n));
        const float3 b = cross(n, t);
        const auto q = tangent_frame(t, n);
        const auto start = static_cast<uint16_t>(vertices.size());
        for (float2 corner : {float2{-0.5f,-0.5f}, float2{0.5f,-0.5f}, float2{0.5f,0.5f}, float2{-0.5f,0.5f}})
            vertices.push_back({n * 0.5f + t * corner.x + b * corner.y, q, {1,1,1,1}});
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
            vertices.push_back({n * 0.5f, tangent_frame(float3{-std::sin(phi), 0, std::cos(phi)}, n), {1,1,1,1}});
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
    const std::vector<Vertex> vertices{{{-0.5f,0,-0.5f}, q, {1,1,1,1}}, {{0.5f,0,-0.5f}, q, {1,1,1,1}},
                                       {{0.5f,0,0.5f}, q, {1,1,1,1}}, {{-0.5f,0,0.5f}, q, {1,1,1,1}}};
    return mesh(engine, vertices, {0,2,1,0,3,2}, {{0,0,0},{0.5f,0.01f,0.5f}});
}
}
struct DesktopRenderer::Impl {
    struct Object { utils::Entity entity; MaterialInstance *material = nullptr; MeshHandle mesh; };
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
    Texture *color = nullptr, *depth = nullptr;
    RenderTarget *target = nullptr;
    utils::Entity camera_entity, grid, sun, fill;
    bool editor_lights = true;
    Mesh cube_mesh, sphere_mesh, plane_mesh, grid_mesh;
    std::map<Key, Object> objects;
    std::map<Key, Light> lights;
    std::unique_ptr<detail::ImGuiRenderer> ui;
    unsigned viewport_width = 0, viewport_height = 0;
    const Mesh *builtin(MeshHandle handle) const {
        if (handle == builtin_cube) return &cube_mesh;
        if (handle == builtin_sphere) return &sphere_mesh;
        if (handle == builtin_plane) return &plane_mesh;
        return nullptr;
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
        cube_mesh = cube(*engine); sphere_mesh = sphere(*engine); plane_mesh = plane(*engine);
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
    void remove(const Object &o) { scene->remove(o.entity); remove(o.entity); engine->destroy(o.material); }
    void remove(const Light &l) { scene->remove(l.entity); remove(l.entity); }
    void destroy(const Mesh &m) { engine->destroy(m.vertices); engine->destroy(m.indices); }
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
        destroy(cube_mesh); destroy(sphere_mesh); destroy(plane_mesh); destroy(grid_mesh);
        engine->destroy(lit_material); engine->destroy(grid_material); engine->destroy(sky);
        engine->destroy(renderer); engine->destroy(swap);
        engine->flushAndWait(); Engine::destroy(&engine);
    }
    void sync_objects(const FrameSnapshot &frame) {
        std::set<Key> live;
        for (const auto &object : frame.objects) {
            const Mesh *geometry = builtin(object.mesh);
            if (!geometry || object.material != builtin_lit)
                throw std::runtime_error("Unknown or stale mesh/material handle");
            const Key key{frame.stamp.scene, object.entity}; live.insert(key);
            auto it = objects.find(key);
            if (it != objects.end() && it->second.mesh != object.mesh) { remove(it->second); objects.erase(it); it = objects.end(); }
            if (it == objects.end()) {
                Object created{utils::EntityManager::get().create(), lit_material->createInstance(), object.mesh};
                RenderableManager::Builder(1).boundingBox(geometry->bounds)
                    .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, geometry->vertices, geometry->indices, 0, geometry->count)
                    .material(0, created.material).castShadows(false).receiveShadows(false).build(*engine, created.entity);
                scene->addEntity(created.entity); it = objects.emplace(key, created).first;
            }
            auto *material = it->second.material;
            material->setParameter("baseColor", vec(object.surface.linear_color));
            material->setParameter("roughness", float(object.surface.roughness));
            material->setParameter("metallic", float(object.surface.metallic));
            auto &tm = engine->getTransformManager();
            tm.setTransform(tm.getInstance(it->second.entity), matrix(object.world));
        }
        for (auto it = objects.begin(); it != objects.end();)
            if (!live.contains(it->first)) { remove(it->second); it = objects.erase(it); } else ++it;
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
bool DesktopRenderer::render(const FrameSnapshot &frame, ImDrawData *data, unsigned width, unsigned height) {
    auto &p = *impl_;
    if (!width || !height || !p.target) return false;
    p.sync_objects(frame);
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
