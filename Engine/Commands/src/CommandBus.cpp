#include <algorithm>
#include <array>
#include <engine/commands/CommandBus.hpp>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
namespace engine::commands {
namespace {
constexpr std::array<CommandDescriptor, 18> descriptors{
    {{"entity.create", Capability::Edit, "name, transform, parent, optional new stable ID, components", "Create Entity"},
     {"entity.delete", Capability::Edit, "stable ID; deletes subtree", "Delete Entity"},
     {"entity.rename", Capability::Edit, "stable ID, name", "Rename Entity"},
     {"transform.set", Capability::Edit, "stable ID, finite local TRS", "Change Transform"},
     {"hierarchy.reparent", Capability::Edit, "stable ID, parent; preserves local transform", "Reparent Entity"},
     {"scene.replace", Capability::Edit, "validated versioned snapshot", "Replace Scene"},
     {"component.mesh.set", Capability::Edit, "stable ID, built-in mesh and surface, or none to remove", "Change Mesh"},
     {"component.camera.set", Capability::Edit, "stable ID, perspective lens and primary flag, or none to remove", "Change Camera"},
     {"component.light.set", Capability::Edit, "stable ID, light type, color, intensity, range, or none to remove", "Change Light"},
     {"entity.duplicate", Capability::Edit, "stable ID, optional name for the copy; copies the subtree with new IDs", "Duplicate Entity"},
     {"component.body.set", Capability::Edit, "stable ID, rigid body kind and initial motion, or none to remove", "Change Rigid Body"},
     {"component.colliders.set", Capability::Edit, "stable ID, complete collider list (empty removes all)", "Change Colliders"},
     {"component.environment.set", Capability::Edit, "stable ID, environment lighting, exposure and shadow settings, or none to remove", "Change Environment"},
     {"component.behaviors.set", Capability::Edit, "stable ID, complete behavior list (empty removes all)", "Change Behaviors"},
     {"component.emitter.set", Capability::Edit, "stable ID, particle effect, auto play, loop and rate scale, or none to remove", "Change Particle Emitter"},
     {"component.animator.set", Capability::Edit, "stable ID, clip source model, clip, speed, loop, start time and edit preview, or none to remove", "Change Animator"},
     {"component.attach.set", Capability::Edit, "stable ID, joint name of the parent's model, or none to remove", "Change Attach To Bone"},
     {"component.ragdoll.set", Capability::Edit, "stable ID, ragdoll trigger, density, joint limits and friction, and settings per bone, or none to remove", "Change Ragdoll"}}};
struct Guard {
    bool &busy;
    explicit Guard(bool &b) : busy(b) {
        busy = true;
    }
    ~Guard() {
        busy = false;
    }
};
using Records = std::map<EntityId, EntityRecord>;
Records records(const SceneSnapshot &s) {
    Records r;
    for (const auto &e : s.entities)
        r.emplace(e.id, e);
    return r;
}
SceneSnapshot snapshot(const Records &r, EntityId next) {
    SceneSnapshot s;
    s.next_id = next;
    s.entities.reserve(r.size());
    for (const auto &[id, e] : r) {
        (void)id;
        s.entities.push_back(e);
    }
    return s;
}
std::optional<EntityRecord> get(const Records &r, EntityId id) {
    auto i = r.find(id);
    if (i == r.end())
        return std::nullopt;
    return i->second;
}
} // namespace
std::span<const CommandDescriptor> registry() noexcept {
    return descriptors;
}
const CommandDescriptor &describe(const Command &c) noexcept {
    return descriptors[c.index()];
}
CommandBus::CommandBus(Scene &s, std::size_t limit) : scene_(s), history_limit_(limit) {}
Result<void> CommandBus::authorize(const Context &c, Capability p) const {
    if (!scene_.on_owner_thread())
        return fail(ErrorCode::WrongThread, "Command access requires the world owner thread");
    if (executing_)
        return fail(ErrorCode::InvalidState, "Reentrant command execution is prohibited");
    if (c.actor.empty() || c.actor.size() > 128)
        return fail(ErrorCode::InvalidArgument,
                    "Actor identifier is required and limited to 128 bytes");
    if (!c.allows(p))
        return fail(ErrorCode::PermissionDenied,
                    "Required capability was not granted by the trusted host");
    return {};
}
Result<SceneSnapshot> CommandBus::inspect(const Context &c) const {
    auto auth = authorize(c, Capability::Inspect);
    if (!auth)
        return auth.error();
    return scene_.snapshot();
}
void CommandBus::merge(History &into, std::vector<Delta> deltas) {
    for (auto &delta : deltas) {
        const auto existing = std::ranges::find(into.deltas, delta.id, &Delta::id);
        if (existing == into.deltas.end())
            into.deltas.push_back(std::move(delta));
        else
            existing->after = std::move(delta.after);
    }
}
void CommandBus::record(History history) {
    redo_.clear();
    if (!history_limit_)
        return;
    undo_.push_back(std::move(history));
    ++recorded_;
    while (undo_.size() > history_limit_)
        undo_.pop_front();
}
Result<void> CommandBus::begin_transaction(std::string name) {
    if (open_)
        return fail(ErrorCode::InvalidState, "A transaction is already open");
    open_ = History{std::move(name), {}};
    return {};
}
Result<void> CommandBus::end_transaction() {
    if (!open_)
        return fail(ErrorCode::InvalidState, "No transaction is open");
    auto history = std::move(*open_);
    open_.reset();
    std::erase_if(history.deltas, [](const Delta &d) { return d.before == d.after; });
    if (!history.deltas.empty())
        record(std::move(history));
    return {};
}
Result<Receipt> CommandBus::abort_transaction(const Context &ctx) {
    auto auth = authorize(ctx, Capability::Edit);
    if (!auth)
        return auth.error();
    if (!open_)
        return fail(ErrorCode::InvalidState, "No transaction is open");
    if (!history_limit_)
        return fail(ErrorCode::InvalidState, "A bus without history cannot restore a transaction");
    auto history = std::move(*open_);
    open_.reset();
    Guard lock(executing_);
    const auto current = scene_.snapshot();
    auto staged = records(current);
    for (const auto &d : history.deltas) {
        if (d.before)
            staged[d.id] = *d.before;
        else
            staged.erase(d.id);
    }
    auto applied = scene_.commit(snapshot(staged, current.next_id));
    if (!applied)
        return applied.error();
    return Receipt{++revision_, {}, history.deltas.size()};
}
Result<Receipt> CommandBus::execute(const Context &ctx, std::uint64_t expected,
                                    std::span<const Command> batch, std::string_view name) {
    auto auth = authorize(ctx, Capability::Edit);
    if (!auth)
        return auth.error();
    if (expected != revision_)
        return fail(ErrorCode::Conflict, "Stale scene revision");
    if (batch.empty() || batch.size() > max_transaction)
        return fail(ErrorCode::InvalidArgument, "Transaction must contain 1..256 commands");
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        return fail(ErrorCode::InvalidState, "Revision space exhausted");
    Guard lock(executing_);
    const auto before = scene_.snapshot();
    const auto original = records(before);
    auto staged = original;
    auto next = before.next_id;
    Receipt receipt;
    for (std::size_t index = 0; index < batch.size(); ++index) {
        const auto status = std::visit(
            [&](const auto &cmd) -> Result<void> {
                using T = std::decay_t<decltype(cmd)>;
                if constexpr (std::is_same_v<T, CreateEntity>) {
                    EntityId id = cmd.requested_id ? cmd.requested_id : next;
                    if (id < next || id == std::numeric_limits<EntityId>::max())
                        return fail(ErrorCode::InvalidArgument,
                                    "Requested ID is recycled or exhausted");
                    if (staged.size() >= Scene::max_entities)
                        return fail(ErrorCode::InvalidArgument, "Scene entity limit exceeded");
                    staged.emplace(id, EntityRecord{id, cmd.name, cmd.parent, cmd.transform, cmd.components});
                    next = id + 1;
                    receipt.created.push_back(id);
                } else if constexpr (std::is_same_v<T, DuplicateEntity>) {
                    const auto source = staged.find(cmd.id);
                    if (source == staged.end())
                        return fail(ErrorCode::NotFound, "Entity does not exist");
                    std::map<EntityId, std::vector<EntityId>> children;
                    for (const auto &[id, e] : staged)
                        children[e.parent].push_back(id);
                    // (original, parent of its copy), breadth first; parents are always copied first.
                    std::vector<std::pair<EntityId, EntityId>> queue{{cmd.id, source->second.parent}};
                    std::map<EntityId, bool> visited; // A staged cycle must not loop forever.
                    for (std::size_t i = 0; i < queue.size(); ++i) {
                        const auto [original, parent] = queue[i];
                        if (!visited.emplace(original, true).second)
                            continue;
                        if (staged.size() >= Scene::max_entities || next == std::numeric_limits<EntityId>::max())
                            return fail(ErrorCode::InvalidArgument, "Scene entity limit exceeded");
                        auto copy = staged.at(original);
                        copy.id = next++;
                        copy.parent = parent;
                        if (i == 0 && !cmd.name.empty())
                            copy.name = cmd.name;
                        receipt.created.push_back(copy.id);
                        if (const auto kids = children.find(original); kids != children.end())
                            for (const auto child : kids->second)
                                queue.emplace_back(child, copy.id);
                        staged.emplace(copy.id, std::move(copy));
                    }
                } else if constexpr (std::is_same_v<T, ReplaceScene>) {
                    auto valid = Scene::validate(cmd.snapshot);
                    if (!valid)
                        return valid.error();
                    staged = records(cmd.snapshot);
                    next = std::max(next, cmd.snapshot.next_id);
                } else {
                    auto it = staged.find(cmd.id);
                    if (it == staged.end())
                        return fail(ErrorCode::NotFound, "Entity does not exist");
                    if constexpr (std::is_same_v<T, RenameEntity>)
                        it->second.name = cmd.name;
                    else if constexpr (std::is_same_v<T, SetTransform>)
                        it->second.transform = cmd.transform;
                    else if constexpr (std::is_same_v<T, SetParent>)
                        it->second.parent = cmd.parent;
                    else if constexpr (std::is_same_v<T, SetMesh>)
                        it->second.components.mesh = cmd.mesh;
                    else if constexpr (std::is_same_v<T, SetCamera>)
                        it->second.components.camera = cmd.camera;
                    else if constexpr (std::is_same_v<T, SetLight>)
                        it->second.components.light = cmd.light;
                    else if constexpr (std::is_same_v<T, SetRigidBody>)
                        it->second.components.body = cmd.body;
                    else if constexpr (std::is_same_v<T, SetColliders>)
                        it->second.components.colliders = cmd.colliders;
                    else if constexpr (std::is_same_v<T, SetEnvironment>)
                        it->second.components.environment = cmd.environment;
                    else if constexpr (std::is_same_v<T, SetBehaviors>)
                        it->second.components.behaviors = cmd.behaviors;
                    else if constexpr (std::is_same_v<T, SetParticleEmitter>)
                        it->second.components.emitter = cmd.emitter;
                    else if constexpr (std::is_same_v<T, SetAnimator>)
                        it->second.components.animator = cmd.animator;
                    else if constexpr (std::is_same_v<T, SetAttachToBone>)
                        it->second.components.attach = cmd.attach;
                    else if constexpr (std::is_same_v<T, SetRagdoll>)
                        it->second.components.ragdoll = cmd.ragdoll;
                    else if constexpr (std::is_same_v<T, DeleteEntity>) {
                        std::map<EntityId, std::vector<EntityId>> children;
                        for (const auto &[id, e] : staged)
                            children[e.parent].push_back(id);
                        std::vector<EntityId> remove{cmd.id};
                        // Temporary invalid cycles must not make deletion loop forever.
                        std::map<EntityId, bool> visited;
                        for (std::size_t i = 0; i < remove.size(); ++i) {
                            auto id = remove[i];
                            if (visited.contains(id))
                                continue;
                            visited.emplace(id, true);
                            auto child = children.find(id);
                            if (child != children.end())
                                remove.insert(remove.end(), child->second.begin(),
                                              child->second.end());
                            staged.erase(id);
                        }
                    }
                }
                return {};
            },
            batch[index]);
        if (!status) {
            auto err = status.error();
            err.operation = index;
            return err;
        }
    }
    auto after = snapshot(staged, next);
    auto valid = Scene::validate(after);
    if (!valid) {
        auto err = valid.error();
        err.operation = batch.size() - 1;
        return err;
    }
    History history{std::string(name.empty() ? describe(batch.front()).label : name), {}};
    for (const auto &[id, e] : original) {
        auto dest = get(staged, id);
        if (!dest || *dest != e)
            history.deltas.push_back({id, e, std::move(dest)});
    }
    for (const auto &[id, e] : staged)
        if (!original.contains(id))
            history.deltas.push_back({id, std::nullopt, e});
    if (history.deltas.empty() && after.next_id == before.next_id) {
        receipt.revision = revision_;
        return receipt;
    }
    auto applied = scene_.commit(after);
    if (!applied)
        return applied.error();
    receipt.revision = ++revision_;
    receipt.changed_entities = history.deltas.size();
    if (open_) {
        // Physics publishes through a history-free bus every tick, so nothing is kept where it cannot be used.
        if (history_limit_)
            merge(*open_, std::move(history.deltas));
    } else
        record(std::move(history));
    return receipt;
}
Result<Receipt> CommandBus::replay(const Context &ctx, std::uint64_t expected, bool is_undo) {
    auto auth = authorize(ctx, Capability::Edit);
    if (!auth)
        return auth.error();
    if (expected != revision_)
        return fail(ErrorCode::Conflict, "Stale scene revision");
    if (open_)
        return fail(ErrorCode::InvalidState, "Finish the transaction before undo or redo");
    auto &from = is_undo ? undo_ : redo_;
    auto &to = is_undo ? redo_ : undo_;
    if (from.empty())
        return fail(ErrorCode::InvalidState, "History is empty");
    if (revision_ == std::numeric_limits<std::uint64_t>::max())
        return fail(ErrorCode::InvalidState, "Revision space exhausted");
    Guard lock(executing_);
    const auto current = scene_.snapshot();
    auto staged = records(current);
    for (const auto &d : from.back().deltas) {
        const auto &required = is_undo ? d.after : d.before;
        if (get(staged, d.id) != required)
            return fail(ErrorCode::Conflict, "Managed scene state changed outside command history");
        const auto &value = is_undo ? d.before : d.after;
        if (value)
            staged[d.id] = *value;
        else
            staged.erase(d.id);
    }
    auto applied = scene_.commit(snapshot(staged, current.next_id));
    if (!applied)
        return applied.error();
    Receipt r{++revision_, {}, from.back().deltas.size()};
    to.push_back(std::move(from.back()));
    from.pop_back();
    return r;
}
} // namespace engine::commands
