#include <engine/editor/EditorSession.hpp>
#include <engine/assets/importers.h>
#include <engine/assets/skeleton.h>
#include <engine/editor/EditorPrefs.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <tuple>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace engine::editor {
namespace {
WorldTransform copy(const glm::dmat4 &m) {
    WorldTransform result;
    std::copy_n(glm::value_ptr(m), 16, result.matrix.begin());
    return result;
}
glm::dvec3 vector(Vec3 v) { return {v.x, v.y, v.z}; }
double linear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
Vec3 linear(const Vec3 &srgb) { return {linear(srgb.x), linear(srgb.y), linear(srgb.z)}; }
renderer::MeshHandle mesh_handle(BuiltinMesh mesh) {
    switch (mesh) {
    case BuiltinMesh::Sphere: return renderer::builtin_sphere;
    case BuiltinMesh::Plane: return renderer::builtin_plane;
    default: return renderer::builtin_cube;
    }
}
// A posed skinned model's palette, empty for any other model or a pose that does not fit its skeleton.
std::vector<assets::Matrix4> posed_palette(const renderer::RenderObject &object, const assets::MeshData &data) {
    if (data.skin.empty() || !object.pose || object.pose->size() != data.skin.joints.size()) return {};
    return assets::skinning_palette(data.skin, *object.pose);
}
// A model's box in its own space: where its pose puts it, or as it rests.
assets::Bounds model_box(const renderer::RenderObject &object, const assets::MeshData &data) {
    const auto palette = posed_palette(object, data);
    return palette.empty() ? data.bounds : assets::skinned_bounds(data.skin, palette);
}
// Nearest t in [0, limit] where the local ray origin + t * ray enters the box, or a negative value for a miss.
double box_hit(const glm::dvec3 &low, const glm::dvec3 &high, const glm::dvec3 &origin, const glm::dvec3 &ray, double limit) {
    double near_t = 0, far_t = limit;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(ray[axis]) < 1e-12) {
            if (origin[axis] < low[axis] || origin[axis] > high[axis]) return -1;
        } else {
            const double t0 = (low[axis] - origin[axis]) / ray[axis], t1 = (high[axis] - origin[axis]) / ray[axis];
            near_t = std::max(near_t, std::min(t0, t1)); far_t = std::min(far_t, std::max(t0, t1));
        }
    }
    return far_t >= near_t ? near_t : -1;
}
// The same for a built-in shape.
double hit(const renderer::MeshHandle &mesh, const glm::dvec3 &origin, const glm::dvec3 &ray, double limit) {
    if (mesh == renderer::builtin_sphere) {
        const double a = glm::dot(ray, ray), b = 2 * glm::dot(origin, ray), c = glm::dot(origin, origin) - 0.25;
        const double disc = b * b - 4 * a * c;
        if (a < 1e-24 || disc < 0) return -1;
        const double root = std::sqrt(disc), t0 = (-b - root) / (2 * a), t1 = (-b + root) / (2 * a);
        const double t = t0 >= 0 ? t0 : t1; // Inside the sphere, the exit point is the first hit ahead.
        return t >= 0 && t <= limit ? t : -1;
    }
    if (mesh == renderer::builtin_plane) {
        if (std::abs(ray.y) < 1e-12) return -1;
        const double t = -origin.y / ray.y;
        const auto p = origin + t * ray;
        return t >= 0 && t <= limit && std::abs(p.x) <= 0.5 && std::abs(p.z) <= 0.5 ? t : -1;
    }
    return box_hit(glm::dvec3(-0.5), glm::dvec3(0.5), origin, ray, limit);
}
glm::dvec3 vector(const std::array<float, 3> &v) { return {v[0], v[1], v[2]}; }
constexpr Vec3 placeholder_color{1, 0, 1}; // Marks an asset that cannot be drawn.
AssetRef reference(const assets::AssetMetadata &meta) { return {meta.id.value, meta.path}; }
std::string trimmed_name(std::string name) {
    if (name.size() > 256) {
        name.resize(256);
        while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80) name.pop_back();
        if (!name.empty() && static_cast<unsigned char>(name.back()) >= 0xC0) name.pop_back();
    }
    return name;
}
}
std::string write_view_state(const EditorViewState &state) {
    std::ostringstream out;
    out.precision(17);
    out << "EDITORVIEW 1\n";
    for (const auto id : state.hidden) out << "HIDDEN " << id << '\n';
    for (const auto id : state.locked) out << "LOCKED " << id << '\n';
    for (std::size_t slot = 0; slot < state.bookmarks.size(); ++slot)
        if (const auto &b = state.bookmarks[slot])
            out << "BOOKMARK " << slot << ' ' << b->target.x << ' ' << b->target.y << ' ' << b->target.z << ' ' << b->yaw << ' ' << b->pitch << ' ' << b->distance << ' ' << (b->orthographic ? 1 : 0) << '\n';
    if (!state.folders.folders.empty()) out << "NEXTFOLDER " << state.folders.next_id << '\n';
    for (const auto &folder : state.folders.folders) out << "FOLDER " << folder.id << ' ' << folder.parent << ' ' << folder.name << '\n';
    for (const auto &[entity, folder] : state.folders.members) out << "MEMBER " << entity << ' ' << folder << '\n';
    return out.str();
}
Result<EditorViewState> read_view_state(std::string_view text) {
    std::istringstream in{std::string(text)};
    std::string word;
    int version = 0;
    if (!(in >> word >> version) || word != "EDITORVIEW" || version != 1) return fail(ErrorCode::ParseError, "Not an editor view state file");
    EditorViewState state;
    while (in >> word) {
        if (word == "HIDDEN" || word == "LOCKED") {
            EntityId id = 0;
            if (!(in >> id) || !id) return fail(ErrorCode::ParseError, "Bad " + word + " record");
            (word == "HIDDEN" ? state.hidden : state.locked).insert(id);
        } else if (word == "BOOKMARK") {
            std::size_t slot = 0;
            CameraBookmark b;
            int flat = 0;
            if (!(in >> slot >> b.target.x >> b.target.y >> b.target.z >> b.yaw >> b.pitch >> b.distance >> flat) || slot >= state.bookmarks.size() || !(b.distance > 0))
                return fail(ErrorCode::ParseError, "Bad BOOKMARK record");
            b.orthographic = flat != 0;
            state.bookmarks[slot] = b;
        } else if (word == "NEXTFOLDER") {
            if (!(in >> state.folders.next_id)) return fail(ErrorCode::ParseError, "Bad NEXTFOLDER record");
        } else if (word == "FOLDER") {
            SceneFolder folder;
            if (!(in >> folder.id >> folder.parent) || !folder.id) return fail(ErrorCode::ParseError, "Bad FOLDER record");
            std::getline(in >> std::ws, folder.name); // The rest of the line: names have spaces.
            if (!folder.name.empty() && folder.name.back() == '\r') folder.name.pop_back();
            state.folders.folders.push_back(std::move(folder));
        } else if (word == "MEMBER") {
            EntityId entity = 0;
            std::uint64_t folder = 0;
            if (!(in >> entity >> folder) || !entity) return fail(ErrorCode::ParseError, "Bad MEMBER record");
            state.folders.members[entity] = folder;
        } else return fail(ErrorCode::ParseError, "Unknown record " + word);
    }
    return state;
}
struct EditorSession::Runtime {
    Scene scene;
    commands::CommandBus bus{scene, 0}; // Runtime edits are discarded on stop, so they keep no history.
    std::unique_ptr<physics::ScenePhysics> physics; // Destroyed before the bus and scene (C-3).
    std::unique_ptr<gameplay::Game> game; // Holds references to the three above.
    effects::ParticleSystem particles{1}; // The same seed each run, so a run's particles repeat.
    animation::AnimationSystem animation; // Ticks with the game, so what gameplay plays is in step with it.
    // Entities lying as ragdolls: their bodies are in `physics`; this turns them back into a pose each tick (D7).
    struct RagdollRun {
        std::uint64_t id = 0;
        std::vector<int> parents;                // Of the model's joints.
        std::vector<animation::JointMatrix> base; // The pose it fell from; joints without a body keep its local transforms.
        std::vector<glm::dvec3> scales;          // Each joint's world scale when it fell, which bodies do not carry.
        renderer::Pose pose;
    };
    std::map<EntityId, RagdollRun> ragdolls;
    std::vector<std::string> game_issues; // Requests of the game that could not be carried out, newest last.
    bool paused = false;
    std::uint64_t ticks = 0;
    std::chrono::nanoseconds accumulated{0};
};
namespace {
std::unique_ptr<physics::ScenePhysics> make_physics(bool simulate) {
    physics::WorldConfig config;
    config.fixed_timestep = EditorSession::fixed_step;
    auto created = physics::ScenePhysics::create(config);
    if (!created) throw std::runtime_error("Physics world creation failed: " + created.error().message);
    auto result = std::make_unique<physics::ScenePhysics>(std::move(created.value()));
    result->set_simulation_enabled(simulate);
    return result;
}
const commands::Context physics_context{"physics", 3};
}
std::size_t apply_selection_tags(Scene &scene, const SelectionSet &selection) {
    auto &world = scene.ecs().native();
    // Found first and changed after: adding or removing a tag moves an entity to another table under a running query.
    std::vector<flecs::entity> unselected, unprimary;
    world.query_builder<const StableId>().with<SelectedTag>().build().each([&](flecs::entity e, const StableId &id) {
        if (!selection.contains(id.value)) unselected.push_back(e);
    });
    world.query_builder<const StableId>().with<PrimarySelectedTag>().build().each([&](flecs::entity e, const StableId &id) {
        if (id.value != selection.primary()) unprimary.push_back(e);
    });
    std::size_t changed = 0;
    for (auto e : unselected) { e.remove<SelectedTag>(); ++changed; }
    for (auto e : unprimary) { e.remove<PrimarySelectedTag>(); ++changed; }
    for (const auto id : selection.items()) {
        const auto native = scene.native(id);
        if (!native) continue;
        const flecs::entity e(world, native);
        if (!e.has<SelectedTag>()) { e.add<SelectedTag>(); ++changed; }
        if (id == selection.primary() && !e.has<PrimarySelectedTag>()) { e.add<PrimarySelectedTag>(); ++changed; }
    }
    return changed;
}
EditorSession::EditorSession() : edit_physics_(make_physics(false)) { (void)packages_.mount(game_mount, asset_folder()); }
EditorSession::~EditorSession() = default;
const Scene &EditorSession::scene() const { return runtime_ ? runtime_->scene : scene_; }
void EditorSession::reconcile() { selection_.retain([this](EntityId id) { return scene().inspect(id).has_value(); }); }
void EditorSession::select(EntityId id) { selection_.set(scene().inspect(id) ? id : 0); }
void EditorSession::select_all() {
    std::vector<EntityId> ids;
    for (const auto &e : scene().snapshot().entities) ids.push_back(e.id);
    selection_.set(std::span<const EntityId>(ids));
}
std::vector<EntityId> EditorSession::selection_roots() const {
    std::vector<EntityId> roots;
    for (const auto id : selection_.items()) {
        const auto record = scene().inspect(id);
        if (!record) continue;
        bool covered = false;
        for (EntityId p = record->parent; p && !covered; p = scene().inspect(p)->parent) covered = selection_.contains(p);
        if (!covered) roots.push_back(id);
    }
    return roots;
}
void EditorSession::sync_history() {
    if (bus_.recorded() == bus_recorded_) return;
    bus_recorded_ = bus_.recorded();
    undo_steps_.push_back({std::string(bus_.latest_name()), std::nullopt, std::nullopt, true});
    redo_steps_.clear();
    // The bus forgets its oldest step past its limit, so its name has to go too.
    auto scene_steps = std::size_t(std::ranges::count_if(undo_steps_, [](const HistoryStep &step) { return step.scene; }));
    for (auto it = undo_steps_.begin(); it != undo_steps_.end() && scene_steps > bus_.undo_size();)
        if (it->scene) { it = undo_steps_.erase(it); --scene_steps; } else ++it;
}
Result<void> EditorSession::begin_transaction(std::string name) {
    if (transaction_depth_++ == 0 && !runtime_) {
        transaction_cancelled_ = false;
        transaction_name_ = name;
        transaction_folders_.reset();
        if (auto begun = bus_.begin_transaction(std::move(name)); !begun) { --transaction_depth_; return begun; }
    }
    return {};
}
Result<void> EditorSession::end_transaction() {
    if (!transaction_depth_) return fail(ErrorCode::InvalidState, "No transaction is open");
    if (--transaction_depth_ || !bus_.in_transaction()) return {};
    const auto folders_before = std::exchange(transaction_folders_, std::nullopt);
    if (std::exchange(transaction_cancelled_, false)) {
        auto restored = bus_.abort_transaction(context_);
        if (folders_before) { view_state_.folders = *folders_before; store_view_state(); }
        reconcile();
        if (!restored) return restored.error();
        return {};
    }
    const auto recorded = bus_.recorded();
    auto ended = bus_.end_transaction();
    sync_history();
    reconcile();
    if (folders_before && *folders_before != view_state_.folders) {
        // Filed with the scene's step when there is one, so one undo restores both.
        if (bus_.recorded() != recorded) undo_steps_.back().folders = FolderEdit{*folders_before, view_state_.folders};
        else {
            undo_steps_.push_back({transaction_name_, std::nullopt, FolderEdit{*folders_before, view_state_.folders}, false});
            redo_steps_.clear();
            bus_.discard_redo();
        }
        store_view_state();
    }
    return ended;
}
std::vector<std::string> EditorSession::history_names() const {
    std::vector<std::string> names;
    for (const auto &step : undo_steps_) names.push_back(step.name);
    for (auto it = redo_steps_.rbegin(); it != redo_steps_.rend(); ++it) names.push_back(it->name);
    return names;
}
Result<void> EditorSession::jump_history(std::size_t position) {
    if (position > undo_steps_.size() + redo_steps_.size()) return fail(ErrorCode::InvalidArgument, "No such point in the history");
    while (undo_steps_.size() > position) if (auto r = undo(); !r) return r.error();
    while (undo_steps_.size() < position) if (auto r = redo(); !r) return r.error();
    return {};
}
Result<void> EditorSession::editing_only(const char *action) const {
    if (runtime_) return fail(ErrorCode::InvalidState, std::string(action) + " is unavailable in Play mode; press Stop first");
    return {};
}
Result<commands::Receipt> EditorSession::execute(commands::Command cmd, std::string_view label) {
    if (runtime_) {
        if (std::holds_alternative<commands::ReplaceScene>(cmd))
            return fail(ErrorCode::InvalidState, "Replacing the scene is unavailable in Play mode; press Stop first");
        auto result = runtime_->bus.execute(context_, runtime_->bus.revision(), std::move(cmd));
        reconcile();
        return result;
    }
    auto result = bus_.execute(context_, bus_.revision(), std::move(cmd), label);
    sync_history();
    reconcile();
    return result;
}
Result<commands::Receipt> EditorSession::execute(std::span<const commands::Command> batch, std::string_view label) {
    if (runtime_) {
        if (std::ranges::any_of(batch, [](const commands::Command &c) { return std::holds_alternative<commands::ReplaceScene>(c); }))
            return fail(ErrorCode::InvalidState, "Replacing the scene is unavailable in Play mode; press Stop first");
        auto result = runtime_->bus.execute(context_, runtime_->bus.revision(), batch);
        reconcile();
        return result;
    }
    auto result = bus_.execute(context_, bus_.revision(), batch, label);
    sync_history();
    reconcile();
    return result;
}
Result<commands::Receipt> EditorSession::create(std::string name, EntityId parent, Components components) {
    Transform t; t.position.y = 0.5;
    const auto label = "Create " + name;
    auto r = execute(commands::CreateEntity{std::move(name), t, parent, 0, std::move(components)}, label);
    if (r) selection_.set(r.value().created.front());
    return r;
}
Result<commands::Receipt> EditorSession::undo() {
    if (auto allowed = editing_only("Undo"); !allowed) return allowed.error();
    if (in_transaction()) return fail(ErrorCode::InvalidState, "Finish the drag before undoing");
    if (undo_steps_.empty()) return fail(ErrorCode::InvalidState, "History is empty");
    auto &step = undo_steps_.back();
    commands::Receipt receipt{bus_.revision(), {}, 0};
    if (step.scene) {
        auto r = bus_.undo(context_, bus_.revision());
        reconcile();
        if (!r) return r;
        receipt = r.value();
    }
    if (step.material) {
        if (auto saved = assets_->save_material(step.material->id, step.material->before); !saved) return saved.error();
        forget_materials();
    }
    if (step.effect) {
        if (auto saved = assets_->save_effect(step.effect->id, step.effect->before); !saved) return saved.error();
        effect_drafts_.erase(step.effect->id);
    }
    if (step.events)
        if (auto saved = assets_->set_clip_events(step.events->id, step.events->before); !saved) return saved.error();
    if (step.folders) { view_state_.folders = step.folders->before; store_view_state(); }
    redo_steps_.push_back(std::move(step));
    undo_steps_.pop_back();
    return receipt;
}
Result<commands::Receipt> EditorSession::redo() {
    if (auto allowed = editing_only("Redo"); !allowed) return allowed.error();
    if (in_transaction()) return fail(ErrorCode::InvalidState, "Finish the drag before redoing");
    if (redo_steps_.empty()) return fail(ErrorCode::InvalidState, "History is empty");
    auto &step = redo_steps_.back();
    commands::Receipt receipt{bus_.revision(), {}, 0};
    if (step.scene) {
        auto r = bus_.redo(context_, bus_.revision());
        reconcile();
        if (!r) return r;
        receipt = r.value();
    }
    if (step.material) {
        if (auto saved = assets_->save_material(step.material->id, step.material->after); !saved) return saved.error();
        forget_materials();
    }
    if (step.effect) {
        if (auto saved = assets_->save_effect(step.effect->id, step.effect->after); !saved) return saved.error();
        effect_drafts_.erase(step.effect->id);
    }
    if (step.events)
        if (auto saved = assets_->set_clip_events(step.events->id, step.events->after); !saved) return saved.error();
    if (step.folders) { view_state_.folders = step.folders->after; store_view_state(); }
    undo_steps_.push_back(std::move(step));
    redo_steps_.pop_back();
    return receipt;
}
Result<void> EditorSession::new_scene() {
    if (auto allowed = editing_only("New Scene"); !allowed) return allowed;
    // next_id is kept by ReplaceScene, so stable IDs are never recycled within a session.
    auto r = execute(commands::ReplaceScene{SceneSnapshot{}}, "New Scene");
    if (!r) return r.error();
    selection_.clear();
    poses_.clear();
    clean_.clear(); dirty_revision_ = ~std::uint64_t(0);
    document_.clear();
    view_state_ = {};
    return {};
}
bool EditorSession::dirty() const {
    if (dirty_revision_ != bus_.revision()) {
        dirty_ = scene_.snapshot().entities != clean_;
        dirty_revision_ = bus_.revision();
    }
    return dirty_;
}
void EditorSession::mark_clean() { clean_ = scene_.snapshot().entities; dirty_revision_ = ~std::uint64_t(0); }
Result<void> EditorSession::save(const std::filesystem::path &path) {
    if (auto allowed = editing_only("Save"); !allowed) return allowed;
    if (path.empty()) return fail(ErrorCode::InvalidArgument, "Choose a scene filename");
    document_ = path;
    auto temp = path; temp += ".tmp";
    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
    if (!stream) return fail(ErrorCode::InvalidState, "Cannot open temporary scene file");
    auto written = write_scene(stream, scene_.snapshot());
    stream.flush();
    const bool good = static_cast<bool>(stream);
    stream.close();
    if (!written || !good) return fail(ErrorCode::InvalidState, "Scene write failed; original preserved");
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return fail(ErrorCode::InvalidState, "Cannot replace scene file; original preserved");
#else
    std::error_code ec; std::filesystem::rename(temp, path, ec);
    if (ec) return fail(ErrorCode::InvalidState, ec.message());
#endif
    mark_clean();
    store_view_state();
    return {};
}
Result<void> EditorSession::load(const std::filesystem::path &path) {
    if (auto allowed = editing_only("Load"); !allowed) return allowed;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return fail(ErrorCode::NotFound, "Cannot open scene file");
    auto snapshot = read_scene(stream);
    if (!snapshot) return snapshot.error();
    auto r = execute(commands::ReplaceScene{std::move(snapshot.value())}, "Load Scene");
    if (!r) return r.error();
    selection_.clear();
    poses_.clear();
    mark_clean();
    document_ = path;
    load_view_state();
    return {};
}
Result<Transform> EditorSession::local_from_world(EntityId parent, const WorldTransform &world, const std::string *bone) const {
    auto local = glm::make_mat4(world.matrix.data());
    if (parent) {
        // The frame the entity's transform is relative to, as drawn: its parent's, then the joint it is attached to.
        auto frame = glm::make_mat4(drawn_world(parent)->matrix.data());
        if (bone)
            if (const auto joint = joint_model(parent, *bone)) frame *= glm::dmat4(glm::make_mat4(joint->data()));
        local = glm::inverse(frame) * local;
    }
    glm::dvec3 scale, position, skew;
    glm::dvec4 perspective;
    glm::dquat rotation;
    if (!glm::decompose(local, scale, rotation, position, skew, perspective))
        return fail(ErrorCode::InvalidArgument, "Cannot decompose world transform");
    const auto recomposed = glm::translate(glm::dmat4(1), position) * glm::mat4_cast(rotation) * glm::scale(glm::dmat4(1), scale);
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
        if (!std::isfinite(local[c][r]) || std::abs(local[c][r] - recomposed[c][r]) > 1e-5 * std::max(1.0, std::abs(local[c][r])))
            return fail(ErrorCode::InvalidArgument, "World edit introduces shear; use local inspector or uniform parent scale");
    return Transform{{position.x, position.y, position.z}, {rotation.x, rotation.y, rotation.z, rotation.w}, {scale.x, scale.y, scale.z}};
}
Result<void> EditorSession::set_world(EntityId id, const WorldTransform &world) {
    const auto record = scene().inspect(id);
    if (!record) return fail(ErrorCode::NotFound, "Entity no longer exists");
    auto t = local_from_world(record->parent, world, record->components.attach ? &record->components.attach->bone : nullptr);
    if (!t) return t.error();
    auto result = execute(commands::SetTransform{id, t.value()});
    if (!result) return result.error();
    return {};
}
Result<commands::Receipt> EditorSession::reparent(EntityId id, EntityId parent) {
    const auto record = scene().inspect(id);
    if (!record) return fail(ErrorCode::NotFound, "Entity no longer exists");
    if (parent && !scene().inspect(parent)) return fail(ErrorCode::NotFound, "New parent no longer exists");
    for (EntityId ancestor = parent; ancestor; ancestor = scene().inspect(ancestor)->parent)
        if (ancestor == id) return fail(ErrorCode::Cycle, "Cannot parent an entity under itself or one of its descendants");
    auto local = local_from_world(parent, *drawn_world(id), record->components.attach ? &record->components.attach->bone : nullptr);
    if (!local) return fail(ErrorCode::InvalidArgument, "Cannot keep the world transform under that parent: " + local.error().message);
    const commands::Command batch[] = {commands::SetParent{id, parent}, commands::SetTransform{id, local.value()}};
    return execute(std::span<const commands::Command>(batch), parent ? "Reparent " + record->name : "Move " + record->name + " to Root");
}
Result<commands::Receipt> EditorSession::duplicate(EntityId id) {
    const auto record = scene().inspect(id);
    if (!record) return fail(ErrorCode::NotFound, "Entity no longer exists");
    // "<name> (copy)", trimmed to the 256-byte name limit without splitting a UTF-8 sequence.
    constexpr std::string_view suffix = " (copy)";
    auto name = record->name;
    if (name.size() + suffix.size() > 256) {
        name.resize(256 - suffix.size());
        while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80) name.pop_back();
        if (!name.empty() && static_cast<unsigned char>(name.back()) >= 0xC0) name.pop_back();
    }
    auto result = execute(commands::DuplicateEntity{id, name + std::string(suffix)}, "Duplicate " + record->name);
    if (result) selection_.set(result.value().created.front());
    return result;
}
namespace {
std::string many(std::string_view verb, std::size_t count, const std::string &single) {
    return std::string(verb) + ' ' + (count == 1 ? single : std::to_string(count) + " Entities");
}
}
Result<void> EditorSession::duplicate_selection() {
    const auto roots = selection_roots();
    if (roots.empty()) return fail(ErrorCode::InvalidState, "Nothing is selected");
    ScopedTransaction transaction(*this, many("Duplicate", roots.size(), scene().inspect(roots.front())->name));
    std::vector<EntityId> copies;
    for (const auto id : roots) {
        auto copied = duplicate(id);
        if (!copied) { transaction.cancel(); return copied.error(); }
        copies.push_back(copied.value().created.front());
    }
    selection_.set(std::span<const EntityId>(copies));
    return {};
}
Result<void> EditorSession::delete_selection() {
    const auto roots = selection_roots();
    if (roots.empty()) return fail(ErrorCode::InvalidState, "Nothing is selected");
    ScopedTransaction transaction(*this, many("Delete", roots.size(), scene().inspect(roots.front())->name));
    for (const auto id : roots)
        if (auto deleted = execute(commands::DeleteEntity{id}); !deleted) { transaction.cancel(); return deleted.error(); }
    return {};
}
Result<void> EditorSession::begin_drag(std::string_view verb, bool duplicate) {
    if (dragging()) return fail(ErrorCode::InvalidState, "A drag is already in progress");
    const auto roots = selection_roots();
    if (roots.empty()) return fail(ErrorCode::InvalidState, "Nothing is selected");
    const auto name = many(verb, roots.size(), scene().inspect(roots.front())->name);
    if (auto begun = begin_transaction(duplicate ? "Duplicate and " + name : name); !begun) return begun;
    if (duplicate) {
        if (auto copied = duplicate_selection(); !copied) { cancel_transaction(); (void)end_transaction(); return copied; }
    }
    for (const auto id : selection_roots()) drag_start_.emplace_back(id, *drawn_world(id));
    return {};
}
Result<void> EditorSession::update_drag(const WorldTransform &delta) {
    if (!dragging()) return fail(ErrorCode::InvalidState, "No drag is in progress");
    const auto change = glm::make_mat4(delta.matrix.data());
    std::vector<commands::Command> moves;
    // Worked out before anything is applied, so one entity that cannot follow leaves them all where they were.
    for (const auto &[id, start] : drag_start_) {
        const auto record = scene().inspect(id);
        if (!record) continue;
        auto local = local_from_world(record->parent, copy(change * glm::make_mat4(start.matrix.data())),
                                      record->components.attach ? &record->components.attach->bone : nullptr);
        if (!local) return local.error();
        moves.emplace_back(commands::SetTransform{id, local.value()});
    }
    for (std::size_t at = 0; at < moves.size(); at += commands::CommandBus::max_transaction) {
        const auto count = std::min(moves.size() - at, commands::CommandBus::max_transaction);
        if (auto moved = execute(std::span<const commands::Command>(moves).subspan(at, count), {}); !moved) return moved.error();
    }
    return {};
}
Result<void> EditorSession::end_drag() {
    if (!dragging()) return fail(ErrorCode::InvalidState, "No drag is in progress");
    drag_start_.clear();
    return end_transaction();
}
Result<void> EditorSession::cancel_drag() {
    if (!dragging()) return fail(ErrorCode::InvalidState, "No drag is in progress");
    // A runtime scene keeps no record to restore from, so it is put back by hand.
    if (runtime_) {
        WorldTransform unchanged;
        std::copy_n(glm::value_ptr(glm::dmat4(1)), 16, unchanged.matrix.begin());
        if (auto restored = update_drag(unchanged); !restored) return restored;
    }
    drag_start_.clear();
    cancel_transaction();
    return end_transaction();
}
std::set<EntityId> EditorSession::selection_subtrees() const {
    std::set<EntityId> all(selection_.items().begin(), selection_.items().end());
    for (const auto &e : scene().snapshot().entities)
        for (EntityId p = e.parent; p; p = scene().inspect(p)->parent)
            if (all.contains(p)) { all.insert(e.id); break; }
    return all;
}
namespace {
// World-space box around a local box.
std::pair<glm::dvec3, glm::dvec3> world_box(const glm::dmat4 &m, const Vec3 &low, const Vec3 &high) {
    glm::dvec3 lo(std::numeric_limits<double>::max()), hi(std::numeric_limits<double>::lowest());
    for (int i = 0; i < 8; ++i) {
        const glm::dvec3 corner(m * glm::dvec4(i & 1 ? high.x : low.x, i & 2 ? high.y : low.y, i & 4 ? high.z : low.z, 1));
        lo = glm::min(lo, corner); hi = glm::max(hi, corner);
    }
    return {lo, hi};
}
}
std::optional<OrbitCamera> EditorSession::focus_goal() const {
    if (selection_.empty()) return std::nullopt;
    glm::dvec3 lo(std::numeric_limits<double>::max()), hi(std::numeric_limits<double>::lowest());
    bool boxed = false;
    for (const auto id : selection_.items()) {
        const auto w = drawn_world(id);
        if (!w) continue;
        const auto m = glm::make_mat4(w->matrix.data());
        if (const auto bounds = local_bounds(id)) {
            const auto [low, high] = world_box(m, bounds->first, bounds->second);
            lo = glm::min(lo, low); hi = glm::max(hi, high);
            boxed = true;
        } else { lo = glm::min(lo, glm::dvec3(m[3])); hi = glm::max(hi, glm::dvec3(m[3])); }
    }
    auto goal = camera;
    const auto center = (lo + hi) / 2.0;
    goal.target = {center.x, center.y, center.z};
    const double size = glm::length(hi - lo);
    // A marker with no size is framed as a unit cube would be.
    goal.distance = std::clamp(boxed || size > 0 ? 1.75 * size : 3.0, 0.2, 500.0);
    return goal;
}
std::optional<Vec3> EditorSession::selection_center() const {
    const auto goal = focus_goal();
    if (!goal) return std::nullopt;
    return goal->target;
}
std::pair<EntityId, std::optional<double>> EditorSession::cast_ray(const renderer::FrameSnapshot &frame, const Vec3 &from, const Vec3 &direction, double limit, const std::set<EntityId> &exclude) const {
    const glm::dvec4 a(from.x, from.y, from.z, 1), d(direction.x, direction.y, direction.z, 0);
    double nearest = limit;
    EntityId entity = 0;
    for (const auto &object : frame.objects) {
        // Debris and the like cannot be picked, and do not hide what is behind them.
        if (exclude.contains(object.entity) || object.entity >= renderer::runtime_object_base) continue;
        const auto inverse = glm::inverse(glm::make_mat4(object.world.matrix.data()));
        // The local ray is not normalized, so t stays a world-space distance along the ray.
        const glm::dvec3 origin(inverse * a), ray(inverse * d);
        // Models are tested by their bounding box.
        const auto data = resources_.find(object.mesh);
        const auto box = data ? model_box(object, *data) : assets::Bounds{};
        const double t = data ? box_hit(vector(box.min), vector(box.max), origin, ray, nearest) : hit(object.mesh, origin, ray, nearest);
        if (t >= 0 && t < nearest) { nearest = t; entity = object.entity; }
    }
    if (!entity) return {0, std::nullopt};
    return {entity, nearest};
}
std::optional<Vec3> EditorSession::ray_hit(const renderer::FrameSnapshot &frame, double x, double y) const {
    const auto inv = glm::inverse(glm::make_mat4(frame.camera.projection.matrix.data()) * glm::make_mat4(frame.camera.view.matrix.data()));
    auto a = inv * glm::dvec4(x, y, -1, 1), b = inv * glm::dvec4(x, y, 1, 1);
    a /= a.w; b /= b.w;
    const auto segment = glm::dvec3(b - a);
    const double length = glm::length(segment);
    const auto direction = segment / length;
    const auto [entity, t] = cast_ray(frame, {a.x, a.y, a.z}, {direction.x, direction.y, direction.z}, length, {});
    if (entity) { const auto p = glm::dvec3(a) + direction * *t; return Vec3{p.x, p.y, p.z}; }
    if (direction.y >= -1e-9 || a.y <= 0) return std::nullopt;
    const auto p = glm::dvec3(a) + direction * (-a.y / direction.y);
    return Vec3{p.x, 0, p.z};
}
Result<void> EditorSession::snap_selection_to_floor(const renderer::FrameSnapshot &frame) {
    const auto roots = selection_roots();
    if (roots.empty()) return fail(ErrorCode::InvalidState, "Nothing is selected");
    const auto exclude = selection_subtrees();
    ScopedTransaction transaction(*this, many("Snap", roots.size(), scene().inspect(roots.front())->name) + " to Floor");
    bool moved = false;
    for (const auto id : roots) {
        const auto w = drawn_world(id);
        const auto m = glm::make_mat4(w->matrix.data());
        glm::dvec3 lo(m[3]), hi(m[3]);
        if (const auto bounds = local_bounds(id)) std::tie(lo, hi) = world_box(m, bounds->first, bounds->second);
        const auto middle = (lo + hi) / 2.0;
        const auto [entity, t] = cast_ray(frame, {middle.x, middle.y, middle.z}, {0, -1, 0}, 100000, exclude);
        double surface = 0;
        if (entity) surface = middle.y - *t;
        else if (lo.y <= 0) continue; // Nothing below, and already at or under the ground plane.
        if (std::abs(surface - lo.y) < 1e-9) continue;
        auto placed = glm::make_mat4(w->matrix.data());
        placed[3].y += surface - lo.y;
        const auto record = scene().inspect(id);
        auto local = local_from_world(record->parent, copy(placed), record->components.attach ? &record->components.attach->bone : nullptr);
        if (!local) { transaction.cancel(); return local.error(); }
        if (auto done = execute(commands::SetTransform{id, local.value()}); !done) { transaction.cancel(); return done.error(); }
        moved = true;
    }
    if (!moved) return fail(ErrorCode::NotFound, "Nothing to snap to below the selection");
    return {};
}
std::optional<Vec3> EditorSession::nearest_vertex(const renderer::FrameSnapshot &frame, double x, double y, double width, double height, double radius) const {
    const auto clip_from_world = glm::make_mat4(frame.camera.projection.matrix.data()) * glm::make_mat4(frame.camera.view.matrix.data());
    const auto exclude = selection_subtrees();
    std::optional<Vec3> best;
    double best_distance = radius * radius;
    const auto consider = [&](const glm::dmat4 &world, const glm::dvec3 &local) {
        const glm::dvec4 at = world * glm::dvec4(local, 1);
        const auto clip = clip_from_world * at;
        if (clip.w <= 1e-6) return;
        const double sx = (clip.x / clip.w + 1) / 2 * width - x, sy = (1 - clip.y / clip.w) / 2 * height - y;
        const double squared = sx * sx + sy * sy;
        if (squared < best_distance) { best_distance = squared; best = Vec3{at.x, at.y, at.z}; }
    };
    for (const auto &object : frame.objects) {
        if (exclude.contains(object.entity) || object.entity >= renderer::runtime_object_base) continue;
        const auto world = glm::make_mat4(object.world.matrix.data());
        if (const auto data = resources_.find(object.mesh)) {
            // A model far from the cursor is passed over by its box, so one with many vertices costs nothing unless near.
            // A skinned model's box and vertices are where its pose, or its rest pose, puts them.
            auto palette = posed_palette(object, *data);
            const assets::Bounds box = palette.empty() ? data->bounds : assets::skinned_bounds(data->skin, palette);
            if (palette.empty() && !data->skin.empty()) palette = assets::skinning_palette(data->skin, assets::rest_pose(data->skin));
            double min_x = std::numeric_limits<double>::max(), max_x = std::numeric_limits<double>::lowest(), min_y = min_x, max_y = max_x;
            bool behind = false;
            const auto low = vector(box.min), high = vector(box.max);
            for (int i = 0; i < 8; ++i) {
                const auto clip = clip_from_world * world * glm::dvec4(i & 1 ? high.x : low.x, i & 2 ? high.y : low.y, i & 4 ? high.z : low.z, 1);
                if (clip.w <= 1e-6) { behind = true; break; }
                const double sx = (clip.x / clip.w + 1) / 2 * width, sy = (1 - clip.y / clip.w) / 2 * height;
                min_x = std::min(min_x, sx); max_x = std::max(max_x, sx); min_y = std::min(min_y, sy); max_y = std::max(max_y, sy);
            }
            if (!behind && (x < min_x - radius || x > max_x + radius || y < min_y - radius || y > max_y + radius)) continue;
            if (palette.empty()) {
                for (const auto &vertex : data->vertices) consider(world, vector(vertex.position));
            } else {
                for (std::size_t i = 0; i < data->vertices.size(); ++i) consider(world, vector(assets::skinned_position(data->skin, palette, data->vertices[i], i)));
            }
        } else if (object.mesh == renderer::builtin_plane) {
            for (const double sx : {-0.5, 0.5}) for (const double sz : {-0.5, 0.5}) consider(world, {sx, 0, sz});
        } else if (object.mesh == renderer::builtin_sphere) {
            for (const auto &p : {glm::dvec3(0.5, 0, 0), glm::dvec3(-0.5, 0, 0), glm::dvec3(0, 0.5, 0), glm::dvec3(0, -0.5, 0), glm::dvec3(0, 0, 0.5), glm::dvec3(0, 0, -0.5)}) consider(world, p);
        } else {
            for (int i = 0; i < 8; ++i) consider(world, {i & 1 ? 0.5 : -0.5, i & 2 ? 0.5 : -0.5, i & 4 ? 0.5 : -0.5});
        }
    }
    return best;
}
void EditorSession::hide_selection() {
    const auto all = selection_subtrees();
    view_state_.hidden.insert(all.begin(), all.end());
    selection_.clear();
    store_view_state();
}
void EditorSession::unhide_all() {
    view_state_.hidden.clear();
    store_view_state();
}
void EditorSession::set_flag_on_subtree(std::set<EntityId> &flags, EntityId id, bool on) {
    const auto entities = scene().snapshot().entities;
    std::set<EntityId> subtree{id};
    // Entities are in ID order, and a child may have a lower ID than its parent after a reparent, so walk up from each.
    for (const auto &e : entities)
        for (EntityId p = e.parent; p; p = scene().inspect(p)->parent)
            if (p == id) { subtree.insert(e.id); break; }
    for (const auto entity : subtree) { if (on) flags.insert(entity); else flags.erase(entity); }
    store_view_state();
}
void EditorSession::set_hidden(EntityId id, bool on) {
    set_flag_on_subtree(view_state_.hidden, id, on);
    if (on) for (const auto entity : view_state_.hidden) selection_.remove(entity);
}
void EditorSession::set_locked(EntityId id, bool on) { set_flag_on_subtree(view_state_.locked, id, on); }
Result<void> EditorSession::commit_folders(EditorFolders next, std::string name) {
    if (auto allowed = editing_only("Changing folders"); !allowed) return allowed;
    if (next == view_state_.folders) return {};
    if (in_transaction()) {
        if (!transaction_folders_) transaction_folders_ = view_state_.folders;
        view_state_.folders = std::move(next);
        return {};
    }
    undo_steps_.push_back({std::move(name), std::nullopt, FolderEdit{view_state_.folders, next}, false});
    redo_steps_.clear();
    bus_.discard_redo();
    view_state_.folders = std::move(next);
    store_view_state();
    return {};
}
Result<std::uint64_t> EditorSession::create_folder(const std::string &name, std::uint64_t parent) {
    if (name.empty()) return fail(ErrorCode::InvalidArgument, "A folder needs a name");
    auto next = view_state_.folders;
    if (parent && !next.find(parent)) return fail(ErrorCode::NotFound, "That folder no longer exists");
    const auto id = next.next_id++;
    next.folders.push_back({id, parent, name});
    if (auto done = commit_folders(std::move(next), "Create Folder " + name); !done) return done.error();
    return id;
}
Result<void> EditorSession::rename_folder(std::uint64_t id, const std::string &name) {
    if (name.empty()) return fail(ErrorCode::InvalidArgument, "A folder needs a name");
    auto next = view_state_.folders;
    const auto at = std::ranges::find(next.folders, id, &SceneFolder::id);
    if (at == next.folders.end()) return fail(ErrorCode::NotFound, "That folder no longer exists");
    at->name = name;
    return commit_folders(std::move(next), "Rename Folder");
}
Result<void> EditorSession::delete_folder(std::uint64_t id) {
    auto next = view_state_.folders;
    const auto at = std::ranges::find(next.folders, id, &SceneFolder::id);
    if (at == next.folders.end()) return fail(ErrorCode::NotFound, "That folder no longer exists");
    const auto parent = at->parent;
    const auto name = at->name;
    next.folders.erase(at);
    for (auto &folder : next.folders) if (folder.parent == id) folder.parent = parent;
    for (auto it = next.members.begin(); it != next.members.end();) {
        if (it->second != id) { ++it; continue; }
        if (parent) { it->second = parent; ++it; } else it = next.members.erase(it);
    }
    return commit_folders(std::move(next), "Delete Folder " + name);
}
Result<void> EditorSession::move_folder(std::uint64_t id, std::uint64_t parent) {
    auto next = view_state_.folders;
    const auto at = std::ranges::find(next.folders, id, &SceneFolder::id);
    if (at == next.folders.end()) return fail(ErrorCode::NotFound, "That folder no longer exists");
    for (auto p = parent; p; ) {
        if (p == id) return fail(ErrorCode::Cycle, "Cannot put a folder inside itself");
        const auto *up = next.find(p);
        if (!up) return fail(ErrorCode::NotFound, "That folder no longer exists");
        p = up->parent;
    }
    at->parent = parent;
    return commit_folders(std::move(next), "Move Folder");
}
Result<void> EditorSession::file_entity(EntityId id, std::uint64_t folder) {
    const auto record = scene().inspect(id);
    if (!record) return fail(ErrorCode::NotFound, "Entity no longer exists");
    auto next = view_state_.folders;
    if (folder && !next.find(folder)) return fail(ErrorCode::NotFound, "That folder no longer exists");
    if (folder) next.members[id] = folder; else next.members.erase(id);
    ScopedTransaction transaction(*this, "Move " + record->name + " to Folder");
    if (record->parent)
        if (auto moved = reparent(id, 0); !moved) { transaction.cancel(); return moved.error(); }
    if (auto filed = commit_folders(std::move(next), {}); !filed) { transaction.cancel(); return filed; }
    return {};
}
std::uint64_t EditorSession::folder_of(EntityId id) const {
    const auto found = view_state_.folders.members.find(id);
    return found == view_state_.folders.members.end() ? 0 : found->second;
}
Result<std::string> EditorSession::copy_selection() const {
    const auto roots = selection_roots();
    if (roots.empty()) return fail(ErrorCode::InvalidState, "Nothing is selected");
    const auto all = selection_subtrees();
    SceneSnapshot copied;
    for (const auto &e : scene().snapshot().entities) {
        if (!all.contains(e.id)) continue;
        auto entity = e;
        if (std::ranges::find(roots, e.id) != roots.end()) {
            // Without its parent, a root takes its place in the world as its own transform.
            entity.parent = 0;
            if (const auto local = local_from_world(0, *drawn_world(e.id))) entity.transform = local.value();
        } else if (!all.contains(e.parent)) entity.parent = 0;
        copied.entities.push_back(std::move(entity));
        copied.next_id = std::max(copied.next_id, e.id + 1);
    }
    std::ostringstream out;
    if (auto written = write_scene(out, copied); !written) return written.error();
    return out.str();
}
Result<void> EditorSession::paste(std::string_view text) {
    std::istringstream in{std::string(text)};
    const auto copied = read_scene(in);
    if (!copied || copied.value().entities.empty()) return fail(ErrorCode::ParseError, "The clipboard holds no entities");
    const auto &entities = copied.value().entities;
    ScopedTransaction transaction(*this, entities.size() == 1 ? "Paste " + entities.front().name : "Paste " + std::to_string(entities.size()) + " Entities");
    std::map<EntityId, EntityId> created;
    std::vector<EntityId> roots;
    // Parents first, however the IDs happen to be ordered.
    std::vector<const EntityRecord *> waiting;
    for (const auto &e : entities) waiting.push_back(&e);
    while (!waiting.empty()) {
        const auto before = waiting.size();
        std::erase_if(waiting, [&](const EntityRecord *e) {
            if (e->parent && !created.contains(e->parent)) return false;
            const auto made = execute(commands::CreateEntity{e->name, e->transform, e->parent ? created.at(e->parent) : 0, 0, e->components});
            if (!made) return false;
            created[e->id] = made.value().created.front();
            if (!e->parent) roots.push_back(made.value().created.front());
            return true;
        });
        if (waiting.size() == before) { transaction.cancel(); return fail(ErrorCode::InvalidArgument, "The clipboard's entities could not be added"); }
    }
    selection_.set(std::span<const EntityId>(roots));
    return {};
}
Result<EntityId> EditorSession::group_selection() {
    const auto roots = selection_roots();
    if (roots.empty()) return fail(ErrorCode::InvalidState, "Nothing is selected");
    const auto center = selection_center();
    Transform t;
    t.position = *center;
    ScopedTransaction transaction(*this, roots.size() == 1 ? "Group " + scene().inspect(roots.front())->name : "Group " + std::to_string(roots.size()) + " Entities");
    const auto group = execute(commands::CreateEntity{"Group", t, 0, 0, Components{}});
    if (!group) { transaction.cancel(); return group.error(); }
    const auto id = group.value().created.front();
    for (const auto root : roots) {
        const auto moved = reparent(root, id);
        if (!moved) { transaction.cancel(); return moved.error(); }
    }
    selection_.set(id);
    return id;
}
std::vector<commands::Command> EditorSession::reference_edits(const std::function<bool(AssetRef &)> &fix) const {
    std::vector<commands::Command> edits;
    for (const auto &e : scene_.snapshot().entities) {
        if (auto mesh = e.components.mesh) {
            bool changed = fix(mesh->mesh_asset);
            changed |= fix(mesh->texture);
            changed |= fix(mesh->material);
            for (auto &slot : mesh->material_slots) changed |= fix(slot.material);
            if (changed) edits.emplace_back(commands::SetMesh{e.id, std::move(mesh)});
        }
        if (auto environment = e.components.environment; environment && fix(environment->image)) edits.emplace_back(commands::SetEnvironment{e.id, std::move(environment)});
        if (auto emitter = e.components.emitter; emitter && fix(emitter->effect)) edits.emplace_back(commands::SetParticleEmitter{e.id, std::move(emitter)});
        if (auto animator = e.components.animator; animator && fix(animator->clips)) edits.emplace_back(commands::SetAnimator{e.id, std::move(animator)});
    }
    return edits;
}
Result<void> EditorSession::rename_asset(assets::AssetId id, const std::string &new_path) {
    if (auto allowed = editing_only("Renaming an asset"); !allowed) return allowed;
    if (!assets_) return fail(ErrorCode::NotFound, "No asset folder");
    const auto before = assets_->find(id);
    if (!before) return fail(ErrorCode::NotFound, "unknown asset id " + assets::to_string(id));
    // A package name: "/Game/Textures/Walls/Brick", in any case, keeping the file's extension when it gives none.
    std::string target = new_path;
    if (const std::string prefix = std::string(game_mount) + "/"; assets::fold_case(target).starts_with(assets::fold_case(prefix))) {
        target = target.substr(prefix.size());
        if (assets::asset_type_for(target) == assets::AssetType::Unknown) {
            const auto dot = before->path.rfind('.');
            if (dot != std::string::npos) target += before->path.substr(dot);
        }
    } else if (!target.empty() && (target.front() == '/' || target.front() == '\\')) {
        return fail(ErrorCode::InvalidArgument, "Package names start with " + std::string(game_mount) + "/: " + new_path);
    }
    if (auto moved = assets_->move(id, target, true); !moved) return moved;
    const auto meta = assets_->find(id);
    if (!meta) return fail(ErrorCode::NotFound, "The asset could not be found after moving it");
    resolved_.clear();
    const auto fix = [&](AssetRef &ref) {
        if (ref.id != id.value || ref.path == meta->path) return false;
        ref.path = meta->path;
        return true;
    };
    const auto edits = reference_edits(fix);
    ScopedTransaction transaction(*this, "Update References to " + meta->path);
    for (std::size_t at = 0; at < edits.size(); at += commands::CommandBus::max_transaction) {
        const auto count = std::min(edits.size() - at, commands::CommandBus::max_transaction);
        if (auto done = execute(std::span<const commands::Command>(edits).subspan(at, count), {}); !done) return done.error();
    }
    // Materials name their maps too. These are not scene edits, so they are saved straight away.
    for (const auto &other : assets_->assets()) {
        if (other.type != assets::AssetType::Material) continue;
        auto data = material(other.id);
        if (!data) continue;
        bool changed = false;
        for (auto *map : {&data->base_color_map, &data->metallic_roughness_map, &data->normal_map, &data->occlusion_map, &data->emissive_map})
            if (map->id == id && map->path != meta->path) { map->path = meta->path; changed = true; }
        if (changed) if (auto saved = assets_->save_material(other.id, *data); !saved) return saved;
    }
    for (const auto &other : assets_->assets()) {
        if (other.type != assets::AssetType::Effect) continue;
        auto data = effect(other.id);
        if (!data || data->texture.id != id || data->texture.path == meta->path) continue;
        data->texture.path = meta->path;
        if (auto saved = assets_->save_effect(other.id, *data); !saved) return saved;
        effect_drafts_.erase(other.id);
    }
    forget_materials();
    return {};
}
const assets::VirtualFileSystem &EditorSession::packages() const {
    const std::uint64_t scans = assets_ ? assets_->scans() : 0;
    if (packages_scans_ != scans) {
        const auto catalogue = assets_ ? assets_->assets() : std::vector<assets::AssetMetadata>{};
        const std::vector<assets::Redirector> none;
        const auto indexed = packages_.index(game_mount, catalogue, assets_ ? assets_->redirectors() : std::span<const assets::Redirector>(none));
        package_conflicts_ = indexed ? indexed.value() : std::vector<std::string>{indexed.error().message};
        packages_scans_ = scans;
    }
    return packages_;
}
Result<assets::PackageResolution> EditorSession::resolve_package(std::string_view name) const { return packages().resolve(name); }
std::string EditorSession::package_name_of(assets::AssetId id) const { return packages().name_of(id).value_or(std::string{}); }
std::vector<assets::Redirector> EditorSession::redirectors() const {
    if (!assets_) return {};
    const auto found = assets_->redirectors();
    return {found.begin(), found.end()};
}
std::vector<std::string> EditorSession::package_conflicts() const { packages(); return package_conflicts_; }
Result<EditorSession::FixUpReport> EditorSession::fix_up_redirectors() {
    if (auto allowed = editing_only("Fixing up redirectors"); !allowed) return allowed.error();
    if (!assets_) return fail(ErrorCode::NotFound, "No asset folder");
    FixUpReport report;
    // Where each redirector leads now. Its own name may be taken by another asset since, so it is followed from its target.
    struct Move { std::string old_path; assets::AssetId id; assets::AssetMetadata now; };
    std::vector<Move> moves;
    std::vector<std::string> resolved_paths;
    for (const auto &r : redirectors()) {
        const auto found = resolve_package(std::string(game_mount) + "/" + r.target);
        if (!found) { report.kept.push_back(r.path + ": " + found.error().message); continue; }
        const auto now = assets_->find(found.value().id);
        if (!now) { report.kept.push_back(r.path + ": its asset is not catalogued"); continue; }
        moves.push_back({r.path, r.id, *now});
        resolved_paths.push_back(r.path);
    }
    if (moves.empty()) return report;
    // A reference is stale when it names an old path, or when its ID is the moved asset's and its path is not where it is.
    const auto fix = [&](AssetRef &ref) {
        if (ref.empty()) return false;
        for (const auto &m : moves) {
            const bool by_path = assets::fold_case(ref.path) == assets::fold_case(m.old_path);
            const bool by_id = !m.id.is_null() && ref.id == m.id.value;
            if (!by_path && !by_id) continue;
            if (ref.path == m.now.path && ref.id == m.now.id.value) return false;
            // An ID with no asset behind it any more (a deleted .meta) takes the asset's ID too.
            if (!assets_->find(assets::AssetId{ref.id})) ref.id = m.now.id.value;
            if (ref.id != m.now.id.value) continue; // Another asset, which only shares the old name.
            ref.path = m.now.path;
            return true;
        }
        return false;
    };
    // Scene files on disk, apart from the open one: rewritten in place, through a temporary file.
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    const auto utf8 = [](const std::filesystem::path &p) { const auto u = p.u8string(); return std::string(reinterpret_cast<const char *>(u.data()), u.size()); };
    for (std::filesystem::recursive_directory_iterator it(project_, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code probe;
        if (it->is_directory(probe) && (it.depth() >= 8 || utf8(it->path().filename()).starts_with("."))) { it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(probe) || assets::fold_case(utf8(it->path().extension())) != ".scene") continue;
        if (!document_.empty() && std::filesystem::equivalent(it->path(), document_, probe)) continue;
        files.push_back(it->path());
    }
    for (const auto &file : files) {
        std::ifstream in(file, std::ios::binary);
        auto snapshot = read_scene(in);
        in.close();
        if (!snapshot) { report.kept.push_back(file.generic_string() + " could not be read: " + snapshot.error().message); continue; }
        std::size_t changed = 0;
        for (auto &e : snapshot.value().entities) {
            auto &c = e.components;
            if (c.mesh) {
                changed += fix(c.mesh->mesh_asset) + fix(c.mesh->texture) + fix(c.mesh->material);
                for (auto &slot : c.mesh->material_slots) changed += fix(slot.material);
            }
            if (c.environment) changed += fix(c.environment->image);
            if (c.emitter) changed += fix(c.emitter->effect);
            if (c.animator) changed += fix(c.animator->clips);
        }
        if (!changed) continue;
        auto temporary = file; temporary += ".tmp";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out || !write_scene(out, snapshot.value()) || !out.flush()) {
                std::filesystem::remove(temporary, ec);
                return fail(ErrorCode::InvalidState, "Cannot write " + file.generic_string() + "; no redirector was removed");
            }
        }
        std::filesystem::rename(temporary, file, ec);
        if (ec) { std::filesystem::remove(temporary, ec); return fail(ErrorCode::InvalidState, "Cannot replace " + file.generic_string() + "; no redirector was removed"); }
        report.scenes.push_back(file);
        report.references += changed;
    }
    // The open scene: one undo step, left for its owner to save.
    std::size_t in_scene = 0;
    const auto counted = [&](AssetRef &ref) { const bool done = fix(ref); in_scene += done; return done; };
    const auto edits = reference_edits(counted);
    if (!edits.empty()) {
        ScopedTransaction transaction(*this, "Fix Up Redirectors");
        for (std::size_t at = 0; at < edits.size(); at += commands::CommandBus::max_transaction) {
            const auto count = std::min(edits.size() - at, commands::CommandBus::max_transaction);
            if (auto done = execute(std::span<const commands::Command>(edits).subspan(at, count), {}); !done) return done.error();
        }
        report.open_scene = true;
        report.references += in_scene;
    }
    // Materials name their maps, and effects their images. These are saved at once, as a rename saves them.
    const auto fix_map = [&](assets::MaterialTexture &map) {
        if (map.id.is_null()) return false;
        AssetRef ref{map.id.value, map.path};
        if (!fix(ref)) return false;
        map.id = assets::AssetId{ref.id}; map.path = ref.path;
        return true;
    };
    for (const auto &other : assets_->assets()) {
        if (other.type == assets::AssetType::Material) {
            auto data = material(other.id);
            if (!data) continue;
            bool changed = false;
            for (auto *map : {&data->base_color_map, &data->metallic_roughness_map, &data->normal_map, &data->occlusion_map, &data->emissive_map}) changed |= fix_map(*map);
            if (!changed) continue;
            if (auto saved = assets_->save_material(other.id, *data); !saved) return saved.error();
            ++report.assets;
        } else if (other.type == assets::AssetType::Effect) {
            auto data = effect(other.id);
            if (!data || !fix_map(data->texture)) continue;
            if (auto saved = assets_->save_effect(other.id, *data); !saved) return saved.error();
            effect_drafts_.erase(other.id);
            ++report.assets;
        }
    }
    forget_materials();
    auto removed = assets_->remove_redirectors(resolved_paths);
    resolved_.clear();
    if (!removed) return removed.error();
    report.redirectors = removed.value();
    return report;
}
void EditorSession::request_thumbnail_data(assets::AssetId id) {
    const auto meta = assets_ ? assets_->find(id) : std::nullopt;
    if (!meta || (meta->type != assets::AssetType::Mesh && meta->type != assets::AssetType::Texture)) return;
    auto source = asset_folder() / std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(meta->path.data()), meta->path.size()));
    // The worker touches only its own copy of the path, like an import.
    thumbnail_jobs_.push_back({id, meta->type, std::async(std::launch::async, [source = std::move(source)]() -> std::shared_ptr<const assets::PreparedImport> {
        auto prepared = assets::prepare_import(source);
        return prepared ? std::make_shared<const assets::PreparedImport>(std::move(prepared.value())) : nullptr;
    })});
}
std::vector<EditorSession::ThumbnailData> EditorSession::poll_thumbnail_data() {
    std::vector<ThumbnailData> done;
    for (std::size_t i = 0; i < thumbnail_jobs_.size();) {
        auto &job = thumbnail_jobs_[i];
        if (job.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++i; continue; }
        ThumbnailData data;
        data.id = job.id; data.type = job.type;
        const auto prepared = job.result.get();
        thumbnail_jobs_.erase(thumbnail_jobs_.begin() + std::ptrdiff_t(i));
        if (prepared && prepared->mesh && assets_ && assets_->find(data.id)) {
            data.mesh = resources_.mesh(data.id, assets_->revision(data.id), prepared->mesh);
            data.vertices = prepared->mesh->vertices.size();
            data.triangles = prepared->mesh->indices.size() / 3;
            data.joints = prepared->mesh->skin.joints.size();
            for (const auto &clip : prepared->mesh->clips) data.clips.push_back(clip.name);
        } else if (prepared && prepared->texture && prepared->texture->width && prepared->texture->height) {
            const auto &image = *prepared->texture;
            data.source_width = image.width; data.source_height = image.height;
            const auto longest = std::max(image.width, image.height);
            const double step = std::max(double(longest) / thumbnail_size, 1.0);
            data.width = std::max(std::uint32_t(image.width / step), 1u);
            data.height = std::max(std::uint32_t(image.height / step), 1u);
            data.pixels.resize(std::size_t(data.width) * data.height * 4);
            const auto stride = image.pixels.size() / (std::size_t(image.width) * image.height);
            for (std::uint32_t y = 0; y < data.height; ++y)
                for (std::uint32_t x = 0; x < data.width; ++x) {
                    const auto source_x = std::min(std::uint32_t(x * step), image.width - 1), source_y = std::min(std::uint32_t(y * step), image.height - 1);
                    const auto *from = image.pixels.data() + (std::size_t(source_y) * image.width + source_x) * stride;
                    auto *to = data.pixels.data() + (std::size_t(y) * data.width + x) * 4;
                    to[0] = from[0]; to[1] = stride > 1 ? from[1] : from[0]; to[2] = stride > 2 ? from[2] : from[0]; to[3] = stride > 3 ? from[3] : 255;
                }
        } else data.failed = true;
        done.push_back(std::move(data));
    }
    return done;
}
void EditorSession::set_bookmark(std::size_t slot) {
    if (slot >= view_state_.bookmarks.size()) return;
    view_state_.bookmarks[slot] = CameraBookmark{camera.target, camera.yaw, camera.pitch, camera.distance, camera.orthographic};
    store_view_state();
}
std::optional<OrbitCamera> EditorSession::bookmark(std::size_t slot) const {
    if (slot >= view_state_.bookmarks.size() || !view_state_.bookmarks[slot]) return std::nullopt;
    const auto &b = *view_state_.bookmarks[slot];
    auto goal = camera;
    goal.target = b.target; goal.yaw = b.yaw; goal.pitch = b.pitch; goal.distance = b.distance; goal.orthographic = b.orthographic;
    return goal;
}
void EditorSession::store_view_state() {
    if (document_.empty()) return;
    auto file = document_;
    file += ".editor";
    std::error_code error;
    if (view_state_.empty()) { std::filesystem::remove(file, error); return; }
    if (auto written = write_text_file(file, write_view_state(view_state_)); !written) view_state_problem_ = written.error().message;
}
void EditorSession::load_view_state() {
    view_state_ = {};
    auto file = document_;
    file += ".editor";
    std::error_code error;
    if (!std::filesystem::exists(file, error)) return;
    const auto text = read_text_file(file);
    auto state = text ? read_view_state(text.value()) : Result<EditorViewState>(text.error());
    if (state) view_state_ = std::move(state.value());
    else view_state_problem_ = file.generic_string() + " was ignored: " + state.error().message;
}
std::optional<std::pair<Vec3, Vec3>> EditorSession::local_bounds(EntityId id) const {
    const auto record = scene().inspect(id);
    if (!record || !record->components.mesh) return std::nullopt;
    const auto &mesh = *record->components.mesh;
    if (const auto data = model(mesh.mesh_asset)) {
        auto box = data->bounds;
        // A posed model outlines where its pose puts it.
        const renderer::Pose posed = shown_pose(id);
        if (posed && !data->skin.empty() && posed->size() == data->skin.joints.size())
            box = assets::skinned_bounds(data->skin, assets::skinning_palette(data->skin, *posed));
        const auto low = vector(box.min), high = vector(box.max);
        return std::pair<Vec3, Vec3>{{low.x, low.y, low.z}, {high.x, high.y, high.z}};
    }
    if (mesh.mesh == BuiltinMesh::Plane) return std::pair<Vec3, Vec3>{{-0.5, 0, -0.5}, {0.5, 0, 0.5}};
    return std::pair<Vec3, Vec3>{{-0.5, -0.5, -0.5}, {0.5, 0.5, 0.5}};
}
void EditorSession::focus() {
    if (const auto goal = focus_goal()) { camera.target = goal->target; camera.distance = goal->distance; }
}
Result<renderer::Frame> EditorSession::frame(double aspect) {
    sync_physics();
    const auto &shown = scene();
    // The selection as Flecs tags on what is shown; the outlines below are read back from them (slice 7).
    Scene &tagged = runtime_ ? runtime_->scene : scene_;
    apply_selection_tags(tagged, selection_);
    const auto entities = shown.snapshot().entities;
    std::vector<renderer::MeshBinding> meshes;
    std::vector<renderer::LightBinding> lights;
    asset_problems_.clear();
    const auto problem = [this](const std::string &text) {
        if (std::find(asset_problems_.begin(), asset_problems_.end(), text) == asset_problems_.end()) asset_problems_.push_back(text);
    };
    // Animators first, so the meshes below draw this frame's poses.
    auto &animated = animation();
    animated.set_animators(animator_inputs(entities, problem));
    // How a mesh renderer draws: its model, material and surface, under `key`.
    const auto look_of = [&](const MeshComponent &look, EntityId key) {
        const auto *m = &look;
        renderer::Surface surface{linear(m->color), m->roughness, m->metallic};
        surface.uv_scale_u = m->uv_scale_u; surface.uv_scale_v = m->uv_scale_v;
        auto handle = mesh_handle(m->mesh);
        // The built-in plane has always drawn from both sides; closed shapes cull their back faces.
        surface.double_sided = m->mesh_asset.empty() && m->mesh == BuiltinMesh::Plane;
        if (!m->mesh_asset.empty()) {
            const auto &resolved = resolve(m->mesh_asset, assets::AssetType::Mesh);
            handle = resolved.mesh.index ? resolved.mesh : renderer::builtin_cube;
            if (!resolved.mesh.index) { surface.linear_color = placeholder_color; problem(resolved.problem); }
        }
        if (!m->texture.empty()) {
            const auto &resolved = resolve(m->texture, assets::AssetType::Texture);
            surface.texture = resolved.texture;
            if (!resolved.texture.index) { surface.linear_color = placeholder_color; problem(resolved.problem); }
        }
        renderer::MaterialHandle material = renderer::builtin_lit;
        if (!m->material.empty()) {
            const auto &resolved = resolve(m->material, assets::AssetType::Material);
            if (resolved.material.index) material = resolved.material;
            else { surface.linear_color = placeholder_color; problem(resolved.problem); }
            if (!resolved.problem.empty() && resolved.material.index) problem(resolved.problem);
        }
        std::vector<renderer::PartMaterial> parts;
        for (const auto &slot : m->material_slots) {
            const auto &resolved = resolve(slot.material, assets::AssetType::Material);
            if (resolved.material.index) parts.push_back({slot.slot, resolved.material});
            if (!resolved.problem.empty()) problem(resolved.problem);
        }
        return renderer::MeshBinding{key, handle, material, true, surface, std::move(parts), nullptr, std::nullopt};
    };
    // Where things attached to a joint are drawn. Most scenes have none, and then nothing is worked out.
    std::map<EntityId, const EntityRecord *> by_id;
    bool attached = false;
    for (const auto &e : entities) { by_id.emplace(e.id, &e); attached = attached || e.components.attach.has_value(); }
    const auto placed_on_joint = [&](EntityId id) -> std::optional<WorldTransform> {
        if (!attached) return std::nullopt;
        const auto correction = attachment_correction(id, [&](EntityId at) -> std::optional<EntityRecord> {
            const auto found = by_id.find(at);
            return found == by_id.end() ? std::nullopt : std::optional<EntityRecord>(*found->second);
        });
        if (correction == animation::identity) return std::nullopt;
        const auto world = shown.world_transform(id);
        if (!world) return std::nullopt;
        return copy(glm::make_mat4(correction.data()) * glm::make_mat4(world->matrix.data()));
    };
    // The nearest tagged entity at or above a model decides its outline, so a selected group outlines its members.
    auto &tags = tagged.ecs().native();
    const auto outline_of = [&](EntityId id) {
        if (!view_flags.outlines || selection_.empty()) return renderer::Outline::None;
        for (int depth = 0; id && depth < 256; ++depth) {
            if (const auto native = shown.native(id)) {
                const flecs::entity e(tags, native);
                if (e.has<PrimarySelectedTag>()) return renderer::Outline::Primary;
                if (e.has<SelectedTag>()) return renderer::Outline::Selected;
            }
            const auto found = by_id.find(id);
            id = found == by_id.end() ? 0 : found->second->parent;
        }
        return renderer::Outline::None;
    };
    for (const auto &e : entities) {
        if (const auto &m = e.components.mesh) {
            auto binding = look_of(*m, e.id);
            binding.visible = runtime_ || !hidden(e.id);
            binding.pose = shown_pose(e.id);
            binding.world = placed_on_joint(e.id);
            binding.outline = outline_of(e.id);
            meshes.push_back(std::move(binding));
        }
        if (const auto &l = e.components.light; l && (runtime_ || !hidden(e.id))) {
            const auto kind = l->type == LightType::Point ? renderer::LightKind::Point : l->type == LightType::Spot ? renderer::LightKind::Spot : renderer::LightKind::Directional;
            lights.push_back({e.id, kind, linear(l->color), l->intensity, l->range, glm::radians(l->inner_cone_degrees), glm::radians(l->outer_cone_degrees), l->cast_shadows,
                              placed_on_joint(e.id)});
        }
    }
    // Debris looks like the entity it was made from, at that entity's scale, wherever its body is (D11).
    if (runtime_)
        for (const auto &piece : runtime_->physics->debris()) {
            const auto look = by_id.find(piece.look);
            if (look == by_id.end() || !look->second->components.mesh) continue;
            const auto world = shown.world_transform(piece.look);
            if (!world) continue;
            const glm::dmat4 m = glm::make_mat4(world->matrix.data());
            const glm::dvec3 scale(glm::length(glm::dvec3(m[0])), glm::length(glm::dvec3(m[1])), glm::length(glm::dvec3(m[2])));
            const auto &p = piece.pose;
            const glm::dmat4 at = glm::translate(glm::dmat4(1), glm::dvec3(p.position.x, p.position.y, p.position.z)) *
                                  glm::mat4_cast(glm::dquat(p.rotation.w, p.rotation.x, p.rotation.y, p.rotation.z)) * glm::scale(glm::dmat4(1), scale);
            auto binding = look_of(*look->second->components.mesh, renderer::runtime_object_base + piece.id);
            binding.world = copy(at);
            meshes.push_back(std::move(binding));
        }
    const EnvironmentComponent authored = environment();
    renderer::RenderEnvironment environment_settings;
    environment_settings.source = authored.source;
    environment_settings.sky_color = linear(authored.sky_color);
    environment_settings.horizon_color = linear(authored.horizon_color);
    environment_settings.ground_color = linear(authored.ground_color);
    environment_settings.intensity = authored.intensity;
    environment_settings.rotation_radians = glm::radians(authored.rotation_degrees);
    // A sky has no meaning in a parallel view, where every pixel would look the same way.
    const bool editor_view = !runtime_ || ejected_;
    environment_settings.show_skybox = authored.show_skybox && view_flags.skybox && !(camera.orthographic && editor_view);
    environment_settings.ev100 = authored.ev100;
    environment_settings.tone_mapping = authored.tone_mapping;
    environment_settings.shadow_quality = view_flags.shadows ? authored.shadow_quality : ShadowQuality::Off;
    environment_settings.shadow_distance = authored.shadow_distance;
    environment_settings.ambient_occlusion = authored.ambient_occlusion && view_flags.ambient_occlusion;
    environment_settings.bloom = authored.bloom && view_flags.bloom;
    if (authored.source == EnvironmentSource::Image) {
        const auto &resolved = resolve(authored.image, assets::AssetType::Environment);
        environment_settings.image = resolved.environment;
        // Graceful: without its image the scene is still lit, by the procedural sky.
        if (!resolved.environment.index) { environment_settings.source = EnvironmentSource::Procedural; problem(resolved.problem); }
    }
    if (++frames_since_trim_ >= 240) { frames_since_trim_ = 0; release_unused_assets(); }
    // Particles: the shown scene's emitters, then what its particle system holds, ready to draw.
    auto &particles = shown_particles();
    if (runtime_ || view_flags.effects) particles.set_emitters(emitter_inputs(entities, problem));
    else particles.set_emitters({});
    std::vector<renderer::RenderParticles> particle_batches;
    for (auto &batch : particles.batches()) {
        renderer::RenderParticles drawn;
        drawn.key = batch.key;
        drawn.blend = batch.effect->blend == assets::ParticleBlend::Alpha ? renderer::ParticleBlendMode::Alpha : renderer::ParticleBlendMode::Additive;
        drawn.strength = batch.effect->emissive_strength;
        drawn.stretch = batch.effect->stretch;
        if (const auto &image = batch.effect->texture; !image.empty()) {
            const auto &resolved = resolve(AssetRef{image.id.value, image.path}, assets::AssetType::Texture);
            drawn.texture = resolved.texture;
            if (!resolved.texture.index) problem(resolved.problem);
        }
        drawn.particles.reserve(batch.particles.size());
        for (const auto &p : batch.particles) drawn.particles.push_back({p.position, p.velocity, p.size, p.rotation, p.color});
        particle_batches.push_back(std::move(drawn));
    }

    // Each play session is a distinct scene identity, so the renderer never reuses editor objects for it.
    const renderer::FrameStamp stamp{scene_identity(), scene_revision(), ++frame_};
    view_camera_ = 0; view_camera_issue_.clear();
    if (runtime_ && !ejected_) {
        const auto primary = std::find_if(entities.begin(), entities.end(),
            [](const EntityRecord &e) { return e.components.camera && e.components.camera->primary; });
        if (primary != entities.end()) {
            const auto &c = *primary->components.camera;
            const renderer::PerspectiveCamera lens{primary->id, glm::radians(c.vertical_fov_degrees), aspect, c.near_plane, c.far_plane};
            auto through_camera = renderer::extract_frame(shown, stamp, lens, meshes, lights, environment_settings, !runtime_ && view_flags.grid, grid_spacing, particle_batches);
            if (through_camera) { view_camera_ = primary->id; return through_camera; }
            view_camera_issue_ = "Camera '" + primary->name + "' cannot be used: " + through_camera.error().message;
        }
    }
    renderer::PerspectiveCamera lens; lens.aspect_ratio = aspect;
    lens.vertical_fov_radians = glm::radians(camera.fov_degrees);
    auto eye = camera;
    if (camera.orthographic) {
        // Distance does not change what an orthographic view shows, so the eye stands far back and nothing is behind it.
        constexpr double stand_back = 1000;
        lens.orthographic_half_height = camera.half_height();
        eye.distance += stand_back;
        lens.far_plane = eye.distance + 2 * stand_back;
    }
    return renderer::extract_frame(shown, stamp, lens, eye.world(), meshes, lights, environment_settings, editor_view && view_flags.grid, grid_spacing,
                                   std::move(particle_batches));
}
effects::ParticleSystem &EditorSession::shown_particles() { return runtime_ ? runtime_->particles : edit_particles_; }
const effects::ParticleSystem &EditorSession::shown_particles() const { return runtime_ ? runtime_->particles : edit_particles_; }
std::optional<EditorSession::EffectSource> EditorSession::effect_source(const AssetRef &ref, std::string &problem) const {
    const auto reason = [&](const std::string &why) {
        problem = "Effect '" + ref.path + "' cannot be used: " + why;
        return std::nullopt;
    };
    if (!assets_) return reason("there is no asset folder at " + asset_folder().generic_string());
    auto meta = assets_->find(assets::AssetId{ref.id});
    if (!meta) meta = assets_->find(std::string_view(ref.path));
    if (!meta) return reason("it is not in the asset folder");
    if (meta->type != assets::AssetType::Effect) return reason("it is not a particle effect");
    auto loaded = assets_->load_effect(meta->id);
    if (!loaded) return reason(loaded.error().message);
    EffectSource source{meta->id, loaded.value().revision, loaded.value().data};
    // A draft keeps the saved revision, so emitters take its values without starting again.
    if (const auto draft = effect_drafts_.find(meta->id); draft != effect_drafts_.end())
        source.data = std::make_shared<const assets::EffectData>(draft->second);
    return source;
}
std::vector<effects::EmitterInput> EditorSession::emitter_inputs(const std::vector<EntityRecord> &entities,
                                                                  const std::function<void(const std::string &)> &problem) const {
    std::vector<effects::EmitterInput> inputs;
    const auto &shown = scene();
    for (const auto &e : entities) {
        const auto &emitter = e.components.emitter;
        if (!emitter || emitter->effect.empty() || (!runtime_ && hidden(e.id))) continue;
        std::string why;
        const auto source = effect_source(emitter->effect, why);
        if (!source) { problem(why); continue; }
        effects::EmitterInput in;
        in.entity = e.id;
        in.effect = source->data;
        in.asset = source->id.value;
        in.revision = source->revision;
        (void)shown;
        if (const auto world = drawn_world(e.id)) in.world = world->matrix;
        in.auto_play = emitter->auto_play;
        in.loop = emitter->loop;
        in.rate_scale = emitter->rate_scale;
        inputs.push_back(std::move(in));
    }
    return inputs;
}
void EditorSession::advance_effects(std::chrono::nanoseconds elapsed) {
    if (runtime_ || !view_flags.effects || elapsed.count() <= 0) return;
    edit_particles_.update(std::chrono::duration<double>(elapsed).count());
}
void EditorSession::restart_effects() { shown_particles().restart(); }
effects::Stats EditorSession::effect_stats() const { return shown_particles().stats(); }
std::uint64_t EditorSession::scene_identity() const { return runtime_ ? 1 + play_sessions_ : 1; }
std::uint64_t EditorSession::scene_revision() const { return runtime_ ? runtime_->bus.revision() : bus_.revision(); }
PlayState EditorSession::play_state() const {
    if (!runtime_) return PlayState::Editing;
    return runtime_->paused ? PlayState::Paused : PlayState::Playing;
}
Result<void> EditorSession::play() {
    if (in_transaction()) return fail(ErrorCode::InvalidState, "Finish the drag before Play");
    if (runtime_) {
        runtime_->paused = false;
        if (runtime_->game) runtime_->game->resume();
        return {};
    }
    auto runtime = std::make_unique<Runtime>();
    auto copied = runtime->bus.execute(context_, 0, commands::ReplaceScene{scene_.snapshot()});
    if (!copied) return copied.error();
    runtime->physics = make_physics(true);
    // The first sync builds the bodies, so the game's first tick can already query them.
    if (auto synced = runtime->physics->sync(runtime->scene); !synced) return synced.error();
    runtime->game = gameplay::Game::create(runtime->scene, runtime->bus, *runtime->physics, fixed_step);
    runtime_ = std::move(runtime);
    physics_error_.clear();
    ++play_sessions_;
    reconcile();
    return {};
}
Result<void> EditorSession::simulate() {
    if (runtime_) return fail(ErrorCode::InvalidState, "Stop first");
    if (auto started = play(); !started) return started;
    ejected_ = simulating_ = true;
    return {};
}
Result<void> EditorSession::set_ejected(bool ejected) {
    if (!runtime_) return fail(ErrorCode::InvalidState, "Nothing is running");
    ejected_ = ejected;
    if (!ejected) simulating_ = false; // Possessing turns a simulation into a run.
    return {};
}
Result<void> EditorSession::pause() {
    if (!runtime_) return fail(ErrorCode::InvalidState, "Pause requires Play mode");
    runtime_->paused = true;
    if (runtime_->game) runtime_->game->pause();
    return {};
}
Result<void> EditorSession::step() {
    if (!runtime_ || !runtime_->paused) return fail(ErrorCode::InvalidState, "Step requires paused Play mode");
    tick();
    runtime_->particles.update(std::chrono::duration<double>(fixed_step).count());
    if (!physics_error_.empty()) return fail(ErrorCode::InvalidState, physics_error_);
    return {};
}
Result<void> EditorSession::stop() {
    if (!runtime_) return fail(ErrorCode::InvalidState, "Not in Play mode");
    runtime_.reset();
    physics_error_.clear();
    transaction_depth_ = 0; drag_start_.clear(); // A runtime transaction has nothing to finish.
    ejected_ = simulating_ = false;
    reconcile();
    return {};
}
Result<void> EditorSession::restart() {
    if (!runtime_) return fail(ErrorCode::InvalidState, "Not in Play mode");
    runtime_.reset();
    physics_error_.clear();
    transaction_depth_ = 0; drag_start_.clear();
    ejected_ = simulating_ = false;
    return play();
}
gameplay::Game *EditorSession::game() const { return runtime_ ? runtime_->game.get() : nullptr; }
void EditorSession::update(std::chrono::nanoseconds elapsed) {
    advance_effects(elapsed);
    advance_animations(elapsed);
    if (!runtime_ || runtime_->paused || elapsed.count() <= 0) return;
    auto &r = *runtime_;
    const auto before_effects = std::chrono::steady_clock::now();
    r.particles.update(std::chrono::duration<double>(elapsed).count());
    const auto effects_took = std::chrono::steady_clock::now() - before_effects;
    ++timing_.effect_updates;
    timing_.effects += effects_took;
    timing_.effects_max = std::max<std::chrono::nanoseconds>(timing_.effects_max, effects_took);
    r.accumulated += elapsed;
    int ticks = 0;
    while (r.accumulated >= fixed_step && ticks < max_ticks_per_update && !r.paused) { r.accumulated -= fixed_step; tick(); ++ticks; }
    // A long stall (breakpoint, window drag) drops the backlog instead of catching up in a burst.
    if (ticks == max_ticks_per_update) r.accumulated = std::min(r.accumulated, fixed_step);
}
std::uint64_t EditorSession::runtime_ticks() const { return runtime_ ? runtime_->ticks : 0; }
// One fixed tick of the runtime scene: physics syncs, steps and publishes dynamic poses (C-3).
void EditorSession::tick() {
    auto &r = *runtime_;
    ++r.ticks;
    const auto start = std::chrono::steady_clock::now();
    if (r.game) r.game->pre_physics();
    const auto before_physics = std::chrono::steady_clock::now();
    if (auto report = r.physics->fixed_update(fixed_step, r.scene, r.bus, physics_context); !report) {
        physics_error_ = "Physics stopped: " + report.error().message;
        r.paused = true;
    }
    const auto after_physics = std::chrono::steady_clock::now();
    // Poses advance with the tick, so a run's animation is the same however the frames fall.
    r.animation.update(std::chrono::duration<double>(fixed_step).count());
    auto animation_took = std::chrono::steady_clock::now() - after_physics;
    auto heard = r.animation.take_events();
    if (r.game) {
        // The game hears clip events where the entity's foot is: its lowest joint. A ragdoll's animator is not its pose.
        std::vector<gameplay::AnimationNotice> notices;
        for (const auto &event : heard)
            if (!r.ragdolls.contains(event.entity)) notices.push_back({event.entity, event.name, lowest_joint(event.entity)});
        r.game->hear(notices);
    }
    keep_events(std::move(heard));
    const auto before_ragdolls = std::chrono::steady_clock::now();
    pose_ragdolls();
    animation_took += std::chrono::steady_clock::now() - before_ragdolls;
    if (r.game) {
        r.game->post_physics();
        // What the game asked of the animations and ragdolls.
        for (const auto &request : r.game->animation_requests()) play_cue(request.entity, request.cue);
        for (const auto &request : r.game->ragdoll_requests()) {
            const auto push = request.pushed ? std::optional<std::pair<Vec3, Vec3>>(std::pair<Vec3, Vec3>{
                                                   {request.impulse.x, request.impulse.y, request.impulse.z}, {request.point.x, request.point.y, request.point.z}})
                                             : std::nullopt;
            if (auto fell = start_ragdoll(request.entity, push); !fell) r.game_issues.push_back(fell.error().message);
        }
        // One-shot effects the game asked for, from the emitter on their source entity. Visual only.
        for (const auto &event : r.game->effect_events()) {
            const auto source = r.scene.inspect(event.source);
            if (!source || !source->components.emitter) continue;
            std::string why;
            if (const auto effect = effect_source(source->components.emitter->effect, why))
                r.particles.fire(event.source, effect->data, {event.point.x, event.point.y, event.point.z},
                                 {event.direction.x, event.direction.y, event.direction.z}, event.strength * source->components.emitter->rate_scale);
        }
    }
    const auto end = std::chrono::steady_clock::now();
    ++timing_.ticks;
    timing_.physics += after_physics - before_physics;
    timing_.physics_max = std::max<std::chrono::nanoseconds>(timing_.physics_max, after_physics - before_physics);
    timing_.gameplay += (before_physics - start) + (end - after_physics) - animation_took;
    timing_.animation += animation_took;
    timing_.animation_max = std::max<std::chrono::nanoseconds>(timing_.animation_max, animation_took);
    reconcile();
}
void EditorSession::sync_physics() {
    if (!runtime_) (void)edit_physics_->sync(scene_); // Only fails off the owner thread.
}
const physics::ScenePhysics &EditorSession::physics_bridge() const { return runtime_ ? *runtime_->physics : *edit_physics_; }
EntityId EditorSession::pick(const renderer::FrameSnapshot &frame, double x, double y) const {
    const auto inv = glm::inverse(glm::make_mat4(frame.camera.projection.matrix.data()) * glm::make_mat4(frame.camera.view.matrix.data()));
    auto a = inv * glm::dvec4(x, y, -1, 1), b = inv * glm::dvec4(x, y, 1, 1);
    a /= a.w; b /= b.w;
    const auto segment = glm::dvec3(b - a);
    const double length = glm::length(segment);
    const auto direction = segment / length;
    // A locked entity cannot be picked, and what is behind it can.
    return cast_ray(frame, {a.x, a.y, a.z}, {direction.x, direction.y, direction.z}, length, runtime_ ? std::set<EntityId>{} : view_state_.locked).first;
}
const EditorSession::Resolved &EditorSession::resolve(const AssetRef &ref, assets::AssetType type) const {
    const auto key = std::make_tuple(ref.id, ref.path, type);
    if (const auto found = resolved_.find(key); found != resolved_.end()) return found->second;
    Resolved result;
    const auto reason = [&]() -> std::string {
        if (!assets_) return "there is no asset folder at " + asset_folder().generic_string();
        auto meta = assets_->find(assets::AssetId{ref.id});
        if (!meta) meta = assets_->find(std::string_view(ref.path));
        // An ID with nothing behind it and a path that has moved: the redirector left there knows where it went. A path
        // written as a package name ("/Game/...") is resolved the same way.
        if (!meta) {
            const std::string name = ref.path.starts_with('/') ? ref.path : std::string(game_mount) + "/" + ref.path;
            if (const auto found = resolve_package(name)) meta = assets_->find(found.value().id);
        }
        if (!meta) return "it is not in the asset folder";
        if (type == assets::AssetType::Mesh) {
            auto loaded = assets_->load_mesh(meta->id);
            if (!loaded) return loaded.error().message;
            result.mesh = resources_.mesh(meta->id, loaded.value().revision, loaded.value().data);
        } else if (type == assets::AssetType::Environment) {
            auto loaded = assets_->load_environment(meta->id);
            if (!loaded) return loaded.error().message;
            result.environment = resources_.environment(meta->id, loaded.value().revision, loaded.value().data);
        } else if (type == assets::AssetType::Material) {
            const auto data = material(meta->id);
            if (!data) {
                auto loaded = assets_->load_material(meta->id);
                return loaded ? std::string("it could not be read") : loaded.error().message;
            }
            renderer::MaterialDesc desc;
            desc.base_color = linear(Vec3{data->base_color[0], data->base_color[1], data->base_color[2]});
            desc.alpha = data->base_color[3]; desc.roughness = data->roughness; desc.metallic = data->metallic;
            const Vec3 emissive = linear(Vec3{data->emissive[0], data->emissive[1], data->emissive[2]});
            desc.emissive = {emissive.x * data->emissive_strength, emissive.y * data->emissive_strength, emissive.z * data->emissive_strength};
            desc.normal_scale = data->normal_scale; desc.occlusion_strength = data->occlusion_strength;
            desc.alpha_cutoff = data->alpha_cutoff; desc.double_sided = data->double_sided;
            desc.alpha_mode = data->alpha_mode == assets::AlphaMode::Mask ? renderer::AlphaMode::Mask : data->alpha_mode == assets::AlphaMode::Blend ? renderer::AlphaMode::Blend : renderer::AlphaMode::Opaque;
            desc.uv_scale_u = data->uv_scale[0]; desc.uv_scale_v = data->uv_scale[1];
            // A missing map is reported and the material still draws, with that slot's factor alone.
            std::string missing;
            const auto map = [&](const assets::MaterialTexture &texture, renderer::TextureHandle &out) {
                if (texture.empty()) return;
                const auto &found = resolve(AssetRef{texture.id.value, texture.path}, assets::AssetType::Texture);
                out = found.texture;
                if (!found.texture.index) missing += (missing.empty() ? "" : "; ") + found.problem;
            };
            map(data->base_color_map, desc.base_color_map);
            map(data->metallic_roughness_map, desc.metallic_roughness_map);
            map(data->normal_map, desc.normal_map);
            map(data->occlusion_map, desc.occlusion_map);
            map(data->emissive_map, desc.emissive_map);
            result.material = resources_.material(meta->id, desc);
            return missing;
        } else {
            auto loaded = assets_->load_texture(meta->id);
            if (!loaded) return loaded.error().message;
            result.texture = resources_.texture(meta->id, loaded.value().revision, loaded.value().data);
        }
        return {};
    }();
    const char *kind = type == assets::AssetType::Mesh ? "Model '" : type == assets::AssetType::Material ? "Material '" : type == assets::AssetType::Environment ? "Environment '" : "Texture '";
    if (!reason.empty()) result.problem = kind + ref.path + "' cannot be used: " + reason;
    return resolved_.emplace(key, std::move(result)).first->second;
}
void EditorSession::forget_materials() {
    for (auto it = resolved_.begin(); it != resolved_.end();)
        if (std::get<2>(it->first) == assets::AssetType::Material) it = resolved_.erase(it); else ++it;
}
Result<ImportedAsset> EditorSession::finish_import(const assets::PreparedImport &prepared) {
    if (project_.empty()) return fail(ErrorCode::InvalidState, "No project is open");
    if (!assets_) {
        std::error_code ec;
        std::filesystem::create_directories(asset_folder(), ec);
        if (ec) return fail(ErrorCode::InvalidState, "Cannot create the asset folder: " + ec.message());
        auto database = assets::AssetDatabase::open(asset_folder());
        if (!database) return database.error();
        assets_.emplace(std::move(database.value()));
    }
    auto id = assets_->commit_import(prepared);
    if (!id) return id.error();
    resolved_.clear(); // A reference that was missing may exist now.
    const auto meta = assets_->find(id.value());
    ImportedAsset imported{id.value(), meta->path, meta->type, {}};
    if (prepared.mesh) imported.warnings = prepared.mesh->warnings;
    return imported;
}
std::uint64_t EditorSession::begin_import(const std::filesystem::path &file) {
    // std::async: the worker touches only its own copy of the path; the catalogue is left to poll_imports().
    imports_.push_back(std::async(std::launch::async, [file] { return assets::prepare_import(file); }).share());
    import_sources_.push_back(file);
    import_ids_.push_back(next_import_id_);
    return next_import_id_++;
}
bool EditorSession::cancel_import(std::uint64_t id) {
    if (std::ranges::find(import_ids_, id) == import_ids_.end()) return false;
    cancelled_imports_.insert(id);
    return true;
}
std::vector<FinishedImport> EditorSession::poll_imports() {
    std::vector<FinishedImport> finished;
    for (std::size_t i = 0; i < imports_.size();) {
        if (imports_[i].wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++i; continue; }
        const auto &decoded = imports_[i].get();
        const auto id = import_ids_[i];
        if (cancelled_imports_.erase(id)) finished.push_back({import_sources_[i], fail(ErrorCode::InvalidState, "Cancelled"), id, true});
        else if (decoded) finished.push_back({import_sources_[i], finish_import(decoded.value()), id});
        else finished.push_back({import_sources_[i], decoded.error(), id});
        imports_.erase(imports_.begin() + std::ptrdiff_t(i));
        import_sources_.erase(import_sources_.begin() + std::ptrdiff_t(i));
        import_ids_.erase(import_ids_.begin() + std::ptrdiff_t(i));
    }
    return finished;
}
std::vector<FinishedImport> EditorSession::wait_for_imports() {
    for (const auto &import : imports_) import.wait();
    return poll_imports();
}
std::set<assets::AssetId> EditorSession::referenced_assets() {
    std::set<assets::AssetId> used;
    const auto add = [&](const AssetRef &ref) {
        if (ref.empty()) return;
        if (!assets_) return;
        auto meta = assets_->find(assets::AssetId{ref.id});
        if (!meta) meta = assets_->find(std::string_view(ref.path));
        if (!meta) return;
        used.insert(meta->id);
        if (meta->type == assets::AssetType::Effect) {
            if (const auto data = effect(meta->id); data && !data->texture.empty()) {
                auto image = assets_->find(data->texture.id);
                if (!image) image = assets_->find(std::string_view(data->texture.path));
                if (image) used.insert(image->id);
            }
            return;
        }
        if (meta->type != assets::AssetType::Material) return;
        if (const auto data = material(meta->id))
            for (const auto *texture : {&data->base_color_map, &data->metallic_roughness_map, &data->normal_map, &data->occlusion_map, &data->emissive_map})
                if (!texture->empty()) {
                    auto map = assets_->find(texture->id);
                    if (!map) map = assets_->find(std::string_view(texture->path));
                    if (map) used.insert(map->id);
                }
    };
    const auto scan = [&](const Scene &scene) {
        for (const auto &e : scene.snapshot().entities) {
            if (const auto &m = e.components.mesh) {
                add(m->mesh_asset); add(m->texture); add(m->material);
                for (const auto &slot : m->material_slots) add(slot.material);
            }
            if (const auto &v = e.components.environment) add(v->image);
            if (const auto &p = e.components.emitter) add(p->effect);
            if (const auto &a = e.components.animator) add(a->clips);
        }
    };
    scan(scene_);
    if (runtime_) scan(runtime_->scene);
    for (const auto &[id, draft] : material_drafts_) { (void)draft; used.insert(id); }
    for (const auto &[id, draft] : effect_drafts_) { (void)draft; used.insert(id); }
    return used;
}
std::size_t EditorSession::release_unused_assets() {
    if (!assets_) return 0;
    const auto used = referenced_assets();
    resources_.release_except(used);
    // Cached references to released assets would otherwise keep answering with handles that find nothing.
    for (auto it = resolved_.begin(); it != resolved_.end();) {
        const auto type = std::get<2>(it->first);
        auto meta = assets_->find(assets::AssetId{std::get<0>(it->first)});
        if (!meta) meta = assets_->find(std::string_view(std::get<1>(it->first)));
        const bool keep = type == assets::AssetType::Material || (meta && used.contains(meta->id));
        it = keep ? std::next(it) : resolved_.erase(it);
    }
    const auto freed = assets_->unload_unused();
    // Skeletons and clips of models whose data is now gone.
    std::erase_if(clip_sets_, [](const auto &entry) { return entry.second.model.expired() || entry.second.source.expired(); });
    std::erase_if(rigs_, [](const auto &entry) { return entry.second.model.expired(); });
    std::erase_if(flesh_, [](const auto &entry) { return entry.second.model.expired(); });
    assets_released_ += freed;
    return freed;
}
Result<assets::AssetId> EditorSession::create_material(const std::string &name, const assets::MaterialData &data) {
    if (project_.empty()) return fail(ErrorCode::InvalidState, "No project is open");
    if (!assets_) {
        std::error_code ec;
        std::filesystem::create_directories(asset_folder(), ec);
        if (ec) return fail(ErrorCode::InvalidState, "Cannot create the asset folder: " + ec.message());
        auto database = assets::AssetDatabase::open(asset_folder());
        if (!database) return database.error();
        assets_.emplace(std::move(database.value()));
    }
    auto id = assets_->create_material("materials/" + name + ".material", data);
    if (id) resolved_.clear();
    return id;
}
Result<assets::AssetId> EditorSession::duplicate_material(assets::AssetId id) {
    const auto data = material(id);
    const auto meta = assets_ ? assets_->find(id) : std::nullopt;
    if (!data || !meta) return fail(ErrorCode::NotFound, "Material is not in the asset folder");
    const auto stem = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(meta->path.data()), meta->path.size())).stem().u8string();
    return create_material(std::string(reinterpret_cast<const char *>(stem.data()), stem.size()) + " copy", *data);
}
std::optional<assets::MaterialData> EditorSession::material(assets::AssetId id) const {
    if (const auto draft = material_drafts_.find(id); draft != material_drafts_.end()) return draft->second;
    if (!assets_) return std::nullopt;
    auto loaded = assets_->load_material(id);
    if (!loaded) return std::nullopt;
    return *loaded.value().data;
}
Result<void> EditorSession::preview_material(assets::AssetId id, const assets::MaterialData &data) {
    if (auto valid = assets::validate_material(data); !valid) return valid;
    if (!assets_ || !assets_->find(id)) return fail(ErrorCode::NotFound, "Material is not in the asset folder");
    material_drafts_[id] = data;
    forget_materials();
    return {};
}
Result<void> EditorSession::save_material(assets::AssetId id, const assets::MaterialData &data) {
    if (!assets_) return fail(ErrorCode::NotFound, "Material is not in the asset folder");
    auto saved_value = assets_->load_material(id);
    if (!saved_value) return saved_value.error();
    const assets::MaterialData before = *saved_value.value().data;
    if (auto saved = assets_->save_material(id, data); !saved) return saved;
    material_drafts_.erase(id);
    forget_materials();
    if (before != data) {
        const auto meta = assets_->find(id);
        const auto stem = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(meta->path.data()), meta->path.size())).stem().u8string();
        undo_steps_.push_back({"Edit Material " + std::string(reinterpret_cast<const char *>(stem.data()), stem.size()), MaterialEdit{id, before, data}, std::nullopt, false});
        redo_steps_.clear();
        bus_.discard_redo();
        if (std::ranges::count_if(undo_steps_, [](const HistoryStep &step) { return step.material.has_value(); }) > 128)
            undo_steps_.erase(std::ranges::find_if(undo_steps_, [](const HistoryStep &step) { return step.material.has_value(); }));
    }
    return {};
}
Result<assets::AssetId> EditorSession::create_effect(const std::string &name, const assets::EffectData &data) {
    if (project_.empty()) return fail(ErrorCode::InvalidState, "No project is open");
    if (!assets_) {
        std::error_code ec;
        std::filesystem::create_directories(asset_folder(), ec);
        if (ec) return fail(ErrorCode::InvalidState, "Cannot create the asset folder: " + ec.message());
        auto database = assets::AssetDatabase::open(asset_folder());
        if (!database) return database.error();
        assets_.emplace(std::move(database.value()));
    }
    auto id = assets_->create_effect("effects/" + name + ".effect", data);
    if (id) resolved_.clear();
    return id;
}
std::optional<assets::EffectData> EditorSession::effect(assets::AssetId id) const {
    if (const auto draft = effect_drafts_.find(id); draft != effect_drafts_.end()) return draft->second;
    if (!assets_) return std::nullopt;
    auto loaded = assets_->load_effect(id);
    if (!loaded) return std::nullopt;
    return *loaded.value().data;
}
Result<void> EditorSession::preview_effect(assets::AssetId id, const assets::EffectData &data) {
    if (auto valid = assets::validate_effect(data); !valid) return valid;
    const auto meta = assets_ ? assets_->find(id) : std::nullopt;
    if (!meta || meta->type != assets::AssetType::Effect) return fail(ErrorCode::NotFound, "Effect is not in the asset folder");
    effect_drafts_[id] = data;
    return {};
}
Result<void> EditorSession::save_effect(assets::AssetId id, const assets::EffectData &data) {
    if (!assets_) return fail(ErrorCode::NotFound, "Effect is not in the asset folder");
    auto saved_value = assets_->load_effect(id);
    if (!saved_value) return saved_value.error();
    const assets::EffectData before = *saved_value.value().data;
    if (auto saved = assets_->save_effect(id, data); !saved) return saved;
    effect_drafts_.erase(id);
    if (before != data) {
        const auto meta = assets_->find(id);
        const auto stem = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(meta->path.data()), meta->path.size())).stem().u8string();
        HistoryStep step{"Edit Effect " + std::string(reinterpret_cast<const char *>(stem.data()), stem.size()), std::nullopt, std::nullopt, false, EffectEdit{id, before, data}};
        undo_steps_.push_back(std::move(step));
        redo_steps_.clear();
        bus_.discard_redo();
        if (std::ranges::count_if(undo_steps_, [](const HistoryStep &s) { return s.effect.has_value(); }) > 128)
            undo_steps_.erase(std::ranges::find_if(undo_steps_, [](const HistoryStep &s) { return s.effect.has_value(); }));
    }
    return {};
}
void EditorSession::discard_effect_draft(assets::AssetId id) { effect_drafts_.erase(id); }
Result<commands::Receipt> EditorSession::assign_effect(EntityId id, assets::AssetId asset) {
    const auto record = scene().inspect(id);
    if (!record) return fail(ErrorCode::NotFound, "Entity no longer exists");
    const auto meta = assets_ ? assets_->find(asset) : std::nullopt;
    if (!meta) return fail(ErrorCode::NotFound, "Effect is not in the asset folder");
    if (meta->type != assets::AssetType::Effect) return fail(ErrorCode::InvalidArgument, meta->path + " is not a particle effect");
    auto emitter = record->components.emitter.value_or(ParticleEmitterComponent{});
    emitter.effect = reference(*meta);
    return execute(commands::SetParticleEmitter{id, emitter}, "Assign Effect");
}
void EditorSession::discard_material_draft(assets::AssetId id) {
    if (material_drafts_.erase(id)) forget_materials();
}
Result<commands::Receipt> EditorSession::assign_material(EntityId id, assets::AssetId asset, std::optional<std::uint32_t> slot) {
    const auto record = scene().inspect(id);
    if (!record) return fail(ErrorCode::NotFound, "Entity no longer exists");
    AssetRef ref;
    if (!asset.is_null()) {
        const auto meta = assets_ ? assets_->find(asset) : std::nullopt;
        if (!meta) return fail(ErrorCode::NotFound, "Material is not in the asset folder");
        if (meta->type != assets::AssetType::Material) return fail(ErrorCode::InvalidArgument, meta->path + " is not a material");
        ref = reference(*meta);
    }
    auto mesh = record->components.mesh.value_or(MeshComponent{});
    if (!slot) {
        mesh.material = ref;
        mesh.color = {1, 1, 1};
    } else {
        auto &slots = mesh.material_slots;
        const auto at = std::lower_bound(slots.begin(), slots.end(), *slot, [](const MaterialSlot &item, std::uint32_t value) { return item.slot < value; });
        const bool exists = at != slots.end() && at->slot == *slot;
        if (ref.empty()) { if (exists) slots.erase(at); }
        else if (exists) at->material = ref;
        else slots.insert(at, MaterialSlot{*slot, ref});
    }
    return execute(commands::SetMesh{id, mesh}, "Assign Material");
}
renderer::MaterialHandle EditorSession::material_handle(assets::AssetId id) {
    const auto meta = assets_ ? assets_->find(id) : std::nullopt;
    if (!meta || meta->type != assets::AssetType::Material) return {};
    return resolve(reference(*meta), assets::AssetType::Material).material;
}
std::size_t EditorSession::mesh_part_count(const MeshComponent &mesh) {
    const auto data = model(mesh.mesh_asset);
    return data && !data->submeshes.empty() ? data->submeshes.size() : 1;
}
EnvironmentComponent EditorSession::environment() const {
    for (const auto &e : scene().snapshot().entities) if (e.components.environment) return *e.components.environment;
    return {};
}
EntityId EditorSession::environment_entity() const {
    for (const auto &e : scene().snapshot().entities) if (e.components.environment) return e.id;
    return 0;
}
Result<commands::Receipt> EditorSession::set_environment(const EnvironmentComponent &environment) {
    if (const auto id = environment_entity()) return execute(commands::SetEnvironment{id, environment});
    Components components;
    components.environment = environment;
    return execute(commands::CreateEntity{"Environment", Transform{}, 0, 0, std::move(components)}, "Add Environment");
}
EnvironmentComponent EditorSession::outdoor_environment() { return {}; }
EnvironmentComponent EditorSession::indoor_environment() {
    EnvironmentComponent e;
    e.sky_color = {0.55, 0.53, 0.5};
    e.horizon_color = {0.45, 0.43, 0.4};
    e.ground_color = {0.2, 0.18, 0.16};
    e.intensity = 90;
    e.show_skybox = false;
    e.ev100 = 6;
    e.shadow_distance = 30;
    return e;
}
double EditorSession::point_light_lumens(double ev100) { return 3000 * std::pow(2.0, ev100 - 6); }
Result<commands::Receipt> EditorSession::create_preset_lights(bool outdoor) {
    Components components;
    LightComponent light;
    Transform t;
    if (outdoor) {
        light.type = LightType::Directional;
        light.intensity = 90000;
        light.color = {1, 0.96, 0.88};
        light.cast_shadows = true;
        const glm::dquat q(glm::radians(glm::dvec3(-52, 35, 0)));
        t.rotation = {q.x, q.y, q.z, q.w};
        t.position = {0, 8, 0};
    } else {
        light.type = LightType::Spot;
        light.intensity = point_light_lumens(6);
        light.range = 15;
        light.cast_shadows = true;
        const glm::dquat q(glm::radians(glm::dvec3(-90, 0, 0)));
        t.rotation = {q.x, q.y, q.z, q.w};
        t.position = {0, 4, 0};
    }
    components.light = light;
    return execute(commands::CreateEntity{outdoor ? "Sun" : "Key Light", t, 0, 0, std::move(components)}, outdoor ? "Add Sun" : "Add Key Light");
}
Result<void> EditorSession::open_project(const std::filesystem::path &root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return fail(ErrorCode::NotFound, "Project folder not found: " + root.generic_string());
    project_ = root;
    assets_.reset(); resolved_.clear();
    // Mounting replaces the old project's index; the next packages() indexes this one's catalogue.
    if (auto mounted = packages_.mount(game_mount, asset_folder()); !mounted) return mounted;
    packages_scans_.reset();
    poses_.clear(); // Joint orders belong to the old project's models.
    // Material and effect edits belong to the old project's files.
    material_drafts_.clear();
    effect_drafts_.clear();
    std::erase_if(undo_steps_, [](const HistoryStep &step) { return step.material.has_value() || step.effect.has_value() || step.events.has_value(); });
    std::erase_if(redo_steps_, [](const HistoryStep &step) { return step.material.has_value() || step.effect.has_value() || step.events.has_value(); });
    edit_particles_ = effects::ParticleSystem(1);
    edit_animation_ = animation::AnimationSystem();
    rigs_.clear(); clip_sets_.clear(); flesh_.clear(); recent_events_.clear();
    if (!std::filesystem::is_directory(asset_folder(), ec)) return {};
    auto database = assets::AssetDatabase::open(asset_folder());
    if (!database) return database.error();
    if (auto scanned = database.value().scan(); !scanned) return scanned.error();
    assets_.emplace(std::move(database.value()));
    return {};
}
std::filesystem::path EditorSession::project_of(const std::filesystem::path &scene_file) {
    std::error_code ec;
    auto folder = std::filesystem::absolute(scene_file, ec).lexically_normal().parent_path();
    if (ec) return {};
    while (!folder.empty()) {
        if (std::filesystem::is_directory(folder / "Assets", ec)) return folder;
        auto up = folder.parent_path();
        if (up == folder) break; // The root.
        folder = std::move(up);
    }
    return {};
}
Result<ImportedAsset> EditorSession::import_asset(const std::filesystem::path &file) {
    if (project_.empty()) return fail(ErrorCode::InvalidState, "No project is open");
    if (!assets_) {
        std::error_code ec;
        std::filesystem::create_directories(asset_folder(), ec);
        if (ec) return fail(ErrorCode::InvalidState, "Cannot create the asset folder: " + ec.message());
        auto database = assets::AssetDatabase::open(asset_folder());
        if (!database) return database.error();
        assets_.emplace(std::move(database.value()));
    }
    auto id = assets_->import_file(file);
    if (!id) return id.error();
    resolved_.clear(); // A reference that was missing may exist now.
    const auto meta = assets_->find(id.value());
    ImportedAsset imported{id.value(), meta->path, meta->type, {}};
    if (meta->type == assets::AssetType::Mesh) {
        if (auto mesh = assets_->load_mesh(id.value())) imported.warnings = mesh.value().data->warnings;
    }
    return imported;
}
Result<void> EditorSession::rescan_assets() {
    if (project_.empty()) return fail(ErrorCode::InvalidState, "No project is open");
    if (!assets_) return open_project(project_);
    if (auto scanned = assets_->scan(); !scanned) return scanned.error();
    resolved_.clear();
    return {};
}
Result<void> EditorSession::reload_asset(assets::AssetId id) {
    if (!assets_) return fail(ErrorCode::NotFound, "No asset folder");
    auto reloaded = assets_->reload(id);
    resolved_.clear();
    return reloaded;
}
std::vector<assets::AssetMetadata> EditorSession::assets() const { return assets_ ? assets_->assets() : std::vector<assets::AssetMetadata>{}; }
std::vector<assets::AssetIssue> EditorSession::asset_issues() const {
    if (!assets_) return {};
    const auto issues = assets_->issues();
    return {issues.begin(), issues.end()};
}
assets::AssetState EditorSession::asset_state(assets::AssetId id) const { return assets_ ? assets_->state(id) : assets::AssetState::Unknown; }
Result<commands::Receipt> EditorSession::assign_asset(EntityId id, assets::AssetId asset) {
    const auto record = scene().inspect(id);
    if (!record) return fail(ErrorCode::NotFound, "Entity no longer exists");
    const auto meta = assets_ ? assets_->find(asset) : std::nullopt;
    if (!meta) return fail(ErrorCode::NotFound, "Asset is not in the asset folder");
    auto mesh = record->components.mesh.value_or(MeshComponent{});
    mesh.color = {1, 1, 1};
    (meta->type == assets::AssetType::Mesh ? mesh.mesh_asset : mesh.texture) = reference(*meta);
    return execute(commands::SetMesh{id, mesh}, "Assign " + std::string(assets::to_string(meta->type)));
}
Result<commands::Receipt> EditorSession::place_asset(assets::AssetId asset) { return place_asset_at(asset, {}); }
Result<commands::Receipt> EditorSession::place_asset_at(assets::AssetId asset, const Vec3 &point) {
    const auto meta = assets_ ? assets_->find(asset) : std::nullopt;
    if (!meta) return fail(ErrorCode::NotFound, "Asset is not in the asset folder");
    const auto stem_of = [&] {
        const auto stem = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(meta->path.data()), meta->path.size())).stem().u8string();
        return trimmed_name(std::string(reinterpret_cast<const char *>(stem.data()), stem.size()));
    };
    if (meta->type == assets::AssetType::Effect) {
        // An emitter a little above the point, so a fountain is not born inside the floor.
        Components components;
        components.emitter = ParticleEmitterComponent{reference(*meta)};
        Transform t; t.position = {point.x, point.y + 0.05, point.z};
        auto name = stem_of();
        const auto label = "Place " + name;
        auto result = execute(commands::CreateEntity{std::move(name), t, 0, 0, std::move(components)}, label);
        if (result) selection_.set(result.value().created.front());
        return result;
    }
    if (meta->type != assets::AssetType::Mesh) return fail(ErrorCode::InvalidArgument, meta->path + " is not a model or an effect; assign it to an entity instead");
    const AssetRef ref = reference(*meta);
    const auto data = model(ref);
    if (!data) return fail(ErrorCode::ParseError, resolve(ref, assets::AssetType::Mesh).problem);
    MeshComponent mesh;
    mesh.color = {1, 1, 1};
    mesh.mesh_asset = ref;
    Transform t; t.position = {point.x, point.y - double(data->bounds.min[1]), point.z};
    const auto stem = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(meta->path.data()), meta->path.size())).stem().u8string();
    auto name = trimmed_name(std::string(reinterpret_cast<const char *>(stem.data()), stem.size()));
    const auto label = "Place " + name;
    Components components{mesh, {}, {}};
    // A model with animations plays its first one, as it would be expected to.
    if (!data->skin.empty() && !data->clips.empty()) components.animator = AnimatorComponent{{}, data->clips.front().name};
    auto result = execute(commands::CreateEntity{std::move(name), t, 0, 0, std::move(components)}, label);
    if (result) selection_.set(result.value().created.front());
    return result;
}
void EditorSession::set_pose(EntityId id, renderer::Pose pose) {
    if (pose) poses_[id] = std::move(pose);
    else poses_.erase(id);
}
renderer::Pose EditorSession::pose(EntityId id) const {
    const auto found = poses_.find(id);
    return found == poses_.end() ? nullptr : found->second;
}
std::shared_ptr<const assets::MeshData> EditorSession::model(const AssetRef &ref) const {
    return ref.empty() ? nullptr : resources_.find(resolve(ref, assets::AssetType::Mesh).mesh);
}
std::optional<ColliderComponent> EditorSession::model_collider(const AssetRef &ref) {
    const auto data = model(ref);
    if (!data) return std::nullopt;
    const auto low = vector(data->bounds.min), high = vector(data->bounds.max);
    const auto center = (low + high) / 2.0, half = glm::max((high - low) / 2.0, glm::dvec3(0.001)); // A flat model still gets a volume.
    ColliderComponent collider;
    collider.center = {center.x, center.y, center.z};
    collider.half_extents = {half.x, half.y, half.z};
    return collider;
}
// ---- Animation ----
animation::AnimationSystem &EditorSession::animation() { return runtime_ ? runtime_->animation : edit_animation_; }
const animation::AnimationSystem &EditorSession::animation() const { return runtime_ ? runtime_->animation : edit_animation_; }
std::optional<assets::AssetMetadata> EditorSession::asset_meta(const AssetRef &ref) const {
    if (!assets_ || ref.empty()) return std::nullopt;
    auto meta = assets_->find(assets::AssetId{ref.id});
    if (!meta) meta = assets_->find(std::string_view(ref.path));
    return meta;
}
std::shared_ptr<const animation::Rig> EditorSession::rig_for(const std::shared_ptr<const assets::MeshData> &data, std::string &problem) const {
    problem.clear();
    if (!data || data->skin.empty()) { problem = "has no skeleton"; return nullptr; }
    auto found = rigs_.find(data.get());
    // Freed data's address can come back for other data.
    if (found != rigs_.end() && found->second.model.lock() != data) { rigs_.erase(found); found = rigs_.end(); }
    if (found == rigs_.end()) {
        RigEntry entry{data, nullptr, {}};
        if (auto built = animation::Rig::build(data->skin)) entry.rig = built.value();
        else entry.problem = built.error().message;
        found = rigs_.emplace(data.get(), std::move(entry)).first;
    }
    problem = found->second.problem;
    return found->second.rig;
}
std::shared_ptr<const animation::ClipSet> EditorSession::clips_for(const EntityRecord &e, std::string &problem) const {
    problem.clear();
    const auto &a = e.components.animator;
    const auto &m = e.components.mesh;
    if (!a) return nullptr;
    const auto say = [&](const std::string &why) -> std::shared_ptr<const animation::ClipSet> {
        problem = "Animator on '" + e.name + "' cannot play: " + why;
        return nullptr;
    };
    if (!m || m->mesh_asset.empty()) return say("its entity draws no model");
    const auto data = model(m->mesh_asset);
    if (!data) return nullptr; // The mesh reports its own problem.
    std::string why;
    const auto skeleton = rig_for(data, why);
    if (!skeleton) return say("model '" + m->mesh_asset.path + "' " + why);
    auto source = data;
    if (!a->clips.empty()) {
        source = model(a->clips);
        if (!source) return say("its clips come from '" + a->clips.path + "', which cannot be loaded");
    }
    const auto key = std::make_pair(data.get(), source.get());
    auto found = clip_sets_.find(key);
    if (found != clip_sets_.end() && (found->second.model.lock() != data || found->second.source.lock() != source)) { clip_sets_.erase(found); found = clip_sets_.end(); }
    if (found == clip_sets_.end()) {
        ClipEntry entry{data, source, nullptr, {}};
        if (auto built = animation::ClipSet::build(skeleton, source->skin, source->clips)) entry.clips = built.value();
        else entry.problem = built.error().message;
        found = clip_sets_.emplace(key, std::move(entry)).first;
    }
    if (!found->second.clips) return say(found->second.problem);
    // An unknown clip rests, and gameplay can still play the others.
    if (!a->clip.empty() && !found->second.clips->find(a->clip)) (void)say("there is no clip '" + a->clip + "'");
    return found->second.clips;
}
std::vector<animation::AnimatorInput> EditorSession::animator_inputs(const std::vector<EntityRecord> &entities,
                                                                     const std::function<void(const std::string &)> &problem) const {
    std::vector<animation::AnimatorInput> inputs;
    for (const auto &e : entities) {
        const auto &a = e.components.animator;
        if (!a) continue;
        std::string why;
        auto clips = clips_for(e, why);
        if (!why.empty()) problem(why);
        if (!clips) continue;
        animation::AnimatorInput in;
        in.entity = e.id;
        in.clips = std::move(clips);
        in.clip = a->clip;
        in.speed = a->speed;
        in.loop = a->loop;
        in.start_time = a->start_time;
        // Edit mode plays only the preview the author asked for; Play plays everything.
        in.playing = runtime_ || (view_flags.animations && a->preview);
        if (const auto world = drawn_world(e.id)) in.world = world->matrix;
        if (const auto source = asset_meta(a->clips.empty() ? e.components.mesh->mesh_asset : a->clips)) in.events = source->events;
        inputs.push_back(std::move(in));
    }
    return inputs;
}
std::shared_ptr<const animation::ClipSet> EditorSession::clip_set(EntityId id, std::string *problem) const {
    const auto record = scene().inspect(id);
    std::string why = "it has no animator";
    std::shared_ptr<const animation::ClipSet> clips;
    if (record && record->components.animator) clips = clips_for(*record, why);
    if (problem) *problem = why;
    return clips;
}
std::shared_ptr<const animation::Rig> EditorSession::rig(EntityId id) const {
    const auto record = scene().inspect(id);
    if (!record || !record->components.mesh) return nullptr;
    std::string why;
    return rig_for(model(record->components.mesh->mesh_asset), why);
}
assets::AssetId EditorSession::clip_source(EntityId id) const {
    const auto record = scene().inspect(id);
    if (!record || !record->components.animator) return {};
    const auto &a = *record->components.animator;
    std::optional<assets::AssetMetadata> meta;
    if (!a.clips.empty()) meta = asset_meta(a.clips);
    else if (record->components.mesh) meta = asset_meta(record->components.mesh->mesh_asset);
    return meta && meta->type == assets::AssetType::Mesh ? meta->id : assets::AssetId{};
}
void EditorSession::keep_events(std::vector<animation::AnimationEvent> events) {
    for (auto &event : events) recent_events_.push_back(std::move(event));
    while (recent_events_.size() > 64) recent_events_.pop_front();
}
void EditorSession::advance_animations(std::chrono::nanoseconds elapsed) {
    if (runtime_ || elapsed.count() <= 0) return;
    edit_animation_.update(std::chrono::duration<double>(elapsed).count());
    keep_events(edit_animation_.take_events());
}
void EditorSession::restart_animations() {
    animation().restart();
    recent_events_.clear();
}
std::optional<animation::JointMatrix> EditorSession::joint_model(EntityId id, std::string_view joint) const {
    const auto record = scene().inspect(id);
    if (!record || !record->components.mesh) return std::nullopt;
    const auto data = model(record->components.mesh->mesh_asset);
    if (!data || data->skin.empty()) return std::nullopt;
    const auto &joints = data->skin.joints;
    const auto at = std::find_if(joints.begin(), joints.end(), [&](const assets::Joint &j) { return j.name == joint; });
    if (at == joints.end()) return std::nullopt;
    const auto index = static_cast<std::size_t>(at - joints.begin());
    const renderer::Pose posed = shown_pose(id);
    if (posed && posed->size() == joints.size()) return (*posed)[index];
    std::string why;
    if (const auto skeleton = rig_for(data, why)) return (*skeleton->rest_pose())[index];
    return std::nullopt;
}
std::array<double, 16> EditorSession::attachment_correction(EntityId id, const std::function<std::optional<EntityRecord>(EntityId)> &records) const {
    // Each attachment moves its subtree by W(parent) * joint * W(parent)^-1; outer ones apply first (D2).
    std::vector<std::pair<EntityId, std::string>> attachments;
    std::set<EntityId> seen;
    for (auto record = records(id); record && seen.insert(record->id).second; record = record->parent ? records(record->parent) : std::nullopt)
        if (record->components.attach && record->parent) attachments.emplace_back(record->parent, record->components.attach->bone);
    glm::dmat4 correction(1);
    for (auto it = attachments.rbegin(); it != attachments.rend(); ++it) {
        const auto joint = joint_model(it->first, it->second);
        const auto parent = scene().world_transform(it->first);
        if (!joint || !parent) continue; // An unknown joint leaves it on its parent.
        const auto w = glm::make_mat4(parent->matrix.data());
        correction = correction * w * glm::dmat4(glm::make_mat4(joint->data())) * glm::inverse(w);
    }
    std::array<double, 16> out{};
    std::copy_n(glm::value_ptr(correction), 16, out.begin());
    return out;
}
std::optional<WorldTransform> EditorSession::drawn_world(EntityId id) const {
    const auto world = scene().world_transform(id);
    if (!world) return std::nullopt;
    const auto correction = attachment_correction(id, [this](EntityId at) { return scene().inspect(at); });
    if (correction == animation::identity) return world;
    return copy(glm::make_mat4(correction.data()) * glm::make_mat4(world->matrix.data()));
}
std::optional<WorldTransform> EditorSession::joint_world(EntityId id, std::string_view joint) const {
    const auto model_space = joint_model(id, joint);
    const auto world = drawn_world(id);
    if (!model_space || !world) return std::nullopt;
    return copy(glm::make_mat4(world->matrix.data()) * glm::dmat4(glm::make_mat4(model_space->data())));
}
std::vector<EditorSession::BoneLine> EditorSession::bone_lines(EntityId only) const {
    std::vector<BoneLine> lines;
    for (const auto &e : scene().snapshot().entities) {
        if ((only && e.id != only) || !e.components.mesh || (!runtime_ && hidden(e.id))) continue;
        const auto data = model(e.components.mesh->mesh_asset);
        if (!data || data->skin.empty()) continue;
        renderer::Pose joints = shown_pose(e.id);
        if (!joints || joints->size() != data->skin.joints.size()) {
            std::string why;
            const auto skeleton = rig_for(data, why);
            if (!skeleton) continue;
            joints = skeleton->rest_pose();
        }
        const auto world = drawn_world(e.id);
        if (!world) continue;
        const auto m = glm::make_mat4(world->matrix.data());
        const auto at = [&](std::size_t j) {
            const auto &p = (*joints)[j];
            const auto v = m * glm::dvec4(p[12], p[13], p[14], 1);
            return Vec3{v.x, v.y, v.z};
        };
        for (std::size_t j = 0; j < data->skin.joints.size(); ++j)
            if (const int parent = data->skin.joints[j].parent; parent >= 0) lines.push_back({e.id, at(static_cast<std::size_t>(parent)), at(j)});
    }
    return lines;
}
std::vector<assets::ClipEvent> EditorSession::clip_events(assets::AssetId id) const {
    const auto meta = assets_ ? assets_->find(id) : std::nullopt;
    return meta ? meta->events : std::vector<assets::ClipEvent>{};
}
Result<void> EditorSession::set_clip_events(assets::AssetId id, std::vector<assets::ClipEvent> events) {
    if (auto allowed = editing_only("Editing clip events"); !allowed) return allowed;
    const auto meta = assets_ ? assets_->find(id) : std::nullopt;
    if (!meta) return fail(ErrorCode::NotFound, "Model is not in the asset folder");
    if (meta->type != assets::AssetType::Mesh) return fail(ErrorCode::InvalidArgument, meta->path + " is not a model");
    auto normalized = assets::normalize_clip_events(std::move(events));
    if (!normalized) return normalized.error();
    if (normalized.value() == meta->events) return {};
    if (auto saved = assets_->set_clip_events(id, normalized.value()); !saved) return saved;
    const auto stem = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(meta->path.data()), meta->path.size())).stem().u8string();
    HistoryStep step{"Edit Clip Events of " + std::string(reinterpret_cast<const char *>(stem.data()), stem.size()), std::nullopt, std::nullopt, false,
                     std::nullopt, EventsEdit{id, meta->events, normalized.value()}};
    undo_steps_.push_back(std::move(step));
    redo_steps_.clear();
    bus_.discard_redo();
    if (std::ranges::count_if(undo_steps_, [](const HistoryStep &s) { return s.events.has_value(); }) > 128)
        undo_steps_.erase(std::ranges::find_if(undo_steps_, [](const HistoryStep &s) { return s.events.has_value(); }));
    return {};
}
Result<commands::Receipt> EditorSession::assign_clip(EntityId id, const std::string &clip) {
    const auto record = scene().inspect(id);
    if (!record) return fail(ErrorCode::NotFound, "Entity no longer exists");
    auto animator = record->components.animator.value_or(AnimatorComponent{});
    animator.clip = clip;
    return execute(commands::SetAnimator{id, animator}, clip.empty() ? "Clear Clip" : "Play " + clip);
}
renderer::Pose EditorSession::shown_pose(EntityId id) const {
    if (auto host = pose(id)) return host;
    if (runtime_)
        if (const auto run = runtime_->ragdolls.find(id); run != runtime_->ragdolls.end() && run->second.pose) return run->second.pose;
    return animation().pose(id);
}
// ---- Ragdolls and debris (D7, D11) ----
namespace {
// A matrix as a rigid pose and the length of each axis.
struct Rigid {
    physics::Pose pose;
    glm::dvec3 scale{1};
};
Rigid rigid(const glm::dmat4 &m) {
    Rigid out;
    out.scale = {glm::length(glm::dvec3(m[0])), glm::length(glm::dvec3(m[1])), glm::length(glm::dvec3(m[2]))};
    const glm::dmat3 r(glm::dvec3(m[0]) / out.scale.x, glm::dvec3(m[1]) / out.scale.y, glm::dvec3(m[2]) / out.scale.z);
    const glm::dquat q = glm::normalize(glm::quat_cast(r));
    out.pose.position = {float(m[3].x), float(m[3].y), float(m[3].z)};
    out.pose.rotation = {float(q.x), float(q.y), float(q.z), float(q.w)};
    return out;
}
glm::dmat4 widen(const animation::JointMatrix &m) { return glm::dmat4(glm::make_mat4(m.data())); }
physics::RagdollSettings ragdoll_settings(const RagdollComponent &r) {
    physics::RagdollSettings s;
    s.density = float(r.density);
    s.cone = float(glm::radians(r.swing_degrees));
    s.twist = float(glm::radians(r.twist_degrees));
    s.joint_friction = float(r.joint_friction);
    for (const auto &b : r.bones)
        s.bones.push_back({b.bone, b.body == RagdollBody::Own ? 1 : b.body == RagdollBody::None ? 0 : -1, float(b.radius), float(b.density),
                           b.swing_degrees < 0 ? -1.0f : float(glm::radians(b.swing_degrees)),
                           b.twist_degrees < 0 ? -1.0f : float(glm::radians(b.twist_degrees))});
    return s;
}
}
std::optional<EditorSession::RagdollBuild> EditorSession::ragdoll_build(EntityId id, std::string &problem) const {
    problem.clear();
    const auto record = scene().inspect(id);
    if (!record || !record->components.ragdoll) { problem = "it has no ragdoll"; return std::nullopt; }
    const auto say = [&](const std::string &why) -> std::optional<RagdollBuild> {
        problem = "Ragdoll on '" + record->name + "' cannot be built: " + why;
        return std::nullopt;
    };
    const auto &m = record->components.mesh;
    if (!m || m->mesh_asset.empty()) return say("its entity draws no model");
    const auto data = model(m->mesh_asset);
    if (!data) return say("its model cannot be loaded");
    if (data->skin.empty()) return say("model '" + m->mesh_asset.path + "' has no skeleton");
    const auto drawn = drawn_world(id);
    if (!drawn) return say("it has no place in the world");
    const glm::dmat4 world = glm::make_mat4(drawn->matrix.data());
    const auto split = rigid(world);
    const double s = split.scale.x;
    if (!(s > 1e-9) || std::abs(split.scale.y - s) > 1e-4 * s || std::abs(split.scale.z - s) > 1e-4 * s)
        return say("its scale must be the same on every axis");
    // The skeleton at rest as it stands in the world, in meters, with the box of the skin each joint moves.
    const auto rest = assets::rest_pose(data->skin);
    const auto &flesh = flesh_for(data);
    std::vector<physics::RagdollJoint> skeleton;
    skeleton.reserve(rest.size());
    for (std::size_t j = 0; j < rest.size(); ++j) {
        const auto joint = rigid(glm::scale(glm::dmat4(1), glm::dvec3(s)) * widen(rest[j]));
        physics::RagdollJoint out;
        out.name = data->skin.joints[j].name;
        out.parent = data->skin.joints[j].parent;
        out.rest = joint.pose;
        if (flesh.any[j]) {
            out.skinned = true;
            // The joint's own space is scaled as the joint is; the builder wants meters.
            const auto &box = flesh.boxes[j];
            const glm::dvec3 lo = glm::dvec3(box.min[0], box.min[1], box.min[2]) * joint.scale.x;
            const glm::dvec3 hi = glm::dvec3(box.max[0], box.max[1], box.max[2]) * joint.scale.x;
            out.flesh_min = {float(lo.x), float(lo.y), float(lo.z)};
            out.flesh_max = {float(hi.x), float(hi.y), float(hi.z)};
        }
        skeleton.push_back(std::move(out));
    }
    RagdollBuild build;
    build.model = data;
    build.plan = physics::plan_ragdoll(skeleton, ragdoll_settings(*record->components.ragdoll));
    if (!build.plan.ok()) return say(build.plan.problem);
    std::copy_n(glm::value_ptr(world), 16, build.world.begin());
    return build;
}
const EditorSession::FleshEntry &EditorSession::flesh_for(const std::shared_ptr<const assets::MeshData> &data) const {
    auto found = flesh_.find(data.get());
    // Freed data's address can come back for other data.
    if (found != flesh_.end() && found->second.model.lock() != data) { flesh_.erase(found); found = flesh_.end(); }
    if (found != flesh_.end()) return found->second;
    FleshEntry entry{data, std::vector<assets::Bounds>(data->skin.joints.size()), std::vector<char>(data->skin.joints.size(), 0)};
    const auto &skin = data->skin;
    for (std::size_t v = 0; v < data->vertices.size() && v < skin.influences.size(); ++v) {
        // The strongest influence comes first.
        const auto b = skin.influences[v].binding[0];
        if (b >= skin.bindings.size() || skin.bindings[b].joint < 0) continue;
        const auto j = std::size_t(skin.bindings[b].joint);
        if (j >= entry.boxes.size()) continue;
        const auto p = assets::transform_point(skin.bindings[b].inverse_bind, data->vertices[v].position);
        auto &box = entry.boxes[j];
        if (!entry.any[j]) { box.min = box.max = p; entry.any[j] = 1; continue; }
        for (int k = 0; k < 3; ++k) { box.min[k] = std::min(box.min[k], p[k]); box.max[k] = std::max(box.max[k], p[k]); }
    }
    return flesh_.emplace(data.get(), std::move(entry)).first->second;
}
std::optional<physics::RagdollPlan> EditorSession::ragdoll_plan(EntityId id, std::string *problem) const {
    std::string why;
    auto build = ragdoll_build(id, why);
    if (problem) *problem = why;
    if (!build) return std::nullopt;
    return std::move(build->plan);
}
Result<void> EditorSession::start_ragdoll(EntityId id, std::optional<std::pair<Vec3, Vec3>> impulse_at) {
    if (!runtime_) return fail(ErrorCode::InvalidState, "Ragdolls fall only while the scene runs");
    if (runtime_->ragdolls.contains(id)) return {};
    std::string why;
    auto build = ragdoll_build(id, why);
    if (!build) return fail(ErrorCode::InvalidArgument, why);
    const auto &skin = build->model->skin;
    const std::size_t n = skin.joints.size();
    // The pose it is in, and the one a tick before: their difference is how it was moving.
    renderer::Pose now = shown_pose(id);
    if (!now || now->size() != n) {
        std::string unused;
        const auto skeleton = rig_for(build->model, unused);
        if (!skeleton) return fail(ErrorCode::InvalidArgument, "Its model has no usable skeleton");
        now = skeleton->rest_pose();
    }
    renderer::Pose before = pose(id) ? nullptr : runtime_->animation.previous_pose(id);
    if (!before || before->size() != n) before = now;
    const glm::dmat4 world = glm::make_mat4(build->world.data());
    std::vector<physics::Pose> at_now(n), at_before(n);
    Runtime::RagdollRun run;
    run.scales.resize(n);
    for (std::size_t j = 0; j < n; ++j) {
        const auto placed = rigid(world * widen((*now)[j]));
        at_now[j] = placed.pose;
        run.scales[j] = placed.scale;
        at_before[j] = rigid(world * widen((*before)[j])).pose;
        run.parents.push_back(skin.joints[j].parent);
    }
    auto desc = physics::place_ragdoll(build->plan, at_now, at_before, float(std::chrono::duration<double>(fixed_step).count()));
    if (desc.parts.empty()) return fail(ErrorCode::InvalidArgument, "Its pose is not usable");
    // A character carried by its own body keeps that body's speed.
    if (const auto body = runtime_->physics->body_state(id); body && body->type != physics::BodyType::Static)
        for (auto &part : desc.parts) {
            part.linear_velocity.x += body->linear_velocity.x;
            part.linear_velocity.y += body->linear_velocity.y;
            part.linear_velocity.z += body->linear_velocity.z;
        }
    auto made = runtime_->physics->create_ragdoll(id, desc);
    if (!made) return made.error();
    if (impulse_at) {
        const auto &[impulse, point] = *impulse_at;
        runtime_->physics->apply_ragdoll_impulse(made.value(), {float(impulse.x), float(impulse.y), float(impulse.z)},
                                                 {float(point.x), float(point.y), float(point.z)});
    }
    run.id = made.value();
    run.base = *now;
    run.pose = now;
    runtime_->ragdolls[id] = std::move(run);
    return {};
}
Result<void> EditorSession::stop_ragdoll(EntityId id) {
    if (!runtime_) return fail(ErrorCode::InvalidState, "Ragdolls fall only while the scene runs");
    const auto run = runtime_->ragdolls.find(id);
    if (run == runtime_->ragdolls.end()) return fail(ErrorCode::NotFound, "It is not a ragdoll");
    runtime_->physics->destroy_ragdoll(run->second.id);
    runtime_->ragdolls.erase(run);
    return {};
}
bool EditorSession::ragdolled(EntityId id) const { return runtime_ && runtime_->ragdolls.contains(id); }
std::vector<EntityId> EditorSession::ragdolled_entities() const {
    std::vector<EntityId> ids;
    if (runtime_)
        for (const auto &[id, run] : runtime_->ragdolls) { (void)run; ids.push_back(id); }
    return ids;
}
void EditorSession::pose_ragdolls() {
    auto &r = *runtime_;
    for (auto it = r.ragdolls.begin(); it != r.ragdolls.end();) {
        auto &run = it->second;
        const auto state = r.physics->ragdoll(run.id);
        const auto world = r.scene.world_transform(it->first);
        // Its entity is gone, and physics took the bodies with it.
        if (!state || !world) { it = r.ragdolls.erase(it); continue; }
        // Each body puts its joint in the world; the model space is the entity's.
        const glm::dmat4 to_model = glm::inverse(glm::make_mat4(drawn_world(it->first).value_or(*world).matrix.data()));
        std::vector<std::optional<animation::JointMatrix>> placed(run.base.size());
        for (const auto &part : state->parts) {
            const auto j = std::size_t(part.bone);
            if (j >= placed.size()) continue;
            const auto &p = part.pose;
            const glm::dmat4 at = glm::translate(glm::dmat4(1), glm::dvec3(p.position.x, p.position.y, p.position.z)) *
                                  glm::mat4_cast(glm::dquat(p.rotation.w, p.rotation.x, p.rotation.y, p.rotation.z)) * glm::scale(glm::dmat4(1), run.scales[j]);
            const glm::mat4 model(to_model * at);
            animation::JointMatrix m{};
            std::copy_n(glm::value_ptr(model), 16, m.begin());
            placed[j] = m;
        }
        if (auto posed = animation::place_joints(run.parents, run.base, placed)) run.pose = std::move(posed);
        ++it;
    }
}
std::vector<physics::DebugLine> EditorSession::ragdoll_lines(EntityId only) const {
    std::vector<physics::DebugLine> lines;
    for (const auto &e : scene().snapshot().entities) {
        if ((only && e.id != only) || !e.components.ragdoll || (!runtime_ && hidden(e.id))) continue;
        std::string why;
        const auto build = ragdoll_build(e.id, why);
        if (!build) continue;
        const std::size_t n = build->model->skin.joints.size();
        renderer::Pose joints = shown_pose(e.id);
        if (!joints || joints->size() != n) {
            const auto skeleton = rig_for(build->model, why);
            if (!skeleton) continue;
            joints = skeleton->rest_pose();
        }
        const glm::dmat4 world = glm::make_mat4(build->world.data());
        for (const auto &body : build->plan.bodies) {
            const auto at = rigid(world * widen((*joints)[std::size_t(body.joint)])).pose;
            physics::append_shape_lines(lines, body.capsule, at, e.id, physics::DebugCategory::Ragdoll, 12);
            for (const auto &more : body.more) physics::append_shape_lines(lines, more, at, e.id, physics::DebugCategory::Ragdoll, 12);
        }
    }
    return lines;
}
Result<std::uint64_t> EditorSession::spawn_debris(EntityId look, const Vec3 &position, const Quaternion &rotation, const Vec3 &velocity,
                                                  const Vec3 &spin, double lifetime) {
    if (!runtime_) return fail(ErrorCode::InvalidState, "Debris falls only while the scene runs");
    const auto record = scene().inspect(look);
    if (!record || !record->components.mesh) return fail(ErrorCode::InvalidArgument, "Debris looks like an entity with a mesh renderer");
    const auto world = scene().world_transform(look);
    const auto bounds = local_bounds(look);
    if (!world || !bounds) return fail(ErrorCode::InvalidArgument, "That entity cannot be drawn as debris");
    // A box around how it looks, at its scale.
    const glm::dvec3 scale = rigid(glm::make_mat4(world->matrix.data())).scale;
    const glm::dvec3 lo(bounds->first.x, bounds->first.y, bounds->first.z), hi(bounds->second.x, bounds->second.y, bounds->second.z);
    const glm::dvec3 half = glm::max((hi - lo) * 0.5 * scale, glm::dvec3(0.005)), center = (hi + lo) * 0.5 * scale;
    physics::Debris piece;
    piece.look = look;
    piece.pose.position = {float(position.x), float(position.y), float(position.z)};
    piece.pose.rotation = {float(rotation.x), float(rotation.y), float(rotation.z), float(rotation.w)};
    piece.linear_velocity = {float(velocity.x), float(velocity.y), float(velocity.z)};
    piece.angular_velocity = {float(spin.x), float(spin.y), float(spin.z)};
    piece.collider.shape = physics::BoxShape{{float(half.x), float(half.y), float(half.z)}, {{float(center.x), float(center.y), float(center.z)}, {}}};
    piece.collider.density = record->components.colliders.empty() ? 1000.0f : float(record->components.colliders.front().density);
    piece.collider.restitution = 0.1f;
    piece.lifetime = lifetime;
    return runtime_->physics->spawn_debris(piece);
}
std::vector<physics::DebrisState> EditorSession::debris() const {
    return runtime_ ? runtime_->physics->debris() : std::vector<physics::DebrisState>{};
}
physics::Vec3 EditorSession::lowest_joint(EntityId id) const {
    const auto world = drawn_world(id);
    if (!world) return {};
    const glm::dmat4 m = glm::make_mat4(world->matrix.data());
    glm::dvec3 lowest(m[3]);
    if (const auto posed = shown_pose(id); posed && !posed->empty()) {
        lowest.y = std::numeric_limits<double>::max();
        for (const auto &joint : *posed) {
            const glm::dvec3 p(m * glm::dvec4(joint[12], joint[13], joint[14], 1));
            if (p.y < lowest.y) lowest = p;
        }
    }
    return {float(lowest.x), float(lowest.y), float(lowest.z)};
}
namespace {
bool names(std::string_view clip, std::string_view part) {
    return !std::ranges::search(clip, part, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }).empty();
}
}
void EditorSession::play_cue(EntityId id, gameplay::AnimationCue cue) {
    if (!runtime_ || runtime_->ragdolls.contains(id)) return;
    const auto record = scene().inspect(id);
    const auto clips = clip_set(id);
    if (!record || !record->components.animator || !clips) return;
    // Walk is what the Animator plays; the others are found by name.
    std::string clip = cue == gameplay::AnimationCue::Walk ? record->components.animator->clip : std::string{};
    const std::string_view part = cue == gameplay::AnimationCue::Idle ? "idle" : "hit";
    // A clip whose name starts with the word first ("Idle_Loop" over "Crouch_Idle_Loop"), then any that has it.
    for (std::size_t i = 0; i < clips->size() && clip.empty(); ++i)
        if (names(std::string_view(clips->name(i)).substr(0, part.size()), part)) clip = clips->name(i);
    for (std::size_t i = 0; i < clips->size() && clip.empty(); ++i)
        if (names(clips->name(i), part)) clip = clips->name(i);
    if (clip.empty()) return;
    if (cue == gameplay::AnimationCue::Flinch) (void)runtime_->animation.play_layered(id, clip, 1.0, 0.08);
    else (void)runtime_->animation.play(id, clip, 0.25, true);
}
const std::vector<std::string> &EditorSession::game_issues() const {
    static const std::vector<std::string> none;
    return runtime_ ? runtime_->game_issues : none;
}
} // namespace engine::editor
