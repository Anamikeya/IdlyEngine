# Particle simulation, CPU and visual only (Milestone 5). Reads effect assets; knows nothing of the scene, physics,
# Filament or the editor, so any host can run it and gameplay never depends on it.
add_library(engine_effects STATIC
	include/engine/effects/particles.h
	src/particles.cpp
)
add_library(engine::effects ALIAS engine_effects)
target_include_directories(engine_effects PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(engine_effects PUBLIC engine::assets)
gameengine_target_defaults(engine_effects)
set_target_properties(engine_effects PROPERTIES FOLDER "Engine")
