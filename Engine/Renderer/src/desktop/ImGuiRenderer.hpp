#pragma once
#include <filament/Engine.h>
#include <filament/View.h>
#include <utils/Entity.h>
#include <filesystem>
#include <vector>
struct ImDrawData;
namespace filament { class Material; class MaterialInstance; class Texture; class VertexBuffer; class IndexBuffer; class Scene; class Camera; }
namespace engine::renderer::detail {
// Draw-list translation using public Filament APIs, based on the approach in
// upstream filagui. No Vulkan/OpenGL handles or cross-context sharing.
class ImGuiRenderer {
    filament::Engine &engine_;
    filament::View *view_ = nullptr;
    filament::Scene *scene_ = nullptr;
    filament::Camera *camera_ = nullptr;
    utils::Entity camera_entity_, renderable_;
    filament::Material *material_ = nullptr;
    filament::Texture *font_ = nullptr;
    filament::VertexBuffer *vertices_ = nullptr;
    filament::IndexBuffer *indices_ = nullptr;
    std::vector<filament::MaterialInstance *> materials_;
public:
    ImGuiRenderer(filament::Engine &, const std::filesystem::path &);
    ~ImGuiRenderer();
    void update(ImDrawData *, unsigned width, unsigned height);
    filament::View *view() const { return view_; }
};
}
