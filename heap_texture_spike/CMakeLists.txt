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

# ─── Slang 分离绑定变体（可选）──────────────────────────────────────────────
# SPIKE_SHADER=slang 用 OpTypeImage + OpTypeSampler 分离对（主工程同款 shader 形态）；
# 找不到 slangc 时跳过编译，运行时该变体报不可用（glsl 基线路径不受影响）。
# slangc 解析：$ENV{SLANGC_DIR} 显式覆盖（离线/试别的版本）；否则 FetchContent 拉
# Slang 官方预构建（cmake/SlangFetch.cmake，与主工程同源，一次性下载到本 build/_deps/）。
# 刻意不查系统 PATH——vcpkg 旧 shader-slang（2026.7.1）残留会被误拾，破坏 pin 意义
#（2026-08-27 首次接入时踩中：PATH 里还有 build/vcpkg_installed/.../slangc）。
find_program(SLANGC_EXECUTABLE slangc
    HINTS $ENV{SLANGC_DIR}
    NO_SYSTEM_ENVIRONMENT_PATH NO_CMAKE_SYSTEM_PATH
)
if(NOT SLANGC_EXECUTABLE)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SlangFetch.cmake)
    rgl_fetch_slang()
endif()
if(SLANGC_EXECUTABLE)
    message(STATUS "slangc: ${SLANGC_EXECUTABLE}")
    set(SLANG_FLAGS -target spirv -capability spvDescriptorHeapEXT -profile sm_6_6+spirv_1_6)
    set(SLANG_SRCDIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
    foreach(SLANG_SRC slang_mesh slang_frag slang_frag_ctrl)
        add_custom_command(OUTPUT "${GEN_DIR}/${SLANG_SRC}.spv"
            COMMAND ${SLANGC_EXECUTABLE} "${SLANG_SRCDIR}/${SLANG_SRC}.slang" ${SLANG_FLAGS}
                -I "${SLANG_SRCDIR}" -o "${GEN_DIR}/${SLANG_SRC}.spv"
            DEPENDS "${SLANG_SRCDIR}/${SLANG_SRC}.slang"
            COMMENT "Slang: ${SLANG_SRC}.slang -> ${SLANG_SRC}.spv"
            VERBATIM
        )
    endforeach()
    target_sources(heap_texture_spike PRIVATE "${GEN_DIR}/slang_mesh.spv" "${GEN_DIR}/slang_frag.spv" "${GEN_DIR}/slang_frag_ctrl.spv")
    target_compile_definitions(heap_texture_spike PRIVATE SPIKE_HAS_SLANG=1)
    add_custom_command(TARGET heap_texture_spike POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GEN_DIR}/slang_mesh.spv" "$<TARGET_FILE_DIR:heap_texture_spike>/slang_mesh.spv"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GEN_DIR}/slang_frag.spv" "$<TARGET_FILE_DIR:heap_texture_spike>/slang_frag.spv"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GEN_DIR}/slang_frag_ctrl.spv" "$<TARGET_FILE_DIR:heap_texture_spike>/slang_frag_ctrl.spv"
        COMMENT "Copying Slang shaders..."
    )
else()
    message(STATUS "slangc not found: Slang 变体不可用（SPIKE_SHADER=slang 将跳过）")
endif()
