#include <algorithm>
#include <charconv>
#include <engine/scene/Scene.hpp>
#include <iomanip>
#include <istream>
#include <limits>
#include <locale>
#include <ostream>
#include <set>
namespace engine {
namespace {
struct StreamState {
    std::ios &stream;
    std::locale locale;
    std::ios::fmtflags flags;
    std::streamsize precision;
    explicit StreamState(std::ios &s)
        : stream(s), locale(s.getloc()), flags(s.flags()), precision(s.precision()) {
        s.imbue(std::locale::classic());
    }
    ~StreamState() noexcept {
        try {
            stream.imbue(locale);
            stream.flags(flags);
            stream.precision(precision);
        } catch (...) {
        }
    }
};
class Reader {
    std::istream &in_;
    std::size_t bytes_ = 0;
    static constexpr std::size_t byte_limit = 64 * 1024 * 1024;
    bool exhausted_ = false;
    static bool space(int c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    }
    bool take(char &c) {
        if (bytes_ >= byte_limit) {
            exhausted_ = true;
            return false;
        }
        if (!in_.get(c))
            return false;
        ++bytes_;
        return true;
    }

  public:
    explicit Reader(std::istream &in) : in_(in) {}
    bool whitespace() {
        char c = 0;
        while (space(in_.peek()))
            if (!take(c))
                return false;
        return !in_.bad() && !exhausted_;
    }
    bool token(std::string &value, std::size_t limit = 64) {
        value.clear();
        if (!whitespace())
            return false;
        char c = 0;
        while (in_.peek() != std::char_traits<char>::eof() && !space(in_.peek())) {
            if (value.size() >= limit || !take(c))
                return false;
            value += c;
        }
        return !value.empty() && !in_.bad();
    }
    template <class T> bool number(T &value) {
        std::string text;
        if (!token(text))
            return false;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    }
    bool name(std::string &value, std::size_t limit = 256) {
        if (!whitespace())
            return false;
        char c = 0;
        if (!take(c) || c != '"')
            return false;
        while (take(c)) {
            if (c == '"')
                return true;
            if (c == '\\' && !take(c))
                return false;
            if (static_cast<unsigned char>(c) < 32 || value.size() >= limit)
                return false;
            value += c;
        }
        return false;
    }
    // "-", or 16 hex digits and a quoted path.
    bool asset_ref(AssetRef &ref) {
        std::string text;
        if (!token(text, 16))
            return false;
        if (text == "-")
            return true;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), ref.id, 16);
        return text.size() == 16 && result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
               name(ref.path, max_asset_path_bytes);
    }
    bool end() {
        return whitespace() && in_.peek() == std::char_traits<char>::eof() && !in_.bad();
    }
};
void write_ref(std::ostream &out, const AssetRef &ref) {
    if (ref.empty()) {
        out << '-';
        return;
    }
    static constexpr char digits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4)
        out << digits[(ref.id >> shift) & 15];
    out << ' ' << std::quoted(ref.path);
}
const char *mesh_token(BuiltinMesh mesh) {
    switch (mesh) {
    case BuiltinMesh::Sphere:
        return "sphere";
    case BuiltinMesh::Plane:
        return "plane";
    default:
        return "cube";
    }
}
const char *body_token(BodyKind kind) {
    return kind == BodyKind::Static ? "static" : kind == BodyKind::Kinematic ? "kinematic" : "dynamic";
}
bool parse_body(const std::string &token, BodyKind &kind) {
    for (auto candidate : {BodyKind::Static, BodyKind::Kinematic, BodyKind::Dynamic})
        if (token == body_token(candidate)) {
            kind = candidate;
            return true;
        }
    return false;
}
const char *shape_token(ColliderShape shape) {
    return shape == ColliderShape::Sphere ? "sphere" : shape == ColliderShape::Capsule ? "capsule" : "box";
}
bool parse_shape(const std::string &token, ColliderShape &shape) {
    for (auto candidate : {ColliderShape::Sphere, ColliderShape::Box, ColliderShape::Capsule})
        if (token == shape_token(candidate)) {
            shape = candidate;
            return true;
        }
    return false;
}
const char *light_token(LightType type) {
    return type == LightType::Point ? "point" : type == LightType::Spot ? "spot" : "directional";
}
const char *source_token(EnvironmentSource source) {
    return source == EnvironmentSource::None ? "none" : source == EnvironmentSource::Image ? "image" : "procedural";
}
bool parse_source(const std::string &token, EnvironmentSource &source) {
    for (auto candidate : {EnvironmentSource::None, EnvironmentSource::Procedural, EnvironmentSource::Image})
        if (token == source_token(candidate)) {
            source = candidate;
            return true;
        }
    return false;
}
const char *tone_token(ToneMapping tone) {
    switch (tone) {
    case ToneMapping::Filmic:
        return "filmic";
    case ToneMapping::Agx:
        return "agx";
    case ToneMapping::PbrNeutral:
        return "pbr_neutral";
    case ToneMapping::Linear:
        return "linear";
    default:
        return "aces";
    }
}
bool parse_tone(const std::string &token, ToneMapping &tone) {
    for (auto candidate : {ToneMapping::Aces, ToneMapping::Filmic, ToneMapping::Agx, ToneMapping::PbrNeutral, ToneMapping::Linear})
        if (token == tone_token(candidate)) {
            tone = candidate;
            return true;
        }
    return false;
}
const char *quality_token(ShadowQuality quality) {
    return quality == ShadowQuality::Off ? "off" : quality == ShadowQuality::Low ? "low" : quality == ShadowQuality::High ? "high" : "medium";
}
bool parse_quality(const std::string &token, ShadowQuality &quality) {
    for (auto candidate : {ShadowQuality::Off, ShadowQuality::Low, ShadowQuality::Medium, ShadowQuality::High})
        if (token == quality_token(candidate)) {
            quality = candidate;
            return true;
        }
    return false;
}
bool parse_mesh(const std::string &token, BuiltinMesh &mesh) {
    for (auto candidate : {BuiltinMesh::Cube, BuiltinMesh::Sphere, BuiltinMesh::Plane})
        if (token == mesh_token(candidate)) {
            mesh = candidate;
            return true;
        }
    return false;
}
} // namespace
Result<void> write_scene(std::ostream &out, const SceneSnapshot &s) {
    auto valid = Scene::validate(s);
    if (!valid)
        return valid.error();
    try {
        StreamState guard(out);
        out << std::dec << std::defaultfloat << std::noshowpos
            << std::setprecision(std::numeric_limits<double>::max_digits10);
        out << "ENGINE_SCENE " << scene_format_version << ' ' << s.next_id << ' '
            << s.entities.size() << '\n';
        for (const auto &e : s.entities) {
            const auto &t = e.transform;
            out << "ENTITY " << e.id << ' ' << e.parent << ' ' << std::quoted(e.name) << ' '
                << t.position.x << ' ' << t.position.y << ' ' << t.position.z << ' ' << t.rotation.x
                << ' ' << t.rotation.y << ' ' << t.rotation.z << ' ' << t.rotation.w << ' '
                << t.scale.x << ' ' << t.scale.y << ' ' << t.scale.z << '\n';
        }
        // docs/architecture/006-scene-format.md. Sorted by entity, then MESH, CAMERA, LIGHT, BODY,
        // COLLIDER (colliders in list order).
        std::size_t components = 0;
        for (const auto &e : s.entities)
            components += e.components.mesh.has_value() + e.components.camera.has_value() +
                          e.components.light.has_value() + e.components.body.has_value() +
                          e.components.colliders.size() + e.components.environment.has_value() +
                          e.components.behaviors.size() + e.components.emitter.has_value() +
                          (e.components.mesh ? e.components.mesh->material_slots.size() : 0);
        out << "COMPONENTS " << components << '\n';
        for (const auto &e : s.entities) {
            if (const auto &m = e.components.mesh)
            {
                out << "MESH " << e.id << ' ' << mesh_token(m->mesh) << ' ' << m->color.x << ' '
                    << m->color.y << ' ' << m->color.z << ' ' << m->roughness << ' ' << m->metallic << ' ';
                write_ref(out, m->mesh_asset);
                out << ' ';
                write_ref(out, m->texture);
                out << ' ';
                write_ref(out, m->material);
                out << ' ' << m->uv_scale_u << ' ' << m->uv_scale_v << '\n';
                for (const auto &slot : m->material_slots) {
                    out << "MATSLOT " << e.id << ' ' << slot.slot << ' ';
                    write_ref(out, slot.material);
                    out << '\n';
                }
            }
            if (const auto &c = e.components.camera)
                out << "CAMERA " << e.id << ' ' << c->vertical_fov_degrees << ' ' << c->near_plane
                    << ' ' << c->far_plane << ' ' << (c->primary ? 1 : 0) << '\n';
            if (const auto &l = e.components.light)
                out << "LIGHT " << e.id << ' '
                    << light_token(l->type) << ' ' << l->color.x
                    << ' ' << l->color.y << ' ' << l->color.z << ' ' << l->intensity << ' '
                    << l->range << ' ' << l->inner_cone_degrees << ' ' << l->outer_cone_degrees << ' '
                    << (l->cast_shadows ? 1 : 0) << '\n';
            if (const auto &b = e.components.body)
                out << "BODY " << e.id << ' ' << body_token(b->kind) << ' ' << b->linear_velocity.x
                    << ' ' << b->linear_velocity.y << ' ' << b->linear_velocity.z << ' '
                    << b->angular_velocity.x << ' ' << b->angular_velocity.y << ' '
                    << b->angular_velocity.z << ' ' << b->linear_damping << ' ' << b->angular_damping
                    << ' ' << b->gravity_scale << ' ' << (b->enable_sleep ? 1 : 0) << ' '
                    << (b->is_bullet ? 1 : 0) << ' ' << (b->lock_rotation ? 1 : 0) << '\n';
            for (const auto &k : e.components.colliders)
                out << "COLLIDER " << e.id << ' ' << shape_token(k.shape) << ' ' << k.center.x << ' '
                    << k.center.y << ' ' << k.center.z << ' ' << k.rotation.x << ' ' << k.rotation.y
                    << ' ' << k.rotation.z << ' ' << k.rotation.w << ' ' << k.half_extents.x << ' '
                    << k.half_extents.y << ' ' << k.half_extents.z << ' ' << k.radius << ' '
                    << k.half_height << ' ' << k.density << ' ' << k.friction << ' ' << k.restitution
                    << ' ' << (k.sensor ? 1 : 0) << ' ' << (k.events ? 1 : 0) << ' ' << k.category << ' '
                    << k.mask << '\n';
            if (const auto &v = e.components.environment) {
                out << "ENVIRONMENT " << e.id << ' ' << source_token(v->source) << ' ';
                write_ref(out, v->image);
                out << ' ' << v->sky_color.x << ' ' << v->sky_color.y << ' ' << v->sky_color.z << ' '
                    << v->horizon_color.x << ' ' << v->horizon_color.y << ' ' << v->horizon_color.z << ' '
                    << v->ground_color.x << ' ' << v->ground_color.y << ' ' << v->ground_color.z << ' ' << v->intensity
                    << ' ' << v->rotation_degrees << ' ' << (v->show_skybox ? 1 : 0) << ' ' << v->ev100 << ' '
                    << tone_token(v->tone_mapping) << ' ' << quality_token(v->shadow_quality) << ' ' << v->shadow_distance
                    << ' ' << (v->ambient_occlusion ? 1 : 0) << ' ' << (v->bloom ? 1 : 0) << '\n';
            }
            for (const auto &b : e.components.behaviors) {
                out << "BEHAVIOR " << e.id << ' ' << b.type << ' ' << b.values.size();
                for (const auto &[key, value] : b.values)
                    out << ' ' << key << ' ' << value;
                out << '\n';
            }
            if (const auto &p = e.components.emitter) {
                out << "EMITTER " << e.id << ' ';
                write_ref(out, p->effect);
                out << ' ' << (p->auto_play ? 1 : 0) << ' ' << (p->loop ? 1 : 0) << ' ' << p->rate_scale << '\n';
            }
        }
        if (!out)
            return fail(ErrorCode::ParseError, "Scene output stream failed");
        return {};
    } catch (const std::ios_base::failure &) {
        return fail(ErrorCode::ParseError, "Scene output stream threw an I/O failure");
    }
}
Result<SceneSnapshot> read_scene(std::istream &in) {
    try {
        Reader reader(in);
        std::string word;
        unsigned version = 0;
        std::size_t count = 0;
        SceneSnapshot s;
        if (!reader.token(word, 12) || word != "ENGINE_SCENE" || !reader.number(version) ||
            version < 1 || version > scene_format_version || !reader.number(s.next_id) ||
            !reader.number(count))
            return fail(ErrorCode::ParseError,
                        "Invalid scene header or unsupported schema version");
        if (count > Scene::max_entities)
            return fail(ErrorCode::InvalidArgument, "Scene entity limit exceeded");
        s.entities.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            EntityRecord e;
            auto &t = e.transform;
            if (!reader.token(word, 6) || word != "ENTITY" || !reader.number(e.id) ||
                !reader.number(e.parent) || !reader.name(e.name) || !reader.number(t.position.x) ||
                !reader.number(t.position.y) || !reader.number(t.position.z) ||
                !reader.number(t.rotation.x) || !reader.number(t.rotation.y) ||
                !reader.number(t.rotation.z) || !reader.number(t.rotation.w) ||
                !reader.number(t.scale.x) || !reader.number(t.scale.y) || !reader.number(t.scale.z))
                return fail(ErrorCode::ParseError,
                            "Malformed entity record or input limit exceeded");
            s.entities.push_back(std::move(e));
        }
        if (version >= 2) {
            std::size_t components = 0;
            if (!reader.token(word, 10) || word != "COMPONENTS" || !reader.number(components))
                return fail(ErrorCode::ParseError, "Missing or malformed COMPONENTS section");
            // Version 2 defined no component kinds; version 3 allows one MESH, CAMERA and LIGHT per entity;
            // version 4 adds one BODY and up to max_colliders_per_entity COLLIDER records; version 6 adds one
            // ENVIRONMENT and up to max_material_slots MATSLOT and max_behaviors_per_entity BEHAVIOR records;
            // version 7 adds one EMITTER.
            const std::size_t per_entity = version >= 7   ? 6 + max_colliders_per_entity + max_material_slots + max_behaviors_per_entity
                                           : version >= 6 ? 5 + max_colliders_per_entity + max_material_slots + max_behaviors_per_entity
                                           : version >= 4 ? 4 + max_colliders_per_entity
                                           : version >= 3 ? 3
                                                          : 0;
            if (components > per_entity * s.entities.size())
                return fail(ErrorCode::ParseError, "Too many component records");
            std::map<EntityId, std::size_t> index;
            std::set<EntityId> mesh_seen;
            for (std::size_t i = 0; i < s.entities.size(); ++i)
                index.emplace(s.entities[i].id, i);
            for (std::size_t i = 0; i < components; ++i) {
                EntityId id = 0;
                if (!reader.token(word, 16))
                    return fail(ErrorCode::ParseError, "Malformed component record");
                const std::string kind = word;
                const bool physics = kind == "BODY" || kind == "COLLIDER";
                const bool v6_only = kind == "MATSLOT" || kind == "ENVIRONMENT" || kind == "BEHAVIOR";
                const bool v7_only = kind == "EMITTER";
                if ((kind != "MESH" && kind != "CAMERA" && kind != "LIGHT" && !physics && !v6_only && !v7_only) ||
                    (physics && version < 4) || (v6_only && version < 6) || (v7_only && version < 7))
                    return fail(ErrorCode::ParseError, "Unsupported component kind '" + kind + "'");
                if (!reader.number(id) || !index.contains(id))
                    return fail(ErrorCode::ParseError, kind + " record names a missing entity");
                auto &c = s.entities[index.at(id)].components;
                bool parsed = false, duplicate = false;
                if (kind == "MESH") {
                    MeshComponent m;
                    duplicate = !mesh_seen.insert(id).second;
                    parsed = reader.token(word, 16) && parse_mesh(word, m.mesh) &&
                             reader.number(m.color.x) && reader.number(m.color.y) &&
                             reader.number(m.color.z) && reader.number(m.roughness) &&
                             reader.number(m.metallic) &&
                             (version < 5 || (reader.asset_ref(m.mesh_asset) && reader.asset_ref(m.texture))) &&
                             (version < 6 || (reader.asset_ref(m.material) && reader.number(m.uv_scale_u) && reader.number(m.uv_scale_v)));
                    // Slots may already have been read: records come in any order.
                    if (c.mesh)
                        m.material_slots = std::move(c.mesh->material_slots);
                    c.mesh = m;
                } else if (kind == "CAMERA") {
                    CameraComponent cam;
                    int primary = -1;
                    duplicate = c.camera.has_value();
                    parsed = reader.number(cam.vertical_fov_degrees) && reader.number(cam.near_plane) &&
                             reader.number(cam.far_plane) && reader.number(primary) &&
                             (primary == 0 || primary == 1);
                    cam.primary = primary == 1;
                    c.camera = cam;
                } else if (kind == "BODY") {
                    RigidBodyComponent b;
                    int sleep = -1, bullet = -1, lock = 0;
                    duplicate = c.body.has_value();
                    parsed = reader.token(word, 16) && parse_body(word, b.kind) &&
                             reader.number(b.linear_velocity.x) && reader.number(b.linear_velocity.y) &&
                             reader.number(b.linear_velocity.z) && reader.number(b.angular_velocity.x) &&
                             reader.number(b.angular_velocity.y) && reader.number(b.angular_velocity.z) &&
                             reader.number(b.linear_damping) && reader.number(b.angular_damping) &&
                             reader.number(b.gravity_scale) && reader.number(sleep) && reader.number(bullet) &&
                             (sleep == 0 || sleep == 1) && (bullet == 0 || bullet == 1) &&
                             (version < 6 || (reader.number(lock) && (lock == 0 || lock == 1)));
                    b.enable_sleep = sleep == 1;
                    b.is_bullet = bullet == 1;
                    b.lock_rotation = lock == 1;
                    c.body = b;
                } else if (kind == "COLLIDER") {
                    ColliderComponent k;
                    int sensor = -1, events = -1;
                    duplicate = c.colliders.size() >= max_colliders_per_entity;
                    parsed = reader.token(word, 16) && parse_shape(word, k.shape) && reader.number(k.center.x) &&
                             reader.number(k.center.y) && reader.number(k.center.z) &&
                             reader.number(k.rotation.x) && reader.number(k.rotation.y) &&
                             reader.number(k.rotation.z) && reader.number(k.rotation.w) &&
                             reader.number(k.half_extents.x) && reader.number(k.half_extents.y) &&
                             reader.number(k.half_extents.z) && reader.number(k.radius) &&
                             reader.number(k.half_height) && reader.number(k.density) &&
                             reader.number(k.friction) && reader.number(k.restitution) &&
                             reader.number(sensor) && reader.number(events) && (sensor == 0 || sensor == 1) &&
                             (events == 0 || events == 1) &&
                             (version < 6 || (reader.number(k.category) && reader.number(k.mask)));
                    k.sensor = sensor == 1;
                    k.events = events == 1;
                    c.colliders.push_back(k);
                } else if (kind == "MATSLOT") {
                    MaterialSlot slot;
                    parsed = reader.number(slot.slot) && reader.asset_ref(slot.material);
                    if (!c.mesh)
                        c.mesh = MeshComponent{};
                    // Kept in slot order whatever the record order; Scene::validate rejects a repeated slot.
                    auto &slots = c.mesh->material_slots;
                    duplicate = slots.size() >= max_material_slots;
                    slots.insert(std::upper_bound(slots.begin(), slots.end(), slot.slot,
                                                  [](std::uint32_t value, const MaterialSlot &item) { return value < item.slot; }),
                                 slot);
                } else if (kind == "ENVIRONMENT") {
                    EnvironmentComponent v;
                    int skybox = -1, occlusion = -1, bloom = -1;
                    std::string tone, quality;
                    duplicate = c.environment.has_value();
                    parsed = reader.token(word, 16) && parse_source(word, v.source) && reader.asset_ref(v.image) &&
                             reader.number(v.sky_color.x) && reader.number(v.sky_color.y) && reader.number(v.sky_color.z) &&
                             reader.number(v.horizon_color.x) && reader.number(v.horizon_color.y) && reader.number(v.horizon_color.z) &&
                             reader.number(v.ground_color.x) && reader.number(v.ground_color.y) && reader.number(v.ground_color.z) &&
                             reader.number(v.intensity) && reader.number(v.rotation_degrees) && reader.number(skybox) &&
                             reader.number(v.ev100) && reader.token(tone, 16) && parse_tone(tone, v.tone_mapping) &&
                             reader.token(quality, 16) && parse_quality(quality, v.shadow_quality) &&
                             reader.number(v.shadow_distance) && reader.number(occlusion) && reader.number(bloom) &&
                             (skybox == 0 || skybox == 1) && (occlusion == 0 || occlusion == 1) && (bloom == 0 || bloom == 1);
                    v.show_skybox = skybox == 1;
                    v.ambient_occlusion = occlusion == 1;
                    v.bloom = bloom == 1;
                    c.environment = v;
                } else if (kind == "BEHAVIOR") {
                    BehaviorComponent b;
                    std::size_t values = 0;
                    duplicate = c.behaviors.size() >= max_behaviors_per_entity;
                    parsed = reader.token(b.type, 32) && reader.number(values) && values <= max_behavior_values;
                    for (std::size_t v = 0; parsed && v < values; ++v) {
                        std::pair<std::string, double> entry;
                        parsed = reader.token(entry.first, 32) && reader.number(entry.second);
                        b.values.push_back(std::move(entry));
                    }
                    c.behaviors.push_back(std::move(b));
                } else if (kind == "EMITTER") {
                    ParticleEmitterComponent p;
                    int autoplay = -1, loop = -1;
                    duplicate = c.emitter.has_value();
                    parsed = reader.asset_ref(p.effect) && reader.number(autoplay) && reader.number(loop) &&
                             reader.number(p.rate_scale) && (autoplay == 0 || autoplay == 1) && (loop == 0 || loop == 1);
                    p.auto_play = autoplay == 1;
                    p.loop = loop == 1;
                    c.emitter = p;
                } else {
                    LightComponent l;
                    int shadows = 0;
                    duplicate = c.light.has_value();
                    parsed = reader.token(word, 16) && (word == "directional" || word == "point" || (version >= 6 && word == "spot")) &&
                             reader.number(l.color.x) && reader.number(l.color.y) &&
                             reader.number(l.color.z) && reader.number(l.intensity) &&
                             reader.number(l.range);
                    l.type = word == "point" ? LightType::Point : word == "spot" ? LightType::Spot : LightType::Directional;
                    if (parsed && version >= 6)
                        parsed = reader.number(l.inner_cone_degrees) && reader.number(l.outer_cone_degrees) &&
                                 reader.number(shadows) && (shadows == 0 || shadows == 1);
                    l.cast_shadows = shadows == 1;
                    c.light = l;
                }
                if (duplicate)
                    return fail(ErrorCode::ParseError, kind == "COLLIDER" ? "Too many COLLIDER records for one entity"
                                                                         : "Duplicate " + kind + " record for one entity");
                if (!parsed)
                    return fail(ErrorCode::ParseError, "Malformed " + kind + " record");
            }
        }
        // Before version 3 there were no components and the editor drew every entity as the built-in cube.
        if (version < 3)
            for (auto &e : s.entities)
                e.components.mesh = MeshComponent{};
        if (!reader.end())
            return fail(ErrorCode::ParseError, "Trailing scene content or input limit exceeded");
        auto valid = Scene::validate(s);
        if (!valid)
            return valid.error();
        return s;
    } catch (const std::ios_base::failure &) {
        return fail(ErrorCode::ParseError, "Scene input stream threw an I/O failure");
    }
}

} // namespace engine
