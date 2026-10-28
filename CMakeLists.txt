cmake_minimum_required(VERSION 3.25)

project(GameEngine
	VERSION 0.1.0
	DESCRIPTION "Agent-first C++20 game engine"
	LANGUAGES C CXX
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
	message(FATAL_ERROR "In-source builds are not supported. Use a preset: cmake --preset windows-msvc-debug")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dynamic CRT (/MD, /MDd) to match engine::core. Any dependency that forces the static CRT must be overridden.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(GAMEENGINE_BUILD_TESTS "Build engine tests and register them with CTest" ON)
option(GAMEENGINE_WARNINGS_AS_ERRORS "Treat warnings in engine-owned targets as errors" OFF)
option(GAMEENGINE_WITH_PHYSICS "Build the Box3D physics subsystem" ON)
option(GAMEENGINE_WITH_GNS "Build the GameNetworkingSockets transport (requires protobuf)" OFF)
option(GAMEENGINE_BUILD_EDITOR "Provide GLFW/ImGui targets and build Engine/Editor when present" ON)

include(GameEngineCompilerOptions)
include(GameEngineDependencies)
if(GAMEENGINE_BUILD_EDITOR)
	include(GameEngineEditorDependencies)
endif()

if(GAMEENGINE_BUILD_TESTS)
	enable_testing()
	include(GameEngineTesting)
endif()

# Codex-owned core. Included only once it exists on this branch.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Core/CMakeLists.txt")
	set(ENGINE_DEPENDENCY_ROOT "${GAMEENGINE_RESOURCES_DIR}" CACHE PATH "Dependency root consumed by engine::core" FORCE)
	add_subdirectory(Engine/Core)
endif()

# Include the render snapshot module in the combined build and CTest run.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Renderer/CMakeLists.txt")
    add_subdirectory(Engine/Renderer)
endif()

if(GAMEENGINE_WITH_PHYSICS)
	add_subdirectory(Engine/Physics)
endif()
# Claude-owned CPU asset pipeline; depends on engine::core for the shared Result type.
if(TARGET engine::core AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Assets/CMakeLists.txt")
	add_subdirectory(Engine/Assets)
endif()
foreach(module IN ITEMS Networking OnlineServices)
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Engine/${module}/CMakeLists.txt")
		add_subdirectory(Engine/${module})
	endif()
endforeach()

# Codex-owned editor (contract C-6). Links gameengine::imgui / gameengine::glfw and the engine libraries.
if(GAMEENGINE_BUILD_EDITOR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Editor/CMakeLists.txt")
	add_subdirectory(Engine/Editor)
endif()

if(GAMEENGINE_BUILD_TESTS)
	add_subdirectory(Tests)
endif()

message(STATUS "GameEngine ${PROJECT_VERSION}: physics=${GAMEENGINE_WITH_PHYSICS} gns=${GAMEENGINE_WITH_GNS} editor=${GAMEENGINE_BUILD_EDITOR} tests=${GAMEENGINE_BUILD_TESTS}")
