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
# 6 条 slang 管线统一走 reflection_tool slang 子命令（链接 libslang.so 直接查 C API，
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

# 调用：rgl_reflect_compute_access(<产物名 相对 generated/> <相对 rgl/ 的 shader>...)
# compute entry point 的 binding **读写方向**表。与 rgl_reflect_slang 的差别不是"另一条同类
# 管线"，而是**另一个问题**：那条反射 `*.slang` 契约文件的 struct 布局（C++ 要按它发字段），
# 本条反射 `*.comp.slang` 入口的 binding 表 + 方向（C++ 不消费，消费者是 checks 规则）。
# 产物是 JSON 不是 .cppm ⟹ **不进 RGL_GENERATED_CPPM**（没有 C++ 消费者），但**必须进
# REFLECTION_PAIRS**——不进护栏的产物就是没人守的漂移产物：方向表过期时"零警告"会被读成
# 「同步边对过账」，那正是它存在的理由的反面。立论 → docs/compute_调研.md §7.10。
# 🔴 护栏输入 = 显式列的 comp shader **+ 整个 rgl/shaders/*.slang 的 glob**：方向会从被
# import 的文件来（cull.comp.slang 的 objectMatrixTable 来自 `import scene_tables`），只列
# 直接输入 = 改了被 import 的文件不触发护栏 = 静默使用过期方向表。代价是**任何** shader
# 改动都会触发一次护栏——这是**故意的保守**：宁可多跑一次 just reflect（秒级），不要静默
# 拿一份过期方向表去对同步边（对错的同步边看不出来）。glob 展开见 ReflectionManifest.cmake
# 的 expand_inputs（write/check 共用同一个函数）。
function(rgl_reflect_compute_access product)
    set(json "${GEN_SRC_DIR}/${product}")
    set(shader_args "")
    set(pair_ins "")
    foreach(shard ${ARGN})
        list(APPEND shader_args --shader "${CMAKE_CURRENT_SOURCE_DIR}/${shard}")
        list(APPEND pair_ins "${CMAKE_CURRENT_SOURCE_DIR}/${shard}")
    endforeach()
    add_custom_command(TARGET reflect PRE_BUILD
        COMMAND reflection_tool compute-access
                ${shader_args}
                --output ${json}
                --flags "${SLANG_FLAGS_STR} -no-codegen -entry main -stage compute"
                --include "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        VERBATIM)
    # glob 串不能含 `,`（PAIRS 的输入分隔符）或 `|`（产物/输入分隔符）——本仓路径不含。
    # configure 期就拼成绝对路径：清单脚本跑在 `cmake -P` 里，那里没有 CMAKE_SOURCE_DIR。
    list(APPEND pair_ins "glob:${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.slang")
    string(REPLACE ";" "," pair_ins "${pair_ins}")
    set(_pair_list ${REFLECTION_PAIRS} "${json}|${pair_ins}")
    set(REFLECTION_PAIRS "${_pair_list}" PARENT_SCOPE)
endfunction()

# 调用：rgl_reflect_compute_push(<module_name> <相对 rgl/ 的 shader>...)
# compute shader 的 push constant → C++ struct（产物 = generated/<module_name>.cppm）。
#
# 🔴 与 rgl_reflect_slang 的差别**不是"另一条同类管线"，而是没有 schema**，且这个"没有"是
# 定案不是省事（docs/compute_调研.md §7.12：双相对账成立的前提 = 两个来源各自携带对方推不出
# 的信息）。那条的 *_schema.lua 带 heapSlotOffset（运行期堆槽 = reservedSlots() + offset，
# C++ 侧分配策略，shader 里根本没有）⟹ 配得上当第二来源；而 compute push 契约只能携带
# stride/offset/字段名——**全部是从 shader 手抄的派生信息**，按同一条判据它不是真第二来源。
# 所以本条的提取清单是**隐式**的（= 调用处那份 shader 名单），参数里没有 schema 位。
#
# 与 rgl_reflect_compute_access 的差别 = 消费面：那条产 JSON、消费者是 checks 规则；本条产
# .cppm、消费者是产品代码（evssm/izb 原先手写的镜像 push struct）。手写镜像的那几条
# `static_assert(offsetof(...))` 量的是**手抄的 C++ struct 而不是 shader** ⟹ shader 换序而
# C++ 没动时 assert 全过（关于 C++ 侧的同义反复）。本条让那份布局第一次有出处。
function(rgl_reflect_compute_push module_name)
    set(cppm "${GEN_SRC_DIR}/${module_name}.cppm")
    set(shader_args "")
    set(pair_ins "")
    foreach(shard ${ARGN})
        list(APPEND shader_args --shader "${CMAKE_CURRENT_SOURCE_DIR}/${shard}")
        list(APPEND pair_ins "${CMAKE_CURRENT_SOURCE_DIR}/${shard}")
    endforeach()
    # 工具端一枚 shader 一个 SlangContext（八枚都有全局 `push`/`main`，同批编译必炸），
    # 所以这里只是把名单传下去，不在 CMake 侧循环起进程。
    add_custom_command(TARGET reflect PRE_BUILD
        COMMAND reflection_tool compute-push
                ${shader_args}
                --output ${cppm} --module-name ${module_name} --template ${TYPES_TEMPLATE}
                --flags "${SLANG_FLAGS_STR} -no-codegen"
                --include "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
        VERBATIM)
    # glob 闭包同 rgl_reflect_compute_access（理由见那里的注释）：push struct 的字段声明可能
    # 住在被 import 的文件里，只列直接输入 = 改了被 import 的文件不触发护栏 = 静默使用过期
    # 布局。glob 串不能含 `,` 或 `|`（PAIRS 的分隔符）——本仓路径不含。
    list(APPEND pair_ins "glob:${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.slang")
    string(REPLACE ";" "," pair_ins "${pair_ins}")
    set(_cppm_list ${RGL_GENERATED_CPPM} ${cppm})
    set(_pair_list ${REFLECTION_PAIRS} "${cppm}|${pair_ins}")
    set(RGL_GENERATED_CPPM "${_cppm_list}" PARENT_SCOPE)
    set(REFLECTION_PAIRS "${_pair_list}" PARENT_SCOPE)
endfunction()

# reflect：显式重建全部反射产物（工具由 add_dependencies 先建；管线无条件重跑）。
add_custom_target(reflect
    COMMENT "重跑 10 条反射管线（6 条 slangc reflection + features + param_semantics + compute_access + compute_push）")
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
# 逐簇剔除契约（★1-a2-i）：MeshletCullItem / cullPush 经 C API 反射生成 C++ struct
rgl_reflect_slang(shaders/meshlet_cull.slang shaders/meshlet_cull_schema.lua meshlet_cull_types_generated)

# compute binding 读写方向表（B2，docs/compute_调研.md §7.10/§7.11）。九枚按名字典序列出，
# 与产物的 shaders 段同序（产物自己也会排一遍，这里同序只为 diff 好读）。
rgl_reflect_compute_access(compute_access.json
    shaders/cull.comp.slang
    shaders/env_prefilter.comp.slang
    shaders/evssm_downsample.comp.slang
    shaders/evssm_moments.comp.slang
    shaders/izb_create.comp.slang
    shaders/izb_prime.comp.slang
    shaders/meshlet_cull.comp.slang
    shaders/mip_gen.comp.slang
    shaders/stream_fanout.comp.slang)

# 🔴 上面 6 条 rgl_reflect_slang 的护栏输入**至今只列直接文件、不含 import 闭包**——同一个
# 洞（改了被 import 的 `*.slang` 不触发 SHA1 守卫 = 静默使用过期生成码），只是它落在布局
# 契约上、比方向表更贵。本条管线用 glob 把这个洞堵上了，那 6 条**本批不修**：改它们的输入
# 集要重跑并复核全部 6 份已提交 .cppm，是独立的一刀。账 → docs/compute_调研.md §7.10。

# compute push constant 的 C++ struct（无 schema，理由见函数头注释）。八枚按名字典序列出。
# 🔴 **不含 meshlet_cull.comp.slang**：它的 push（`cullPush` → MeshletCullPush）已由上面
# meshlet_cull_schema.lua 那条 slang 管线产出，这里再产一份 = 给同一个契约造第二个真相源
# （反向锚点 = tests/test_compute_push_layout.cppm）。
rgl_reflect_compute_push(compute_push_types_generated
    shaders/cull.comp.slang
    shaders/env_prefilter.comp.slang
    shaders/evssm_downsample.comp.slang
    shaders/evssm_moments.comp.slang
    shaders/izb_create.comp.slang
    shaders/izb_prime.comp.slang
    shaders/mip_gen.comp.slang
    shaders/stream_fanout.comp.slang)

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
