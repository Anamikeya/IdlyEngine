#include <algorithm>
#include <cmath>
#include <engine/scene/Scene.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <limits>
#include <stdexcept>
namespace engine {
namespace {
bool unit(double v) { return std::isfinite(v) && v >= 0 && v <= 1; }
bool color(const Vec3 &c) { return unit(c.x) && unit(c.y) && unit(c.z); }
Result<void> validate_components(const Components &c) {
    if (const auto &m = c.mesh) {
        if (m->mesh != BuiltinMesh::Cube && m->mesh != BuiltinMesh::Sphere && m->mesh != BuiltinMesh::Plane)
            return fail(ErrorCode::InvalidArgument, "Unknown built-in mesh");
        if (!color(m->color) || !unit(m->roughness) || !unit(m->metallic))
            return fail(ErrorCode::InvalidArgument, "Mesh color, roughness and metallic must be within [0, 1]");
    }
    if (const auto &cam = c.camera) {
        if (!std::isfinite(cam->vertical_fov_degrees) || cam->vertical_fov_degrees < 1 || cam->vertical_fov_degrees > 179 ||
            !std::isfinite(cam->near_plane) || !std::isfinite(cam->far_plane) || cam->near_plane <= 0 ||
            cam->far_plane <= cam->near_plane)
            return fail(ErrorCode::InvalidArgument, "Camera needs a 1..179 degree field of view and 0 < near < far");
    }
    if (const auto &l = c.light) {
        if (l->type != LightType::Directional && l->type != LightType::Point)
            return fail(ErrorCode::InvalidArgument, "Unknown light type");
        if (!color(l->color) || !std::isfinite(l->intensity) || l->intensity < 0 || !std::isfinite(l->range) ||
            l->range <= 0)
            return fail(ErrorCode::InvalidArgument, "Light color must be within [0, 1], intensity >= 0 and range > 0");
    }
    return {};
}
template <class T> void mirror(flecs::entity e, const std::optional<T> &value) {
    if (!value) {
        if (e.has<T>())
            e.remove<T>();
    } else if (!e.has<T>() || e.get<T>() != *value)
        e.set<T>(*value);
}
template <class T> std::optional<T> read(flecs::entity e) {
    if (!e.has<T>())
        return std::nullopt;
    return e.get<T>();
}
Result<std::map<EntityId, WorldTransform>> matrices(const SceneSnapshot &s) {
    if (s.entities.size() > Scene::max_entities)
        return fail(ErrorCode::InvalidArgument, "Scene entity limit exceeded");
    if (s.next_id == 0)
        return fail(ErrorCode::InvalidArgument, "next_id must be nonzero");
    std::map<EntityId, const EntityRecord *> records;
    std::map<EntityId, std::vector<EntityId>> children;
    EntityId previous = 0;
    for (const auto &e : s.entities) {
        if (e.id <= previous || e.id >= s.next_id)
            return fail(ErrorCode::InvalidArgument,
                        "IDs must be sorted, unique, nonzero and below next_id");
        previous = e.id;
        if (e.name.empty() || e.name.size() > 256 ||
            std::any_of(e.name.begin(), e.name.end(), [](unsigned char c) { return c < 32; }))
            return fail(ErrorCode::InvalidArgument, "Names must contain 1..256 non-control bytes");
        const auto &t = e.transform;
        const double values[] = {t.position.x, t.position.y, t.position.z, t.rotation.x,
                                 t.rotation.y, t.rotation.z, t.rotation.w, t.scale.x,
                                 t.scale.y,    t.scale.z};
        for (double v : values)
            if (!std::isfinite(v))
                return fail(ErrorCode::InvalidArgument, "Transform contains a non-finite value");
        double norm = t.rotation.x * t.rotation.x + t.rotation.y * t.rotation.y +
                      t.rotation.z * t.rotation.z + t.rotation.w * t.rotation.w;
        if (std::abs(norm - 1) > 1e-6)
            return fail(ErrorCode::InvalidArgument, "Quaternion must have unit length");
        if (std::abs(t.scale.x) < 1e-8 || std::abs(t.scale.y) < 1e-8 || std::abs(t.scale.z) < 1e-8)
            return fail(ErrorCode::InvalidArgument, "Scale must be invertible");
        if (auto components = validate_components(e.components); !components)
            return components.error();
        records.emplace(e.id, &e);
        children[e.parent].push_back(e.id);
    }
    for (const auto &e : s.entities)
        if (e.parent && !records.contains(e.parent))
            return fail(ErrorCode::NotFound, "Parent entity does not exist");
    std::vector<EntityId> queue = children[0];
    std::map<EntityId, WorldTransform> out;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        const auto &e = *records.at(queue[i]);
        const auto &t = e.transform;
        auto local =
            glm::translate(glm::dmat4(1), glm::dvec3(t.position.x, t.position.y, t.position.z)) *
            glm::mat4_cast(glm::dquat(t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z)) *
            glm::scale(glm::dmat4(1), glm::dvec3(t.scale.x, t.scale.y, t.scale.z));
        auto world = e.parent ? glm::make_mat4(out.at(e.parent).matrix.data()) * local : local;
        WorldTransform value;
        std::copy_n(glm::value_ptr(world), 16, value.matrix.begin());
        for (double v : value.matrix)
            if (!std::isfinite(v))
                return fail(ErrorCode::InvalidArgument, "World transform overflow");
        out.emplace(e.id, value);
        const auto it = children.find(e.id);
        if (it != children.end())
            queue.insert(queue.end(), it->second.begin(), it->second.end());
    }
    if (out.size() != s.entities.size())
        return fail(ErrorCode::Cycle, "Transform hierarchy contains a cycle");
    return out;
}
} // namespace
Scene::Scene() {
    auto &w = world_.native();
    w.component<StableId>();
    w.component<Name>();
    w.component<Parent>();
    w.component<Transform>();
    w.component<WorldTransform>();
    w.component<MeshComponent>();
    w.component<CameraComponent>();
    w.component<LightComponent>();
}
std::optional<EntityRecord> Scene::inspect(EntityId id) const {
    if (!on_owner_thread())
        throw std::logic_error("Scene inspected from non-owner thread");
    auto it = entities_.find(id);
    if (it == entities_.end())
        return std::nullopt;
    const auto e = world_.native().entity(it->second);
    return EntityRecord{id, e.get<Name>().value, e.get<Parent>().value, e.get<Transform>(),
                        {read<MeshComponent>(e), read<CameraComponent>(e), read<LightComponent>(e)}};
}
SceneSnapshot Scene::snapshot() const {
    if (!on_owner_thread())
        throw std::logic_error("Scene snapshot from non-owner thread");
    SceneSnapshot s;
    s.next_id = next_id_;
    s.entities.reserve(entities_.size());
    for (const auto &[id, native] : entities_) {
        (void)native;
        s.entities.push_back(*inspect(id));
    }
    return s;
}
std::optional<WorldTransform> Scene::world_transform(EntityId id) const {
    if (!on_owner_thread())
        throw std::logic_error("World transform from non-owner thread");
    auto it = entities_.find(id);
    if (it == entities_.end())
        return std::nullopt;
    return world_.native().entity(it->second).get<WorldTransform>();
}
Result<void> Scene::validate(const SceneSnapshot &s) {
    auto m = matrices(s);
    if (!m)
        return m.error();
    return {};
}
Result<void> Scene::commit(const SceneSnapshot &s) {
    if (!on_owner_thread())
        return fail(ErrorCode::WrongThread, "Scene mutation requires the owner thread");
    auto transforms = matrices(s);
    if (!transforms)
        return transforms.error();
    auto &w = world_.native();
    // Only the owner thread can observe this commit. Native observers must not emit external
    // effects.
    std::map<EntityId, const EntityRecord *> desired;
    for (const auto &e : s.entities)
        desired.emplace(e.id, &e);
    for (auto it = entities_.begin(); it != entities_.end();) {
        if (!desired.contains(it->first)) {
            w.entity(it->second).destruct();
            it = entities_.erase(it);
        } else
            ++it;
    }
    for (const auto &record : s.entities) {
        auto it = entities_.find(record.id);
        if (it == entities_.end()) {
            auto e = w.entity();
            entities_.emplace(record.id, e.id());
            e.set<StableId>({record.id})
                .set<Name>({record.name})
                .set<Parent>({record.parent})
                .set<Transform>(record.transform);
        } else {
            auto e = w.entity(it->second);
            if (e.get<Name>().value != record.name)
                e.set<Name>({record.name});
            if (e.get<Parent>().value != record.parent)
                e.set<Parent>({record.parent});
            if (e.get<Transform>() != record.transform)
                e.set<Transform>(record.transform);
        }
        auto e = w.entity(entities_.at(record.id));
        mirror(e, record.components.mesh);
        mirror(e, record.components.camera);
        mirror(e, record.components.light);
        const auto &desired_transform = transforms.value().at(record.id);
        if (!e.has<WorldTransform>() || e.get<WorldTransform>().matrix != desired_transform.matrix)
            e.set<WorldTransform>(desired_transform);
    }
    next_id_ = std::max(next_id_, s.next_id);
    return {};
}
} // namespace engine
