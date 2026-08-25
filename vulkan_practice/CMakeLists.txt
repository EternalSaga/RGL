# slangc 由 vcpkg 的 shader-slang port 提供：vcpkg toolchain 把
# build/vcpkg_installed/<triplet>/tools/shader-slang 注入 CMAKE_PROGRAM_PATH，
# find_program 自动解析到 vcpkg 版本（CMAKE_PROGRAM_PATH 优先于显式 PATHS）。
# 旧版曾硬编码 $HOME/program/slang/build/Release——那是死代码（从未生效）且误导，已移除。
find_program(SLANGC_EXECUTABLE slangc REQUIRED)

message(STATUS "找到 Slang 编译器: ${SLANGC_EXECUTABLE}")

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
file(MAKE_DIRECTORY ${GEN_DIR})

# ─── Phase 0: Shader Reflection ───────────────────────────────────────────────
# Generated .cppm must live in source tree for CMake FILE_SET CXX_MODULES.
# It is git-ignored via vulkan_practice/generated/ pattern in .gitignore.
set(GEN_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated")
file(MAKE_DIRECTORY ${GEN_SRC_DIR})

set(TYPES_SLANG "${CMAKE_CURRENT_SOURCE_DIR}/shaders/types.slang")
set(TYPES_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/shaders/types_schema.lua")
set(TYPES_REFLECTION_JSON "${GEN_DIR}/types_reflection.json")
set(TYPES_CPPM "${GEN_SRC_DIR}/types_generated.cppm")
set(TYPES_TEMPLATE "${CMAKE_SOURCE_DIR}/tools/templates/types_cppm.inja")

# ─── Features Codegen ────────────────────────────────────────────────────────
# Single-source-of-truth JSON for Vulkan feature set.
# Generated .cppm lives in source tree for CMake FILE_SET CXX_MODULES.
set(FEATURES_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/features_config.json")
set(FEATURES_TEMPLATE "${CMAKE_SOURCE_DIR}/tools/templates/features_cppm.inja")
set(FEATURES_CPPM "${GEN_SRC_DIR}/features_generated.cppm")

add_custom_command(
    OUTPUT ${TYPES_REFLECTION_JSON}
    COMMAND ${SLANGC_EXECUTABLE} ${TYPES_SLANG}
            -target spirv
            -capability spvDescriptorHeapEXT
            -profile sm_6_6+spirv_1_6
            -I "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
            -no-codegen
            -reflection-json ${TYPES_REFLECTION_JSON}
    DEPENDS ${TYPES_SLANG} ${TYPES_SCHEMA}
    COMMENT "Slangc reflection: types.slang -> types_reflection.json"
    VERBATIM
)

add_custom_command(
    OUTPUT ${TYPES_CPPM}
    COMMAND reflection_tool validate
            --json ${TYPES_REFLECTION_JSON}
            --schema ${TYPES_SCHEMA}
    COMMAND reflection_tool generate
            --json ${TYPES_REFLECTION_JSON}
            --schema ${TYPES_SCHEMA}
            --output ${TYPES_CPPM}
            --module-name types_generated
            --template ${TYPES_TEMPLATE}
    DEPENDS ${TYPES_REFLECTION_JSON} ${TYPES_SCHEMA} ${TYPES_TEMPLATE} reflection_tool
    COMMENT "Reflection: validate schema + generate types_generated.cppm"
    VERBATIM
)

add_custom_target(reflection_generated ALL DEPENDS ${TYPES_CPPM})

# ─── Phase 2: Model Reflection ───────────────────────────────────────────────
# 第二条反射管线：model.slang 契约 → model_types_generated.cppm
#（ModelDrawArgs / ModelVertex / HeapSlotModelVertexBase）。
# 多契约靠 CMake 增加调用，reflection_tool CLI 已参数化（--module-name/--template）。
set(MODEL_SLANG "${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.slang")
set(MODEL_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/shaders/model_schema.lua")
set(MODEL_REFLECTION_JSON "${GEN_DIR}/model_reflection.json")
set(MODEL_CPPM "${GEN_SRC_DIR}/model_types_generated.cppm")

add_custom_command(
    OUTPUT ${MODEL_REFLECTION_JSON}
    COMMAND ${SLANGC_EXECUTABLE} ${MODEL_SLANG}
            -target spirv
            -capability spvDescriptorHeapEXT
            -profile sm_6_6+spirv_1_6
            -I "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
            -no-codegen
            -reflection-json ${MODEL_REFLECTION_JSON}
    DEPENDS ${MODEL_SLANG} ${MODEL_SCHEMA}
    COMMENT "Slangc reflection: model.slang -> model_reflection.json"
    VERBATIM
)

add_custom_command(
    OUTPUT ${MODEL_CPPM}
    COMMAND reflection_tool validate
            --json ${MODEL_REFLECTION_JSON}
            --schema ${MODEL_SCHEMA}
    COMMAND reflection_tool generate
            --json ${MODEL_REFLECTION_JSON}
            --schema ${MODEL_SCHEMA}
            --output ${MODEL_CPPM}
            --module-name model_types_generated
            --template ${TYPES_TEMPLATE}
    DEPENDS ${MODEL_REFLECTION_JSON} ${MODEL_SCHEMA} ${TYPES_TEMPLATE} reflection_tool
    COMMENT "Reflection: validate model schema + generate model_types_generated.cppm"
    VERBATIM
)

add_custom_target(model_reflection_generated ALL DEPENDS ${MODEL_CPPM})

# ─── Features Codegen ────────────────────────────────────────────────────────
add_custom_command(
    OUTPUT ${FEATURES_CPPM}
    COMMAND reflection_tool render
            --json ${FEATURES_CONFIG}
            --output ${FEATURES_CPPM}
            --template ${FEATURES_TEMPLATE}
    DEPENDS ${FEATURES_CONFIG} ${FEATURES_TEMPLATE} reflection_tool
    COMMENT "Generating features_generated.cppm from features_config.json"
    VERBATIM
)

add_custom_target(features_generated ALL DEPENDS ${FEATURES_CPPM})

# ─── Shader Compilation ───────────────────────────────────────────────────────

function(compile_slang SHADER_SOURCE OUT_SPV_PATH)
    get_filename_component(SHADER_ABS ${SHADER_SOURCE} ABSOLUTE)
    get_filename_component(SHADER_FULL_NAME ${SHADER_SOURCE} NAME)
    get_filename_component(SHADER_DIR ${SHADER_ABS} DIRECTORY)
    if(NOT EXISTS ${SHADER_SOURCE})
        message(FATAL_ERROR "【Slang 编译错误】找不到该 Shader 文件: ${SHADER_SOURCE}")
    endif()
    string(REPLACE ".slang" ".spv" SPV_NAME "${SHADER_FULL_NAME}")

    set(OUTPUT_SPV "${GEN_DIR}/${SPV_NAME}")

    add_custom_command(
        OUTPUT ${OUTPUT_SPV}
        COMMAND ${SLANGC_EXECUTABLE} ${SHADER_ABS}
                -target spirv
                -capability spvDescriptorHeapEXT
                -profile sm_6_6+spirv_1_6
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
compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.mesh.slang MODEL_MESH_SPV)
compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.frag.slang MODEL_FRAG_SPV)
message(STATUS "MESH_SPV is: ${MESH_SPV}")
message(STATUS "FRAG_SPV is: ${FRAG_SPV}")
message(STATUS "MODEL_MESH_SPV is: ${MODEL_MESH_SPV}")
message(STATUS "MODEL_FRAG_SPV is: ${MODEL_FRAG_SPV}")

# tinygltf_impl.cpp：tinygltf C++ API 实现 TU（v3 非 header-only，见该文件头注释）。
# 单 TU 编译 8756 行实现，-O2 下 clang 又慢又吃内存（24 并发时打爆 swap）；
# 模型加载是启动期一次性路径、非热路径，-O0 编译省内存省时间。
set_source_files_properties(tinygltf_impl.cpp PROPERTIES COMPILE_OPTIONS "-O0")
add_executable(vulkan_practice main.cpp vam_impl.cpp tinygltf_impl.cpp stb_impl.cpp ${MESH_SPV} ${FRAG_SPV} ${MODEL_MESH_SPV} ${MODEL_FRAG_SPV})

# ─── Runtime config copy (configs/ → build dir) ──────────────────────────────
# 程序运行时按相对路径读取 configs/*.json，build 目录必须完整。
# copy_if_different 增量拷贝：修改 config 后下次 just build 即生效，无需重链接。
# 新增 config 文件需要 just config（file GLOB 在 configure 时求值）。
file(GLOB CONFIG_FILES "${PROJECT_SOURCE_DIR}/configs/*")
set(COPIED_CONFIGS)
foreach(cfg ${CONFIG_FILES})
    get_filename_component(cfg_name ${cfg} NAME)
    list(APPEND COPIED_CONFIGS "${CMAKE_CURRENT_BINARY_DIR}/configs/${cfg_name}")
endforeach()
add_custom_command(
    OUTPUT ${COPIED_CONFIGS}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CONFIG_FILES} ${CMAKE_CURRENT_BINARY_DIR}/configs/
    DEPENDS ${CONFIG_FILES}
    COMMENT "Copying configs to build directory..."
    VERBATIM
)
add_custom_target(copy_configs ALL DEPENDS ${COPIED_CONFIGS})

# ─── Model copy (assets/*.glb → build dir, CWD 相对路径读取) ────────────────
# glb 在 assets/（git-lfs 管理），运行时按相对路径 models/ 读取，构建期拷入 build。
# 新增模型需在此加一行 + just config。
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/models)
set(MODEL_GLB_SRC "${PROJECT_SOURCE_DIR}/assets/transparent.glb")
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/models/transparent.glb
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MODEL_GLB_SRC} ${CMAKE_CURRENT_BINARY_DIR}/models/
    DEPENDS ${MODEL_GLB_SRC}
    COMMENT "Copying model (transparent.glb) to build directory..."
    VERBATIM
)
add_custom_target(copy_models ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/models/transparent.glb)

target_sources(vulkan_practice PUBLIC
               FILE_SET CXX_MODULES
                FILES vkerror.cppm utils.cppm sdl3window.cppm QueueFamilyManager.cppm cube.cppm
                      camera.cppm scene_config.cppm
                      shader.cppm VmaBuffer.cppm features.cppm vkinstance.cppm vkdevice.cppm
                      vkswapchain.cppm vkresources.cppm vkrenderer.cppm ecs.cppm
                      gltf_loader.cppm model_importer.cppm scene.cppm llm_debug.cppm
                      ${TYPES_CPPM}
                      ${MODEL_CPPM}
                      ${FEATURES_CPPM}
)

add_dependencies(vulkan_practice reflection_generated model_reflection_generated features_generated)

find_package(Vulkan REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(Sol2 CONFIG REQUIRED)
find_package(unofficial-lua CONFIG REQUIRED)
# tinygltf 是 header-only 且该 vcpkg port 不导出 CMake target：find_path 取头文件目录
find_path(TINYGLTF_INCLUDE_DIRS tiny_gltf.h REQUIRED)
target_include_directories(vulkan_practice PRIVATE ${TINYGLTF_INCLUDE_DIRS})
target_link_libraries(vulkan_practice PRIVATE Vulkan::Vulkan rllogger glm::glm EnTT::EnTT sol2::sol2 lua rlstd)
target_compile_definitions(vulkan_practice PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)
find_package(SDL3 CONFIG REQUIRED)

find_package(VulkanMemoryAllocator CONFIG REQUIRED)
target_link_libraries(vulkan_practice
    PRIVATE
    SDL3::SDL3
    GPUOpen::VulkanMemoryAllocator
    )



target_compile_features(vulkan_practice PRIVATE cxx_std_23)
