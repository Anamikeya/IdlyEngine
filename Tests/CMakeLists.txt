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

if(TARGET engine::networking)
	gameengine_add_test_executable(networking_tests
		LABELS networking unit
		SOURCES
			Networking/serialization_tests.cpp
			Networking/loopback_transport_tests.cpp
			Networking/connection_manager_tests.cpp
		LIBRARIES engine::networking)
endif()

if(TARGET engine::networking_gns)
	gameengine_add_test_executable(networking_gns_tests
		LABELS networking integration gns
		TIMEOUT 120
		SOURCES Networking/gns_transport_tests.cpp
		LIBRARIES engine::networking_gns)
endif()

if(TARGET engine::online_services)
	gameengine_add_test_executable(online_services_tests
		LABELS online unit
		SOURCES OnlineServices/null_online_service_tests.cpp
		LIBRARIES engine::online_services)
endif()
