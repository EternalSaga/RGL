find_package(glslang CONFIG REQUIRED)

find_program(
    GLSLANG_VALIDATOR_EXECUTABLE # 这个变量会存储找到的完整路径
    NAMES glslangValidator       # 要查找的程序名
    REQUIRED                     # 如果找不到，就报错
)

message(STATUS "Found glslangValidator at: ${GLSLANG_VALIDATOR_EXECUTABLE}")

function(compile_shader SHADER_SOURCE)
    get_filename_component(SHADER_NAME ${SHADER_SOURCE} NAME_WE)
    set(SPIRV_OUTPUT "${CMAKE_BINARY_DIR}/shaders/${SHADER_NAME}.spv")
    
    add_custom_command(
        OUTPUT ${SPIRV_OUTPUT}
        # 这里使用我们找到的变量，而不是 CMake 目标名
        COMMAND ${GLSLANG_VALIDATOR_EXECUTABLE} -V ${SHADER_SOURCE} -o ${SPIRV_OUTPUT}
        DEPENDS ${SHADER_SOURCE}
        COMMENT "Compiling ${SHADER_SOURCE} to SPIR-V"
    )

    # 让其他目标可以依赖这个生成的spirv文件
    add_custom_target(
        SPIRV_${SHADER_NAME} ALL
        DEPENDS ${SPIRV_OUTPUT}
    )
endfunction()

compile_shader(${CMAKE_SOURCE_DIR}/shaders/Light/phong_ubo_instanced.vert)
compile_shader(${CMAKE_SOURCE_DIR}/shaders/Light/grass-fragment.frag)