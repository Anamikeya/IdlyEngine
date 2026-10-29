#include <engine/editor/EditorSession.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
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
// Nearest t in [0, limit] where the local ray origin + t * ray enters the shape, or a negative value for a miss.
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
    double low = 0, high = limit;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(ray[axis]) < 1e-12) {
            if (std::abs(origin[axis]) > 0.5) return -1;
        } else {
            const double t0 = (-0.5 - origin[axis]) / ray[axis], t1 = (0.5 - origin[axis]) / ray[axis];
            low = std::max(low, std::min(t0, t1)); high = std::min(high, std::max(t0, t1));
        }
    }
    return high >= low ? low : -1;
}
}
void OrbitCamera::orbit(double dx, double dy) {
    yaw = std::remainder(yaw - dx * 0.008, 6.283185307179586);
    pitch = std::clamp(pitch + dy * 0.008, -1.5, 1.5);
}
void OrbitCamera::pan(double dx, double dy, double height) {
    const auto m = glm::make_mat4(world().matrix.data());
    const auto shift = (-dx * glm::dvec3(m[0]) + dy * glm::dvec3(m[1])) *
        (2 * distance * std::tan(0.523598775598) / std::max(height, 1.0));
    target = {target.x + shift.x, target.y + shift.y, target.z + shift.z};
}
void OrbitCamera::zoom(double steps) { distance = std::clamp(distance * std::exp(-steps * 0.15), 0.2, 500.0); }
WorldTransform OrbitCamera::world() const {
    const auto center = vector(target);
    const auto eye = center + distance * glm::dvec3(std::cos(pitch) * std::sin(yaw),
        std::sin(pitch), std::cos(pitch) * std::cos(yaw));
    return copy(glm::inverse(glm::lookAtRH(eye, center, glm::dvec3(0, 1, 0))));
}
struct EditorSession::Runtime {
    Scene scene;
    commands::CommandBus bus{scene, 0}; // Runtime edits are discarded on stop, so they keep no history.
    bool paused = false;
    std::uint64_t ticks = 0;
    std::chrono::nanoseconds accumulated{0};
};
EditorSession::EditorSession() = default;
EditorSession::~EditorSession() = default;
const Scene &EditorSession::scene() const { return runtime_ ? runtime_->scene : scene_; }
void EditorSession::reconcile() { if (!scene().inspect(selected_)) selected_ = 0; }
void EditorSession::select(EntityId id) { selected_ = scene().inspect(id) ? id : 0; }
Result<void> EditorSession::editing_only(const char *action) const {
    if (runtime_) return fail(ErrorCode::InvalidState, std::string(action) + " is unavailable in Play mode; press Stop first");
    return {};
}
Result<commands::Receipt> EditorSession::execute(commands::Command cmd) {
    if (runtime_) {
        if (std::holds_alternative<commands::ReplaceScene>(cmd))
            return fail(ErrorCode::InvalidState, "Replacing the scene is unavailable in Play mode; press Stop first");
        auto result = runtime_->bus.execute(context_, runtime_->bus.revision(), std::move(cmd));
        reconcile();
        return result;
    }
    auto result = bus_.execute(context_, bus_.revision(), std::move(cmd));
    reconcile();
    return result;
}
Result<commands::Receipt> EditorSession::create(std::string name, EntityId parent, Components components) {
    Transform t; t.position.y = 0.5;
    auto r = execute(commands::CreateEntity{std::move(name), t, parent, 0, std::move(components)});
    if (r) selected_ = r.value().created.front();
    return r;
}
Result<commands::Receipt> EditorSession::undo() {
    if (auto allowed = editing_only("Undo"); !allowed) return allowed.error();
    auto r = bus_.undo(context_, bus_.revision()); reconcile(); return r;
}
Result<commands::Receipt> EditorSession::redo() {
    if (auto allowed = editing_only("Redo"); !allowed) return allowed.error();
    auto r = bus_.redo(context_, bus_.revision()); reconcile(); return r;
}
Result<void> EditorSession::new_scene() {
    if (auto allowed = editing_only("New Scene"); !allowed) return allowed;
    // next_id is kept by ReplaceScene, so stable IDs are never recycled within a session.
    auto r = execute(commands::ReplaceScene{SceneSnapshot{}});
    if (!r) return r.error();
    selected_ = 0;
    clean_.clear(); dirty_revision_ = ~std::uint64_t(0);
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
    return {};
}
Result<void> EditorSession::load(const std::filesystem::path &path) {
    if (auto allowed = editing_only("Load"); !allowed) return allowed;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return fail(ErrorCode::NotFound, "Cannot open scene file");
    auto snapshot = read_scene(stream);
    if (!snapshot) return snapshot.error();
    auto r = execute(commands::ReplaceScene{std::move(snapshot.value())});
    if (!r) return r.error();
    selected_ = 0;
    mark_clean();
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
    auto &bus = runtime_ ? runtime_->bus : bus_;
    auto result = bus.execute(context_, bus.revision(), std::span<const commands::Command>(batch));
    reconcile();
    return result;
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
    auto result = execute(commands::DuplicateEntity{id, name + std::string(suffix)});
    if (result) selected_ = result.value().created.front();
    return result;
}
void EditorSession::focus() {
    if (const auto w = scene().world_transform(selected_)) {
        const auto m = glm::make_mat4(w->matrix.data());
        camera.target = {m[3].x, m[3].y, m[3].z};
        camera.distance = std::clamp(3.0 * std::max({glm::length(glm::dvec3(m[0])), glm::length(glm::dvec3(m[1])), glm::length(glm::dvec3(m[2]))}), 0.2, 500.0);
    }
}
Result<renderer::Frame> EditorSession::frame(double aspect) {
    const auto &shown = scene();
    const auto entities = shown.snapshot().entities;
    std::vector<renderer::MeshBinding> meshes;
    std::vector<renderer::LightBinding> lights;
    for (const auto &e : entities) {
        if (const auto &m = e.components.mesh)
            meshes.push_back({e.id, mesh_handle(m->mesh), renderer::builtin_lit, true, {linear(m->color), m->roughness, m->metallic}});
        if (const auto &l = e.components.light)
            lights.push_back({e.id, l->type == LightType::Point ? renderer::LightKind::Point : renderer::LightKind::Directional,
                              linear(l->color), l->intensity, l->range});
    }
    // Each play session is a distinct scene identity, so the renderer never reuses editor objects for it.
    const renderer::FrameStamp stamp{scene_identity(), scene_revision(), ++frame_};
    view_camera_ = 0; view_camera_issue_.clear();
    if (runtime_) {
        const auto primary = std::find_if(entities.begin(), entities.end(),
            [](const EntityRecord &e) { return e.components.camera && e.components.camera->primary; });
        if (primary != entities.end()) {
            const auto &c = *primary->components.camera;
            const renderer::PerspectiveCamera lens{primary->id, glm::radians(c.vertical_fov_degrees), aspect, c.near_plane, c.far_plane};
            auto through_camera = renderer::extract_frame(shown, stamp, lens, meshes, lights);
            if (through_camera) { view_camera_ = primary->id; return through_camera; }
            view_camera_issue_ = "Camera '" + primary->name + "' cannot be used: " + through_camera.error().message;
        }
    }
    renderer::PerspectiveCamera lens; lens.aspect_ratio = aspect;
    return renderer::extract_frame(shown, stamp, lens, camera.world(), meshes, lights);
}
std::uint64_t EditorSession::scene_identity() const { return runtime_ ? 1 + play_sessions_ : 1; }
std::uint64_t EditorSession::scene_revision() const { return runtime_ ? runtime_->bus.revision() : bus_.revision(); }
PlayState EditorSession::play_state() const {
    if (!runtime_) return PlayState::Editing;
    return runtime_->paused ? PlayState::Paused : PlayState::Playing;
}
Result<void> EditorSession::play() {
    if (runtime_) { runtime_->paused = false; return {}; }
    auto runtime = std::make_unique<Runtime>();
    auto copied = runtime->bus.execute(context_, 0, commands::ReplaceScene{scene_.snapshot()});
    if (!copied) return copied.error();
    runtime_ = std::move(runtime);
    ++play_sessions_;
    reconcile();
    return {};
}
Result<void> EditorSession::pause() {
    if (!runtime_) return fail(ErrorCode::InvalidState, "Pause requires Play mode");
    runtime_->paused = true;
    return {};
}
Result<void> EditorSession::step() {
    if (!runtime_ || !runtime_->paused) return fail(ErrorCode::InvalidState, "Step requires paused Play mode");
    ++runtime_->ticks;
    return {};
}
Result<void> EditorSession::stop() {
    if (!runtime_) return fail(ErrorCode::InvalidState, "Not in Play mode");
    runtime_.reset();
    reconcile();
    return {};
}
void EditorSession::update(std::chrono::nanoseconds elapsed) {
    if (!runtime_ || runtime_->paused || elapsed.count() <= 0) return;
    auto &r = *runtime_;
    r.accumulated += elapsed;
    // Runtime systems run once per fixed tick; none are connected to the runtime scene yet.
    int ticks = 0;
    while (r.accumulated >= fixed_step && ticks < max_ticks_per_update) { r.accumulated -= fixed_step; ++r.ticks; ++ticks; }
    // A long stall (breakpoint, window drag) drops the backlog instead of catching up in a burst.
    if (ticks == max_ticks_per_update) r.accumulated = std::min(r.accumulated, fixed_step);
}
std::uint64_t EditorSession::runtime_ticks() const { return runtime_ ? runtime_->ticks : 0; }
EntityId EditorSession::pick(const renderer::FrameSnapshot &frame, double x, double y) const {
    const auto inv = glm::inverse(glm::make_mat4(frame.camera.projection.matrix.data()) * glm::make_mat4(frame.camera.view.matrix.data()));
    auto a = inv * glm::dvec4(x, y, -1, 1), b = inv * glm::dvec4(x, y, 1, 1);
    a /= a.w; b /= b.w;
    const auto segment = glm::dvec3(b - a);
    double nearest = glm::length(segment);
    const auto direction = segment / nearest;
    EntityId nearest_entity = 0;
    for (const auto &object : frame.objects) {
        const auto inverse = glm::inverse(glm::make_mat4(object.world.matrix.data()));
        // The local ray is not normalized, so t stays a world-space distance along the pick ray.
        const glm::dvec3 origin(inverse * a), ray(inverse * glm::dvec4(direction, 0));
        const double t = hit(object.mesh, origin, ray, nearest);
        if (t >= 0 && t < nearest) { nearest = t; nearest_entity = object.entity; }
    }
    return nearest_entity;
}
} // namespace engine::editor
