#pragma once
#include <engine/animation/animation.h>
#include <engine/assets/asset_database.h>
#include <engine/commands/CommandBus.hpp>
#include <engine/editor/CameraController.hpp>
#include <engine/editor/Selection.hpp>
#include <engine/effects/particles.h>
#include <engine/gameplay/game.h>
#include <engine/physics/scene_physics.h>
#include <engine/renderer/RenderSnapshot.hpp>
#include <engine/renderer/ResourceRegistry.hpp>
#include <engine/scripting/scene_values.h>
#include <engine/scripting/script_loader.h>
#include <engine/scripting/script_runtime.h>
#include <array>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace engine::editor {
// What the open scene remembers for the editor only, in "<scene file>.editor" beside the scene. It is view state, not
// content: it joins no undo step, does not make the scene dirty, and Play mode ignores it.
struct CameraBookmark {
    Vec3 target;
    double yaw = 0, pitch = 0, distance = 8;
    bool orthographic = false;
    bool operator==(const CameraBookmark &) const = default;
};
// Folders in the hierarchy. They organize the editor's view and mean nothing to the scene or to gameplay.
struct SceneFolder {
    std::uint64_t id = 0, parent = 0; // Parent 0 is the root.
    std::string name;
    bool operator==(const SceneFolder &) const = default;
};
struct EditorFolders {
    std::vector<SceneFolder> folders;
    std::map<EntityId, std::uint64_t> members; // Root entities filed in a folder; children follow their parent.
    std::uint64_t next_id = 1;
    bool operator==(const EditorFolders &) const = default;
    const SceneFolder *find(std::uint64_t id) const {
        for (const auto &folder : folders) if (folder.id == id) return &folder;
        return nullptr;
    }
};
struct EditorViewState {
    std::set<EntityId> hidden, locked; // Locked: cannot be picked in the viewport.
    std::array<std::optional<CameraBookmark>, 10> bookmarks;
    EditorFolders folders;
    bool operator==(const EditorViewState &) const = default;
    bool empty() const { return *this == EditorViewState{}; }
};
std::string write_view_state(const EditorViewState &);
Result<EditorViewState> read_view_state(std::string_view);
// What the viewport leaves out. These only take things away from what the scene says.
struct ViewFlags {
    bool grid = true, skybox = true, shadows = true, ambient_occlusion = true, bloom = true;
    bool effects = true; // Particle emitters play while editing. Only the preview: it never changes the scene.
    bool animations = true; // Animators whose preview is on play while editing; the same promise.
    bool bones = false;     // Skeletons are drawn over the view.
    bool ragdolls = false;  // Ragdolls' capsules are drawn over the view: where they would be, or are.
    bool outlines = true;   // Selected models are outlined. Off in the game's own view.
};

enum class PlayState { Editing, Playing, Paused };

// Time spent in fixed ticks, since the session began. `animation` is posing animators and ragdolls in a tick, apart from
// `gameplay`. `effects` is advancing Play's particles, once an update, over `effect_updates` updates.
struct SimulationTiming {
    std::uint64_t ticks = 0, effect_updates = 0;
    std::chrono::nanoseconds physics{0}, physics_max{0}, gameplay{0}, animation{0}, animation_max{0}, effects{0}, effects_max{0};
};

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
    std::uint64_t id = 0;
    bool cancelled = false; // Its result was discarded, and nothing was added.
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
    SimulationTiming timing_;
    SelectionSet selection_;
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
    // Loading an asset to answer a question about it changes no document, so these are mutable.
    mutable std::optional<assets::AssetDatabase> assets_;
    mutable renderer::ResourceRegistry resources_;
    mutable std::map<std::tuple<std::uint64_t, std::string, assets::AssetType>, Resolved> resolved_;
    // "/Game" and its index, rebuilt when the catalogue's scan count moves on (slice 7).
    mutable assets::VirtualFileSystem packages_;
    mutable std::optional<std::uint64_t> packages_scans_;
    mutable std::vector<std::string> package_conflicts_;
    // The scene edits that make every asset reference `fix` changes the way `fix` leaves it.
    std::vector<commands::Command> reference_edits(const std::function<bool(AssetRef &)> &fix) const;
    std::vector<std::string> asset_problems_;
    std::map<EntityId, renderer::Pose> poses_;
    // Material edits in progress, not saved yet (a drag in the Material Editor); these win over the file.
    std::map<assets::AssetId, assets::MaterialData> material_drafts_;
    // The same for particle effects in the Effect Editor.
    std::map<assets::AssetId, assets::EffectData> effect_drafts_;
    // What each script declares, as read when its file was last written or moved (script_info).
    struct ScriptInfoEntry {
        std::filesystem::file_time_type written;
        std::string path;
        bool read = false;
        std::optional<scripting::ScriptInfo> info;
        std::optional<Error> error;
    };
    mutable std::map<std::uint64_t, ScriptInfoEntry> script_infos_;
    // Plays the editor scene's emitters while editing (view_flags.effects). Play mode has its own, in Runtime.
    effects::ParticleSystem edit_particles_{1};
    // Plays the editor scene's animators while editing (view_flags.animations). Play mode has its own, in Runtime.
    animation::AnimationSystem edit_animation_;
    // Skeletons and clips built from models, by the model data they came from. A model reloaded from disk is new data, so
    // it gets a new rig. An entry whose model data has been freed is stale: it is rebuilt if asked for, and dropped when
    // unused assets are released.
    struct RigEntry { std::weak_ptr<const assets::MeshData> model; std::shared_ptr<const animation::Rig> rig; std::string problem; };
    struct ClipEntry { std::weak_ptr<const assets::MeshData> model, source; std::shared_ptr<const animation::ClipSet> clips; std::string problem; };
    mutable std::map<const assets::MeshData *, RigEntry> rigs_;
    // The flesh of each joint of a model, for sizing ragdolls: the box, in the joint's own space, of the vertices it
    // moves more than any other joint does. Kept and dropped like rigs.
    struct FleshEntry { std::weak_ptr<const assets::MeshData> model; std::vector<assets::Bounds> boxes; std::vector<char> any; };
    mutable std::map<const assets::MeshData *, FleshEntry> flesh_;
    const FleshEntry &flesh_for(const std::shared_ptr<const assets::MeshData> &) const;
    mutable std::map<std::pair<const assets::MeshData *, const assets::MeshData *>, ClipEntry> clip_sets_;
    std::deque<animation::AnimationEvent> recent_events_;
    // One ordered history of named steps. A scene step's data lives in the command bus; a material edit carries its own.
    struct MaterialEdit { assets::AssetId id; assets::MaterialData before, after; };
    struct EffectEdit { assets::AssetId id; assets::EffectData before, after; };
    struct EventsEdit { assets::AssetId id; std::vector<assets::ClipEvent> before, after; };
    struct FolderEdit { EditorFolders before, after; };
    // `scene`: the command bus holds a matching step. A transaction can change the scene and folders together.
    struct HistoryStep {
        std::string name;
        std::optional<MaterialEdit> material;
        std::optional<FolderEdit> folders;
        bool scene = true;
        std::optional<EffectEdit> effect;
        std::optional<EventsEdit> events;
    };
    std::vector<HistoryStep> undo_steps_, redo_steps_;
    std::uint64_t bus_recorded_ = 0;
    int transaction_depth_ = 0;
    bool transaction_cancelled_ = false;
    std::string transaction_name_;
    std::optional<EditorFolders> transaction_folders_; // Folders as they were when the open transaction first changed them.
    Result<void> commit_folders(EditorFolders next, std::string name);
    void set_flag_on_subtree(std::set<EntityId> &flags, EntityId id, bool on);
    bool ejected_ = false, simulating_ = false;
    // A worker decoding an asset for its thumbnail.
    struct ThumbnailJob { assets::AssetId id; assets::AssetType type; std::future<std::shared_ptr<const assets::PreparedImport>> result; };
    std::vector<ThumbnailJob> thumbnail_jobs_;
    // A gizmo drag in progress: where each moved entity started.
    std::vector<std::pair<EntityId, WorldTransform>> drag_start_;
    EditorViewState view_state_;
    std::filesystem::path document_; // The scene file last saved or loaded; empty for a new scene.
    std::string view_state_problem_;
    void store_view_state();
    void load_view_state();
    std::set<EntityId> selection_subtrees() const;
    // Nearest hit along a world ray, skipping `exclude`. 0 and an empty distance when nothing is hit.
    std::pair<EntityId, std::optional<double>> cast_ray(const renderer::FrameSnapshot &, const Vec3 &origin, const Vec3 &direction, double limit, const std::set<EntityId> &exclude) const;
    void sync_history();
    std::vector<std::shared_future<Result<assets::PreparedImport>>> imports_;
    std::vector<std::filesystem::path> import_sources_;
    std::vector<std::uint64_t> import_ids_;
    std::set<std::uint64_t> cancelled_imports_;
    std::uint64_t next_import_id_ = 1;
    std::uint64_t frames_since_trim_ = 0;
    std::uint64_t assets_released_ = 0;
    const Resolved &resolve(const AssetRef &, assets::AssetType) const;
    void forget_materials();
    Result<ImportedAsset> finish_import(const assets::PreparedImport &);
    std::set<assets::AssetId> referenced_assets();
    void reconcile();
    Result<void> editing_only(const char *action) const;
    // Local TRS that places an entity drawn at `world` under `parent` (0 = root), on the parent's joint `bone` when it
    // is attached to one; rejects shear.
    Result<Transform> local_from_world(EntityId parent, const WorldTransform &world, const std::string *bone = nullptr) const;
public:
    static constexpr std::chrono::nanoseconds fixed_step{16'666'667}; // 60 Hz runtime ticks
    static constexpr int max_ticks_per_update = 5;
    OrbitCamera camera;
    ViewFlags view_flags;
    double grid_spacing = 1; // Meters between the grid's lines.
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
    // The primary selected entity, or 0: what gizmos and the Inspector follow.
    EntityId selection() const { return selection_.primary(); }
    const SelectionSet &selected() const { return selection_; }
    // For subscribing to changes. Entities that do not exist are dropped when the scene next changes.
    SelectionSet &selection_set() { return selection_; }
    void select(EntityId id);
    void select_all();
    // The selected entities with no selected ancestor: moving, copying or deleting these covers the rest.
    std::vector<EntityId> selection_roots() const;
    // `label` names the undo step; empty uses the command's own label. Ignored inside a transaction.
    Result<commands::Receipt> execute(commands::Command, std::string_view label = {});
    // All or nothing, at most commands::CommandBus::max_transaction commands.
    Result<commands::Receipt> execute(std::span<const commands::Command>, std::string_view label);
    // Everything until the matching end_transaction() is one undo step, drawn live as it happens. They nest, and the
    // outermost name wins. In Play mode the runtime scene keeps no history, so these do nothing there.
    Result<void> begin_transaction(std::string name);
    Result<void> end_transaction();
    // Marks the transaction to restore what it changed when the outermost one ends.
    void cancel_transaction() { transaction_cancelled_ = transaction_depth_ > 0; }
    bool in_transaction() const { return transaction_depth_ > 0; }
    // The history as a list, oldest first. The first history_position() entries are applied, the rest can be redone.
    std::vector<std::string> history_names() const;
    // ---- Whole-selection operations, each one undo step ----
    // The selection and its children as scene text, for the clipboard. Roots keep their place in the world.
    Result<std::string> copy_selection() const;
    // Adds what copy_selection() wrote, with new IDs, at the root, and selects it.
    Result<void> paste(std::string_view text);
    // A new empty entity at the middle of the selection, with the selection under it. Selected afterwards.
    Result<EntityId> group_selection();
    std::size_t history_position() const { return undo_steps_.size(); }
    // Undoes or redoes until exactly `position` entries are applied.
    Result<void> jump_history(std::size_t position);
    // Creates an entity 0.5 m above the parent's origin and selects it. The default is a built-in cube.
    Result<commands::Receipt> create(std::string name = "Cube", EntityId parent = 0,
                                     Components components = Components{MeshComponent{}, {}, {}});
    // Undoes the latest edit, a scene command or a material edit, whichever came last.
    Result<commands::Receipt> undo();
    Result<commands::Receipt> redo();
    bool can_undo() const { return !undo_steps_.empty(); }
    bool can_redo() const { return !redo_steps_.empty(); }
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
    // The same for the whole selection, in one undo step. Selects the copies.
    Result<void> duplicate_selection();
    // Deletes the selection and its subtrees in one undo step.
    Result<void> delete_selection();
    // ---- A gizmo drag: one undo step named "<verb> <name>" or "<verb> 3 Entities", drawn live ----
    // With `duplicate`, the selection is copied first and the copies are what moves.
    Result<void> begin_drag(std::string_view verb, bool duplicate = false);
    // Places every moved entity at `delta` times where it started. All or nothing: a shear leaves the last good place.
    Result<void> update_drag(const WorldTransform &delta);
    Result<void> end_drag();
    // Puts everything back. No undo step.
    Result<void> cancel_drag();
    bool dragging() const { return !drag_start_.empty(); }
    void focus();
    // Where focus() would put the camera: the same angles, framing the whole selection. Empty with nothing selected.
    std::optional<OrbitCamera> focus_goal() const;
    // The middle of the selection's bounds, which Alt+LMB orbits around. Empty with nothing selected.
    std::optional<Vec3> selection_center() const;
    // Moves each selected root straight down so its bounds rest on the surface under its middle, or on the ground plane
    // when it is above that and nothing else is below. One undo step. Fails if nothing can move.
    Result<void> snap_selection_to_floor(const renderer::FrameSnapshot &);
    // Where a ray through the view point hits something, or the ground plane; empty when it does neither.
    std::optional<Vec3> ray_hit(const renderer::FrameSnapshot &, double ndc_x, double ndc_y) const;
    // The drawn vertex nearest the cursor within `radius` pixels, leaving out the selection and its children.
    std::optional<Vec3> nearest_vertex(const renderer::FrameSnapshot &, double x, double y, double width, double height, double radius) const;
    // ---- Editor-only view state ----
    const EditorViewState &view_state() const { return view_state_; }
    bool hidden(EntityId id) const { return view_state_.hidden.contains(id); }
    // Hides the selection and its children in the editor, and deselects them. Saved with the view state.
    void hide_selection();
    void unhide_all();
    // The entity and its children.
    void set_hidden(EntityId id, bool hidden);
    bool locked(EntityId id) const { return view_state_.locked.contains(id); }
    void set_locked(EntityId id, bool locked);
    // ---- Folders in the hierarchy; undoable, and kept in the view state, never in the scene ----
    Result<std::uint64_t> create_folder(const std::string &name, std::uint64_t parent = 0);
    Result<void> rename_folder(std::uint64_t id, const std::string &name);
    // What the folder held moves up to its parent. No entity is deleted.
    Result<void> delete_folder(std::uint64_t id);
    Result<void> move_folder(std::uint64_t id, std::uint64_t parent);
    // Takes the entity out of any parent, keeping its place in the world, and files it. Folder 0 is the root.
    Result<void> file_entity(EntityId id, std::uint64_t folder);
    std::uint64_t folder_of(EntityId id) const;
    void set_bookmark(std::size_t slot);
    std::optional<OrbitCamera> bookmark(std::size_t slot) const;
    // A view state file that could not be read since the last call, for the host to report.
    std::string take_view_state_problem() { return std::exchange(view_state_problem_, {}); }
    // The box the entity draws, in its local space; empty when it draws nothing.
    std::optional<std::pair<Vec3, Vec3>> local_bounds(EntityId) const;
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
    // Runs the scene without possessing the player: the world simulates, a game stays waiting to start, and the view
    // is the editor's, so anything can be selected and inspected while it runs.
    Result<void> simulate();
    bool simulating() const { return simulating_; }
    // Detached from the running game's view: the editor camera is used, and the game carries on. Only while running.
    Result<void> set_ejected(bool);
    bool ejected() const { return ejected_; }
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
    const SimulationTiming &timing() const { return timing_; }
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
    // The project a scene file belongs to: the nearest folder holding it, or above that, with an `Assets` folder. Empty when
    // there is none.
    static std::filesystem::path project_of(const std::filesystem::path &scene_file);
    // Copies a file into the asset folder; see assets::AssetDatabase::import_file. References are not touched.
    Result<ImportedAsset> import_asset(const std::filesystem::path &file);
    Result<void> rescan_assets();
    // Re-imports from disk; everything drawing the asset picks up the new revision on the next frame.
    Result<void> reload_asset(assets::AssetId);
    // Moves or renames the file, keeping its ID, and updates the paths stored in the scene (one undo step) and in
    // materials (saved at once). References find assets by ID first, so a stale path would still resolve. A redirector
    // is left at the old path for everything else that names it (assets/virtual_paths.h). `new_path` is a path in the
    // asset folder or a package name, "/Game/Textures/Walls/Brick"; without an extension it keeps the old one.
    Result<void> rename_asset(assets::AssetId, const std::string &new_path);

    // ---- Package names and redirectors (Milestone 5, slice 7; assets/virtual_paths.h) ----
    // The mount point of the project's asset folder.
    static constexpr std::string_view game_mount = "/Game";
    // Mounted on open_project and indexed again whenever the catalogue has been scanned since. Safe to resolve with from
    // any thread; the index is rebuilt here, on the session's.
    const assets::VirtualFileSystem &packages() const;
    // "/Game/Textures/Brick", in any case, followed through redirectors to the catalogued asset.
    Result<assets::PackageResolution> resolve_package(std::string_view name) const;
    // What people see for an asset: its package name ("/Game/Textures/Brick"), or "" when it is not catalogued.
    std::string package_name_of(assets::AssetId) const;
    // The redirectors in the asset folder, and what indexing them found wrong (assets/virtual_paths.h).
    std::vector<assets::Redirector> redirectors() const;
    std::vector<std::string> package_conflicts() const;
    struct FixUpReport {
        std::size_t redirectors = 0;                // Removed.
        std::size_t references = 0;                 // Rewritten, in scene files and the open scene.
        std::vector<std::filesystem::path> scenes;  // Scene files rewritten on disk.
        bool open_scene = false;                    // The open scene changed, as one undo step; it is not saved.
        std::size_t assets = 0;                     // Materials and effects saved.
        std::vector<std::string> kept;              // Redirectors left in place, and why.
    };
    // Rewrites every reference that still names a redirector's path, or names the asset it stood for by ID, to where the
    // asset is now. That covers the project's scene files (written in place), the open scene (one undo step, left
    // unsaved), and materials and effects (saved). Then the redirectors it resolved are deleted. One that leads nowhere,
    // or round in a loop, is kept and listed. Edit mode only.
    Result<FixUpReport> fix_up_redirectors();
    // What a thumbnail needs, from a worker. A mesh is ready to draw; a texture comes reduced to at most `thumbnail_size`.
    static constexpr std::uint32_t thumbnail_size = 128;
    struct ThumbnailData {
        assets::AssetId id;
        assets::AssetType type = assets::AssetType::Unknown;
        renderer::MeshHandle mesh;
        std::vector<std::uint8_t> pixels;
        std::uint32_t width = 0, height = 0;                // Of `pixels`.
        std::uint32_t source_width = 0, source_height = 0;  // Of the image.
        std::size_t vertices = 0, triangles = 0;
        std::size_t joints = 0;            // Of a model's skeleton.
        std::vector<std::string> clips;    // A model's animations, by name.
        bool failed = false;
    };
    // Decodes a model or texture on a worker, without touching the catalogue.
    void request_thumbnail_data(assets::AssetId);
    std::size_t thumbnail_jobs() const { return thumbnail_jobs_.size(); }
    std::vector<ThumbnailData> poll_thumbnail_data();
    std::vector<assets::AssetMetadata> assets() const;
    std::vector<assets::AssetIssue> asset_issues() const;
    assets::AssetState asset_state(assets::AssetId) const;
    // Sets the entity's model or texture according to the asset's type, adding a mesh renderer if it has none, and
    // makes the color white so the asset shows as authored. One undo step.
    Result<commands::Receipt> assign_asset(EntityId, assets::AssetId);
    // Creates an entity drawing the model, named after the file and resting on the floor (y = 0), and selects it. For a
    // particle effect, an entity with an emitter playing it.
    Result<commands::Receipt> place_asset(assets::AssetId);
    // The same with its base at `point`.
    Result<commands::Receipt> place_asset_at(assets::AssetId, const Vec3 &point);
    // The model a reference names, loading it if needed; null when it cannot be resolved.
    std::shared_ptr<const assets::MeshData> model(const AssetRef &) const;
    // A box collider around that model, in its local space; empty when it cannot be resolved.
    std::optional<ColliderComponent> model_collider(const AssetRef &);
    // Problems with the references in the last frame().
    const std::vector<std::string> &asset_problems() const { return asset_problems_; }
    const renderer::ResourceRegistry &resources() const { return resources_; }
    // ---- Poses of skinned models: visual only, never in the scene, its file or its history (Milestone 5, D1) ----
    // Model-space joint transforms frames draw an entity's model with, in its skeleton's order. Null: its rest pose.
    void set_pose(EntityId, renderer::Pose);
    renderer::Pose pose(EntityId) const;
    void clear_poses() { poses_.clear(); }

    // ---- Background import: decoding happens on a worker; the catalogue and the GPU stay on this thread ----
    std::uint64_t begin_import(const std::filesystem::path &file);
    std::size_t imports_pending() const { return imports_.size(); }
    // Finishes the imports whose decoding is done.
    std::vector<FinishedImport> poll_imports();
    // A decode cannot be interrupted, so this discards its result when it arrives: nothing reaches the project.
    // False when no such import is running.
    bool cancel_import(std::uint64_t id);
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
    std::optional<assets::MaterialData> material(assets::AssetId) const;
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

    // ---- Particle effects: .effect files in the asset folder ----
    // Creates "effects/<name>.effect" under a free name.
    Result<assets::AssetId> create_effect(const std::string &name, const assets::EffectData & = {});
    // The current value: a draft while one is being edited, otherwise the saved one.
    std::optional<assets::EffectData> effect(assets::AssetId) const;
    // A live change: every emitter using it shows it at once, without restarting. Nothing is saved, no undo step.
    Result<void> preview_effect(assets::AssetId, const assets::EffectData &);
    // Saves the effect to its file: one undo step. A draft is replaced, and emitters using it start again.
    Result<void> save_effect(assets::AssetId, const assets::EffectData &);
    void discard_effect_draft(assets::AssetId);
    // Gives the entity a particle emitter playing this effect, or changes its effect. One undo step.
    Result<commands::Receipt> assign_effect(EntityId, assets::AssetId);
    // Starts every emitter of the shown scene again from its beginning, and drops one-shot effects.
    void restart_effects();

    // ---- Scripts: .lua files in the asset folder (Milestone 6) ----
    // Creates "Scripts/<name>.lua" under a free name, from a template with one property and every function the engine calls.
    Result<assets::AssetId> create_script(const std::string &name);
    // What a script declares, read without running a game: its top level runs in a throwaway sandbox. Read again when its
    // file changes. The error says why it cannot be read, with the line when there is one.
    Result<scripting::ScriptInfo> script_info(assets::AssetId) const;
    // Gives the entity a Script component running the script, or changes its script, keeping the values it holds. An empty
    // id leaves the component with no script. One undo step.
    Result<commands::Receipt> assign_script(EntityId, assets::AssetId);
    // Sets a property's value on the entity, one undo step. A value equal to the script's default removes the entity's own,
    // so it follows the default again; a value of the wrong type is refused.
    Result<commands::Receipt> set_script_property(EntityId, const std::string &name, const ScriptValue &);
    // Removes the entity's own value, so the script's default applies. Also how a value the script no longer declares goes.
    Result<commands::Receipt> reset_script_property(EntityId, const std::string &name);
    // A script's value as a scene holds it: an asset named by package name resolves through the project.
    ScriptValue scene_value(const scripting::Value &) const;
    // A scene's value as the script sees it.
    scripting::Value script_value(const ScriptValue &) const;
    // What the shown scene's particle system holds now.
    effects::Stats effect_stats() const;
    // Advances the edit-mode preview; update() calls it, and so can a host that is not playing.
    void advance_effects(std::chrono::nanoseconds elapsed);

    // ---- Animation: poses are visual only, never in the scene, its file or its history (Milestone 5, D1, D2) ----
    // The shown scene's animation system: the edit-mode preview, or the play session's. Its run-time controls (play,
    // play_layered, set_time, set_paused, set_ik) change nothing in the scene.
    animation::AnimationSystem &animation();
    const animation::AnimationSystem &animation() const;
    // The clips an entity's animator plays, bound to its model's skeleton: from its clip source, or its own model.
    // Null when there are none, with the reason in `problem`.
    std::shared_ptr<const animation::ClipSet> clip_set(EntityId, std::string *problem = nullptr) const;
    // The skeleton of the entity's model; null when it has none.
    std::shared_ptr<const animation::Rig> rig(EntityId) const;
    // The model asset whose clips an entity's animator plays; the null id when there is none.
    assets::AssetId clip_source(EntityId) const;
    // Advances the edit-mode preview; update() calls it.
    void advance_animations(std::chrono::nanoseconds elapsed);
    // Starts every animator of the shown scene again from its authored clip.
    void restart_animations();
    // A joint of the entity's model as drawn now (its pose, or its rest pose), in world space.
    std::optional<WorldTransform> joint_world(EntityId, std::string_view joint) const;
    // Where the entity is drawn: its scene transform, moved by the joint it or an ancestor is attached to (D2). Gizmos,
    // outlines and picking use it; set_world() and drags take positions in the same space.
    std::optional<WorldTransform> drawn_world(EntityId) const;
    // Clip events the shown scene crossed, oldest first; at most 64 are kept.
    const std::deque<animation::AnimationEvent> &animation_events() const { return recent_events_; }
    struct BoneLine { EntityId entity = 0; Vec3 from, to; }; // From a joint's parent to the joint, in world space.
    // The skeletons of the shown scene's skinned and animated models as drawn now; `only` keeps one entity's.
    std::vector<BoneLine> bone_lines(EntityId only = 0) const;
    // The events of a model's clips, kept in its .meta (D5).
    std::vector<assets::ClipEvent> clip_events(assets::AssetId model) const;
    // Replaces them: one undo step. The model is not imported again.
    Result<void> set_clip_events(assets::AssetId model, std::vector<assets::ClipEvent> events);
    // Gives the entity an animator playing `clip` of its own model, or changes its clip. One undo step.
    Result<commands::Receipt> assign_clip(EntityId, const std::string &clip);
    // The pose the entity is drawn with now: the host's, else its ragdoll's, else its animator's. Null: its rest pose.
    renderer::Pose shown_pose(EntityId) const;

    // ---- Ragdolls (Milestone 5, D7): the run's physics holds the bodies; the scene keeps only the Ragdoll settings ----
    // What the entity's Ragdoll would build from its model's skeleton and skin, sized as the entity stands. Empty, with
    // the reason in `problem`, when it cannot be built.
    std::optional<physics::RagdollPlan> ragdoll_plan(EntityId, std::string *problem = nullptr) const;
    // Only while running: turns the entity into a ragdoll from the pose it is in, moving as that pose and its body were.
    // An impulse at a world point (a shot) lands on the part nearest it. The entity's own body sits out meanwhile.
    Result<void> start_ragdoll(EntityId, std::optional<std::pair<Vec3, Vec3>> impulse_at = std::nullopt);
    // Removes its bodies: the animator poses it again, and its own body comes back.
    Result<void> stop_ragdoll(EntityId);
    bool ragdolled(EntityId) const;
    // Entities lying as ragdolls now, ascending.
    std::vector<EntityId> ragdolled_entities() const;
    // Outlines of ragdolls, as Show Ragdolls draws them: the capsules at the entity's pose, which is where its bodies
    // are once it falls. `only` keeps one entity's.
    std::vector<physics::DebugLine> ragdoll_lines(EntityId only = 0) const;

    // ---- Debris (D11): loose pieces in the run's physics, at most physics::max_debris, gone once they settle ----
    // Only while running: a piece that looks like `look` (its model and material, at its scale) with a box around it,
    // starting at a point, turned and moving as given.
    Result<std::uint64_t> spawn_debris(EntityId look, const Vec3 &position, const Quaternion &rotation, const Vec3 &velocity,
                                       const Vec3 &spin = {}, double lifetime = 10);
    std::vector<physics::DebrisState> debris() const;
    // What the run's game asked for that could not be done, such as a ragdoll for a model without a skeleton.
    const std::vector<std::string> &game_issues() const;

    // ---- Scripts in Play and Simulate (Milestone 6, scripting/script_world.h) ----
    // Actions for the game and the scripts, while the game holds the mouse.
    void set_game_input(const gameplay::InputFrame &);
    // What scripts got wrong and printed since the last take, oldest first; each kept at most 512 deep.
    std::vector<scripting::ScriptError> take_script_errors();
    std::vector<scripting::ScriptOutput> take_script_output();
    // The entities whose scripts run, ascending; empty outside Play.
    std::vector<EntityId> script_instances() const;
    // A field of a running script's `self`, when it holds a plain value.
    std::optional<scripting::Value> script_field(EntityId, std::string_view name) const;

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
    struct EffectSource { assets::AssetId id; std::uint32_t revision = 0; std::shared_ptr<const assets::EffectData> data; };
    // The effect a reference names, with a draft winning over the file; `problem` says why there is none.
    std::optional<EffectSource> effect_source(const AssetRef &, std::string &problem) const;
    effects::ParticleSystem &shown_particles();
    const effects::ParticleSystem &shown_particles() const;
    // Emitters of the shown scene, for its particle system.
    std::vector<effects::EmitterInput> emitter_inputs(const std::vector<EntityRecord> &entities, const std::function<void(const std::string &)> &problem) const;
    // Animators of the shown scene, for its animation system.
    std::vector<animation::AnimatorInput> animator_inputs(const std::vector<EntityRecord> &entities, const std::function<void(const std::string &)> &problem) const;
    std::shared_ptr<const animation::Rig> rig_for(const std::shared_ptr<const assets::MeshData> &, std::string &problem) const;
    std::shared_ptr<const animation::ClipSet> clips_for(const EntityRecord &, std::string &problem) const;
    std::optional<assets::AssetMetadata> asset_meta(const AssetRef &) const;
    // The model-space transform of a joint of the entity's model as drawn now; empty when it has no such joint.
    std::optional<animation::JointMatrix> joint_model(EntityId, std::string_view joint) const;
    // What an attachment on the way up from `id` does to its scene transform: drawn = correction * scene. Identity when
    // nothing is attached. `records` finds entities, so a frame can use its snapshot.
    std::array<double, 16> attachment_correction(EntityId id, const std::function<std::optional<EntityRecord>(EntityId)> &records) const;
    void keep_events(std::vector<animation::AnimationEvent> events);
    // An entity's ragdoll as the builder reads it: its model, skeleton and plan, and where it is drawn.
    struct RagdollBuild {
        std::shared_ptr<const assets::MeshData> model;
        physics::RagdollPlan plan;
        std::array<double, 16> world{};
    };
    std::optional<RagdollBuild> ragdoll_build(EntityId, std::string &problem) const;
    // Poses the run's ragdolls from their bodies; tick() calls it after physics.
    void pose_ragdolls();
    // The lowest joint of the entity as drawn, in the world: where a foot lands. Its origin when it has no pose.
    physics::Vec3 lowest_joint(EntityId) const;
    // Plays what the game asked for on the run's animator: Walk is the Animator's clip, Idle a clip named "idle", Flinch
    // one named "hit", layered on top. A name that starts with the word wins over one that only has it.
    void play_cue(EntityId, gameplay::AnimationCue);
    // Fires the effect of the source's emitter, for the game or a script. Visual only.
    void fire_effect(const gameplay::EffectEvent &);
    // on_stop for the run's scripts, before the run ends.
    void stop_scripts();
    void collect_script_messages();
    std::vector<scripting::ScriptError> script_errors_;
    std::vector<scripting::ScriptOutput> script_output_;
};
// Keeps a transaction open for a scope. Nested guards join the outer one.
class ScopedTransaction {
    EditorSession &session_;
    bool open_;
public:
    ScopedTransaction(EditorSession &session, std::string name) : session_(session), open_(static_cast<bool>(session.begin_transaction(std::move(name)))) {}
    ~ScopedTransaction() { if (open_) (void)session_.end_transaction(); }
    ScopedTransaction(const ScopedTransaction &) = delete;
    ScopedTransaction &operator=(const ScopedTransaction &) = delete;
    void cancel() { session_.cancel_transaction(); }
};
} // namespace engine::editor
