cmake_minimum_required(VERSION 3.25)
# Standalone entry point (cmake -S Engine/Editor): configure the root foundation build, which adds this
# directory back as a subdirectory with a different binary dir. (GameEngine_SOURCE_DIR cannot be the guard:
# project() caches it, so a re-configure would skip this branch.)
if(CMAKE_CURRENT_BINARY_DIR STREQUAL CMAKE_BINARY_DIR)
    project(GameEngineEditor LANGUAGES C CXX)
    # CTest reads CTestTestfile.cmake from the top binary dir, which only exists when testing is enabled here.
    enable_testing()
    if(NOT WIN32 OR NOT MSVC)
        message(FATAL_ERROR "The M2 desktop adapter currently supports Windows/MSVC only")
    endif()
    set(GAMEENGINE_RESOURCES_DIR "" CACHE PATH "Existing read-only dependency sources")
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Local staging" FORCE)
    set(GAMEENGINE_BUILD_EDITOR ON CACHE BOOL "" FORCE)
    add_subdirectory(../.. foundation)
    return()
endif()
if(NOT WIN32 OR NOT MSVC)
    message(STATUS "Engine/Editor skipped: the M2 desktop adapter currently supports Windows/MSVC only")
    return()
endif()
gameengine_verify_dependency(filament)
set(USE_STATIC_CRT OFF CACHE BOOL "" FORCE)
set(FILAMENT_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "Do not register dependency maintenance tests" FORCE)
set(FILAMENT_SKIP_SAMPLES ON CACHE BOOL "" FORCE)
set(FILAMENT_SKIP_SDL2 ON CACHE BOOL "" FORCE)
set(FILAMENT_SUPPORTS_OPENGL ON CACHE BOOL "" FORCE)
set(FILAMENT_SUPPORTS_VULKAN OFF CACHE BOOL "" FORCE)
set(FILAMENT_SUPPORTS_WEBGPU OFF CACHE BOOL "" FORCE)
set(FILAMENT_ENABLE_MATDBG OFF CACHE BOOL "" FORCE)
# ON (upstream default) adds /D_ITERATOR_DEBUG_LEVEL=0 to Debug, which cannot link against the engine's Debug libraries.
set(FILAMENT_SHORTEN_MSVC_COMPILATION OFF CACHE BOOL "" FORCE)
# Filament's glslang force-caches CMAKE_INSTALL_PREFIX to the relative "install" when the prefix was defaulted, after
# which every re-configure fails in Box3D's file(RELATIVE_PATH). Directory scope: hides the flag from Filament only.
set(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT OFF)
# Upstream concatenates this path with its source root even for export. Redirect
# the generated tool manifest back to our build tree, keeping resources read-only.
# file(RELATIVE_PATH) cannot cross drives, so the build dir must be on the same drive as GAMEENGINE_RESOURCES_DIR.
file(RELATIVE_PATH IMPORT_EXECUTABLES_DIR "${GAMEENGINE_RESOURCES_DIR}/filament" "${CMAKE_CURRENT_BINARY_DIR}/filament-tools")
add_subdirectory("${GAMEENGINE_RESOURCES_DIR}/filament" filament EXCLUDE_FROM_ALL SYSTEM)
# GLFW comes from the root (cmake/GameEngineEditorDependencies.cmake) as the `glfw` target.
set(IMGUI_ROOT "${GAMEENGINE_RESOURCES_DIR}/imgui")
add_library(editor_imgui STATIC
    "${IMGUI_ROOT}/imgui.cpp" "${IMGUI_ROOT}/imgui_draw.cpp"
    "${IMGUI_ROOT}/imgui_widgets.cpp" "${IMGUI_ROOT}/imgui_tables.cpp"
    "${IMGUI_ROOT}/backends/imgui_impl_glfw.cpp"
    "${GAMEENGINE_RESOURCES_DIR}/box3d/extern/imguizmo/ImGuizmo.cpp")
target_include_directories(editor_imgui SYSTEM PUBLIC "${IMGUI_ROOT}" "${IMGUI_ROOT}/backends"
    "${GAMEENGINE_RESOURCES_DIR}/box3d/extern/imguizmo")
target_link_libraries(editor_imgui PUBLIC glfw)

add_library(engine_editor_session STATIC src/EditorSession.cpp)
target_include_directories(engine_editor_session PUBLIC include)
target_include_directories(engine_editor_session SYSTEM PRIVATE "${GAMEENGINE_RESOURCES_DIR}/glm")
# Play mode simulates physics, so the editor needs the physics bridge (GAMEENGINE_WITH_PHYSICS=ON).
if(NOT TARGET engine::physics_scene)
    message(FATAL_ERROR "Engine/Editor requires engine::physics_scene; configure with GAMEENGINE_WITH_PHYSICS=ON")
endif()
target_link_libraries(engine_editor_session PUBLIC engine::renderer engine::physics_scene)
target_compile_options(engine_editor_session PRIVATE /W4 /WX /permissive- /utf-8)
add_executable(engine_editor_tests tests/EditorTests.cpp)
target_link_libraries(engine_editor_tests PRIVATE engine_editor_session)
target_include_directories(engine_editor_tests SYSTEM PRIVATE "${GAMEENGINE_RESOURCES_DIR}/glm")
target_compile_options(engine_editor_tests PRIVATE /W4 /WX /permissive- /utf-8)
add_test(NAME engine.editor.acceptance COMMAND engine_editor_tests)
set_tests_properties(engine.editor.acceptance PROPERTIES TIMEOUT 60)
set(RENDERER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../Renderer")
set(MATERIAL_DIR "${CMAKE_CURRENT_BINARY_DIR}/materials")
file(MAKE_DIRECTORY "${MATERIAL_DIR}")
foreach(name ui lit grid)
    add_custom_command(OUTPUT "${MATERIAL_DIR}/${name}.filamat"
        COMMAND matc -p desktop -a opengl -o "${MATERIAL_DIR}/${name}.filamat" "${RENDERER_ROOT}/materials/${name}.mat"
        DEPENDS matc "${RENDERER_ROOT}/materials/${name}.mat" VERBATIM)
    list(APPEND EDITOR_MATERIALS "${MATERIAL_DIR}/${name}.filamat")
endforeach()
add_custom_target(editor_materials DEPENDS ${EDITOR_MATERIALS})
add_library(engine_desktop_renderer STATIC "${RENDERER_ROOT}/src/desktop/DesktopRenderer.cpp" "${RENDERER_ROOT}/src/desktop/ImGuiRenderer.cpp")
target_link_libraries(engine_desktop_renderer PUBLIC engine::renderer PRIVATE filament editor_imgui)
target_compile_options(engine_desktop_renderer PRIVATE /W4 /permissive- /utf-8)
add_dependencies(engine_desktop_renderer editor_materials)
add_executable(engine_editor src/Main.cpp)
target_link_libraries(engine_editor PRIVATE engine_editor_session engine_desktop_renderer editor_imgui)
target_include_directories(engine_editor SYSTEM PRIVATE "${GAMEENGINE_RESOURCES_DIR}/glm")
target_compile_definitions(engine_editor PRIVATE EDITOR_MATERIAL_DIR="${MATERIAL_DIR}")
target_compile_options(engine_editor PRIVATE /W4 /permissive- /utf-8)
option(ENGINE_EDITOR_GPU_TESTS "Register visible desktop GPU smoke test (requires interactive desktop)" OFF)
if(ENGINE_EDITOR_GPU_TESTS)
    add_test(NAME engine.editor.gpu COMMAND engine_editor --smoke)
    set_tests_properties(engine.editor.gpu PROPERTIES TIMEOUT 120 LABELS "gpu;desktop")
endif()
