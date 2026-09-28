# ─── 反射管线（显式触发，产物入版本控制）─────────────────────────────────────
# 由 rgl/CMakeLists.txt include；须先 include(RglSlang.cmake)（依赖 SLANG_FLAGS_STR）。
# 2026-09-02 编译提速：原 4 条 add_custom_command（OUTPUT 在源码树、DEPENDS 含
# reflection_tool）把 ~14s 工具链变成每次 clean 构建的串行前缀（clean 后 tool 缺失
# → 命令判过期 → 重生成 → 下游全部重编）。改为：
#   - `just reflect`（= cmake --build --target reflect）显式重跑全部管线；
#   - 产物（*_reflection.json + generated/*.cppm）纳入版本控制（.gitignore 已放行）；
#   - reflection_check（ALL，毫秒级）守卫：输入比产物新 → FATAL，防忘跑 reflect
#     的静默布局错配（见 cmake/ReflectionManifest.cmake）。
# 改 types/model/material/features 契约后必须 just reflect && git add generated/。
include_guard()

if(NOT DEFINED SLANG_FLAGS_STR)
    message(FATAL_ERROR "RglReflection: 须先 include(cmake/RglSlang.cmake)（SLANG_FLAGS_STR）")
endif()

set(GEN_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated")
file(MAKE_DIRECTORY ${GEN_SRC_DIR})

set(TYPES_TEMPLATE "${CMAKE_SOURCE_DIR}/tools/templates/types_cppm.inja")
set(FEATURES_TEMPLATE "${CMAKE_SOURCE_DIR}/tools/templates/features_cppm.inja")
set(FEATURES_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/features_config.json")

set(REFLECTION_PAIRS "")
set(RGL_GENERATED_CPPM "")

# 调用：rgl_reflect_slang(<slang 相对 rgl/> <schema 相对 rgl/> <module_name>)
# 产物 = generated/<module_name>.cppm；模板固定 types_cppm.inja。
# 5 条 slang 管线统一走 reflection_tool slang 子命令（链接 libslang.so 直接查 C API，
# 不消费 JSON 中间层——见 docs/archive/slang_api_reflection_research.md）。struct 布局 = 编译器
# 实查真值（逐字段 offsetof static_assert），schema 降为提取清单 + 交叉校验期望值。
# 每条 = 单一 COMMAND（原 slangc + validate + generate 三步合一）。
function(rgl_reflect_slang slang_rel schema_rel module_name)
    set(slang "${CMAKE_CURRENT_SOURCE_DIR}/${slang_rel}")
    set(schema "${CMAKE_CURRENT_SOURCE_DIR}/${schema_rel}")
    set(cppm "${GEN_SRC_DIR}/${module_name}.cppm")
    add_custom_command(TARGET reflect PRE_BUILD
        COMMAND reflection_tool slang
                --shader ${slang}
                --schema ${schema}
                --output ${cppm} --module-name ${module_name} --template ${TYPES_TEMPLATE}
                --flags "${SLANG_FLAGS_STR} -no-codegen"
                --include "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        VERBATIM)
    # 函数体 set PARENT_SCOPE 写入调用方（rgl/ 目录作用域）；RHS 读继承的旧值做追加
    set(_cppm_list ${RGL_GENERATED_CPPM} ${cppm})
    set(_pair_list ${REFLECTION_PAIRS} "${cppm}|${slang},${schema},${TYPES_TEMPLATE}")
    set(RGL_GENERATED_CPPM "${_cppm_list}" PARENT_SCOPE)
    set(REFLECTION_PAIRS "${_pair_list}" PARENT_SCOPE)
endfunction()

# reflect：显式重建全部反射产物（工具由 add_dependencies 先建；管线无条件重跑）。
add_custom_target(reflect
    COMMENT "重跑 5 条反射管线（slangc reflection + validate + generate/render）")
add_dependencies(reflect reflection_tool)

# types 管线
rgl_reflect_slang(shaders/types.slang shaders/types_schema.lua types_generated)
# model 管线（模板复用 types_cppm.inja，同既有行为）
rgl_reflect_slang(shaders/model.slang shaders/model_schema.lua model_types_generated)
# material 管线
rgl_reflect_slang(shaders/material.slang shaders/material_schema.lua material_types_generated)
# lights 管线（Phase 7 D3：多灯 GpuLight struct + kMaxLights/布局常量）
rgl_reflect_slang(shaders/lights.slang shaders/lights_schema.lua lights_types_generated)
# 前向头管线（Phase 7 债消）：scene.slang 的 SceneLight（forward 单灯头 @0-48）经 C API
# 反射生成 C++ struct，替换 vkresources.cppm 原手写 LightDataGpu（双源消除）。
rgl_reflect_slang(shaders/scene.slang shaders/scene_forward_schema.lua scene_forward_types_generated)

# features 管线（config JSON → 模板渲染，无 slangc 步骤）
set(FEATURES_CPPM "${GEN_SRC_DIR}/features_generated.cppm")
add_custom_command(TARGET reflect PRE_BUILD
    COMMAND reflection_tool render --json ${FEATURES_CONFIG}
            --output ${FEATURES_CPPM} --template ${FEATURES_TEMPLATE}
    VERBATIM)
# 目录作用域直接 set（PARENT_SCOPE 会写到父目录作用域——rgl 的 FILE_SET 读不到）
list(APPEND RGL_GENERATED_CPPM ${FEATURES_CPPM})
list(APPEND REFLECTION_PAIRS "${FEATURES_CPPM}|${FEATURES_CONFIG},${FEATURES_TEMPLATE}")

# param_semantics 管线（Phase 10 批 10.6：面板控件语义 JSON → 模板渲染，无 slangc 步骤，
# 同 features 管线形态）——label/range/speed 单一真相源 = rgl/param_semantics.json
set(PARAM_SEMANTICS_TEMPLATE "${CMAKE_SOURCE_DIR}/tools/templates/param_semantics_cppm.inja")
set(PARAM_SEMANTICS_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/param_semantics.json")
set(PARAM_SEMANTICS_CPPM "${GEN_SRC_DIR}/param_semantics_generated.cppm")
add_custom_command(TARGET reflect PRE_BUILD
    COMMAND reflection_tool render --json ${PARAM_SEMANTICS_CONFIG}
            --output ${PARAM_SEMANTICS_CPPM} --template ${PARAM_SEMANTICS_TEMPLATE}
    VERBATIM)
list(APPEND RGL_GENERATED_CPPM ${PARAM_SEMANTICS_CPPM})
list(APPEND REFLECTION_PAIRS "${PARAM_SEMANTICS_CPPM}|${PARAM_SEMANTICS_CONFIG},${PARAM_SEMANTICS_TEMPLATE}")

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
