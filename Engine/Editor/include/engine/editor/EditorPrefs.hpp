#pragma once
#include <engine/core/Result.hpp>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace engine::editor {
// The per-user settings folder: %APPDATA%\GameEngine, or the current folder if that cannot be found. One name in one
// place, so renaming the engine stays simple.
std::filesystem::path default_config_folder();
Result<std::string> read_text_file(const std::filesystem::path &);
// Replaces the file in one step, creating its folder; a failure leaves the old file as it was.
Result<void> write_text_file(const std::filesystem::path &, const std::string &text);
// Plain "key = value" lines: the editor's own small settings, such as camera speed and snap values.
class EditorSettings {
    std::map<std::string, std::string, std::less<>> values_;
public:
    void parse(std::string_view text);
    std::string text() const;
    void set(std::string_view key, double value);
    void set(std::string_view key, bool value);
    double number(std::string_view key, double fallback) const;
    bool flag(std::string_view key, bool fallback) const;
    void set_text(std::string_view key, std::string_view value);
    std::string text_of(std::string_view key) const;
};
} // namespace engine::editor
