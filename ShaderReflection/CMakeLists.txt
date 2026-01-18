file(GLOB_RECURSE SHADERREFLECTION_FILES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")


add_executable(ShaderReflection ${SHADERREFLECTION_FILES})

target_compile_features(ShaderReflection PRIVATE cxx_std_20)

# 包含clang-format支持
include(${PROJECT_SOURCE_DIR}/cmake/FindClangFormat.cmake)

find_package(spirv_cross_core CONFIG REQUIRED)
find_package(spirv_cross_glsl CONFIG REQUIRED)
find_package(spirv_cross_cpp CONFIG REQUIRED)

find_package(inja CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options unordered)
find_package(sol2 REQUIRED)
find_package(Lua REQUIRED)

target_link_libraries(ShaderReflection PRIVATE spirv-cross-core spirv-cross-glsl spirv-cross-cpp nlohmann_json::nlohmann_json Boost::program_options pantor::inja sol2::sol2 rllogger)
target_include_directories(ShaderReflection PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(ShaderReflection PRIVATE ${LUA_LIBRARIES})
target_include_directories(ShaderReflection PRIVATE)


set(ASSETS_SOURCE_DIR "${PROJECT_SOURCE_DIR}/assets")

set(ASSETS_BUILD_DIR "${CMAKE_BINARY_DIR}/assets")

set(assets_need_to_copy

"grass_variations.glb"

"transparent.glb"

)

add_custom_command(
    TARGET ShaderReflection POST_BUILD
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
        TARGET ShaderReflection POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DST_DIR}"
        COMMENT "Ensuring asset subdirectory exists: ${DST_DIR}"
    )

    # 拷贝文件。使用 'copy_if_different' 可以避免不必要的拷贝，提升性能。
    add_custom_command(
        TARGET ShaderReflection POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SRC_PATH}" "${DST_PATH}"
        COMMENT "Copying asset: ${asset_file}"
    )
endforeach()

set (Config_Dir "${PROJECT_SOURCE_DIR}/configs")
set (Config_Build_Dir "${CMAKE_BINARY_DIR}/configs")

file(GLOB_RECURSE Config_Files "${Config_Dir}/*.json")



add_custom_command(
    TARGET ShaderReflection POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${Config_Build_Dir}"
    COMMENT "Creating config build directory: ${Config_Build_DIR}"
)

foreach(Config_File IN LISTS Config_Files)
    get_filename_component(asset_file_name ${Config_File} NAME)
    add_custom_command(
        TARGET ShaderReflection POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${Config_File}" "${Config_Build_Dir}/${asset_file_name}"
        COMMENT "Copying config: ${asset_file_name}"
    )
endforeach()


function(rgl_reflect_batch)
    cmake_parse_arguments(ARG "" "TARGET;OUTPUT_DIR" "SHADERS" ${ARGN})
    foreach(s IN LISTS ARG_SHADERS)
        compile_spirv_to_cpp(${ARG_TARGET} ${s} ${ARG_OUTPUT_DIR})
    endforeach()
endfunction()