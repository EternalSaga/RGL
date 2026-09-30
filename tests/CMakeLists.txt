# tests/ — RGL 单元测试层（批 12.1 基建 / 12.2c 边界随四库分层更新）
#
# ## 定位与边界
#
# 本目录**只放"不需要 GPU、不需要窗口、不需要引擎启动"的测试**：纯函数数学、字节布局
# 契约、配置解析、资产解析、渲染图编译校验。历史上代价最高的一批 bug（阴影级联
# 5a-3→5a-9 整条链、反射 stride 静默错位、矩阵/平面约定混用、glTF 越界读）——
# **全部可在毫秒级离线复现**。
#
# 需要 GPU 的图像/场景级回归不在这里：那条线走 qa/（外部 harness 起 rgl 进程 +
# RPC 驱动 + OpenCV 量化），理由与指标设计见 docs/技术概念.md §测试体系。
#
# ## 复用方式：链 rlbase，不重编
#
# 被测模块来自 **rlbase**（rgl/CMakeLists.txt 定义的引擎四库最底层），即**与引擎同
# 一次编译产物**——不是"把源码再编一份"。这一点是硬要求：重编会引入两份可能漂移的
# 构建配置（GLM 深度宏、编译选项），而本项目已经吃过"同源漂移"的亏
# （docs/调试历史.md 2026-09-08 跨日 ±1LSB 条目）。
#
# ## 边界由库边界直接保证（批 12.2c 起不再靠"闭包判据 + 排除清单"）
#
# 引擎分四库（rlbase / rlgfx / rlscene / rlframe），依赖只向下。**rlbase 的 import
# 闭包不含 rlvk / VmaRaii / QueueFamilyManager**——这条性质由库边界保证，是逐条
# import 边在真实依赖图上验证过的（74 模块 / 442 边，零逆向边、库级无环）。
# 链接 rlbase 不会把 Vulkan 栈拖进来：静态库按需链接，只 import 用到的模块时其余
# 模块的未定义符号不进二进制（已实测 nm 干净）。
#
# **要测 rlbase 之外的模块**（render_graph / state_kit / frame_draw_list /
# shader_registry / vk_* …）时不要往本目录硬塞——那会把整个 Vulkan 栈拖进测试
# 二进制。那批走设备级测试（rlgfx/rlscene/rlframe 需要设备）与 qa/ 进程级集成测试。
# 划分判据只有一个：**这段逻辑需不需要一个 VkDevice 才能跑**。需要 → 设备级；
# 不需要 → 属于 rlbase，就该在本目录能测。
#
# 头文件约定：doctest 是第三方头，走 GMF include（见各测试文件顶部）。

find_package(doctest CONFIG REQUIRED)

set(RGL_TEST_SOURCES
    test_main.cppm              # DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN + main()
    test_shadow_geometry.cppm   # 级联拟合球/切分/光空间盒/剔除（5a-3→5a-9 回归链）
    test_reflection_layout.cppm # 反射生成物字节布局契约
    test_scene_config.cppm      # 四份配置解析契约
    test_camera.cppm            # 相机数学
    test_cube_geometry.cppm     # 内建 cube 顶点/索引契约
)

# ─── 测试支撑模块 ─────────────────────────────────────────────────────────
# doctest 头（GMF）+ 共享断言宏 + **确定性采样器**。确定性是硬要求：历史教训
# 元模式「验证工具自己错」——属性测试用真随机时，失败无法重跑，"上次跑过"也不等于
# "这次验的是同一组输入"。固定种子 LCG 让失败可重放。
# 单独成模块而非 .hpp：测试代码与项目同走 C++20 modules 工具链（AGENTS.md 模块优先）。
add_library(rgl_test_support STATIC)
target_sources(rgl_test_support PUBLIC
    FILE_SET CXX_MODULES
    FILES test_support.cppm
)
target_compile_features(rgl_test_support PUBLIC cxx_std_23)
# 不在 target 上 -D DOCTEST_CONFIG_IMPLEMENT*：doctest 默认就是"只有声明"，而各测试 TU
# 需要**注册** TEST_CASE（须 `#define DOCTEST_CONFIG_IMPLEMENT`）、test_main 需要出
# main（须 `_WITH_MAIN`）——都由各自文件在 GMF 里按需定义。若在 target 上 -D，会与文件
# 内的 #define 撞出 macro redefined 警告（-Werror 直接挂）。
target_include_directories(rgl_test_support SYSTEM PUBLIC ${DOCTEST_INCLUDE_DIRS})
target_link_libraries(rgl_test_support PUBLIC doctest::doctest rlstd)

add_executable(rgl_tests)
target_sources(rgl_tests PUBLIC
    FILE_SET CXX_MODULES
    FILES ${RGL_TEST_SOURCES}
)
target_compile_features(rgl_tests PRIVATE cxx_std_23)
# rlcore 的 PUBLIC 传递 rlstd/rllogger/glm/EnTT/sol2/lua + GLM 深度约定宏
target_link_libraries(rgl_tests PUBLIC rgl_test_support rlbase)

# 测试代码不纳入 source_checks（tools/checks 扫的是 rgl/）：doctest 宏会触发大量
# 误报（GMF 白名单、行数阈值、函数长度对 TEST_CASE 全是噪声）。测试代码的质量由
# review 保证，不由静态规则保证——这是有意识的取舍，写在这里以免日后有人"顺手加回"。

include(CTest)
add_test(NAME rgl_unit COMMAND rgl_tests)
set_tests_properties(rgl_unit PROPERTIES TIMEOUT 300)
