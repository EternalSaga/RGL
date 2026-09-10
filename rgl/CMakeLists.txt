# slangc 由 Slang 官方预构建提供：FetchContent 下载（cmake/SlangFetch.cmake，
# 根 CMakeLists 顶部已调用 rgl_fetch_slang() 设置 SLANGC_EXECUTABLE）。
# 替代 vcpkg shader-slang port（port 停在 2026.7.1，上游 master 亦同——
# docs/archive/heap_texture_research.md §九.4；vcpkg.json 已移除该依赖）。
if(NOT SLANGC_EXECUTABLE OR NOT EXISTS "${SLANGC_EXECUTABLE}")
    message(FATAL_ERROR "SLANGC_EXECUTABLE 不可用：根 CMakeLists.txt 须在 add_subdirectory(rgl) 前 include(cmake/SlangFetch.cmake) 并调用 rgl_fetch_slang()")
endif()

message(STATUS "找到 Slang 编译器: ${SLANGC_EXECUTABLE}")

# ─── Scalar Block Layout 项目策略（Phase 4 定案）────────────────────────────
# 所有 slangc 编译（反射 + SPV）统一加 -fvk-use-scalar-layout：push constant 用
# scalar（C 自然）布局而非 GLSL 标准布局。收益：C++ struct（glm 自然对齐 4）与
# shader 逐字段偏移天然一致、零 padding、DrawArgs 100B（标准布局下需 112B + 2 段
# padding）。前提：scalarBlockLayout feature 已启用（features_config.json，
# VkPhysicalDeviceVulkan12Features，validateFeatures 校验）——feature 未启用时
# scalar 布局的 push constant 行为未定义。
# 唯一声明点 = SLANG_FLAGS：反射与 SPV 任何一处 flag 不一致 = 运行时布局错配的隐蔽
# bug（如仅 SPV 侧丢 flag → 反射 100B 与 SPV 标准布局 112B 错配，零报错）。
# 新增 slangc 调用点必须引用本变量，禁止内联重复（generator 的 standardLayoutInfo
# 同步为 scalar 对齐，tools/generator.cppm，并用反射 binding.offset 交叉校验）。
set(SLANG_FLAGS
    -target spirv
    -capability spvDescriptorHeapEXT
    -profile sm_6_6+spirv_1_6
    -fvk-use-scalar-layout
)
# 空格串版：reflection_tool slang 的 --flags 单串参数（boost 不吞 dash 参数，工具端 split）。
# 与 SLANG_FLAGS 同源（string(REPLACE) 派生，非第二声明点）。
string(REPLACE ";" " " SLANG_FLAGS_STR "${SLANG_FLAGS}")
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
file(MAKE_DIRECTORY ${GEN_DIR})

# ─── 反射管线（显式触发，产物入版本控制）─────────────────────────────────────
# 2026-09-02 编译提速：原 4 条 add_custom_command（OUTPUT 在源码树、DEPENDS 含
# reflection_tool）把 ~14s 工具链变成每次 clean 构建的串行前缀（clean 后 tool 缺失
# → 命令判过期 → 重生成 → 下游全部重编）。改为：
#   - `just reflect`（= cmake --build --target reflect）显式重跑全部管线；
#   - 产物（*_reflection.json + generated/*.cppm）纳入版本控制（.gitignore 已放行）；
#   - reflection_check（ALL，毫秒级）守卫：输入比产物新 → FATAL，防忘跑 reflect
#     的静默布局错配（见 cmake/CheckReflectionStale.cmake）。
# 改 types/model/material/features 契约后必须 just reflect && git add generated/。
set(GEN_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated")
file(MAKE_DIRECTORY ${GEN_SRC_DIR})

# 反射管线输入/输出（C API 路径不产 JSON 中间层，只产 generated/*.cppm）
set(TYPES_SLANG "${CMAKE_CURRENT_SOURCE_DIR}/shaders/types.slang")
set(TYPES_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/shaders/types_schema.lua")
set(TYPES_CPPM "${GEN_SRC_DIR}/types_generated.cppm")
set(TYPES_TEMPLATE "${CMAKE_SOURCE_DIR}/tools/templates/types_cppm.inja")

set(MODEL_SLANG "${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.slang")
set(MODEL_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/shaders/model_schema.lua")
set(MODEL_CPPM "${GEN_SRC_DIR}/model_types_generated.cppm")

set(MATERIAL_SLANG "${CMAKE_CURRENT_SOURCE_DIR}/shaders/material.slang")
set(MATERIAL_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/shaders/material_schema.lua")
set(MATERIAL_CPPM "${GEN_SRC_DIR}/material_types_generated.cppm")

set(LIGHTS_SLANG "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lights.slang")
set(LIGHTS_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lights_schema.lua")
set(LIGHTS_CPPM "${GEN_SRC_DIR}/lights_types_generated.cppm")

# 前向头管线（Phase 7 债消）：scene.slang 的 SceneLight（forward 单灯头 @0-48）经 C API
# 反射生成 C++ struct，替换 vkresources.cppm 原手写 LightDataGpu（双源消除）。
set(SCENE_FORWARD_SLANG "${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene.slang")
set(SCENE_FORWARD_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene_forward_schema.lua")
set(SCENE_FORWARD_CPPM "${GEN_SRC_DIR}/scene_forward_types_generated.cppm")

set(FEATURES_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/features_config.json")
set(FEATURES_TEMPLATE "${CMAKE_SOURCE_DIR}/tools/templates/features_cppm.inja")
set(FEATURES_CPPM "${GEN_SRC_DIR}/features_generated.cppm")

# reflect：显式重建全部反射产物（工具由 add_dependencies 先建；管线无条件重跑）。
add_custom_target(reflect
    COMMENT "重跑 5 条反射管线（slangc reflection + validate + generate/render）")
add_dependencies(reflect reflection_tool)
# 4 条 slang 管线统一走 reflection_tool slang 子命令（链接 libslang.so 直接查 C API，
# 不消费 JSON 中间层——见 docs/slang_api_reflection_research.md）。struct 布局 = 编译器
# 实查真值（逐字段 offsetof static_assert），schema 降为提取清单 + 交叉校验期望值。
# 每条 = 单一 COMMAND（原 slangc + validate + generate 三步合一）。
# types 管线
add_custom_command(TARGET reflect PRE_BUILD
    COMMAND reflection_tool slang
            --shader ${TYPES_SLANG}
            --schema ${TYPES_SCHEMA}
            --output ${TYPES_CPPM} --module-name types_generated --template ${TYPES_TEMPLATE}
            --flags "${SLANG_FLAGS_STR} -no-codegen"
            --include "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    VERBATIM)
# model 管线（模板复用 types_cppm.inja，同既有行为）
add_custom_command(TARGET reflect PRE_BUILD
    COMMAND reflection_tool slang
            --shader ${MODEL_SLANG}
            --schema ${MODEL_SCHEMA}
            --output ${MODEL_CPPM} --module-name model_types_generated --template ${TYPES_TEMPLATE}
            --flags "${SLANG_FLAGS_STR} -no-codegen"
            --include "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    VERBATIM)
# material 管线
add_custom_command(TARGET reflect PRE_BUILD
    COMMAND reflection_tool slang
            --shader ${MATERIAL_SLANG}
            --schema ${MATERIAL_SCHEMA}
            --output ${MATERIAL_CPPM} --module-name material_types_generated --template ${TYPES_TEMPLATE}
            --flags "${SLANG_FLAGS_STR} -no-codegen"
            --include "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    VERBATIM)
# lights 管线（Phase 7 D3：多灯 GpuLight struct + kMaxLights/布局常量）
add_custom_command(TARGET reflect PRE_BUILD
    COMMAND reflection_tool slang
            --shader ${LIGHTS_SLANG}
            --schema ${LIGHTS_SCHEMA}
            --output ${LIGHTS_CPPM} --module-name lights_types_generated --template ${TYPES_TEMPLATE}
            --flags "${SLANG_FLAGS_STR} -no-codegen"
            --include "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    VERBATIM)
# 前向头管线（Phase 7 债消）：scene.slang SceneLight → 替换手写 LightDataGpu（双源消除）
add_custom_command(TARGET reflect PRE_BUILD
    COMMAND reflection_tool slang
            --shader ${SCENE_FORWARD_SLANG}
            --schema ${SCENE_FORWARD_SCHEMA}
            --output ${SCENE_FORWARD_CPPM} --module-name scene_forward_types_generated --template ${TYPES_TEMPLATE}
            --flags "${SLANG_FLAGS_STR} -no-codegen"
            --include "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    VERBATIM)
# features 管线（config JSON → 模板渲染，无 slangc 步骤）
add_custom_command(TARGET reflect PRE_BUILD
    COMMAND reflection_tool render --json ${FEATURES_CONFIG}
            --output ${FEATURES_CPPM} --template ${FEATURES_TEMPLATE}
    VERBATIM)
# 收尾：写输入哈希清单（check 守卫的比对基准；与产物一起提交）
add_custom_command(TARGET reflect PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -DMODE=write
            -DPAIRS_FILE=${CMAKE_CURRENT_BINARY_DIR}/reflection_pairs.cmake
            -DROOT=${CMAKE_SOURCE_DIR}
            -DMANIFEST=${GEN_SRC_DIR}/reflection_manifest.txt
            -P ${CMAKE_SOURCE_DIR}/cmake/ReflectionManifest.cmake
    VERBATIM)

# 新鲜度守卫：每次构建开头跑（~30ms），输入新于产物即拦。
# PAIRS 经文件传入（命令行 -D 参数会被 CMake 按 |/, 拆分转义，踩过）。
# 新鲜度 = 输入 SHA1 清单（file(TIMESTAMP) 在本机 CMake 4.3.4 恒空，不可用）。
set(REFLECTION_PAIRS
    "${TYPES_CPPM}|${TYPES_SLANG},${TYPES_SCHEMA},${TYPES_TEMPLATE}"
    "${MODEL_CPPM}|${MODEL_SLANG},${MODEL_SCHEMA},${TYPES_TEMPLATE}"
    "${MATERIAL_CPPM}|${MATERIAL_SLANG},${MATERIAL_SCHEMA},${TYPES_TEMPLATE}"
    "${LIGHTS_CPPM}|${LIGHTS_SLANG},${LIGHTS_SCHEMA},${TYPES_TEMPLATE}"
    "${SCENE_FORWARD_CPPM}|${SCENE_FORWARD_SLANG},${SCENE_FORWARD_SCHEMA},${TYPES_TEMPLATE}"
    "${FEATURES_CPPM}|${FEATURES_CONFIG},${FEATURES_TEMPLATE}")
string(REPLACE ";" "\n" _pairs_src "${REFLECTION_PAIRS}")
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/reflection_pairs.cmake CONTENT "
set(PAIRS
${_pairs_src}
)
")
add_custom_target(reflection_check ALL
    COMMAND ${CMAKE_COMMAND} -DMODE=check
        -DPAIRS_FILE=${CMAKE_CURRENT_BINARY_DIR}/reflection_pairs.cmake
        -DROOT=${CMAKE_SOURCE_DIR}
        -DMANIFEST=${GEN_SRC_DIR}/reflection_manifest.txt
        -P ${CMAKE_SOURCE_DIR}/cmake/ReflectionManifest.cmake
    COMMENT "检查反射输入哈希（变更时请 just reflect）")

# ─── Doxygen 注释检查（doc_check，只警告不阻断）───────────────────────────────
# 每次构建随 ALL 并行跑（与编译不互锁）：复用 compile_commands.json 对 rgl/ 下
# .cppm 做 clang -ast-dump（同一 LLVM，纯标准库 Python，见
# tools/doccheck/doccheck.py），检查类/枚举/函数/方法声明正上方有 /** */ 块。
# AST 缓存 build/doccheck_cache（源+命令哈希键）：增量只重 dump 改动文件（秒级），
# 全量 ~1min。接口已声明函数在 impl 的定义处不重复要求（注释真相源 = 声明处）。
# 单独跑：just doc-check [--methods/--strict/...]（默认构建已含 --methods）。
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_target(doc_check ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/doccheck/doccheck.py
            --build-dir ${CMAKE_BINARY_DIR} --methods
    COMMENT "Doxygen 注释检查（警告不阻断）")

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
        COMMAND ${SLANGC_EXECUTABLE} ${SHADER_ABS} ${SLANG_FLAGS}
                -I "${SHADER_DIR}"
                -o ${OUTPUT_SPV}
        DEPENDS ${SHADER_ABS} ${EXTRA_DEPS}
        COMMENT "Slangc automatically compiling: ${SHADER_FULL_NAME} -> ${SPV_NAME}"
        VERBATIM
    )

    set(${OUT_SPV_PATH} ${OUTPUT_SPV} PARENT_SCOPE)
endfunction()
# EXTRA_DEPS = 本 shader import 的契约文件（改契约须触发重编译，见 compile_slang 内注释的踩坑记录）
compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube.mesh.slang MESH_SPV
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/types.slang ${CMAKE_CURRENT_SOURCE_DIR}/shaders/material.slang)
compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube.frag.slang FRAG_SPV
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/types.slang)
compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.mesh.slang MODEL_MESH_SPV
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.slang ${CMAKE_CURRENT_SOURCE_DIR}/shaders/material.slang)
 compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.frag.slang MODEL_FRAG_SPV
     ${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.slang ${CMAKE_CURRENT_SOURCE_DIR}/shaders/material.slang)
 # Phase 5 批次 3：真简化 PBR frag（mesh 复用 model.mesh.spv，import scene.slang 光照数据）
 compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/model_pbr.frag.slang MODEL_PBR_FRAG_SPV
     ${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.slang ${CMAKE_CURRENT_SOURCE_DIR}/shaders/material.slang
     ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene.slang)
 # Phase 5 批次 3：基础 Phong frag（第二条光照路线，同契约同 mesh SPV，scene.lua styles 一行切换）
 compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/model_phong.frag.slang MODEL_PHONG_FRAG_SPV
     ${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.slang ${CMAKE_CURRENT_SOURCE_DIR}/shaders/material.slang
     ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene.slang)
 # Phase 5.5：IBL 路线（model_pbr + 环境 cubemap 采样替换常 ambient；env.slang 契约）
 compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/model_pbr_ibl.frag.slang MODEL_PBR_IBL_FRAG_SPV
     ${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.slang ${CMAKE_CURRENT_SOURCE_DIR}/shaders/material.slang
     ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene.slang ${CMAKE_CURRENT_SOURCE_DIR}/shaders/env.slang)
 # Phase 5.5 批次 4：天空盒（全屏大三角 mesh + clip→world 射线采 env cubemap frag）
 compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/skybox.mesh.slang SKYBOX_MESH_SPV
     ${CMAKE_CURRENT_SOURCE_DIR}/shaders/sky.slang ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene.slang)
 compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/skybox.frag.slang SKYBOX_FRAG_SPV
     ${CMAKE_CURRENT_SOURCE_DIR}/shaders/sky.slang ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene.slang
     ${CMAKE_CURRENT_SOURCE_DIR}/shaders/env.slang)
 # Phase 7 D1：deferred G-buffer 几何 frag（mesh 复用 model.mesh.spv，写 4 MRT，import scene 对象矩阵）
 compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/deferred_geometry.frag.slang DEFERRED_GEOM_FRAG_SPV
     ${CMAKE_CURRENT_SOURCE_DIR}/shaders/model.slang ${CMAKE_CURRENT_SOURCE_DIR}/shaders/material.slang
     ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene.slang)
  # Phase 7 D1：light pass（全屏大三角 mesh + 采样 G-buffer 的 deferred 光照 frag）
  compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/light.mesh.slang LIGHT_MESH_SPV)
  compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/light.frag.slang LIGHT_FRAG_SPV
      ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene.slang)
  # Phase 7b（批次 5）：GPU culling compute（独立契约 cull.comp.slang 头注释，无 import 契约文件）
  compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/cull.comp.slang CULL_SPV)
  # 静态 shader DGC spike：per-entry 序列流扇出（独立契约 kernel 头注释，docs/radv_static_dgc_spike.md）
  compile_slang(${CMAKE_CURRENT_SOURCE_DIR}/shaders/stream_fanout.comp.slang STREAM_FANOUT_SPV)
 message(STATUS "MESH_SPV is: ${MESH_SPV}")
 message(STATUS "FRAG_SPV is: ${FRAG_SPV}")
 message(STATUS "MODEL_MESH_SPV is: ${MODEL_MESH_SPV}")
 message(STATUS "MODEL_FRAG_SPV is: ${MODEL_FRAG_SPV}")
 message(STATUS "MODEL_PBR_FRAG_SPV is: ${MODEL_PBR_FRAG_SPV}")
 message(STATUS "MODEL_PHONG_FRAG_SPV is: ${MODEL_PHONG_FRAG_SPV}")
 message(STATUS "MODEL_PBR_IBL_FRAG_SPV is: ${MODEL_PBR_IBL_FRAG_SPV}")

# tinygltf_impl.cpp：tinygltf C++ API 实现 TU（v3 非 header-only，见该文件头注释）。
# 曾用 -O0（-O2 下编译吃内存，24 并发时打爆 swap）；2026-09-02 用户调整内存/显存
# 分配至 32GB 后回归目标级 -O2（启动路径代码质量优先，编译峰值内存已非瓶颈）。

add_executable(rgl main.cpp vam_impl.cpp tinygltf_impl.cpp stb_impl.cpp ${MESH_SPV} ${FRAG_SPV} ${MODEL_MESH_SPV} ${MODEL_FRAG_SPV} ${MODEL_PBR_FRAG_SPV} ${MODEL_PHONG_FRAG_SPV} ${MODEL_PBR_IBL_FRAG_SPV} ${SKYBOX_MESH_SPV} ${SKYBOX_FRAG_SPV} ${DEFERRED_GEOM_FRAG_SPV} ${LIGHT_MESH_SPV} ${LIGHT_FRAG_SPV} ${CULL_SPV} ${STREAM_FANOUT_SPV})

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
    "${PROJECT_SOURCE_DIR}/assets/realistic_wooden_cottage_garden_house_3d_model.glb"
    "${PROJECT_SOURCE_DIR}/assets/castle_of_loarre.glb"
    "${PROJECT_SOURCE_DIR}/assets/kyoto_fushimi_inari_taisha_temple.glb"
    "${PROJECT_SOURCE_DIR}/assets/bistro_exterior.glb")
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

# ─── Skybox 素材拷贝 (assets/<dir>/*.png → build skybox/<dir>/, Phase 5.5) ──────
# 6 面 cubemap 素材（nightSky/rainbow，面顺序定案见 docs/archive/phase5_5_plan.md），运行时按
# 相对路径 skybox/<dir>/ 读取。新增素材目录需在此加 dir 名 + just config（GLOB 于
# configure 时求值）。
set(SKYBOX_DIRS nightSky rainbow)
set(SKYBOX_OUTPUTS)
foreach(dir ${SKYBOX_DIRS})
    file(GLOB SKYBOX_DIR_FILES "${PROJECT_SOURCE_DIR}/assets/${dir}/*")
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/skybox/${dir})
    foreach(f ${SKYBOX_DIR_FILES})
        get_filename_component(fname ${f} NAME)
        list(APPEND SKYBOX_OUTPUTS ${CMAKE_CURRENT_BINARY_DIR}/skybox/${dir}/${fname})
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/skybox/${dir}/${fname}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${f} ${CMAKE_CURRENT_BINARY_DIR}/skybox/${dir}/
            DEPENDS ${f}
            COMMENT "Copying skybox asset (${dir}/${fname}) to build directory..."
            VERBATIM
        )
    endforeach()
endforeach()
add_custom_target(copy_skybox ALL DEPENDS ${SKYBOX_OUTPUTS})

target_sources(rgl PUBLIC
               FILE_SET CXX_MODULES
                FILES rlvk.cppm vkerror.cppm utils.cppm sdl3window.cppm QueueFamilyManager.cppm cube.cppm
                      camera.cppm camera_config.cppm scene_config.cppm runtime_config.cppm diagnostics.cppm
                       shader.cppm shader_registry.cppm VmaBuffer.cppm features.cppm vkinstance.cppm vkdevice.cppm
                       vkswapchain.cppm vkresources.cppm vkrenderer.cppm ecs.cppm frame_draw_list.cppm
                       asset_table.cppm render_list.cppm render_graph.cppm render_graph_config.cppm
                      gltf_loader.cppm model_importer.cppm builtin_geometry.cppm texture_pool.cppm texture_upload.cppm scene_context.cppm llm_debug.cppm
                        llm_debug_impl.cppm vkrenderer_impl.cppm render_graph_impl.cppm render_graph_config_impl.cppm model_importer_impl.cppm builtin_geometry_impl.cppm texture_upload_impl.cppm camera_config_impl.cppm scene_config_impl.cppm runtime_config_impl.cppm diagnostics_impl.cppm render_list_impl.cppm gpu_canary_impl.cppm ecs_impl.cppm vkresources_impl.cppm shader_registry_impl.cppm gltf_loader_impl.cppm gpu_cull_impl.cppm vk_indirect_commands_impl.cppm frame_capture_impl.cppm cull_driver_impl.cppm frame_session_impl.cppm
                       gpu_canary.cppm gpu_cull.cppm vk_indirect_commands.cppm state_kit.cppm frame_capture.cppm cull_driver.cppm frame_session.cppm
                         ${TYPES_CPPM}
                         ${MODEL_CPPM}
                         ${MATERIAL_CPPM}
                         ${LIGHTS_CPPM}
                         ${SCENE_FORWARD_CPPM}
                         ${FEATURES_CPPM}
)

add_dependencies(rgl reflection_check)

find_package(Vulkan REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(Sol2 CONFIG REQUIRED)
find_package(unofficial-lua CONFIG REQUIRED)
# tinygltf 是 header-only 且该 vcpkg port 不导出 CMake target：find_path 取头文件目录
find_path(TINYGLTF_INCLUDE_DIRS tiny_gltf.h REQUIRED)
target_include_directories(rgl PRIVATE ${TINYGLTF_INCLUDE_DIRS})
target_link_libraries(rgl PRIVATE Vulkan::Vulkan rllogger glm::glm EnTT::EnTT sol2::sol2 lua rlstd)
target_compile_definitions(rgl PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)
find_package(SDL3 CONFIG REQUIRED)

find_package(VulkanMemoryAllocator CONFIG REQUIRED)
target_link_libraries(rgl
    PRIVATE
    SDL3::SDL3
    GPUOpen::VulkanMemoryAllocator
    )

# NVIDIA Aftermath GPU crash dump（诊断能力，编译期开关；默认关，排查 NV 崩溃时开）
# 用法：just config 前设 RGL_AFTERMATH=ON，或 cmake -DRGL_AFTERMATH=ON
option(RGL_AFTERMATH "Enable NVIDIA Aftermath GPU crash dumps (Linux/NVIDIA only)" OFF)
if(RGL_AFTERMATH AND UNIX AND NOT APPLE)
    set(AFTERMATH_SDK_DIR "${CMAKE_SOURCE_DIR}/../Aftermath")
    target_include_directories(rgl PRIVATE "${AFTERMATH_SDK_DIR}/include")
    target_link_libraries(rgl PRIVATE "${AFTERMATH_SDK_DIR}/lib/x64/libGFSDK_Aftermath_Lib.x64.so")
    target_compile_definitions(rgl PRIVATE RGL_AFTERMATH=1)
endif()



target_compile_features(rgl PRIVATE cxx_std_23)
