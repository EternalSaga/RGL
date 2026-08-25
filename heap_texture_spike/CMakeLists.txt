cmake_minimum_required(VERSION 3.28)
project(heap_texture_spike LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)

find_package(Vulkan REQUIRED)

add_executable(heap_texture_spike main.cpp)
target_link_libraries(heap_texture_spike PRIVATE Vulkan::Vulkan)

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${GEN_DIR})

find_program(GLSLANG_EXECUTABLE glslangValidator REQUIRED)
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
compile_glsl(shaders/frag.glsl frag FRAG_SPV)

target_sources(heap_texture_spike PRIVATE ${MESH_SPV} ${FRAG_SPV})

add_custom_command(TARGET heap_texture_spike POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MESH_SPV}" "$<TARGET_FILE_DIR:heap_texture_spike>/mesh.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FRAG_SPV}" "$<TARGET_FILE_DIR:heap_texture_spike>/frag.spv"
    COMMENT "Copying shaders..."
)
