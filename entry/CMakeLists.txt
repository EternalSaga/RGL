add_executable(rgl_main main.cpp)

target_link_libraries(rgl_main PRIVATE RLApp)
target_include_directories(rgl_main PRIVATE ${PROJECT_SOURCE_DIR}/front_end ${PROJECT_SOURCE_DIR}/api_abstractor)

target_compile_features(rgl_main PRIVATE cxx_std_23)
