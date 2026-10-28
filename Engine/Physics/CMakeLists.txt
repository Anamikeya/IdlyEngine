add_library(engine_physics STATIC
	include/engine/physics/types.h
	include/engine/physics/physics_world.h
	src/physics_world.cpp
)
add_library(engine::physics ALIAS engine_physics)

target_include_directories(engine_physics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Box3D is a private implementation detail: no Box3D type appears in engine::physics headers.
target_link_libraries(engine_physics PRIVATE box3d::box3d)
gameengine_target_defaults(engine_physics)
set_target_properties(engine_physics PROPERTIES FOLDER "Engine")

# Scene integration: maps engine::core entities to bodies and publishes poses through the command bus.
# Built only in the combined tree, where Codex's engine::core exists.
if(TARGET engine::core)
	add_library(engine_physics_scene STATIC
		include/engine/physics/scene_physics.h
		src/scene_physics.cpp
	)
	add_library(engine::physics_scene ALIAS engine_physics_scene)
	target_link_libraries(engine_physics_scene PUBLIC engine::physics engine::core)
	gameengine_target_defaults(engine_physics_scene)
	set_target_properties(engine_physics_scene PROPERTIES FOLDER "Engine")
endif()
