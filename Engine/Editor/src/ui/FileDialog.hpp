#pragma once
#include <filesystem>
#include <optional>

struct GLFWwindow;
namespace engine::editor {
// The system's file dialogs. They block until closed. Empty when cancelled. `filter` is Win32's "Name\0*.ext\0\0".
std::optional<std::filesystem::path> open_file_dialog(GLFWwindow *owner, const wchar_t *title, const wchar_t *filter);
std::optional<std::filesystem::path> save_file_dialog(GLFWwindow *owner, const wchar_t *title, const wchar_t *filter, const wchar_t *extension);
} // namespace engine::editor
