find_package(nlohmann_json CONFIG REQUIRED)
find_package(Sol2 CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(glm CONFIG REQUIRED)
find_package(unofficial-lua CONFIG REQUIRED)
find_package(inja CONFIG REQUIRED)

add_executable(reflection_tool reflection_tool.cpp)

target_sources(reflection_tool PRIVATE
    FILE_SET CXX_MODULES
    FILES
        schema_loader.cppm
        validator.cppm
        generator.cppm
)

target_compile_features(reflection_tool PRIVATE cxx_std_23)

target_link_libraries(reflection_tool
    PRIVATE
        nlohmann_json::nlohmann_json
        sol2::sol2
        Boost::program_options
        glm::glm
        lua
        pantor::inja
        rllogger
)

target_compile_definitions(reflection_tool PRIVATE
    LOG_CONFIG_PATH="${CMAKE_SOURCE_DIR}/configs/log_level.json"
)

add_custom_command(
    TARGET reflection_tool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/templates"
        "${CMAKE_CURRENT_BINARY_DIR}/templates"
    COMMENT "Copying inja templates to build directory..."
)
