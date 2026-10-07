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
include(${PROJECT_SOURCE_DIR}/cmake/FindClangFormat.cmake)
# Slang 编译器（slangc）：FetchContent 官方预构建，替代 vcpkg shader-slang port（见 cmake/SlangFetch.cmake）
include(${PROJECT_SOURCE_DIR}/cmake/SlangFetch.cmake)
rgl_fetch_slang()

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
