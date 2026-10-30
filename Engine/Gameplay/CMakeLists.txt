# Runtime gameplay: input actions, player controller, weapons, damage and game rules. It reads the scene and drives the
# physics bridge. It knows nothing of ImGui, Filament, Box3D or the editor, so any host can run it.
add_library(engine_gameplay STATIC
	include/engine/gameplay/input.h
	include/engine/gameplay/game.h
	src/game.cpp
)
add_library(engine::gameplay ALIAS engine_gameplay)
target_include_directories(engine_gameplay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(engine_gameplay PUBLIC engine::physics_scene)
gameengine_target_defaults(engine_gameplay)
set_target_properties(engine_gameplay PROPERTIES FOLDER "Engine")
