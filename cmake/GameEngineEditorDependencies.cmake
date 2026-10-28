# Window and UI dependencies for the editor (contract C-6). Built from the read-only resources checkout at the
# commits pinned in DependencyLock.cmake; nothing is downloaded. Both targets are EXCLUDE_FROM_ALL, so they are only
# compiled when something links them (Codex's editor, or the headless smoke test).
#
#   gameengine::glfw   GLFW, static, dynamic CRT
#   gameengine::imgui  Dear ImGui (docking) core + GLFW platform backend. No renderer backend: that is the
#                      renderer's choice (Filament ships its own ImGui integration).

gameengine_verify_dependency(glfw)
gameengine_verify_dependency(imgui)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
# Matches CMAKE_MSVC_RUNTIME_LIBRARY (/MD, /MDd) set by the root.
set(USE_MSVC_RUNTIME_LIBRARY_DLL ON CACHE BOOL "" FORCE)
add_subdirectory("${GAMEENGINE_RESOURCES_DIR}/glfw" "${CMAKE_BINARY_DIR}/_deps/glfw" EXCLUDE_FROM_ALL SYSTEM)
set_target_properties(glfw PROPERTIES FOLDER "ThirdParty")
add_library(gameengine::glfw ALIAS glfw)

set(_imgui_dir "${GAMEENGINE_RESOURCES_DIR}/imgui")
add_library(gameengine_imgui STATIC EXCLUDE_FROM_ALL
	"${_imgui_dir}/imgui.cpp"
	"${_imgui_dir}/imgui_demo.cpp"
	"${_imgui_dir}/imgui_draw.cpp"
	"${_imgui_dir}/imgui_tables.cpp"
	"${_imgui_dir}/imgui_widgets.cpp"
	"${_imgui_dir}/backends/imgui_impl_glfw.cpp"
)
add_library(gameengine::imgui ALIAS gameengine_imgui)
target_include_directories(gameengine_imgui SYSTEM PUBLIC "${_imgui_dir}" "${_imgui_dir}/backends")
target_link_libraries(gameengine_imgui PUBLIC glfw)
target_compile_features(gameengine_imgui PUBLIC cxx_std_20)
set_target_properties(gameengine_imgui PROPERTIES FOLDER "ThirdParty")
set(GAMEENGINE_IMGUI_DIR "${_imgui_dir}" CACHE INTERNAL "Dear ImGui source directory, for optional backends")
unset(_imgui_dir)
