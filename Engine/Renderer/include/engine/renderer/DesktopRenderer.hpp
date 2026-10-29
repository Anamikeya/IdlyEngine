#pragma once
#include <engine/renderer/RenderSnapshot.hpp>
#include <filesystem>
#include <memory>
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
    // Synchronous diagnostic readback; only used by acceptance runs.
    void capture(const std::filesystem::path &ppm, unsigned width, unsigned height);
    std::size_t object_count() const;
};
} // namespace engine::renderer
