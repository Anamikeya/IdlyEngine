add_library(engine_networking STATIC
	include/engine/net/types.h
	include/engine/net/byte_stream.h
	include/engine/net/transport.h
	include/engine/net/loopback_transport.h
	include/engine/net/connection_manager.h
	src/types.cpp
	src/byte_stream.cpp
	src/loopback_transport.cpp
	src/connection_manager.cpp
)
add_library(engine::networking ALIAS engine_networking)
target_include_directories(engine_networking PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
gameengine_target_defaults(engine_networking)
set_target_properties(engine_networking PROPERTIES FOLDER "Engine")

set(_gns_include "${GAMEENGINE_RESOURCES_DIR}/GameNetworkingSockets/include")

if(GAMEENGINE_WITH_GNS)
	add_library(engine_networking_gns STATIC
		include/engine/net/gns_transport.h
		src/gns_transport.cpp
	)
	add_library(engine::networking_gns ALIAS engine_networking_gns)
	target_link_libraries(engine_networking_gns PUBLIC engine::networking PRIVATE GameNetworkingSockets::static)
	gameengine_target_defaults(engine_networking_gns)
	set_target_properties(engine_networking_gns PROPERTIES FOLDER "Engine")
elseif(IS_DIRECTORY "${_gns_include}")
	# Without protobuf the GNS library cannot be built, but its public headers are self-contained.
	# Compile (never link) the backend against them so API drift is caught on every build.
	option(GAMEENGINE_GNS_COMPILE_CHECK "Compile the GNS backend against GNS headers when GNS is disabled" ON)
	if(GAMEENGINE_GNS_COMPILE_CHECK)
		add_library(engine_networking_gns_compile_check OBJECT src/gns_transport.cpp)
		target_link_libraries(engine_networking_gns_compile_check PRIVATE engine::networking)
		target_include_directories(engine_networking_gns_compile_check SYSTEM PRIVATE "${_gns_include}")
		target_compile_definitions(engine_networking_gns_compile_check PRIVATE STEAMNETWORKINGSOCKETS_STATIC_LINK)
		gameengine_target_defaults(engine_networking_gns_compile_check)
		set_target_properties(engine_networking_gns_compile_check PROPERTIES FOLDER "Engine")
	endif()
endif()
