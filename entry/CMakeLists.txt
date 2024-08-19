add_executable(rgl_main "main.cpp")



target_include_directories(rgl_main PRIVATE ${PROJECT_SOURCE_DIR}/front_end ${PROJECT_SOURCE_DIR}/api_abstractor)

target_compile_features(rgl_main PRIVATE cxx_std_23)


target_include_directories(rgl_main PUBLIC  ${PROJECT_SOURCE_DIR}/render_core/gl ${PROJECT_SOURCE_DIR}/app)

target_link_libraries(rgl_main PRIVATE app RGLFramework)
find_package(glad CONFIG REQUIRED)
target_link_libraries(rgl_main PRIVATE glad::glad)


