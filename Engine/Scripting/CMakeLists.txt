# Lua scripts (Milestone 6). The runtime, its sandbox and budgets, script classes and their properties, and finding
# scripts by package name. Lua and sol2 stay private: nothing outside src/ includes a Lua header, so sol2's templates
# compile here only.
add_library(engine_scripting STATIC
	include/engine/scripting/script_types.h
	include/engine/scripting/script_runtime.h
	include/engine/scripting/script_loader.h
	src/script_runtime.cpp
	src/script_loader.cpp
)
add_library(engine::scripting ALIAS engine_scripting)
target_include_directories(engine_scripting PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(engine_scripting PUBLIC engine::assets PRIVATE gameengine::sol2)
gameengine_target_defaults(engine_scripting)
set_target_properties(engine_scripting PROPERTIES FOLDER "Engine")
