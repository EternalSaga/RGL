# ─── third_party/fsr —— AMD FidelityFX SDK 1.1.4 的 FSR3 Upscaler Vulkan 后端 ─────────
#
# 来源与裁决见 docs/phase14_plan.md §1；vendor/ 是 c6efa6b（= tag v1.1.4）的**include
# 闭包裁剪版**，只留 FSR3-upscaler + VK 后端真正拉到的 95 个文件（原 sdk/ 是 13 MiB
# 全量）。改动全部以 `RGL-PATCH(Pn)` 注释标出，逐条清单见 PATCHES.md。
#
# 为什么不是上游 CMake：sdk/tools/ffx_shader_compiler（排列编译器 ffx_sc）是
# MSVC-only + Windows 预编译 dxc/glslang 库。本目录用自己的 stdlib-only 生成器
# （tools/gen_ffx_shader_blobs.py）产出同符号契约的 blobs/*.h，见该文件头注释。
#
# 本 target **故意不链进 rgl**：批 14.1 的验收是「独立 target 能编出静态库 + 交付路径
# 逐位不变」。接线是批 14.5。

cmake_minimum_required(VERSION 3.29)

find_package(Vulkan REQUIRED)

set(FFX_VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
set(FFX_BLOBS  ${CMAKE_CURRENT_SOURCE_DIR}/blobs)

# 排列产物：缺了就明确点名生成器，别让链接期才报缺符号
if(NOT EXISTS ${FFX_BLOBS}/ffx_fsr3upscaler_blob_pool.h)
    message(FATAL_ERROR
        "third_party/fsr/blobs/ 缺失。先跑：python3 third_party/fsr/tools/gen_ffx_shader_blobs.py")
endif()

add_library(ffx_fsr3upscaler_vk STATIC
    ${FFX_VENDOR}/src/backends/vk/ffx_vk.cpp
    ${FFX_VENDOR}/src/backends/shared/ffx_shader_blobs.cpp
    ${FFX_VENDOR}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
    ${FFX_VENDOR}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
    ${FFX_VENDOR}/src/shared/ffx_assert.cpp
    ${FFX_VENDOR}/src/shared/ffx_message.cpp
    ${FFX_VENDOR}/src/shared/ffx_breadcrumbs_list.cpp
    ${FFX_VENDOR}/src/shared/ffx_object_management.cpp
    # RGL-PATCH(P7)：FrameInterpolationSwapchain 未 vendor（§4 明确不做），补显式失败桩
    ${CMAKE_CURRENT_SOURCE_DIR}/port/ffx_swapchain_framegen_stub.cpp
    # P9 的编译期护栏（static_assert-only TU）：P9 丢了要变成编译失败，不能等 -O2 崩
    ${CMAKE_CURRENT_SOURCE_DIR}/port/ffx_abi_static_checks.cpp)

target_compile_definitions(ffx_fsr3upscaler_vk PRIVATE FFX_FSR3UPSCALER)

# SYSTEM：vendor 头进 include 面但不产警告（与根 CMakeLists「第三方以 SYSTEM 头进入」
# 的口径一致）。blobs/ 也走 SYSTEM——9 MB 的 hex 数组表，警告面全是它的。
target_include_directories(ffx_fsr3upscaler_vk SYSTEM PUBLIC
    ${FFX_VENDOR}/include
    ${FFX_VENDOR}/src/shared
    ${FFX_VENDOR}/src/backends/shared
    # src/components（不是 .../fsr3upscaler）：blob accessor 用
    # `#include "fsr3upscaler/ffx_fsr3upscaler_private.h"` 的相对形态。
    ${FFX_VENDOR}/src/components
    ${FFX_VENDOR}/src/components/fsr3upscaler
    ${FFX_BLOBS})

target_link_libraries(ffx_fsr3upscaler_vk PUBLIC Vulkan::Vulkan)
if(NOT WIN32)
    # vendor 用了 log2/exp2/sinf/... ；Linux 上 libm 要显式链
    target_link_libraries(ffx_fsr3upscaler_vk PUBLIC m)
endif()

# 根 CMakeLists 全局加了 -Wall -Wextra -Werror。vendor 代码不该由我们背警告账，
# 但**也不能整层关掉**：-Werror 必须先撤（否则第三方警告直接变构建失败），
# 再逐条静音 vendor 的既有警告类型，保留其余警告可见。
target_compile_options(ffx_fsr3upscaler_vk PRIVATE
    $<$<CXX_COMPILER_ID:Clang,GNU,AppleClang>:-Wno-error;-g>
    $<$<CXX_COMPILER_ID:MSVC>:/WX->)

if(NOT WIN32)
    # RGL-PATCH(P3) 的载体：MSVC 安全 CRT（*_s）替身，只 force-include 进本 target。
    target_compile_options(ffx_fsr3upscaler_vk PRIVATE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/compat/ffx_posix_compat.h)
endif()

# ── 产物护栏（与 generated/*.cppm + reflection_check 同构）──────────────────────
# 判据 = 「生成物的输入变了、产物没重生成」→ 出声。跑生成器要 27 s，构建开头只做
# 哈希比对；真正的重生成是 `just fsr-blobs`（人触发，产物随改动一起提交）。
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_target(fsr_blobs_check
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_ffx_shader_blobs.py --check
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "FSR shader 排列产物 SHA1 护栏"
    VERBATIM)

# ── 最小设备 + 单 pass 冒烟（批 14.1 第一个里程碑，见 smoke/fsr_smoke.cpp 头注释）────
# EXCLUDE_FROM_ALL：要真设备，不该在 `just build` 里跑；`just fsr-smoke` 显式构建并执行。
add_executable(fsr_smoke EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/smoke/fsr_smoke.cpp)
target_link_libraries(fsr_smoke PRIVATE ffx_fsr3upscaler_vk Vulkan::Vulkan)
target_compile_options(fsr_smoke PRIVATE $<$<CXX_COMPILER_ID:Clang,GNU,AppleClang>:-Wno-error;-g>)

# ── 合成靶实验台（批 14.5 诊断：时域档发糊根因量化，见 smoke/fsr_lab.cpp 头注释）────
# 同为 EXCLUDE_FROM_ALL（要真设备）；`just fsr-lab` 显式构建并执行。
add_executable(fsr_lab EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/smoke/fsr_lab.cpp)
target_link_libraries(fsr_lab PRIVATE ffx_fsr3upscaler_vk Vulkan::Vulkan)
target_compile_options(fsr_lab PRIVATE $<$<CXX_COMPILER_ID:Clang,GNU,AppleClang>:-Wno-error;-g>)
