cmake_minimum_required (VERSION 3.29)
project("RGL" LANGUAGES CXX)
option(FMT_UNICODE "Enable Unicode support." ON)
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
include(${PROJECT_SOURCE_DIR}/cmake/FindClangFormat.cmake)

add_subdirectory("./logger")
add_subdirectory(vulkan_practice)
