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
struct SetEnvironment {
    EntityId id;
    std::optional<EnvironmentComponent> environment;
};
// Replaces the entity's whole behavior list; an empty list removes them all.
struct SetBehaviors {
    EntityId id;
    std::vector<BehaviorComponent> behaviors;
};
struct SetParticleEmitter {
    EntityId id;
    std::optional<ParticleEmitterComponent> emitter;
};
struct SetAnimator {
    EntityId id;
    std::optional<AnimatorComponent> animator;
};
struct SetAttachToBone {
    EntityId id;
    std::optional<AttachToBoneComponent> attach;
};
using Command = std::variant<CreateEntity, DeleteEntity, RenameEntity, SetTransform, SetParent,
                             ReplaceScene, SetMesh, SetCamera, SetLight, DuplicateEntity,
                             SetRigidBody, SetColliders, SetEnvironment, SetBehaviors, SetParticleEmitter,
                             SetAnimator, SetAttachToBone>;
struct CommandDescriptor {
    std::string_view name;
    Capability permission;
    std::string_view contract;
    std::string_view label; // What undo history calls it when the caller names nothing better.
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
        std::string name;
        std::vector<Delta> deltas;
    };
    Scene &scene_;
    std::uint64_t revision_ = 0;
    std::deque<History> undo_, redo_;
    std::size_t history_limit_;
    std::optional<History> open_; // Open transaction: what every execute since begin_transaction() changed.
    std::uint64_t recorded_ = 0;
    bool executing_ = false;
    void record(History);
    static void merge(History &into, std::vector<Delta> deltas);
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
    // Counts the undo steps ever added; an observer compares it to learn that one was.
    std::uint64_t recorded() const noexcept {
        return recorded_;
    }
    // The newest undo step's name; empty when there is none.
    std::string_view latest_name() const noexcept {
        return undo_.empty() ? std::string_view{} : std::string_view(undo_.back().name);
    }
    // For a host whose history also holds other kinds of step: a new step of another kind ends the redo chain here too.
    void discard_redo() noexcept {
        redo_.clear();
    }
    // Everything executed until end_transaction() becomes one undo step, while each execute still validates and commits,
    // so a live drag is drawn as it happens. Undo and redo are refused meanwhile. A transaction that changes nothing
    // adds no step and keeps the redo history. A bus without history has nothing to merge, so these only track the state.
    bool in_transaction() const noexcept {
        return open_.has_value();
    }
    Result<void> begin_transaction(std::string name);
    Result<void> end_transaction();
    // Restores what the open transaction changed. Refused on a bus without history: it keeps no record to restore from.
    Result<Receipt> abort_transaction(const Context &);
    Result<SceneSnapshot> inspect(const Context &) const;
    // `name` labels the undo step; empty uses the first command's label. Ignored inside a transaction, which has its own.
    Result<Receipt> execute(const Context &, std::uint64_t expected_revision,
                            std::span<const Command>, std::string_view name = {});
    Result<Receipt> execute(const Context &c, std::uint64_t r, Command cmd, std::string_view name = {}) {
        return execute(c, r, std::span<const Command>(&cmd, 1), name);
    }
    Result<Receipt> undo(const Context &c, std::uint64_t r) {
        return replay(c, r, true);
    }
    Result<Receipt> redo(const Context &c, std::uint64_t r) {
        return replay(c, r, false);
    }
};
} // namespace engine::commands
