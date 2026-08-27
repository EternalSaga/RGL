# slangc 由 Slang 官方预构建提供：FetchContent 下载（cmake/SlangFetch.cmake，
# 根 CMakeLists 顶部已调用 rgl_fetch_slang() 设置 SLANGC_EXECUTABLE）。
# 替代 vcpkg shader-slang port（port 停在 2026.7.1，上游 master 亦同——
# docs/heap_texture_research.md §九.4；vcpkg.json 已移除该依赖）。
if(NOT SLANGC_EXECUTABLE OR NOT EXISTS "${SLANGC_EXECUTABLE}")
    message(FATAL_ERROR "SLANGC_EXECUTABLE 不可用：根 CMakeLists.txt 须在 add_subdirectory(vulkan_practice) 前 include(cmake/SlangFetch.cmake) 并调用 rgl_fetch_slang()")
endif()

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

# 调用：compile_slang(SHADER_SOURCE OUT_SPV_PATH [EXTRA_DEPS...])
# 不声明命名参数（CMake 4 对固定参数个数校验严格，可选参数/UNPARSED 行为均变化），
# 从 ARGN 手动取参。
function(compile_slang)
    list(GET ARGN 0 SHADER_SOURCE)
    list(GET ARGN 1 OUT_SPV_PATH)
    list(REMOVE_AT ARGN 0 1)
    set(EXTRA_DEPS ${ARGN})
    get_filename_component(SHADER_ABS ${SHADER_SOURCE} ABSOLUTE)
    get_filename_component(SHADER_FULL_NAME ${SHADER_SOURCE} NAME)
    get_filename_component(SHADER_DIR ${SHADER_ABS} DIRECTORY)
    if(NOT EXISTS ${SHADER_SOURCE})
        message(FATAL_ERROR "【Slang 编译错误】找不到该 Shader 文件: ${SHADER_SOURCE}")
    endif()
    string(REPLACE ".slang" ".spv" SPV_NAME "${SHADER_FULL_NAME}")

    set(OUTPUT_SPV "${GEN_DIR}/${SPV_NAME}")

    # EXTRA_DEPS：本 shader import 的模块文件——只依赖自身时，改共享模块不会触发重编译
    #（model.*.slang import model；2026-08-27 model.slang 改 float4 后 spv 未重编译踩坑）
    add_custom_command(
        OUTPUT ${OUTPUT_SPV}
        COMMAND ${SLANGC_EXECUTABLE} ${SHADER_ABS}
                -target spirv
                -capability spvDescriptorHeapEXT
                -profile sm_6_6+spirv_1_6
                -I "${SHADER_DIR}"
                -o ${OUTPUT_SPV}
        DEPENDS ${SHADER_ABS} ${EXTRA_DEPS}
        COMMENT "Slangc automatically compiling: ${SHADER_FULL_NAME} -> ${SPV_NAME}"
        VERBATIM
    )

    set(${OUT_SPV_PATH} ${OUTPUT_SPV} PARENT_SCOPE)
endfunction()
compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube.mesh.slang MESH_SPV)
compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube.frag.slang FRAG_SPV)
compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.mesh.slang MODEL_MESH_SPV ${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.slang)
compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.frag.slang MODEL_FRAG_SPV ${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.slang)
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
set(MODEL_GLB_SOURCES
    "${PROJECT_SOURCE_DIR}/assets/transparent.glb"
    "${PROJECT_SOURCE_DIR}/assets/grass_variations.glb"
    "${PROJECT_SOURCE_DIR}/assets/realistic_wooden_cottage_garden_house_3d_model.glb")
set(MODEL_GLB_OUTPUTS)
foreach(MODEL_GLB_SRC ${MODEL_GLB_SOURCES})
    get_filename_component(MODEL_GLB_NAME ${MODEL_GLB_SRC} NAME)
    list(APPEND MODEL_GLB_OUTPUTS ${CMAKE_CURRENT_BINARY_DIR}/models/${MODEL_GLB_NAME})
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/models/${MODEL_GLB_NAME}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MODEL_GLB_SRC} ${CMAKE_CURRENT_BINARY_DIR}/models/
        DEPENDS ${MODEL_GLB_SRC}
        COMMENT "Copying model (${MODEL_GLB_NAME}) to build directory..."
        VERBATIM
    )
endforeach()
add_custom_target(copy_models ALL DEPENDS ${MODEL_GLB_OUTPUTS})

target_sources(vulkan_practice PUBLIC
               FILE_SET CXX_MODULES
                FILES vkerror.cppm utils.cppm sdl3window.cppm QueueFamilyManager.cppm cube.cppm
                      camera.cppm scene_config.cppm
                      shader.cppm VmaBuffer.cppm features.cppm vkinstance.cppm vkdevice.cppm
                      vkswapchain.cppm vkresources.cppm vkrenderer.cppm ecs.cppm
                      gltf_loader.cppm model_importer.cppm texture_pool.cppm scene.cppm llm_debug.cppm
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
