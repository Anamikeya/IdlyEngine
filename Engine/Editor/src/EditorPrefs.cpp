#include <engine/editor/EditorPrefs.hpp>
#include <array>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

namespace engine::editor {
std::filesystem::path default_config_folder() {
    constexpr const char *engine_folder = "GameEngine";
#ifdef _WIN32
    wchar_t *value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, L"APPDATA") == 0 && value) {
        std::filesystem::path folder(value);
        std::free(value);
        return folder / engine_folder;
    }
#endif
    return std::filesystem::current_path() / engine_folder;
}
Result<std::string> read_text_file(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return fail(ErrorCode::NotFound, "Cannot open " + path.generic_string());
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}
Result<void> write_text_file(const std::filesystem::path &path, const std::string &text) {
    std::error_code error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
    if (error) return fail(ErrorCode::InvalidState, "Cannot create " + path.parent_path().generic_string() + ": " + error.message());
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        stream << text;
        stream.flush();
        if (!stream) return fail(ErrorCode::InvalidState, "Cannot write " + temp.generic_string());
    }
    std::filesystem::rename(temp, path, error);
    if (error) return fail(ErrorCode::InvalidState, "Cannot replace " + path.generic_string() + ": " + error.message());
    return {};
}
void EditorSettings::parse(std::string_view text) {
    const auto trim = [](std::string_view v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\r' || v.front() == '\t')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\t')) v.remove_suffix(1);
        return v;
    };
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = trim(text.substr(0, end));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        const auto equals = line.find('=');
        if (line.empty() || line.front() == '#' || equals == std::string_view::npos) continue;
        values_.insert_or_assign(std::string(trim(line.substr(0, equals))), std::string(trim(line.substr(equals + 1))));
    }
}
std::string EditorSettings::text() const {
    std::string text;
    for (const auto &[key, value] : values_) text += key + " = " + value + "\n";
    return text;
}
void EditorSettings::set(std::string_view key, double value) {
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error == std::errc{}) values_.insert_or_assign(std::string(key), std::string(buffer.data(), end));
}
void EditorSettings::set(std::string_view key, bool value) { values_.insert_or_assign(std::string(key), value ? "1" : "0"); }
double EditorSettings::number(std::string_view key, double fallback) const {
    const auto found = values_.find(key);
    if (found == values_.end()) return fallback;
    double value = fallback;
    const auto &text = found->second;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() ? value : fallback;
}
bool EditorSettings::flag(std::string_view key, bool fallback) const {
    const auto found = values_.find(key);
    return found == values_.end() ? fallback : found->second == "1";
}
} // namespace engine::editor
