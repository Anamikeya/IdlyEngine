#pragma once
#include <engine/renderer/RenderSnapshot.hpp>
#include <filesystem>
#include <memory>
#include <vector>
struct ImDrawData;
namespace engine::renderer {
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
    std::uint64_t viewport_texture() const;
    bool render(const FrameSnapshot &, ImDrawData *, unsigned width, unsigned height);
    // Synchronous diagnostic readback of the whole window (RGBA8, rows top to bottom); acceptance runs only.
    std::vector<std::uint8_t> readback(unsigned width, unsigned height);
    void capture(const std::filesystem::path &ppm, unsigned width, unsigned height);
    std::size_t object_count() const;
    // Scene lights currently mirrored; the built-in editor lights are used only while this is zero.
    std::size_t light_count() const;
};
} // namespace engine::renderer
