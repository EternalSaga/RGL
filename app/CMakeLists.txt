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

find_package(Boost CONFIG REQUIRED COMPONENTS dll)
target_link_libraries(app PRIVATE Boost::dll)



set(ASSETS_SOURCE_DIR "${PROJECT_SOURCE_DIR}/assets")

set(ASSETS_BUILD_DIR "${CMAKE_BINARY_DIR}/assets")

set(assets_need_to_copy

"grass_variations.glb"

"transparent.glb"

)

add_custom_command(
    TARGET app POST_BUILD
    # 1. 确保目标资产目录存在
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ASSETS_BUILD_DIR}"
    COMMENT "Ensuring assets build directory exists: ${ASSETS_BUILD_DIR}"
)

foreach(asset_file IN LISTS assets_need_to_copy)
    set(SRC_PATH "${ASSETS_SOURCE_DIR}/${asset_file}")
    set(DST_PATH "${ASSETS_BUILD_DIR}/${asset_file}")

    # 确保目标文件的父目录存在 (例如，如果 sounds/beep.wav，要确保 sounds 目录存在)
    get_filename_component(DST_DIR "${DST_PATH}" DIRECTORY)
    add_custom_command(
        TARGET app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DST_DIR}"
        COMMENT "Ensuring asset subdirectory exists: ${DST_DIR}"
    )

    # 拷贝文件。使用 'copy_if_different' 可以避免不必要的拷贝，提升性能。
    # DEPENDS 参数很重要，它告诉 CMake 如果源文件发生变化，需要重新执行此命令。
    add_custom_command(
        TARGET app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC_PATH}" "${DST_PATH}"
        COMMENT "Copying asset: ${asset_file}"
        DEPENDS "${SRC_PATH}"
    )
endforeach()

set (Config_Dir "${PROJECT_SOURCE_DIR}/configs")
set (Config_Build_Dir "${CMAKE_BINARY_DIR}/configs")

file(GLOB_RECURSE Config_Files "${Config_Dir}/*.json")


add_custom_command(
    TARGET app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${Config_Build_Dir}"
    COMMENT "Creating config build directory: ${Config_Build_DIR}"
)

foreach(Config_File IN LISTS Config_Files)
    get_filename_component(asset_file_name ${Config_File} NAME)
    add_custom_command(
        TARGET app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${Config_File}" "${Config_Build_Dir}/${asset_file_name}"
        COMMENT "Copying config: ${asset_file_name}"
    )
endforeach()
