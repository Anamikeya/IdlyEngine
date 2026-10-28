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
}
std::optional<EntityRecord> Scene::inspect(EntityId id) const {
    if (!on_owner_thread())
        throw std::logic_error("Scene inspected from non-owner thread");
    auto it = entities_.find(id);
    if (it == entities_.end())
        return std::nullopt;
    const auto e = world_.native().entity(it->second);
    return EntityRecord{id, e.get<Name>().value, e.get<Parent>().value, e.get<Transform>()};
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
        const auto &desired_transform = transforms.value().at(record.id);
        if (!e.has<WorldTransform>() || e.get<WorldTransform>().matrix != desired_transform.matrix)
            e.set<WorldTransform>(desired_transform);
    }
    next_id_ = std::max(next_id_, s.next_id);
    return {};
}
} // namespace engine
