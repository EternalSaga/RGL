find_package(nlohmann_json CONFIG REQUIRED)
find_package(Sol2 CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(glm CONFIG REQUIRED)
find_package(unofficial-lua CONFIG REQUIRED)
find_package(inja CONFIG REQUIRED)

# Slang C API（slang 子命令）：链接预构建包 libslang.so + include/slang.h。
# SLANG_PREBUILT_DIR 由根 CMake 的 rgl_fetch_slang() 设置（Linux FetchContent 解压根）。
# 为空（Windows Vulkan SDK / SLANGC_DIR 覆盖）时 slang 子命令不可用——Linux 主路径不受影响。
# slang_reflect.cppm 无条件编译（需 slang.h），故预构建包必须含 include/slang.h +
# lib/libslang.so。Linux 主路径 = FetchContent 预构建（恒满足）。Windows（Vulkan SDK
# slangc）/ SLANGC_DIR 覆盖若无库，configure 即 FATAL——slang 子命令是本升级核心。
if(NOT (SLANG_PREBUILT_DIR AND EXISTS "${SLANG_PREBUILT_DIR}/include/slang.h"
        AND EXISTS "${SLANG_PREBUILT_DIR}/lib/libslang.so"))
    message(FATAL_ERROR "Slang C API 库缺失：需 ${SLANG_PREBUILT_DIR}/include/slang.h + lib/libslang.so"
        "（Linux 先 just config 触发 FetchContent；reflection_tool 的 'slang' 子命令依赖之）")
endif()
message(STATUS "reflection_tool: Slang C API 链接 ${SLANG_PREBUILT_DIR}")

add_executable(reflection_tool reflection_tool.cpp)

target_sources(reflection_tool PRIVATE
    FILE_SET CXX_MODULES
    FILES
        schema_loader.cppm
        validator.cppm
        generator.cppm
        slang_reflect.cppm
)

target_compile_features(reflection_tool PRIVATE cxx_std_23)

target_link_libraries(reflection_tool
    PRIVATE
        nlohmann_json::nlohmann_json
        sol2::sol2
        Boost::program_options
        glm::glm
        lua
        pantor::inja
        rllogger
        rlstd
)

# Slang C API：include + 链接 + 运行期 rpath（预构建包 lib/ 含 libslang-llvm.so 等依赖）
target_include_directories(reflection_tool PRIVATE "${SLANG_PREBUILT_DIR}/include")
target_link_libraries(reflection_tool PRIVATE "${SLANG_PREBUILT_DIR}/lib/libslang.so")
set_target_properties(reflection_tool PROPERTIES BUILD_RPATH "${SLANG_PREBUILT_DIR}/lib")

target_compile_definitions(reflection_tool PRIVATE
    LOG_CONFIG_PATH="${CMAKE_SOURCE_DIR}/configs/log_level.json"
)

add_custom_command(
    TARGET reflection_tool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/templates"
        "${CMAKE_CURRENT_BINARY_DIR}/templates"
    COMMENT "Copying inja templates to build directory..."
)

# imgqa：区域归因图像判据工具箱（EXCLUDE_FROM_ALL 继承本目录；just imgqa 显式构建）
add_subdirectory(imgqa)
