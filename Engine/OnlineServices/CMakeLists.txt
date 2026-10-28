add_library(engine_online_services STATIC
	include/engine/online/online_service.h
	include/engine/online/null_online_service.h
	src/null_online_service.cpp
)
add_library(engine::online_services ALIAS engine_online_services)
target_include_directories(engine_online_services PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
gameengine_target_defaults(engine_online_services)
set_target_properties(engine_online_services PROPERTIES FOLDER "Engine")
