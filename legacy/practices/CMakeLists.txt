file(GLOB_RECURSE practices_src *.cpp *.hpp)

add_library(practices ${practices_src})
execute_format(${practices_src})
target_link_libraries(practices PRIVATE glFramework shader_systems) 
target_include_directories(practices PUBLIC ${PROJECT_SOURCE_DIR}/api_abstractor ${PROJECT_SOURCE_DIR}/render_core/gl

 ${PROJECT_SOURCE_DIR}/render_core/cpu
 ${PROJECT_SOURCE_DIR}/render_core/gl_shader_gen
 ${PROJECT_SOURCE_DIR}/render_core/shader_systems

 ${PROJECT_SOURCE_DIR}/logger
 ${PROJECT_SOURCE_DIR}/app
 
 )


