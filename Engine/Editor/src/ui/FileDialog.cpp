#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_INCLUDE_NONE
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include "FileDialog.hpp"
#include <array>

namespace engine::editor {
namespace {
OPENFILENAMEW dialog_settings(GLFWwindow *owner, const wchar_t *title, const wchar_t *filter, std::array<wchar_t, 4096> &file) {
    OPENFILENAMEW settings{};
    settings.lStructSize = sizeof settings;
    settings.hwndOwner = owner ? glfwGetWin32Window(owner) : nullptr;
    settings.lpstrFilter = filter;
    settings.lpstrFile = file.data();
    settings.nMaxFile = DWORD(file.size());
    settings.lpstrTitle = title;
    return settings;
}
}
std::optional<std::filesystem::path> open_file_dialog(GLFWwindow *owner, const wchar_t *title, const wchar_t *filter) {
    std::array<wchar_t, 4096> file{};
    auto settings = dialog_settings(owner, title, filter, file);
    settings.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&settings)) return std::nullopt;
    return std::filesystem::path(file.data());
}
std::optional<std::filesystem::path> save_file_dialog(GLFWwindow *owner, const wchar_t *title, const wchar_t *filter, const wchar_t *extension) {
    std::array<wchar_t, 4096> file{};
    auto settings = dialog_settings(owner, title, filter, file);
    settings.lpstrDefExt = extension;
    settings.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&settings)) return std::nullopt;
    return std::filesystem::path(file.data());
}
} // namespace engine::editor
