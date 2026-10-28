# CPU-side asset pipeline (contract C-5). No third-party dependency; GPU resources are the renderer's job.
add_library(engine_assets STATIC
	include/engine/assets/asset_types.h
	include/engine/assets/importers.h
	include/engine/assets/asset_database.h
	src/importers.cpp
	src/asset_database.cpp
)
add_library(engine::assets ALIAS engine_assets)
target_include_directories(engine_assets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Only engine/core/Result.hpp is used; linking engine::core keeps one definition of the error model.
target_link_libraries(engine_assets PUBLIC engine::core)
gameengine_target_defaults(engine_assets)
set_target_properties(engine_assets PROPERTIES FOLDER "Engine")
