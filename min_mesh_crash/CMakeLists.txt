cmake_minimum_required(VERSION 3.28)
project(min_mesh_crash LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
add_compile_options(-fpermissive)

find_package(Vulkan REQUIRED)
find_package(SDL3 CONFIG REQUIRED)

set(AFTERMATH_SDK_DIR "${CMAKE_SOURCE_DIR}/../../Aftermath")
get_filename_component(AFTERMATH_SDK_DIR "${AFTERMATH_SDK_DIR}" ABSOLUTE)

add_executable(min_mesh_crash main.cpp)

target_include_directories(min_mesh_crash PRIVATE
    ${AFTERMATH_SDK_DIR}/include
    ${VULKAN_INCLUDE_DIRS}
)

target_link_libraries(min_mesh_crash PRIVATE
    Vulkan::Vulkan
    SDL3::SDL3
    ${AFTERMATH_SDK_DIR}/lib/x64/libGFSDK_Aftermath_Lib.x64.so
)

set_target_properties(min_mesh_crash PROPERTIES
    BUILD_RPATH "${AFTERMATH_SDK_DIR}/lib/x64"
)

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${GEN_DIR})

find_program(GLSLANG_EXECUTABLE glslangValidator)
message(STATUS "glslangValidator: ${GLSLANG_EXECUTABLE}")

function(compile_glsl SRC STAGE OUT)
    get_filename_component(SRC_ABS ${SRC} ABSOLUTE)
    get_filename_component(SRC_NAME ${SRC} NAME_WE)
    set(DST "${GEN_DIR}/${SRC_NAME}.spv")
    add_custom_command(OUTPUT ${DST}
        COMMAND ${GLSLANG_EXECUTABLE} -V --target-env vulkan1.3 -S ${STAGE} -o ${DST} ${SRC_ABS}
        DEPENDS ${SRC_ABS}
        COMMENT "GLSL: ${SRC} -> ${DST}"
        VERBATIM
    )
    set(${OUT} ${DST} PARENT_SCOPE)
endfunction()

compile_glsl(shaders/mesh.glsl mesh MESH_SPV)
compile_glsl(shaders/vert.glsl vert VERT_SPV)
compile_glsl(shaders/frag.glsl frag FRAG_SPV)

add_dependencies(min_mesh_crash)
target_sources(min_mesh_crash PRIVATE ${MESH_SPV} ${VERT_SPV} ${FRAG_SPV})

add_custom_command(TARGET min_mesh_crash POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MESH_SPV}" "$<TARGET_FILE_DIR:min_mesh_crash>/mesh.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${VERT_SPV}" "$<TARGET_FILE_DIR:min_mesh_crash>/vert.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FRAG_SPV}" "$<TARGET_FILE_DIR:min_mesh_crash>/frag.spv"
    COMMENT "Copying shaders..."
)
