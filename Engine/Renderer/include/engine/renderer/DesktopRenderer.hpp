#pragma once
#include <engine/renderer/RenderSnapshot.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
struct ImDrawData;
namespace engine::renderer {
class ResourceRegistry;
// What the renderer holds, for diagnostics and leak checks.
struct RendererStats {
    std::size_t objects = 0, parts = 0, lights = 0; // `parts` is the primitives submitted before culling.
    std::size_t skinned = 0, posed = 0;             // Objects drawn with bones, and those of them in a pose other than rest.
    std::size_t particle_batches = 0, particles = 0; // Drawn in the latest frame.
    std::size_t meshes = 0, textures = 0;           // Asset resources on the GPU, not built-ins.
    std::size_t material_instances = 0;
    std::uint64_t environment_builds = 0;           // IBL rebuilds so far.
    double gpu_frame_ms = 0;                        // Of the latest finished frame; 0 when the driver does not say.
};
// Desktop host boundary only; never include this in ECS components or scene files.
// UI uses an opaque texture token, not a Filament resource handle.
class DesktopRenderer {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    DesktopRenderer(void *native_window, const std::filesystem::path &materials);
    ~DesktopRenderer();
    DesktopRenderer(const DesktopRenderer &) = delete;
    DesktopRenderer &operator=(const DesktopRenderer &) = delete;
    void resize_viewport(unsigned width, unsigned height);
    // The viewport's image, with the selection outline (RenderObject::outline) drawn over it.
    std::uint64_t viewport_texture() const;
    // How far the selection outline reaches beyond a silhouette, in framebuffer pixels: 1 to 8, 2 at first.
    void set_outline_width(float pixels);
    // Uploads the frame's assets from the registry on first use, and frees those no object uses any more.
    // An object whose handle has gone stale (a frame older than a reload) is skipped for that frame.
    bool render(const FrameSnapshot &, const ResourceRegistry &, ImDrawData *, unsigned width, unsigned height);
    // Assets the GPU could not take since the last call (an image above the size limit), for the host to report.
    std::vector<std::string> take_issues();
    // Synchronous diagnostic readback of the whole window (RGBA8, rows top to bottom); acceptance runs only.
    std::vector<std::uint8_t> readback(unsigned width, unsigned height);
    void capture(const std::filesystem::path &ppm, unsigned width, unsigned height);
    // Draws one material on a sphere for the next render(); the token shows it in the UI. Zero handle: no preview.
    void set_material_preview(MaterialHandle);
    std::uint64_t preview_texture() const;
    // ---- Thumbnails: small images kept on the GPU, by a key the host chooses ----
    // Draws a mesh with a material (builtin_lit: its own) during a later render(), one a frame. Replaces any older one.
    void request_thumbnail(std::uint64_t key, MeshHandle, MaterialHandle);
    // An image given as RGBA8, rows top to bottom.
    void set_thumbnail_image(std::uint64_t key, const std::vector<std::uint8_t> &rgba, unsigned width, unsigned height);
    struct Thumbnail { std::uint64_t texture = 0; bool flipped = false; };
    // Texture zero until it exists. `flipped`: drawn into, so its rows run bottom to top.
    Thumbnail thumbnail(std::uint64_t key) const;
    void forget_thumbnail(std::uint64_t key);
    RendererStats stats() const;
    std::size_t object_count() const;
    // Scene lights currently mirrored; the built-in editor lights are used only while this is zero.
    std::size_t light_count() const;
    // Assets currently on the GPU: meshes (models only, not built-ins) and standalone textures.
    std::size_t asset_mesh_count() const;
    std::size_t asset_texture_count() const;
};
} // namespace engine::renderer
