find_program(SLANGC_EXECUTABLE slangc
    PATHS "${CMAKE_CURRENT_BINARY_DIR}/vulkan_practice/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/tools/slang"
          "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/tools/slang"
)

if(NOT SLANG_COMPILER)
    message(FATAL_ERROR "找不到 slangc 编译器！请检查 vcpkg 是否成功安装了 shader-slang")
endif()

message(STATUS "找到 Slang 编译器: ${SLANG_COMPILER}")


function(compile_slang SHADER_SOURCE OUT_SPV_PATH)
    get_filename_component(SHADER_ABS ${SHADER_SOURCE} ABSOLUTE)
    get_filename_component(SHADER_FULL_NAME ${SHADER_SOURCE} NAME)
    get_filename_component(SHADER_DIR ${SHADER_ABS} DIRECTORY)
    if(NOT EXISTS ${SHADER_SOURCE})
        message(FATAL_ERROR "【Slang 编译错误】找不到该 Shader 文件: ${SHADER_SOURCE}")
    endif()
    # 字符串替换：cube.mesh.slang -> cube.mesh.spv
    string(REPLACE ".slang" ".spv" SPV_NAME "${SHADER_FULL_NAME}")

    set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
    file(MAKE_DIRECTORY ${GEN_DIR})
    set(OUTPUT_SPV "${GEN_DIR}/${SPV_NAME}")

    add_custom_command(
        OUTPUT ${OUTPUT_SPV}
        COMMAND ${SLANGC_EXECUTABLE} ${SHADER_ABS}
                -target spirv
                -profile sm_6_5
                -I "${SHADER_DIR}"
                -o ${OUTPUT_SPV}
        DEPENDS ${SHADER_ABS}
        COMMENT "Slangc automatically compiling: ${SHADER_FULL_NAME} -> ${SPV_NAME}"
        VERBATIM
    )

    set(${OUT_SPV_PATH} ${OUTPUT_SPV} PARENT_SCOPE)
endfunction()
compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube.mesh.slang MESH_SPV)
compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube.frag.slang FRAG_SPV)
message(STATUS "MESH_SPV is: ${MESH_SPV}")
message(STATUS "FRAG_SPV is: ${FRAG_SPV}")

add_executable(vulkan_practice main.cpp vam_impl.cpp ${MESH_SPV} ${FRAG_SPV})


target_sources(vulkan_practice PUBLIC 
            FILE_SET CXX_MODULES
            FILES utils.cppm sdl3window.cppm QueueFamilyManager.cppm cube.cppm shader.cppm
)

find_package(Vulkan REQUIRED)
find_package(glm CONFIG REQUIRED)
target_link_libraries(vulkan_practice PRIVATE Vulkan::Vulkan rllogger glm::glm)

find_package(SDL3 CONFIG REQUIRED)

find_package(VulkanMemoryAllocator CONFIG REQUIRED)
target_link_libraries(vulkan_practice
    PRIVATE
    SDL3::SDL3
    GPUOpen::VulkanMemoryAllocator
    )



target_compile_features(vulkan_practice PRIVATE cxx_std_23)
