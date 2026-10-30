#pragma once
#include <engine/core/Result.hpp>
#include <filesystem>
#include <string>

namespace engine::editor {
// The per-user settings folder: %APPDATA%\GameEngine, or the current folder if that cannot be found. One name in one
// place, so renaming the engine stays simple.
std::filesystem::path default_config_folder();
Result<std::string> read_text_file(const std::filesystem::path &);
// Replaces the file in one step, creating its folder; a failure leaves the old file as it was.
Result<void> write_text_file(const std::filesystem::path &, const std::string &text);
} // namespace engine::editor
