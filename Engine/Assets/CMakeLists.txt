# CPU-side asset pipeline (contract C-5). GPU resources are the renderer's job.
add_library(engine_assets STATIC
	include/engine/assets/asset_types.h
	include/engine/assets/importers.h
	include/engine/assets/asset_database.h
	src/importers.cpp
	src/gltf_importer.cpp
	src/asset_database.cpp
)
add_library(engine::assets ALIAS engine_assets)
target_include_directories(engine_assets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Only engine/core/Result.hpp is used; linking engine::core keeps one definition of the error model.
target_link_libraries(engine_assets PUBLIC engine::core)
# GLB and PNG/JPEG parsers: cgltf and stb_image from Filament's third_party directory, compiled into
# gltf_importer.cpp. Trees without Filament (the Linux CI job) build without them and report it at import time.
set(ENGINE_ASSETS_PARSERS "${GAMEENGINE_RESOURCES_DIR}/filament/third_party")
if(EXISTS "${ENGINE_ASSETS_PARSERS}/cgltf/cgltf.h" AND EXISTS "${ENGINE_ASSETS_PARSERS}/stb/stb_image.h")
	target_include_directories(engine_assets SYSTEM PRIVATE "${ENGINE_ASSETS_PARSERS}/cgltf" "${ENGINE_ASSETS_PARSERS}/stb")
	target_compile_definitions(engine_assets PRIVATE ENGINE_ASSETS_HAVE_GLTF)
endif()
gameengine_target_defaults(engine_assets)
set_target_properties(engine_assets PROPERTIES FOLDER "Engine")
