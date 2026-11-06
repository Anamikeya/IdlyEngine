# Lua scripts (Milestone 6). The runtime, its sandbox and budgets, script classes and their properties, and finding
# scripts by package name. Lua and sol2 stay private: nothing outside src/ includes a Lua header, so sol2's templates
# compile here only.
add_library(engine_scripting STATIC
	include/engine/scripting/script_types.h
	include/engine/scripting/script_runtime.h
	include/engine/scripting/script_loader.h
	include/engine/scripting/scene_values.h
	include/engine/scripting/script_host.h
	src/script_runtime.cpp
	src/script_loader.cpp
	src/scene_values.cpp
)
add_library(engine::scripting ALIAS engine_scripting)
target_include_directories(engine_scripting PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(engine_scripting PUBLIC engine::assets engine::core PRIVATE gameengine::sol2)
gameengine_target_defaults(engine_scripting)
set_target_properties(engine_scripting PROPERTIES FOLDER "Engine")

# The engine API over a play session's scene, physics and game (script_host.h), for any host that runs scripts: the
# editor now, the standalone player later. No Lua here: the runtime binds the interface.
if(TARGET engine::gameplay)
	add_library(engine_script_world STATIC
		include/engine/scripting/script_world.h
		src/script_world.cpp
	)
	add_library(engine::script_world ALIAS engine_script_world)
	target_link_libraries(engine_script_world PUBLIC engine::scripting engine::gameplay)
	gameengine_target_defaults(engine_script_world)
	set_target_properties(engine_script_world PROPERTIES FOLDER "Engine")
endif()
