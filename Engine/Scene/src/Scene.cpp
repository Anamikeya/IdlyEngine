#include <algorithm>
#include <cmath>
#include <engine/scene/Scene.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <limits>
#include <stdexcept>
#include <type_traits>
namespace engine {
namespace {
bool unit(double v) { return std::isfinite(v) && v >= 0 && v <= 1; }
bool color(const Vec3 &c) { return unit(c.x) && unit(c.y) && unit(c.z); }
bool valid_ref(const AssetRef &ref) {
    if (ref.empty())
        return ref.path.empty();
    return !ref.path.empty() && ref.path.size() <= max_asset_path_bytes &&
           std::none_of(ref.path.begin(), ref.path.end(), [](char c) { return static_cast<unsigned char>(c) < 32; });
}
Result<void> validate_components(const Components &c) {
    if (const auto &m = c.mesh) {
        if (m->mesh != BuiltinMesh::Cube && m->mesh != BuiltinMesh::Sphere && m->mesh != BuiltinMesh::Plane)
            return fail(ErrorCode::InvalidArgument, "Unknown built-in mesh");
        if (!color(m->color) || !unit(m->roughness) || !unit(m->metallic))
            return fail(ErrorCode::InvalidArgument, "Mesh color, roughness and metallic must be within [0, 1]");
        if (!valid_ref(m->mesh_asset) || !valid_ref(m->texture) || !valid_ref(m->material))
            return fail(ErrorCode::InvalidArgument, "An asset reference needs both an ID and a short path, or neither");
        if (!std::isfinite(m->uv_scale_u) || !std::isfinite(m->uv_scale_v) || m->uv_scale_u <= 0 || m->uv_scale_v <= 0)
            return fail(ErrorCode::InvalidArgument, "Mesh UV scale must be positive");
        if (m->material_slots.size() > max_material_slots)
            return fail(ErrorCode::InvalidArgument, "Too many material slots on one mesh");
        std::uint32_t next_slot = 0;
        for (const auto &slot : m->material_slots) {
            if (slot.slot < next_slot || slot.material.empty() || !valid_ref(slot.material))
                return fail(ErrorCode::InvalidArgument, "Material slots need ascending indices and a material reference");
            next_slot = slot.slot + 1;
        }
    }
    if (const auto &cam = c.camera) {
        if (!std::isfinite(cam->vertical_fov_degrees) || cam->vertical_fov_degrees < 1 || cam->vertical_fov_degrees > 179 ||
            !std::isfinite(cam->near_plane) || !std::isfinite(cam->far_plane) || cam->near_plane <= 0 ||
            cam->far_plane <= cam->near_plane)
            return fail(ErrorCode::InvalidArgument, "Camera needs a 1..179 degree field of view and 0 < near < far");
    }
    if (const auto &l = c.light) {
        if (l->type != LightType::Directional && l->type != LightType::Point && l->type != LightType::Spot)
            return fail(ErrorCode::InvalidArgument, "Unknown light type");
        if (!std::isfinite(l->inner_cone_degrees) || !std::isfinite(l->outer_cone_degrees) || l->inner_cone_degrees < 0 ||
            l->inner_cone_degrees > l->outer_cone_degrees || l->outer_cone_degrees > 90)
            return fail(ErrorCode::InvalidArgument, "Spot cones need 0 <= inner <= outer <= 90 degrees");
        if (!color(l->color) || !std::isfinite(l->intensity) || l->intensity < 0 || !std::isfinite(l->range) ||
            l->range <= 0)
            return fail(ErrorCode::InvalidArgument, "Light color must be within [0, 1], intensity >= 0 and range > 0");
    }
    const auto finite = [](const Vec3 &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    const auto nonnegative = [](double v) { return std::isfinite(v) && v >= 0; };
    if (const auto &v = c.environment) {
        if (v->source != EnvironmentSource::None && v->source != EnvironmentSource::Procedural && v->source != EnvironmentSource::Image)
            return fail(ErrorCode::InvalidArgument, "Unknown environment source");
        if (!valid_ref(v->image) || (v->source == EnvironmentSource::Image && v->image.empty()))
            return fail(ErrorCode::InvalidArgument, "An image environment needs an image reference");
        if (!color(v->sky_color) || !color(v->horizon_color) || !color(v->ground_color) || !nonnegative(v->intensity) ||
            !std::isfinite(v->rotation_degrees) || !std::isfinite(v->ev100) || v->ev100 < -20 || v->ev100 > 30 ||
            !(v->shadow_distance > 0) || !std::isfinite(v->shadow_distance))
            return fail(ErrorCode::InvalidArgument, "Environment colors must be within [0, 1], intensity >= 0, EV100 within [-20, 30] and shadow distance > 0");
        if (v->tone_mapping < ToneMapping::Aces || v->tone_mapping > ToneMapping::Linear || v->shadow_quality > ShadowQuality::High)
            return fail(ErrorCode::InvalidArgument, "Unknown tone mapping or shadow quality");
    }
    const auto identifier = [](const std::string &text) {
        return !text.empty() && text.size() <= 32 && std::all_of(text.begin(), text.end(), [](char ch) {
            return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
        });
    };
    if (c.behaviors.size() > max_behaviors_per_entity)
        return fail(ErrorCode::InvalidArgument, "Too many behaviors on one entity");
    for (std::size_t i = 0; i < c.behaviors.size(); ++i) {
        const auto &b = c.behaviors[i];
        if (!identifier(b.type) || b.values.size() > max_behavior_values)
            return fail(ErrorCode::InvalidArgument, "A behavior needs a type of 1..32 of [a-z0-9_] and at most 32 values");
        for (std::size_t j = 0; j < i; ++j)
            if (c.behaviors[j].type == b.type)
                return fail(ErrorCode::InvalidArgument, "A behavior type appears twice on one entity");
        for (std::size_t j = 0; j < b.values.size(); ++j) {
            if (!identifier(b.values[j].first) || !std::isfinite(b.values[j].second))
                return fail(ErrorCode::InvalidArgument, "Behavior values need a key of 1..32 of [a-z0-9_] and a finite number");
            for (std::size_t k = 0; k < j; ++k)
                if (b.values[k].first == b.values[j].first)
                    return fail(ErrorCode::InvalidArgument, "A behavior key appears twice");
        }
    }
    if (const auto &p = c.emitter) {
        if (!valid_ref(p->effect))
            return fail(ErrorCode::InvalidArgument, "An asset reference needs both an ID and a short path, or neither");
        if (!std::isfinite(p->rate_scale) || p->rate_scale < 0 || p->rate_scale > 1000)
            return fail(ErrorCode::InvalidArgument, "Emitter rate scale must be within [0, 1000]");
    }
    // Clip and bone names: what a model file may call them, without control characters.
    const auto joint_name = [](const std::string &text, bool allow_empty) {
        return (allow_empty || !text.empty()) && text.size() <= max_clip_name_bytes &&
               std::none_of(text.begin(), text.end(), [](char ch) { return static_cast<unsigned char>(ch) < 32 || ch == 127; });
    };
    if (const auto &a = c.animator) {
        if (!valid_ref(a->clips))
            return fail(ErrorCode::InvalidArgument, "An asset reference needs both an ID and a short path, or neither");
        if (!joint_name(a->clip, true))
            return fail(ErrorCode::InvalidArgument, "A clip name is at most 256 bytes, without control characters");
        if (!std::isfinite(a->speed) || std::abs(a->speed) > 10 || !std::isfinite(a->start_time) || a->start_time < 0 || a->start_time > 3600)
            return fail(ErrorCode::InvalidArgument, "Animator speed must be within [-10, 10] and start time within [0, 3600] s");
    }
    if (const auto &b = c.attach; b && !joint_name(b->bone, false))
        return fail(ErrorCode::InvalidArgument, "A bone name is 1 to 256 bytes, without control characters");
    if (const auto &r = c.ragdoll) {
        const auto within = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; };
        if (r->trigger != RagdollTrigger::OnDeath && r->trigger != RagdollTrigger::OnDemand && r->trigger != RagdollTrigger::Never)
            return fail(ErrorCode::InvalidArgument, "Unknown ragdoll trigger");
        if (!within(r->density, 1, 100000) || !within(r->swing_degrees, 0, 180) || !within(r->twist_degrees, 0, 178) ||
            !within(r->joint_friction, 0, 10))
            return fail(ErrorCode::InvalidArgument,
                        "Ragdoll density must be within [1, 100000], swing [0, 180], twist [0, 178] and joint friction [0, 10]");
        if (r->bones.size() > max_ragdoll_bones)
            return fail(ErrorCode::InvalidArgument, "A ragdoll has settings for at most 256 bones");
        for (std::size_t i = 0; i < r->bones.size(); ++i) {
            const auto &b = r->bones[i];
            if (!joint_name(b.bone, false))
                return fail(ErrorCode::InvalidArgument, "A bone name is 1 to 256 bytes, without control characters");
            for (std::size_t k = 0; k < i; ++k)
                if (r->bones[k].bone == b.bone)
                    return fail(ErrorCode::InvalidArgument, "A ragdoll has settings for bone '" + b.bone + "' twice");
            if ((b.body != RagdollBody::Auto && b.body != RagdollBody::Own && b.body != RagdollBody::None) || !within(b.radius, 0, 100) ||
                !within(b.density, 0, 100000) || !(b.swing_degrees == -1 || within(b.swing_degrees, 0, 180)) ||
                !(b.twist_degrees == -1 || within(b.twist_degrees, 0, 178)))
                return fail(ErrorCode::InvalidArgument, "Ragdoll bone '" + b.bone +
                                                            "': radius within [0, 100], density [0, 100000], swing -1 or [0, 180], twist -1 or [0, 178]");
        }
    }
    if (const auto &s = c.script) {
        if (!valid_ref(s->script))
            return fail(ErrorCode::InvalidArgument, "An asset reference needs both an ID and a short path, or neither");
        if (s->properties.size() > max_script_properties)
            return fail(ErrorCode::InvalidArgument, "A script component holds at most 64 property values");
        const auto lua_name = [](const std::string &text) {
            const auto letter = [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_'; };
            return !text.empty() && text.size() <= 64 && letter(text[0]) &&
                   std::all_of(text.begin(), text.end(), [&](char ch) { return letter(ch) || (ch >= '0' && ch <= '9'); });
        };
        for (std::size_t i = 0; i < s->properties.size(); ++i) {
            const auto &p = s->properties[i];
            if (!lua_name(p.name))
                return fail(ErrorCode::InvalidArgument, "A script property is named by 1 to 64 letters, digits or '_', not starting with a digit");
            for (std::size_t k = 0; k < i; ++k)
                if (s->properties[k].name == p.name)
                    return fail(ErrorCode::InvalidArgument, "Script property '" + p.name + "' appears twice");
            const bool valid = std::visit(
                [&](const auto &v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, double>)
                        return std::isfinite(v);
                    else if constexpr (std::is_same_v<T, std::string>)
                        return v.size() <= max_script_text_bytes &&
                               std::none_of(v.begin(), v.end(), [](char ch) { return static_cast<unsigned char>(ch) < 32; });
                    else if constexpr (std::is_same_v<T, Vec3>)
                        return finite(v);
                    else if constexpr (std::is_same_v<T, ScriptColor>)
                        return unit(v.r) && unit(v.g) && unit(v.b) && unit(v.a);
                    else if constexpr (std::is_same_v<T, AssetRef>)
                        return valid_ref(v);
                    else
                        return true;
                },
                p.value);
            if (!valid)
                return fail(ErrorCode::InvalidArgument, "Script property '" + p.name +
                                                            "': numbers and vectors must be finite, colors within [0, 1], text one line of at most 4096 bytes");
        }
    }
    if (const auto &b = c.body) {
        if (b->kind != BodyKind::Static && b->kind != BodyKind::Kinematic && b->kind != BodyKind::Dynamic)
            return fail(ErrorCode::InvalidArgument, "Unknown rigid body kind");
        if (!finite(b->linear_velocity) || !finite(b->angular_velocity) || !nonnegative(b->linear_damping) ||
            !nonnegative(b->angular_damping) || !std::isfinite(b->gravity_scale))
            return fail(ErrorCode::InvalidArgument, "Rigid body values must be finite and damping >= 0");
    }
    if (c.colliders.size() > max_colliders_per_entity)
        return fail(ErrorCode::InvalidArgument, "Too many colliders on one entity");
    for (const auto &k : c.colliders) {
        const auto &q = k.rotation;
        if (k.shape != ColliderShape::Sphere && k.shape != ColliderShape::Box && k.shape != ColliderShape::Capsule)
            return fail(ErrorCode::InvalidArgument, "Unknown collider shape");
        if (!finite(k.center) || !std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w) ||
            std::abs(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w - 1) > 1e-6)
            return fail(ErrorCode::InvalidArgument, "Collider placement must be finite with a unit-length rotation");
        if (!(k.half_extents.x > 0) || !(k.half_extents.y > 0) || !(k.half_extents.z > 0) || !finite(k.half_extents) ||
            !(k.radius > 0) || !std::isfinite(k.radius) || !nonnegative(k.half_height))
            return fail(ErrorCode::InvalidArgument, "Collider sizes must be positive (half height >= 0)");
        if (!nonnegative(k.density) || !nonnegative(k.friction) || !unit(k.restitution))
            return fail(ErrorCode::InvalidArgument, "Collider density and friction must be >= 0, restitution within [0, 1]");
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
// Flecs component holding an entity's collider list; absent when the list is empty.
struct ColliderList {
    std::vector<ColliderComponent> items;
    bool operator==(const ColliderList &) const = default;
};
void mirror(flecs::entity e, const std::vector<ColliderComponent> &colliders) {
    mirror(e, colliders.empty() ? std::nullopt : std::optional<ColliderList>(ColliderList{colliders}));
}
std::vector<ColliderComponent> read_colliders(flecs::entity e) {
    return e.has<ColliderList>() ? e.get<ColliderList>().items : std::vector<ColliderComponent>{};
}
struct BehaviorList {
    std::vector<BehaviorComponent> items;
    bool operator==(const BehaviorList &) const = default;
};
void mirror(flecs::entity e, const std::vector<BehaviorComponent> &behaviors) {
    mirror(e, behaviors.empty() ? std::nullopt : std::optional<BehaviorList>(BehaviorList{behaviors}));
}
std::vector<BehaviorComponent> read_behaviors(flecs::entity e) {
    return e.has<BehaviorList>() ? e.get<BehaviorList>().items : std::vector<BehaviorComponent>{};
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
    w.component<RigidBodyComponent>();
    w.component<ColliderList>();
    w.component<EnvironmentComponent>();
    w.component<BehaviorList>();
    w.component<ParticleEmitterComponent>();
    w.component<AnimatorComponent>();
    w.component<AttachToBoneComponent>();
    w.component<RagdollComponent>();
    w.component<ScriptComponent>();
}
std::optional<EntityRecord> Scene::inspect(EntityId id) const {
    if (!on_owner_thread())
        throw std::logic_error("Scene inspected from non-owner thread");
    auto it = entities_.find(id);
    if (it == entities_.end())
        return std::nullopt;
    const auto e = world_.native().entity(it->second);
    return EntityRecord{id, e.get<Name>().value, e.get<Parent>().value, e.get<Transform>(),
                        {read<MeshComponent>(e), read<CameraComponent>(e), read<LightComponent>(e),
                         read<RigidBodyComponent>(e), read_colliders(e), read<EnvironmentComponent>(e), read_behaviors(e),
                         read<ParticleEmitterComponent>(e), read<AnimatorComponent>(e), read<AttachToBoneComponent>(e),
                         read<RagdollComponent>(e), read<ScriptComponent>(e)}};
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
        mirror(e, record.components.body);
        mirror(e, record.components.colliders);
        mirror(e, record.components.environment);
        mirror(e, record.components.behaviors);
        mirror(e, record.components.emitter);
        mirror(e, record.components.animator);
        mirror(e, record.components.attach);
        mirror(e, record.components.ragdoll);
        mirror(e, record.components.script);
        const auto &desired_transform = transforms.value().at(record.id);
        if (!e.has<WorldTransform>() || e.get<WorldTransform>().matrix != desired_transform.matrix)
            e.set<WorldTransform>(desired_transform);
    }
    next_id_ = std::max(next_id_, s.next_id);
    return {};
}
} // namespace engine
