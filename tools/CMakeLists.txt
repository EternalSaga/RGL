find_package(nlohmann_json CONFIG REQUIRED)
find_package(Sol2 CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(glm CONFIG REQUIRED)
find_package(unofficial-lua CONFIG REQUIRED)
find_package(inja CONFIG REQUIRED)

# Slang C API（slang 子命令）：链接预构建包 libslang.so + include/slang.h。
# SLANG_PREBUILT_DIR 由根 CMake 的 rgl_fetch_slang() 设置（Linux FetchContent 解压根，
# 恒含 include/slang.h + lib/libslang.so = Linux 主路径）。Windows（Vulkan SDK slangc）
# 与 SLANGC_DIR 覆盖两条路径把该值置空；Windows 官方预构建包的库文件形态**未在 Windows
# 机器核实**（债-25），此处不猜 lib 命名/路径。
# 债-25（2026-10-14）：此前找不到库直接 configure FATAL——与同段注释"子命令不可用、
# 主路径不受影响"互相打脸。现改为：找到库 → 照旧链接（Linux 行为不变）；找不到 →
# WARNING（可归因）+ 把 slang 子命令从编译面剔除（slang_reflect.cppm 不登记，
# RGL_TOOLS_WITH_SLANG 未定义时 reflection_tool.cpp 对该子命令报可归因错误）。
if(SLANG_PREBUILT_DIR AND EXISTS "${SLANG_PREBUILT_DIR}/include/slang.h"
        AND EXISTS "${SLANG_PREBUILT_DIR}/lib/libslang.so")
    set(RGL_TOOLS_SLANG_DIR "${SLANG_PREBUILT_DIR}")
    message(STATUS "reflection_tool: Slang C API 链接 ${SLANG_PREBUILT_DIR}")
else()
    set(RGL_TOOLS_SLANG_DIR "")
    message(WARNING "Slang C API 库缺失（SLANG_PREBUILT_DIR='${SLANG_PREBUILT_DIR}'，需 "
        "include/slang.h + lib/libslang.so）：reflection_tool 的 'slang' 子命令与反射管线"
        "（just reflect）在此配置下不可用。Linux 先 just config 触发 FetchContent 填充该"
        "路径；Windows（Vulkan SDK slangc）形态未验证（债-25）。")
endif()

add_executable(reflection_tool reflection_tool.cpp)

# slang_reflect.cppm 需 slang.h——仅在找到库时登记（generator.cppm / reflection_tool.cpp
# 的 import 与 slang 子命令同在 RGL_TOOLS_WITH_SLANG 条件编译面内）
set(RGL_TOOLS_MODULE_FILES schema_loader.cppm validator.cppm generator.cppm)
if(RGL_TOOLS_SLANG_DIR)
    list(APPEND RGL_TOOLS_MODULE_FILES slang_reflect.cppm)
endif()

target_sources(reflection_tool PRIVATE
    FILE_SET CXX_MODULES
    FILES
        ${RGL_TOOLS_MODULE_FILES}
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
# + 条件编译宏（slang 子命令的编译面开关，见文件头债-25 注释）
if(RGL_TOOLS_SLANG_DIR)
    target_include_directories(reflection_tool PRIVATE "${RGL_TOOLS_SLANG_DIR}/include")
    target_link_libraries(reflection_tool PRIVATE "${RGL_TOOLS_SLANG_DIR}/lib/libslang.so")
    set_target_properties(reflection_tool PROPERTIES BUILD_RPATH "${RGL_TOOLS_SLANG_DIR}/lib")
    target_compile_definitions(reflection_tool PRIVATE RGL_TOOLS_WITH_SLANG=1)
endif()

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
# meshlet_pack：离线簇化打包器（簇化轴 ★0-a；EXCLUDE_FROM_ALL 继承本目录，
# just build-tools 显式构建；依赖红线 = 不链引擎，见其 CMakeLists 头注释）
add_subdirectory(meshlet_pack)
