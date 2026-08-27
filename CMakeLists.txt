cmake_minimum_required (VERSION 3.29)
project("RGL" LANGUAGES CXX)
option(FMT_UNICODE "Enable Unicode support." ON)
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
include(${PROJECT_SOURCE_DIR}/cmake/FindClangFormat.cmake)
# Slang 编译器（slangc）：FetchContent 官方预构建，替代 vcpkg shader-slang port（见 cmake/SlangFetch.cmake）
include(${PROJECT_SOURCE_DIR}/cmake/SlangFetch.cmake)
rgl_fetch_slang()

add_subdirectory(rlstd)
add_subdirectory("./logger")
add_subdirectory(tools)
add_subdirectory(vulkan_practice)
