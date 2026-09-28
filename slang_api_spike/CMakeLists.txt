# ─── Slang C API 反射 spike（独立 CMake，不进主构建）──────────────────────────
# 调研：链接 libslang.so（预构建包自带）经 C API 直接查询非资源 struct 布局，
# 替代 slangc CLI → JSON 中间层。见 docs/archive/slang_api_reflection_research.md。
#
# 构建：cmake -B build -S . && cmake --build build
# 运行：./build/slang_api_spike test_struct.slang SceneLight GpuLightLike
#
# SLANG_DIR 指向主构建 FetchContent 解压的预构建包（含 include/slang.h + lib/libslang.so）。
cmake_minimum_required(VERSION 3.21)
project(slang_api_spike CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SLANG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/_deps/slang-prebuilt-src" CACHE PATH
    "Slang 预构建包根目录（include/ + lib/）")

if(NOT EXISTS "${SLANG_DIR}/include/slang.h")
    message(FATAL_ERROR "SLANG_DIR 无 include/slang.h：${SLANG_DIR}（先 just config 让主构建 FetchContent 解压）")
endif()

add_executable(slang_api_spike main.cpp)
target_include_directories(slang_api_spike PRIVATE "${SLANG_DIR}/include")
target_link_libraries(slang_api_spike PRIVATE "${SLANG_DIR}/lib/libslang.so")
# 运行期找 libslang.so（预构建包 lib/ 含 libslang-llvm.so 等依赖，同目录）
set_target_properties(slang_api_spike PROPERTIES
    BUILD_RPATH "${SLANG_DIR}/lib"
    INSTALL_RPATH "${SLANG_DIR}/lib")
