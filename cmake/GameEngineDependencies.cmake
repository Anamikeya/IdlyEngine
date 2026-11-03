# Resolves the shared, read-only third-party directory and adds the dependencies the engine links.
# Nothing here downloads or writes inside GAMEENGINE_RESOURCES_DIR; all build output goes to the binary dir.

include(DependencyLock)

if(NOT DEFINED GAMEENGINE_RESOURCES_DIR OR GAMEENGINE_RESOURCES_DIR STREQUAL "")
	if(DEFINED ENV{GAMEENGINE_RESOURCES_DIR})
		set(_gameengine_default_resources "$ENV{GAMEENGINE_RESOURCES_DIR}")
	else()
		set(_gameengine_default_resources "${CMAKE_SOURCE_DIR}/resources")
	endif()
	set(GAMEENGINE_RESOURCES_DIR "${_gameengine_default_resources}" CACHE PATH "Shared third-party source directory")
endif()
file(TO_CMAKE_PATH "${GAMEENGINE_RESOURCES_DIR}" GAMEENGINE_RESOURCES_DIR)

if(NOT IS_DIRECTORY "${GAMEENGINE_RESOURCES_DIR}")
	message(FATAL_ERROR
		"GAMEENGINE_RESOURCES_DIR does not exist: '${GAMEENGINE_RESOURCES_DIR}'.\n"
		"Set the GAMEENGINE_RESOURCES_DIR environment variable or pass -DGAMEENGINE_RESOURCES_DIR=<path>.")
endif()
message(STATUS "GameEngine resources: ${GAMEENGINE_RESOURCES_DIR}")

option(GAMEENGINE_STRICT_DEPENDENCY_LOCK "Fail configuration when a dependency is not at its locked commit" OFF)
find_package(Git QUIET)

# Compares the checkout in GAMEENGINE_RESOURCES_DIR/<name> against DependencyLock.cmake.
function(gameengine_verify_dependency name)
	set(dir "${GAMEENGINE_RESOURCES_DIR}/${name}")
	if(NOT IS_DIRECTORY "${dir}")
		message(FATAL_ERROR "Required dependency '${name}' not found at ${dir}")
	endif()

	set(expected "")
	foreach(entry IN LISTS GAMEENGINE_DEPENDENCY_LOCK)
		string(REPLACE "|" ";" fields "${entry}")
		list(GET fields 0 entry_name)
		if(entry_name STREQUAL name)
			list(GET fields 2 expected)
		endif()
	endforeach()
	if(expected STREQUAL "")
		message(FATAL_ERROR "Dependency '${name}' has no entry in cmake/DependencyLock.cmake")
	endif()

	set(actual "")
	if(GIT_FOUND AND EXISTS "${dir}/.git")
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" -C "${dir}" rev-parse HEAD
			OUTPUT_VARIABLE actual
			RESULT_VARIABLE git_result
			ERROR_QUIET
			OUTPUT_STRIP_TRAILING_WHITESPACE)
		if(NOT git_result EQUAL 0)
			set(actual "")
		endif()
	endif()

	if(actual STREQUAL "")
		message(STATUS "Dependency ${name}: revision unverifiable (no git metadata), expected ${expected}")
	elseif(NOT actual STREQUAL expected)
		set(msg "Dependency ${name} is at ${actual}, lock expects ${expected}")
		if(GAMEENGINE_STRICT_DEPENDENCY_LOCK)
			message(FATAL_ERROR "${msg}")
		else()
			message(WARNING "${msg}")
		endif()
	else()
		message(STATUS "Dependency ${name}: ${actual} (locked)")
	endif()
endfunction()

# ---- Box3D ------------------------------------------------------------------------------------
if(GAMEENGINE_WITH_PHYSICS)
	gameengine_verify_dependency(box3d)

	# Box3D hides samples, tests and docs when it is not the top-level project.
	set(BUILD_SHARED_LIBS OFF)
	set(BOX3D_DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
	set(BOX3D_DISABLE_SIMD OFF CACHE BOOL "" FORCE)
	add_subdirectory("${GAMEENGINE_RESOURCES_DIR}/box3d" "${CMAKE_BINARY_DIR}/_deps/box3d" EXCLUDE_FROM_ALL SYSTEM)
	set_target_properties(box3d PROPERTIES FOLDER "ThirdParty")

	# MinGW-w64 GCC does not realign the stack to 32 bytes (GCC bug 54412), so Box3D's AVX2 path
	# faults on aligned spills (vmovaps to the stack). Have the assembler emit unaligned moves
	# instead; requires binutils >= 2.38. Verified with GCC 13.2 / binutils 2.42.
	if(MINGW AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
		target_compile_options(box3d PRIVATE "-Wa,-muse-unaligned-vector-move")
	endif()
endif()

# ---- ozz-animation ----------------------------------------------------------------------------
# Runtime and offline libraries only. Its tools, samples, howtos and tests stay off, so its bundled GLFW, gtest and
# jsoncpp never enter the build. Its CMake writes only under the binary dir. Never build its BUILD_CLANG_FORMAT
# target: it reformats the read-only sources in place (it is not part of `all`).
if(GAMEENGINE_WITH_ANIMATION)
	gameengine_verify_dependency(ozz-animation)
	if(CMAKE_VERSION VERSION_LESS 3.30)
		message(FATAL_ERROR "ozz-animation needs CMake 3.30 or later (this is ${CMAKE_VERSION}). "
			"Upgrade CMake, or configure with -DGAMEENGINE_WITH_ANIMATION=OFF.")
	endif()
	set(BUILD_SHARED_LIBS OFF)
	# The engine uses the DLL runtime (/MD, /MDd); ozz defaults to the static one.
	set(ozz_build_msvc_rt_dll ON CACHE BOOL "" FORCE)
	foreach(ozz_option IN ITEMS ozz_build_tools ozz_build_fbx ozz_build_gltf ozz_build_data ozz_build_samples
			ozz_build_howtos ozz_build_tests ozz_build_postfix ozz_build_simd_ref)
		set(${ozz_option} OFF CACHE BOOL "" FORCE)
	endforeach()
	add_subdirectory("${GAMEENGINE_RESOURCES_DIR}/ozz-animation" "${CMAKE_BINARY_DIR}/_deps/ozz" EXCLUDE_FROM_ALL SYSTEM)
	foreach(ozz_target IN ITEMS ozz_base ozz_options ozz_animation ozz_animation_offline ozz_geometry)
		set_target_properties(${ozz_target} PROPERTIES FOLDER "ThirdParty/ozz")
	endforeach()
	# Only ozz's own code: the engine's targets include ozz's headers as SYSTEM and keep their own warning settings.
	add_library(gameengine_ozz INTERFACE)
	target_link_libraries(gameengine_ozz INTERFACE ozz_animation_offline ozz_animation ozz_base)
	add_library(gameengine::ozz ALIAS gameengine_ozz)
endif()

# ---- GameNetworkingSockets --------------------------------------------------------------------
if(GAMEENGINE_WITH_GNS)
	gameengine_verify_dependency(GameNetworkingSockets)

	# GNS requires protobuf (protoc + libprotobuf of the same version). Supply it through
	# CMAKE_PREFIX_PATH or a vcpkg toolchain; see docs/build/building.md.
	if(WIN32)
		set(USE_CRYPTO "BCrypt" CACHE STRING "" FORCE)
	endif()
	set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
	set(BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
	set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
	set(ENABLE_ICE OFF CACHE BOOL "" FORCE)
	add_subdirectory("${GAMEENGINE_RESOURCES_DIR}/GameNetworkingSockets" "${CMAKE_BINARY_DIR}/_deps/gns" EXCLUDE_FROM_ALL SYSTEM)
endif()
