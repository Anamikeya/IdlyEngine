#pragma once
#include <engine/commands/CommandBus.hpp>
#include <engine/renderer/RenderSnapshot.hpp>
#include <filesystem>

namespace engine::editor {
struct OrbitCamera {
    Vec3 target{0, 0.5, 0};
    double yaw = 0.7, pitch = 0.4, distance = 8;
    void orbit(double dx, double dy);
    void pan(double dx, double dy, double viewport_height);
    void zoom(double steps);
    WorldTransform world() const;
};

// Single owner thread. This owns the actual scene, never a competing authoring model.
class EditorSession {
    Scene scene_;
    commands::CommandBus bus_{scene_};
    commands::Context context_{"local-editor", 3};
    EntityId selected_ = 0;
    std::uint64_t frame_ = 0;
    void reconcile();
public:
    OrbitCamera camera;
    const Scene &scene() const { return scene_; }
    const commands::CommandBus &commands() const { return bus_; }
    EntityId selection() const { return selected_; }
    void select(EntityId id);
    Result<commands::Receipt> execute(commands::Command);
    Result<commands::Receipt> create(std::string name = "Cube", EntityId parent = 0);
    Result<commands::Receipt> undo();
    Result<commands::Receipt> redo();
    Result<void> save(const std::filesystem::path &) const;
    Result<void> load(const std::filesystem::path &);
    Result<void> set_world(EntityId, const WorldTransform &);
    void focus();
    Result<renderer::Frame> frame(double aspect);
    // Ray versus transformed unit cubes; nearest positive hit, zero is empty space.
    EntityId pick(const renderer::FrameSnapshot &, double ndc_x, double ndc_y) const;
};
} // namespace engine::editor
