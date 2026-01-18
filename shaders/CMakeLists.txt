find_package(glslang CONFIG REQUIRED)
find_program(GLSLANG_VALIDATOR_EXECUTABLE NAMES glslangValidator REQUIRED)


# 假设 ShaderReflection 目录下的 CMakeLists 生成了一个叫 "ShaderReflectionTool" 的可执行目标

# =============================================================
# 核心函数：添加 Shader 并自动处理依赖
# =============================================================
function(target_add_shader TARGET_NAME SHADER_SOURCE)
    # 0. 准备路径和文件名
    get_filename_component(SHADER_ABS ${SHADER_SOURCE} ABSOLUTE)
    get_filename_component(SHADER_NAME ${SHADER_SOURCE} NAME_WE)
    get_filename_component(SHADER_EXT ${SHADER_SOURCE} EXT)
    
    # 定义输出目录 (通常在 build 目录下，保持整洁)
    set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
    file(MAKE_DIRECTORY ${GEN_DIR})

    # 定义中间文件 (SPIR-V) 和 最终文件 (C++/HPP) 的路径
    set(SPIRV_OUTPUT "${GEN_DIR}/${SHADER_NAME}.spv")
    set(CPP_OUTPUT   "${GEN_DIR}/${SHADER_NAME}.cpp")
    set(HPP_OUTPUT   "${GEN_DIR}/${SHADER_NAME}.hpp")

    add_custom_command(
        OUTPUT ${SPIRV_OUTPUT}
        COMMAND ${GLSLANG_VALIDATOR_EXECUTABLE} -V ${SHADER_ABS} -o ${SPIRV_OUTPUT}
        DEPENDS ${SHADER_ABS} 
        COMMENT "Compiling GLSL to SPIR-V: ${SHADER_NAME}${SHADER_EXT}"
        VERBATIM
    )


    add_custom_command(
        OUTPUT ${CPP_OUTPUT} ${HPP_OUTPUT}
        COMMAND ShaderReflection 
            -i "${SPIRV_OUTPUT}" 
            -o "${GEN_DIR}/${SHADER_NAME}" #ShaderReflection内对生成目录的处理方式是上级目录，随便给个文件命名
            -t "cpp"
            -d "${PROJECT_SOURCE_DIR}/ShaderReflection/templates"
            -c "${PROJECT_SOURCE_DIR}/configs/shaderRefLogLevel.json"
            -sampler_rule "${PROJECT_SOURCE_DIR}/configs/SamplerRules.lua"
        COMMAND "${CMAKE_COMMAND}" -E echo "Formatting generated code..."
        COMMAND clang-format -i ${CPP_OUTPUT} ${HPP_OUTPUT}
        
        DEPENDS ${SPIRV_OUTPUT} ShaderReflection # <--- 关键：依赖 spv 文件 和 反射工具本身
        COMMENT "Reflecting SPIR-V to C++: ${SHADER_NAME}"
        VERBATIM
    )

    # 3. 第三步：将生成的 C++ 源码加入到主目标
    # CMake 会自动标记这些文件为 GENERATED，允许它们在 Config 时不存在
    target_sources(${TARGET_NAME} PRIVATE ${CPP_OUTPUT})
    
    # 4. 第四步：把生成目录加入 include 路径，这样你在代码里能 #include "test.hpp"
    target_include_directories(${TARGET_NAME} PUBLIC ${GEN_DIR})

    message(STATUS "Shader configured: ${SHADER_NAME} -> Will generate C++ at build time.")
endfunction()
