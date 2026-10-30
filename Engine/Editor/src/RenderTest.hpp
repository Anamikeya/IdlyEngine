#pragma once
#include <filesystem>
struct GLFWwindow;
namespace engine::editor {
// GPU checks of materials and lighting, by readback, in a real window. Writes captures to `output`. Returns the exit code.
int run_render_test(GLFWwindow *, const std::filesystem::path &materials, const std::filesystem::path &output);
} // namespace engine::editor
