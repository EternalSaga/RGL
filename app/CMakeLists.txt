file(GLOB_RECURSE app_src *.cpp *.hpp)
add_executable(app ${app_src})
target_compile_features(app PRIVATE cxx_std_20)

find_package(EnTT CONFIG REQUIRED)
target_link_libraries(app PRIVATE EnTT::EnTT)

execute_format(${app_src})
find_package(SDL3 CONFIG REQUIRED)
    target_link_libraries(app
    PUBLIC
    SDL3::SDL3
    )

find_package(Boost CONFIG REQUIRED)
target_link_libraries(app PRIVATE Boost::headers)

find_package(glm CONFIG REQUIRED)

target_link_libraries(app PRIVATE glm::glm cpuframework glFramework practices)

target_include_directories(app PRIVATE ${PROJECT_SOURCE_DIR}/api_abstractor ${PROJECT_SOURCE_DIR}/render_core/gl ${PROJECT_SOURCE_DIR}/practices)


