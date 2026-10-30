#pragma once
#include <deque>
#include <engine/scene/Scene.hpp>
#include <span>
#include <string_view>
#include <variant>
namespace engine::commands {
enum class Capability : std::uint32_t { Inspect = 1, Edit = 2 };
struct Context {
    std::string actor;
    std::uint32_t grants = 0; // Trusted host sets this, never accept it from remote clients.
    bool allows(Capability p) const noexcept {
        return (grants & static_cast<std::uint32_t>(p)) != 0;
    }
};
struct CreateEntity {
    std::string name;
    Transform transform;
    EntityId parent = 0;
    EntityId requested_id = 0;
    Components components;
};
struct DeleteEntity {
    EntityId id;
};
struct RenameEntity {
    EntityId id;
    std::string name;
};
struct SetTransform {
    EntityId id;
    Transform transform;
};
struct SetParent {
    EntityId id;
    EntityId parent;
};
struct ReplaceScene {
    SceneSnapshot snapshot;
};
// Component commands add or replace a component; an empty optional removes it.
struct SetMesh {
    EntityId id;
    std::optional<MeshComponent> mesh;
};
struct SetCamera {
    EntityId id;
    std::optional<CameraComponent> camera;
};
struct SetLight {
    EntityId id;
    std::optional<LightComponent> light;
};
// Copies the entity and its whole subtree, components included, under the same parent. Copies get new
// stable IDs in breadth-first order (Receipt::created, the copy of `id` first). An empty name keeps the
// original name for the copied root; descendants always keep theirs.
struct DuplicateEntity {
    EntityId id;
    std::string name;
};
struct SetRigidBody {
    EntityId id;
    std::optional<RigidBodyComponent> body;
};
// Replaces the entity's whole collider list; an empty list removes all colliders.
struct SetColliders {
    EntityId id;
    std::vector<ColliderComponent> colliders;
};
using Command = std::variant<CreateEntity, DeleteEntity, RenameEntity, SetTransform, SetParent,
                             ReplaceScene, SetMesh, SetCamera, SetLight, DuplicateEntity,
                             SetRigidBody, SetColliders>;
struct CommandDescriptor {
    std::string_view name;
    Capability permission;
    std::string_view contract;
};
std::span<const CommandDescriptor> registry() noexcept;
const CommandDescriptor &describe(const Command &) noexcept;
struct Receipt {
    std::uint64_t revision = 0;
    std::vector<EntityId> created;
    std::size_t changed_entities = 0;
};
class CommandBus final {
    struct Delta {
        EntityId id;
        std::optional<EntityRecord> before, after;
    };
    struct History {
        std::vector<Delta> deltas;
    };
    Scene &scene_;
    std::uint64_t revision_ = 0;
    std::deque<History> undo_, redo_;
    std::size_t history_limit_;
    bool executing_ = false;
    Result<void> authorize(const Context &, Capability) const;
    Result<Receipt> replay(const Context &, std::uint64_t, bool undo);

  public:
    explicit CommandBus(Scene &scene, std::size_t history_limit = 128);
    CommandBus(const CommandBus &) = delete;
    CommandBus &operator=(const CommandBus &) = delete;
    static constexpr std::size_t max_transaction = 256;
    std::uint64_t revision() const noexcept {
        return revision_;
    }
    std::size_t undo_size() const noexcept {
        return undo_.size();
    }
    std::size_t redo_size() const noexcept {
        return redo_.size();
    }
    Result<SceneSnapshot> inspect(const Context &) const;
    Result<Receipt> execute(const Context &, std::uint64_t expected_revision,
                            std::span<const Command>);
    Result<Receipt> execute(const Context &c, std::uint64_t r, Command cmd) {
        return execute(c, r, std::span<const Command>(&cmd, 1));
    }
    Result<Receipt> undo(const Context &c, std::uint64_t r) {
        return replay(c, r, true);
    }
    Result<Receipt> redo(const Context &c, std::uint64_t r) {
        return replay(c, r, false);
    }
};
} // namespace engine::commands
