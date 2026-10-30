#pragma once
#include <engine/assets/asset_database.h>
#include <engine/commands/CommandBus.hpp>
#include <engine/gameplay/game.h>
#include <engine/physics/scene_physics.h>
#include <engine/renderer/RenderSnapshot.hpp>
#include <engine/renderer/ResourceRegistry.hpp>
#include <chrono>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

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

// What an import left in the asset folder.
struct ImportedAsset {
    assets::AssetId id;
    std::string path;
    assets::AssetType type = assets::AssetType::Unknown;
    std::vector<std::string> warnings; // Features of the source that were ignored.
};
// The outcome of a background import, delivered by poll_imports().
struct FinishedImport {
    std::filesystem::path source;
    Result<ImportedAsset> result;
};

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
    // Project assets: <project>/Assets. References resolve once and stay cached until the catalogue changes.
    struct Resolved {
        renderer::MeshHandle mesh;
        renderer::TextureHandle texture;
        renderer::MaterialHandle material;
        renderer::EnvironmentHandle environment;
        std::string problem;
    };
    std::filesystem::path project_;
    std::optional<assets::AssetDatabase> assets_;
    renderer::ResourceRegistry resources_;
    std::map<std::tuple<std::uint64_t, std::string, assets::AssetType>, Resolved> resolved_;
    std::vector<std::string> asset_problems_;
    // Material edits in progress, not saved yet (a drag in the Material Editor); these win over the file.
    std::map<assets::AssetId, assets::MaterialData> material_drafts_;
    // Material edits join undo and redo. Each records how deep the scene's undo stack was, which orders the two histories.
    struct MaterialEdit { assets::AssetId id; assets::MaterialData before, after; std::size_t scene_depth; };
    std::vector<MaterialEdit> material_undo_, material_redo_;
    std::vector<std::shared_future<Result<assets::PreparedImport>>> imports_;
    std::vector<std::filesystem::path> import_sources_;
    std::uint64_t frames_since_trim_ = 0;
    std::uint64_t assets_released_ = 0;
    const Resolved &resolve(const AssetRef &, assets::AssetType);
    void forget_materials();
    Result<ImportedAsset> finish_import(const assets::PreparedImport &);
    std::set<assets::AssetId> referenced_assets();
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
    // Undoes the latest edit, a scene command or a material edit, whichever came last.
    Result<commands::Receipt> undo();
    Result<commands::Receipt> redo();
    bool can_undo() const { return bus_.undo_size() || !material_undo_.empty(); }
    bool can_redo() const { return bus_.redo_size() || !material_redo_.empty(); }
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
    // Extracts the shown scene: mesh components become lit meshes, light components scene lights. An asset that
    // cannot be resolved draws as a magenta cube and is listed in asset_problems(); nothing else changes.
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
    // Throws away the running copy and starts again from the editor scene, staying in Play mode.
    Result<void> restart();
    // Advances runtime time while playing: whole fixed_step ticks, at most max_ticks_per_update per call.
    // Each tick steps the runtime physics.
    void update(std::chrono::nanoseconds elapsed);
    std::uint64_t runtime_ticks() const;
    // The game the running scene's behaviors set up; null in edit mode and for scenes without one.
    gameplay::Game *game() const;

    // ---- Physics of the shown scene (edit-mode bridge, or the play session's) ----
    // Brings the edit-mode bridge in line with the editor scene; frame() calls it. Never writes the scene.
    void sync_physics();
    const physics::ScenePhysics &physics_bridge() const;
    // A runtime physics failure that paused Play mode; empty otherwise.
    const std::string &physics_error() const { return physics_error_; }

    // ---- Project assets ----
    // Uses <root>/Assets as the asset folder, cataloguing it if it exists; the first import creates it.
    Result<void> open_project(const std::filesystem::path &root);
    const std::filesystem::path &project() const { return project_; }
    std::filesystem::path asset_folder() const { return project_ / "Assets"; }
    // Copies a file into the asset folder; see assets::AssetDatabase::import_file. References are not touched.
    Result<ImportedAsset> import_asset(const std::filesystem::path &file);
    Result<void> rescan_assets();
    // Re-imports from disk; everything drawing the asset picks up the new revision on the next frame.
    Result<void> reload_asset(assets::AssetId);
    std::vector<assets::AssetMetadata> assets() const;
    std::vector<assets::AssetIssue> asset_issues() const;
    assets::AssetState asset_state(assets::AssetId) const;
    // Sets the entity's model or texture according to the asset's type, adding a mesh renderer if it has none, and
    // makes the color white so the asset shows as authored. One undo step.
    Result<commands::Receipt> assign_asset(EntityId, assets::AssetId);
    // Creates an entity drawing the model, named after the file and resting on the floor (y = 0), and selects it.
    Result<commands::Receipt> place_asset(assets::AssetId);
    // The model a reference names, loading it if needed; null when it cannot be resolved.
    std::shared_ptr<const assets::MeshData> model(const AssetRef &);
    // A box collider around that model, in its local space; empty when it cannot be resolved.
    std::optional<ColliderComponent> model_collider(const AssetRef &);
    // Problems with the references in the last frame().
    const std::vector<std::string> &asset_problems() const { return asset_problems_; }
    const renderer::ResourceRegistry &resources() const { return resources_; }

    // ---- Background import: decoding happens on a worker; the catalogue and the GPU stay on this thread ----
    void begin_import(const std::filesystem::path &file);
    std::size_t imports_pending() const { return imports_.size(); }
    // Finishes the imports whose decoding is done.
    std::vector<FinishedImport> poll_imports();
    // Waits for all of them. Tests and shutdown.
    std::vector<FinishedImport> wait_for_imports();

    // ---- Resource lifetime ----
    // Releases the CPU data of assets no scene references; a later reference re-imports from disk. frame() calls this
    // every few seconds. Returns how many cached assets were freed.
    std::size_t release_unused_assets();
    std::uint64_t assets_released() const { return assets_released_; }

    // ---- Material assets: .material files in the asset folder ----
    // Creates "materials/<name>.material" under a free name.
    Result<assets::AssetId> create_material(const std::string &name, const assets::MaterialData & = {});
    Result<assets::AssetId> duplicate_material(assets::AssetId);
    // The current value: a draft while one is being dragged, otherwise the saved one.
    std::optional<assets::MaterialData> material(assets::AssetId);
    // A live change: every object using the material shows it at once, nothing is saved and no undo step is added.
    Result<void> preview_material(assets::AssetId, const assets::MaterialData &);
    // Saves the material to its file: one undo step. A draft is replaced.
    Result<void> save_material(assets::AssetId, const assets::MaterialData &);
    // Drops a draft without saving.
    void discard_material_draft(assets::AssetId);
    // Sets the entity's material, or one part's when `slot` is given, adding a mesh renderer if it has none. An empty
    // id clears it. One undo step.
    Result<commands::Receipt> assign_material(EntityId, assets::AssetId, std::optional<std::uint32_t> slot = std::nullopt);
    // The handle a renderer can preview; zero when the material cannot be resolved.
    renderer::MaterialHandle material_handle(assets::AssetId);
    // Number of parts the entity's mesh has, each of which can take a material.
    std::size_t mesh_part_count(const MeshComponent &);

    // ---- Lighting ----
    // The environment in force for the shown scene: the first by entity ID, or the defaults.
    EnvironmentComponent environment() const;
    // The entity holding it, 0 when the defaults apply.
    EntityId environment_entity() const;
    // Sets the environment on its entity, creating one named "Environment" if needed. One undo step.
    Result<commands::Receipt> set_environment(const EnvironmentComponent &);
    static EnvironmentComponent outdoor_environment();
    static EnvironmentComponent indoor_environment();
    // A point light that reads as bright a few meters away at this exposure: 3000 lm at EV100 6, doubling per stop.
    static double point_light_lumens(double ev100);
    // The sun for the outdoor preset, or the key light for the indoor one.
    Result<commands::Receipt> create_preset_lights(bool outdoor);
private:
    void tick();
};
} // namespace engine::editor
