#include <engine/editor/EditorSession.hpp>
#include <engine/assets/importers.h>
#include <engine/editor/EditorPrefs.hpp>
#include <algorithm>
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
EditorSession::EditorSession() : edit_physics_(make_physics(false)) {}
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
    mark_clean();
    document_ = path;
    load_view_state();
    return {};
}
Result<Transform> EditorSession::local_from_world(EntityId parent, const WorldTransform &world) const {
    auto local = glm::make_mat4(world.matrix.data());
    if (parent) local = glm::inverse(glm::make_mat4(scene().world_transform(parent)->matrix.data())) * local;
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
    auto t = local_from_world(record->parent, world);
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
    auto local = local_from_world(parent, *scene().world_transform(id));
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
    for (const auto id : selection_roots()) drag_start_.emplace_back(id, *scene().world_transform(id));
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
        auto local = local_from_world(record->parent, copy(change * glm::make_mat4(start.matrix.data())));
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
        const auto w = scene().world_transform(id);
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
        if (exclude.contains(object.entity)) continue;
        const auto inverse = glm::inverse(glm::make_mat4(object.world.matrix.data()));
        // The local ray is not normalized, so t stays a world-space distance along the ray.
        const glm::dvec3 origin(inverse * a), ray(inverse * d);
        // Models are tested by their bounding box.
        const auto data = resources_.find(object.mesh);
        const double t = data ? box_hit(vector(data->bounds.min), vector(data->bounds.max), origin, ray, nearest) : hit(object.mesh, origin, ray, nearest);
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
        const auto w = scene().world_transform(id);
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
        auto local = local_from_world(scene().inspect(id)->parent, copy(placed));
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
        if (exclude.contains(object.entity)) continue;
        const auto world = glm::make_mat4(object.world.matrix.data());
        if (const auto data = resources_.find(object.mesh)) {
            // A model far from the cursor is passed over by its box, so one with many vertices costs nothing unless near.
            double min_x = std::numeric_limits<double>::max(), max_x = std::numeric_limits<double>::lowest(), min_y = min_x, max_y = max_x;
            bool behind = false;
            const auto low = vector(data->bounds.min), high = vector(data->bounds.max);
            for (int i = 0; i < 8; ++i) {
                const auto clip = clip_from_world * world * glm::dvec4(i & 1 ? high.x : low.x, i & 2 ? high.y : low.y, i & 4 ? high.z : low.z, 1);
                if (clip.w <= 1e-6) { behind = true; break; }
                const double sx = (clip.x / clip.w + 1) / 2 * width, sy = (1 - clip.y / clip.w) / 2 * height;
                min_x = std::min(min_x, sx); max_x = std::max(max_x, sx); min_y = std::min(min_y, sy); max_y = std::max(max_y, sy);
            }
            if (!behind && (x < min_x - radius || x > max_x + radius || y < min_y - radius || y > max_y + radius)) continue;
            for (const auto &vertex : data->vertices) consider(world, vector(vertex.position));
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
            if (const auto local = local_from_world(0, *scene().world_transform(e.id))) entity.transform = local.value();
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
Result<void> EditorSession::rename_asset(assets::AssetId id, const std::string &new_path) {
    if (auto allowed = editing_only("Renaming an asset"); !allowed) return allowed;
    if (!assets_) return fail(ErrorCode::NotFound, "No asset folder");
    if (auto moved = assets_->move(id, new_path); !moved) return moved;
    const auto meta = assets_->find(id);
    if (!meta) return fail(ErrorCode::NotFound, "The asset could not be found after moving it");
    resolved_.clear();
    const auto fix = [&](AssetRef &ref) {
        if (ref.id != id.value || ref.path == meta->path) return false;
        ref.path = meta->path;
        return true;
    };
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
    }
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
        const auto low = vector(data->bounds.min), high = vector(data->bounds.max);
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
    const auto entities = shown.snapshot().entities;
    std::vector<renderer::MeshBinding> meshes;
    std::vector<renderer::LightBinding> lights;
    asset_problems_.clear();
    const auto problem = [this](const std::string &text) {
        if (std::find(asset_problems_.begin(), asset_problems_.end(), text) == asset_problems_.end()) asset_problems_.push_back(text);
    };
    for (const auto &e : entities) {
        if (const auto &m = e.components.mesh) {
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
            meshes.push_back({e.id, handle, material, runtime_ || !hidden(e.id), surface, std::move(parts)});
        }
        if (const auto &l = e.components.light; l && (runtime_ || !hidden(e.id))) {
            const auto kind = l->type == LightType::Point ? renderer::LightKind::Point : l->type == LightType::Spot ? renderer::LightKind::Spot : renderer::LightKind::Directional;
            lights.push_back({e.id, kind, linear(l->color), l->intensity, l->range, glm::radians(l->inner_cone_degrees), glm::radians(l->outer_cone_degrees), l->cast_shadows});
        }
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
        if (const auto world = shown.world_transform(e.id)) in.world = world->matrix;
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
    if (!runtime_ || runtime_->paused || elapsed.count() <= 0) return;
    auto &r = *runtime_;
    r.particles.update(std::chrono::duration<double>(elapsed).count());
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
    if (r.game) {
        r.game->post_physics();
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
    timing_.gameplay += (before_physics - start) + (end - after_physics);
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
    // Material and effect edits belong to the old project's files.
    material_drafts_.clear();
    effect_drafts_.clear();
    std::erase_if(undo_steps_, [](const HistoryStep &step) { return step.material.has_value() || step.effect.has_value(); });
    std::erase_if(redo_steps_, [](const HistoryStep &step) { return step.material.has_value() || step.effect.has_value(); });
    edit_particles_ = effects::ParticleSystem(1);
    if (!std::filesystem::is_directory(asset_folder(), ec)) return {};
    auto database = assets::AssetDatabase::open(asset_folder());
    if (!database) return database.error();
    if (auto scanned = database.value().scan(); !scanned) return scanned.error();
    assets_.emplace(std::move(database.value()));
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
    auto result = execute(commands::CreateEntity{std::move(name), t, 0, 0, Components{mesh, {}, {}}}, label);
    if (result) selection_.set(result.value().created.front());
    return result;
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
} // namespace engine::editor
