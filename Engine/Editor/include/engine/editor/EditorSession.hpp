#pragma once
#include <engine/commands/CommandBus.hpp>
#include <engine/physics/scene_physics.h>
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
//
// Physics: an edit-mode bridge mirrors the editor scene's bodies and colliders without stepping (outlines,
// issues). Each play session gets its own bridge that steps once per fixed tick and publishes dynamic
// poses into the runtime copy through its history-free bus (contracts C-1, C-3).
class EditorSession {
    struct Runtime;
    Scene scene_;
    commands::CommandBus bus_{scene_};
    commands::Context context_{"local-editor", 3};
    std::unique_ptr<physics::ScenePhysics> edit_physics_;
    std::unique_ptr<Runtime> runtime_;
    std::string physics_error_; // Last runtime physics failure; Play is paused when one occurs.
    std::uint64_t play_sessions_ = 0;
    EntityId selected_ = 0;
    std::uint64_t frame_ = 0;
    // Entities as last saved, loaded or started fresh; the document is dirty when the scene differs.
    std::vector<EntityRecord> clean_;
    mutable std::uint64_t dirty_revision_ = ~std::uint64_t(0);
    mutable bool dirty_ = false;
    EntityId view_camera_ = 0;
    std::string view_camera_issue_;
    void reconcile();
    Result<void> editing_only(const char *action) const;
    // Local TRS that places an entity at `world` under `parent` (0 = root); rejects shear.
    Result<Transform> local_from_world(EntityId parent, const WorldTransform &world) const;
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
    // Creates an entity 0.5 m above the parent's origin and selects it. The default is a built-in cube.
    Result<commands::Receipt> create(std::string name = "Cube", EntityId parent = 0,
                                     Components components = Components{MeshComponent{}, {}, {}});
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
    // Moves the entity under `parent` (0 = root) keeping its world transform: one undo step. Rejects cycles
    // and parents whose transform would force shear.
    Result<commands::Receipt> reparent(EntityId id, EntityId parent);
    // Copies the entity and its subtree, components included, as "<name> (copy)": one undo step. Selects the copy.
    Result<commands::Receipt> duplicate(EntityId id);
    void focus();
    // Extracts the shown scene: mesh components become lit built-in meshes, light components scene lights.
    // Edit mode views through the editor camera. Play mode views through the first primary camera component
    // (by entity ID) and falls back to the editor camera when there is none or it cannot be used.
    Result<renderer::Frame> frame(double aspect);
    // Camera entity the last frame() looked through (0 = editor camera), and why a scene camera was skipped.
    EntityId view_camera() const { return view_camera_; }
    const std::string &view_camera_issue() const { return view_camera_issue_; }
    // Ray versus the rendered shapes (box, sphere, plane) in their local space; nearest hit, zero is empty space.
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
    // Each tick steps the runtime physics.
    void update(std::chrono::nanoseconds elapsed);
    std::uint64_t runtime_ticks() const;

    // ---- Physics of the shown scene (edit-mode bridge, or the play session's) ----
    // Brings the edit-mode bridge in line with the editor scene; frame() calls it. Never writes the scene.
    void sync_physics();
    const physics::ScenePhysics &physics_bridge() const;
    // A runtime physics failure that paused Play mode; empty otherwise.
    const std::string &physics_error() const { return physics_error_; }
private:
    void tick();
};
} // namespace engine::editor
