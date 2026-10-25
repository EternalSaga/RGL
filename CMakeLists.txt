cmake_minimum_required (VERSION 3.29)
project("RGL" LANGUAGES CXX)
option(FMT_UNICODE "Enable Unicode support." ON)
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
# 项目代码警告面（仅本仓库 TU——第三方以预编译库 + SYSTEM 头进入，不产警告）。
# 2026-09-24：第一方存量警告已清零，-Wall -Wextra + -Werror 默认打开。
option(RGL_WERROR "Treat project warnings as errors (-Werror / MSVC /WX)" ON)
add_compile_options(
    "$<$<CXX_COMPILER_ID:Clang,GNU,AppleClang>:-Wall;-Wextra>"
    "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
)
# PROBE-FLAG（隔离实验，稍后删）
add_compile_options("$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wcovered-switch-default>")
# 有意保留的第一方警告抑制：
# - main-attached-to-named-module：main() 放 module main:impl 是 AGENTS 约定（标准合法）
# - missing-designated-field-initializers：部分 designated init（缺字段走默认值）是有意风格
add_compile_options(
    "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-main-attached-to-named-module;-Wno-missing-designated-field-initializers>"
)
if(RGL_WERROR)
    add_compile_options(
        "$<$<CXX_COMPILER_ID:Clang,GNU,AppleClang>:-Werror>"
        "$<$<CXX_COMPILER_ID:MSVC>:/WX>"
    )
endif()
# ─── ASan/UBSan 诊断构建（债-140 取证批首次引入；默认 OFF）─────────────────────
# 为什么走 cache 选项、而不是每次手传三串 flags：sanitizer 必须**同时**进编译与链接，
# 且对 C++20 modules 的 BMI 生成一致。手传最容易漏的两处 = 链接器（报缺 asan 符号，还算
# 好发现）与 rlstd/logger 两个独立库（**编得过、链得过，但那两个 TU 根本没插桩**）——
# 后者产出的是一份"看起来干净"的报告，而它什么都没测。半吊子 ASan 是假绿的标准形态，
# 所以这里把开关做成全有或全无。
# 用法：`just config` 之后 `just asan-on` / `just asan-off`（薄封装 cmake -DRGL_ASAN=…）。
# 🔴 两条使用纪律（写在这里，因为它们的失效形态都是静默的）：
#  ① **不要拿本构建做任何性能读数**——ASan 拖慢 2–5×，且它改变了时序（债-140 这种竞态
#     问题的"没复现"因此不能读成"不存在"）；
#  ② 运行期要 `ASAN_OPTIONS=detect_leaks=0`：vcpkg 依赖是非插桩预编译库，泄漏检测器
#     看不见它们的分配器，会假红。
option(RGL_ASAN "Instrument the WHOLE build with AddressSanitizer + UBSan (diagnostic only; never for timing)" OFF)
if(RGL_ASAN)
    if(MSVC)
        # 显式拒绝而不是静默不生效：/fsanitize=address 的参数面与运行期配置完全不同，
        # 没实测过的路线上跑出来的"干净"是最贵的一种假绿。
        message(FATAL_ERROR "RGL_ASAN 只接 clang/GCC 路线（MSVC 的 /fsanitize=address 未验证过，勿开）")
    endif()
    # -fno-omit-frame-pointer + -g：ASan 报告的栈要能读到函数名与行号，否则等于没栈。
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
    message(STATUS "RGL_ASAN=ON：全构建插桩（address,undefined）——本构建的性能读数一律作废")
endif()

include(${PROJECT_SOURCE_DIR}/cmake/FindClangFormat.cmake)
# Slang 编译器（slangc）：FetchContent 官方预构建，替代 vcpkg shader-slang port（见 cmake/SlangFetch.cmake）
include(${PROJECT_SOURCE_DIR}/cmake/SlangFetch.cmake)
rgl_fetch_slang()

# 🔴 **GLM 宏清单的唯一来源**（住顶层：`rgl/` 与 `tests/` 是兄弟目录，子目录里 set 的变量
# 互相看不见，清单就只能各抄一份——而 glm 是 header-only 模板，宏不一致**不会链接失败**，
# 只会让某些 TU 静默不受管，正是最坏的那种失效）。引用方：`rgl/CMakeLists.txt`（rlbase
# PUBLIC 逐层传递）、`tests/CMakeLists.txt`（rgl_test_support）。
#   · DEPTH_ZERO_TO_ONE + RADIANS：深度/角度约定必须全项目一致。`frustumBoundingSphere`
#     的近远反推、`projWithDepthRange` 全依赖它们；不一致 = 测试在测另一个函数。
#   · EXPLICIT_CTOR（债-147）：禁 glm 隐式跨类型构造 ⟹ `dvec3 → vec3` 静默收窄、
#     `vec4 → vec3` 静默降维、`make_mat4(double*)` 推出 `mat4x4<double>` 再静默收窄，
#     全部变编译错误。**只拦隐式、不拦显式**（`glm::vec3(dv)` 这类显式 direct-init 不动）。
#     引入理由不是审美：债-147 那类静默掉精度**对相对坐标批 3/4 的主判据「门关逐位不变」
#     结构上失明**（门关态原点恒为 0，收窄不改变任何值 ⟹ 照样逐位绿），编译器是当时唯一
#     还活着的防线。实测成本：生产 8 处 / 测试 0 处（2026-10-25 全量编译读数）。
set(RGL_GLM_DEFINES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    GLM_FORCE_EXPLICIT_CTOR)

add_subdirectory(rlstd)
add_subdirectory("./logger")
# EXCLUDE_FROM_ALL（2026-09-02 编译提速）：反射产物（*_reflection.json +
# generated/*.cppm）已纳入版本控制，日常/全新构建无需重跑反射管线；只有
# slang/schema/模板真变更时，ninja 才按需构建 reflection_tool。默认 ALL
# 构建不再付工具链 ~14s 串行前缀。
add_subdirectory(tools EXCLUDE_FROM_ALL)
# Phase 14 批 14.1：vendor 的 FSR3-upscaler VK 后端（独立静态库，**尚未链进 rgl**，
# 接线是批 14.5）。留在 ALL 里是为了让「编不过」当场暴露，而不是等接线那天。
add_subdirectory(third_party/fsr)
add_subdirectory(rgl)
# 单元测试层（Phase 12 批 12.1）：纯函数/契约测试，无 GPU 无窗口。EXCLUDE_FROM_ALL
# ——不进默认构建序列（测试不是引擎产物，AGENTS.md「rgl 不认识测试」），`just test` 显式构建。
add_subdirectory(tests EXCLUDE_FROM_ALL)
