#include <charconv>
#include <engine/scene/Scene.hpp>
#include <iomanip>
#include <istream>
#include <limits>
#include <locale>
#include <ostream>
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
    bool name(std::string &value) {
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
            if (static_cast<unsigned char>(c) < 32 || value.size() >= 256)
                return false;
            value += c;
        }
        return false;
    }
    bool end() {
        return whitespace() && in_.peek() == std::char_traits<char>::eof() && !in_.bad();
    }
};
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
        // docs/architecture/006-scene-format.md. Sorted by entity, then MESH, CAMERA, LIGHT.
        std::size_t components = 0;
        for (const auto &e : s.entities)
            components += e.components.mesh.has_value() + e.components.camera.has_value() +
                          e.components.light.has_value();
        out << "COMPONENTS " << components << '\n';
        for (const auto &e : s.entities) {
            if (const auto &m = e.components.mesh)
                out << "MESH " << e.id << ' ' << mesh_token(m->mesh) << ' ' << m->color.x << ' '
                    << m->color.y << ' ' << m->color.z << ' ' << m->roughness << ' ' << m->metallic
                    << '\n';
            if (const auto &c = e.components.camera)
                out << "CAMERA " << e.id << ' ' << c->vertical_fov_degrees << ' ' << c->near_plane
                    << ' ' << c->far_plane << ' ' << (c->primary ? 1 : 0) << '\n';
            if (const auto &l = e.components.light)
                out << "LIGHT " << e.id << ' '
                    << (l->type == LightType::Point ? "point" : "directional") << ' ' << l->color.x
                    << ' ' << l->color.y << ' ' << l->color.z << ' ' << l->intensity << ' '
                    << l->range << '\n';
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
            // Version 2 defined no component kinds; version 3 allows one of each kind per entity.
            if (components > (version >= 3 ? 3 * s.entities.size() : 0))
                return fail(ErrorCode::ParseError, "Too many component records");
            std::map<EntityId, std::size_t> index;
            for (std::size_t i = 0; i < s.entities.size(); ++i)
                index.emplace(s.entities[i].id, i);
            for (std::size_t i = 0; i < components; ++i) {
                EntityId id = 0;
                if (!reader.token(word, 16))
                    return fail(ErrorCode::ParseError, "Malformed component record");
                const std::string kind = word;
                if (kind != "MESH" && kind != "CAMERA" && kind != "LIGHT")
                    return fail(ErrorCode::ParseError, "Unsupported component kind '" + kind + "'");
                if (!reader.number(id) || !index.contains(id))
                    return fail(ErrorCode::ParseError, kind + " record names a missing entity");
                auto &c = s.entities[index.at(id)].components;
                bool parsed = false, duplicate = false;
                if (kind == "MESH") {
                    MeshComponent m;
                    duplicate = c.mesh.has_value();
                    parsed = reader.token(word, 16) && parse_mesh(word, m.mesh) &&
                             reader.number(m.color.x) && reader.number(m.color.y) &&
                             reader.number(m.color.z) && reader.number(m.roughness) &&
                             reader.number(m.metallic);
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
                } else {
                    LightComponent l;
                    duplicate = c.light.has_value();
                    parsed = reader.token(word, 16) && (word == "directional" || word == "point") &&
                             reader.number(l.color.x) && reader.number(l.color.y) &&
                             reader.number(l.color.z) && reader.number(l.intensity) &&
                             reader.number(l.range);
                    l.type = word == "point" ? LightType::Point : LightType::Directional;
                    c.light = l;
                }
                if (duplicate)
                    return fail(ErrorCode::ParseError, "Duplicate " + kind + " record for one entity");
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
