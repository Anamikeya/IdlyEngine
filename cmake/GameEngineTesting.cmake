# Test executables use Tests/Framework (engine_test_framework). Each executable lists its cases with
# `--list`; after every build the cases are written to a CTest include file so each case is its own
# CTest test and can be filtered, labelled and timed individually.

set(GAMEENGINE_TEST_DISCOVERY_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/scripts/DiscoverTests.cmake")

# gameengine_add_test_executable(<target> LABELS <labels...> SOURCES <sources...> LIBRARIES <libs...>)
function(gameengine_add_test_executable target)
	cmake_parse_arguments(ARG "" "TIMEOUT" "SOURCES;LIBRARIES;LABELS" ${ARGN})
	if(NOT ARG_TIMEOUT)
		set(ARG_TIMEOUT 60)
	endif()

	add_executable(${target} ${ARG_SOURCES})
	target_link_libraries(${target} PRIVATE engine_test_framework ${ARG_LIBRARIES})
	gameengine_target_defaults(${target})
	set_target_properties(${target} PROPERTIES FOLDER "Tests")

	set(ctest_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_tests.cmake")
	string(REPLACE ";" "\\;" labels_escaped "${ARG_LABELS}")
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND "${CMAKE_COMMAND}"
			"-DTEST_EXECUTABLE=$<TARGET_FILE:${target}>"
			"-DTEST_PREFIX=${target}."
			"-DTEST_LABELS=${labels_escaped}"
			"-DTEST_TIMEOUT=${ARG_TIMEOUT}"
			"-DTEST_WORKING_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
			"-DCTEST_FILE=${ctest_file}"
			-P "${GAMEENGINE_TEST_DISCOVERY_SCRIPT}"
		BYPRODUCTS "${ctest_file}"
		VERBATIM)

	# Placeholder until the first build so `ctest` reports a clear failure instead of silently passing.
	if(NOT EXISTS "${ctest_file}")
		file(WRITE "${ctest_file}" "add_test(${target}.NOT_BUILT \"${CMAKE_COMMAND}\" -E false)\n")
	endif()
	set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${ctest_file}")
endfunction()
