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
} // namespace
Result<void> write_scene(std::ostream &out, const SceneSnapshot &s) {
    auto valid = Scene::validate(s);
    if (!valid)
        return valid.error();
    try {
        StreamState guard(out);
        out << std::dec << std::defaultfloat << std::noshowpos
            << std::setprecision(std::numeric_limits<double>::max_digits10);
        out << "ENGINE_SCENE 1 " << s.next_id << ' ' << s.entities.size() << '\n';
        for (const auto &e : s.entities) {
            const auto &t = e.transform;
            out << "ENTITY " << e.id << ' ' << e.parent << ' ' << std::quoted(e.name) << ' '
                << t.position.x << ' ' << t.position.y << ' ' << t.position.z << ' ' << t.rotation.x
                << ' ' << t.rotation.y << ' ' << t.rotation.z << ' ' << t.rotation.w << ' '
                << t.scale.x << ' ' << t.scale.y << ' ' << t.scale.z << '\n';
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
            version != 1 || !reader.number(s.next_id) || !reader.number(count))
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
