# Skeletal animation on ozz-animation (Milestone 5). Reads the plain skeleton and clip types of engine::assets and turns
# them into poses; knows nothing of the scene, Filament, Box3D or the editor. ozz stays a private dependency.
add_library(engine_animation STATIC
	include/engine/animation/animation.h
	src/animation.cpp
)
add_library(engine::animation ALIAS engine_animation)
target_include_directories(engine_animation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(engine_animation PUBLIC engine::assets PRIVATE gameengine::ozz)
gameengine_target_defaults(engine_animation)
set_target_properties(engine_animation PROPERTIES FOLDER "Engine")
