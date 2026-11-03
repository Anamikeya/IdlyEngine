add_library(engine_test_framework STATIC
	Framework/include/engine/test/test.h
	Framework/src/test_main.cpp
)
target_include_directories(engine_test_framework PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Framework/include")
gameengine_target_defaults(engine_test_framework)
set_target_properties(engine_test_framework PROPERTIES FOLDER "Tests")

if(GAMEENGINE_WITH_PHYSICS)
	gameengine_add_test_executable(physics_tests
		LABELS physics unit
		SOURCES Physics/physics_world_tests.cpp
		LIBRARIES engine::physics)
endif()

# Real Box3D world against Codex's real Scene/CommandBus; no mocks.
if(TARGET engine::physics_scene)
	gameengine_add_test_executable(physics_scene_tests
		LABELS physics integration
		SOURCES Physics/scene_physics_tests.cpp
		LIBRARIES engine::physics_scene)
endif()

# The game without a window: real Scene, CommandBus and Box3D, and the sample's real scene file.
if(TARGET engine::gameplay)
	gameengine_add_test_executable(gameplay_tests
		LABELS gameplay integration
		SOURCES Gameplay/gameplay_tests.cpp
		LIBRARIES engine::gameplay)
	target_compile_definitions(gameplay_tests PRIVATE "GAMEENGINE_SAMPLES_DIR=\"${CMAKE_SOURCE_DIR}/Samples\"")
endif()

if(TARGET engine::networking)
	gameengine_add_test_executable(networking_tests
		LABELS networking unit
		SOURCES
			Networking/serialization_tests.cpp
			Networking/loopback_transport_tests.cpp
			Networking/connection_manager_tests.cpp
		LIBRARIES engine::networking)
endif()

# The editor's foundation without a window. The editor builds on Windows/MSVC only, so its session library may not exist.
if(TARGET engine_editor_session)
	gameengine_add_test_executable(editor_ux_tests
		LABELS editor unit
		SOURCES Editor/editor_ux_tests.cpp
		LIBRARIES engine_editor_session)
	target_compile_definitions(editor_ux_tests PRIVATE "GAMEENGINE_TEST_DOCS_DIR=\"${CMAKE_SOURCE_DIR}/docs\"")
	gameengine_add_test_executable(editor_viewport_tests
		LABELS editor unit
		SOURCES Editor/editor_viewport_tests.cpp
		LIBRARIES engine_editor_session)
	target_compile_definitions(editor_viewport_tests PRIVATE "GAMEENGINE_TEST_ASSETS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/Assets\"")
	gameengine_add_test_executable(editor_workflow_tests
		LABELS editor unit
		SOURCES Editor/editor_workflow_tests.cpp
		LIBRARIES engine_editor_session)
	gameengine_add_test_executable(editor_feedback_tests
		LABELS editor unit
		SOURCES Editor/editor_feedback_tests.cpp
		LIBRARIES engine_editor_session)
	target_include_directories(editor_feedback_tests SYSTEM PRIVATE "${GAMEENGINE_RESOURCES_DIR}/glm")
	gameengine_add_test_executable(editor_panels_tests
		LABELS editor unit
		SOURCES Editor/editor_panels_tests.cpp
		LIBRARIES engine_editor_session)
	target_compile_definitions(editor_panels_tests PRIVATE "GAMEENGINE_TEST_ASSETS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/Assets\"")
endif()

if(TARGET engine::online_services)
	gameengine_add_test_executable(online_services_tests
		LABELS online unit
		SOURCES OnlineServices/null_online_service_tests.cpp
		LIBRARIES engine::online_services)
endif()

if(TARGET engine::assets)
	gameengine_add_test_executable(asset_tests
		LABELS assets unit
		SOURCES Assets/asset_tests.cpp
		LIBRARIES engine::assets)
	# Fixtures are read in place from the source tree; mutating cases copy them to the working directory.
	target_compile_definitions(asset_tests PRIVATE "GAMEENGINE_TEST_ASSETS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/Assets\""
		"GAMEENGINE_TEST_RESOURCES_DIR=\"${GAMEENGINE_RESOURCES_DIR}\"" "GAMEENGINE_TEST_SAMPLES_DIR=\"${CMAKE_SOURCE_DIR}/Samples\"")
endif()

if(TARGET engine::physics_scene AND TARGET engine::assets AND TARGET engine::renderer)
	gameengine_add_test_executable(systems_smoke_tests
		LABELS integration smoke
		SOURCES Integration/systems_smoke_tests.cpp
		LIBRARIES engine::physics_scene engine::assets engine::renderer)
	target_compile_definitions(systems_smoke_tests PRIVATE "GAMEENGINE_TEST_ASSETS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/Assets\"")
endif()

# Headless: links GLFW and runs ImGui through its null backend. Not a GUI test.
if(TARGET gameengine::imgui)
	# ImGui's null platform/renderer backend: third-party code, so it keeps its own flags (no engine warnings).
	add_library(gameengine_imgui_null STATIC EXCLUDE_FROM_ALL "${GAMEENGINE_IMGUI_DIR}/backends/imgui_impl_null.cpp")
	target_link_libraries(gameengine_imgui_null PUBLIC gameengine::imgui)
	set_target_properties(gameengine_imgui_null PROPERTIES FOLDER "ThirdParty")
	gameengine_add_test_executable(editor_dependency_smoke
		LABELS editor smoke
		SOURCES Editor/editor_dependency_smoke.cpp
		LIBRARIES gameengine_imgui_null)
endif()
