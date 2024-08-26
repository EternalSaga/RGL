file(GLOB_RECURSE practices_src *.cpp *.hpp)

add_library(practices ${practices_src})
execute_format(${practices_src})
target_link_libraries(practices PRIVATE RGLFramework)
target_include_directories(practices PUBLIC ${PROJECT_SOURCE_DIR}/api_abstractor ${PROJECT_SOURCE_DIR}/render_core/gl
 ${PROJECT_SOURCE_DIR}/render_core/cpu ${PROJECT_SOURCE_DIR}/render_core/logger
 ${PROJECT_SOURCE_DIR}/app
 )