#include <engine/editor/EditorPrefs.hpp>
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
} // namespace engine::editor
