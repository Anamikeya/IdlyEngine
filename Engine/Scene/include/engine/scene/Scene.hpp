#pragma once
#include <array>
#include <cstdint>
#include <engine/core/Result.hpp>
#include <engine/ecs/World.hpp>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <vector>
namespace engine::commands {
class CommandBus;
}
namespace engine {
using EntityId = std::uint64_t;
struct Vec3 {
    double x = 0, y = 0, z = 0;
    bool operator==(const Vec3 &) const = default;
};
struct Quaternion {
    double x = 0, y = 0, z = 0, w = 1;
    bool operator==(const Quaternion &) const = default;
};
struct Transform {
    Vec3 position;
    Quaternion rotation;
    Vec3 scale{1, 1, 1};
    bool operator==(const Transform &) const = default;
};
struct StableId {
    EntityId value;
};
struct Name {
    std::string value;
};
struct Parent {
    EntityId value = 0;
};
struct WorldTransform {
    std::array<double, 16> matrix{};
};
struct EntityRecord {
    EntityId id = 0;
    std::string name;
    EntityId parent = 0;
    Transform transform;
    bool operator==(const EntityRecord &) const = default;
};
struct SceneSnapshot {
    EntityId next_id = 1;
    std::vector<EntityRecord> entities;
    bool operator==(const SceneSnapshot &) const = default;
};
class Scene final {
    ecs::World world_;
    std::map<EntityId, flecs::entity_t> entities_;
    EntityId next_id_ = 1;
    friend class commands::CommandBus;
    Result<void> commit(const SceneSnapshot &);

  public:
    static constexpr std::size_t max_entities = 100000;
    Scene();
    Scene(const Scene &) = delete;
    Scene &operator=(const Scene &) = delete;
    bool on_owner_thread() const noexcept {
        return world_.on_owner_thread();
    }
    std::size_t size() const noexcept {
        return entities_.size();
    }
    std::optional<EntityRecord> inspect(EntityId) const;
    SceneSnapshot snapshot() const;
    std::optional<WorldTransform> world_transform(EntityId) const;
    ecs::World &ecs() noexcept {
        return world_;
    } // Trusted engine systems only.
    static Result<void> validate(const SceneSnapshot &);
};
Result<void> write_scene(std::ostream &, const SceneSnapshot &);
Result<SceneSnapshot> read_scene(std::istream &);
} // namespace engine
