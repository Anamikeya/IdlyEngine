#pragma once
#include <engine/commands/CommandBus.hpp>
#include <engine/renderer/RenderSnapshot.hpp>
#include <chrono>
#include <filesystem>
#include <memory>

namespace engine::editor {
struct OrbitCamera {
    Vec3 target{0, 0.5, 0};
    double yaw = 0.7, pitch = 0.4, distance = 8;
    void orbit(double dx, double dy);
    void pan(double dx, double dy, double viewport_height);
    void zoom(double steps);
    WorldTransform world() const;
};

enum class PlayState { Editing, Playing, Paused };

// Single owner thread. This owns the actual scene, never a competing authoring model.
//
// Play mode copies the editor scene into an isolated runtime scene with its own history-free command bus.
// While playing, scene(), execute(), set_world() and frame() act on the runtime copy; the editor scene, its
// revision and its undo history are untouched, and stop() discards the runtime copy.
class EditorSession {
    struct Runtime;
    Scene scene_;
    commands::CommandBus bus_{scene_};
    commands::Context context_{"local-editor", 3};
    std::unique_ptr<Runtime> runtime_;
    std::uint64_t play_sessions_ = 0;
    EntityId selected_ = 0;
    std::uint64_t frame_ = 0;
    // Entities as last saved, loaded or started fresh; the document is dirty when the scene differs.
    std::vector<EntityRecord> clean_;
    mutable std::uint64_t dirty_revision_ = ~std::uint64_t(0);
    mutable bool dirty_ = false;
    void reconcile();
    Result<void> editing_only(const char *action) const;
public:
    static constexpr std::chrono::nanoseconds fixed_step{16'666'667}; // 60 Hz runtime ticks
    static constexpr int max_ticks_per_update = 5;
    OrbitCamera camera;
    EditorSession();
    ~EditorSession();
    EditorSession(const EditorSession &) = delete;
    EditorSession &operator=(const EditorSession &) = delete;
    // The scene being shown and edited: the runtime copy while playing, otherwise the editor scene.
    const Scene &scene() const;
    const Scene &editor_scene() const { return scene_; }
    // Identity of the shown scene (1 = editor scene, each play session its own) and that scene's revision.
    // Together they change whenever what scene() shows may have changed.
    std::uint64_t scene_identity() const;
    std::uint64_t scene_revision() const;
    const commands::CommandBus &commands() const { return bus_; }
    EntityId selection() const { return selected_; }
    void select(EntityId id);
    Result<commands::Receipt> execute(commands::Command);
    Result<commands::Receipt> create(std::string name = "Cube", EntityId parent = 0);
    Result<commands::Receipt> undo();
    Result<commands::Receipt> redo();
    // Replaces the editor scene with an empty one (undoable) and starts a clean document.
    Result<void> new_scene();
    Result<void> save(const std::filesystem::path &);
    Result<void> load(const std::filesystem::path &);
    // True when the editor scene's entities differ from the last save, load or new_scene().
    bool dirty() const;
    // Treats the current editor scene as saved, e.g. for a starter scene the user has not touched.
    void mark_clean();
    Result<void> set_world(EntityId, const WorldTransform &);
    void focus();
    Result<renderer::Frame> frame(double aspect);
    // Ray versus transformed unit cubes; nearest positive hit, zero is empty space.
    EntityId pick(const renderer::FrameSnapshot &, double ndc_x, double ndc_y) const;

    // ---- Play mode ----
    PlayState play_state() const;
    // Editing: copies the editor scene into a new runtime scene. Paused: resumes.
    Result<void> play();
    Result<void> pause();
    // One fixed tick while paused.
    Result<void> step();
    // Discards the runtime scene and returns to the editor scene.
    Result<void> stop();
    // Advances runtime time while playing: whole fixed_step ticks, at most max_ticks_per_update per call.
    void update(std::chrono::nanoseconds elapsed);
    std::uint64_t runtime_ticks() const;
};
} // namespace engine::editor
