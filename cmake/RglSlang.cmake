# ─── Slang 编译基础设施（SLANG_FLAGS 契约 + compile_slang/rgl_shader）──────────
# 由 rgl/CMakeLists.txt include；根 CMakeLists 须已 rgl_fetch_slang()。
#
# ─── Scalar Block Layout 项目策略（Phase 4 定案）────────────────────────────
# 所有 slangc 编译（反射 + SPV）统一加 -fvk-use-scalar-layout：push constant 用
# scalar（C 自然）布局而非 GLSL 标准布局。收益：C++ struct（glm 自然对齐 4）与
# shader 逐字段偏移天然一致、零 padding、DrawArgs（Phase 4 时 100B——标准布局下需 112B + 2 段
# padding；Phase 7b+ bindless 化后 4B 仅 objectIndex，策略不变）。前提：scalarBlockLayout feature 已启用（features_config.json，
# VkPhysicalDeviceVulkan12Features，validateFeatures 校验）——feature 未启用时
# scalar 布局的 push constant 行为未定义。
# 唯一声明点 = SLANG_FLAGS：反射与 SPV 任何一处 flag 不一致 = 运行时布局错配的隐蔽
# bug（如仅 SPV 侧丢 flag → 反射按 scalar 而 SPV 标准布局错配（当年 100B vs 112B），零报错）。
# 新增 slangc 调用点必须引用本变量，禁止内联重复（generator 的 standardLayoutInfo
# 同步为 scalar 对齐，tools/generator.cppm，并用反射 binding.offset 交叉校验）。
include_guard()

if(NOT SLANGC_EXECUTABLE OR NOT EXISTS "${SLANGC_EXECUTABLE}")
    message(FATAL_ERROR "SLANGC_EXECUTABLE 不可用：根 CMakeLists.txt 须在 add_subdirectory(rgl) 前 include(cmake/SlangFetch.cmake) 并调用 rgl_fetch_slang()")
endif()
message(STATUS "找到 Slang 编译器: ${SLANGC_EXECUTABLE}")

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

# 调用：rgl_shader(<name> <OUT_VAR> [<DEP>...])
# name/DEP 均为 shaders/ 下短名（可含 .frag 等点段，不带 .slang 扩展）。
# DEP = 本 shader import 的契约文件（改契约须触发重编译，见 compile_slang 内注释的踩坑记录）。
# 同时把产物追加到全局属性 RGL_SPV_OUTPUTS，供 add_executable 汇总。
function(rgl_shader src out_var)
    set(deps "")
    foreach(d IN LISTS ARGN)
        list(APPEND deps "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${d}.slang")
    endforeach()
    compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/${src}.slang" _spv ${deps})
    set(${out_var} "${_spv}" PARENT_SCOPE)
    set_property(GLOBAL APPEND PROPERTY RGL_SPV_OUTPUTS "${_spv}")
endfunction()
