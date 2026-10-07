# tests/ — RGL 单元测试层（批 12.1 基建 / 12.2c 边界随四库分层 / 12.2d 链接面 = 全部四层）
#
# ## 定位与边界
#
# 本目录放**两层用例，按 doctest 测试套件分流（不是 tag、不是链接面）**：
# - 无设备子集（默认 `just test`）：纯函数数学、字节布局契约、配置解析、资产解析、
#   渲染图编译校验——无 GPU、无窗口、毫秒级。历史上代价最高的一批 bug（阴影级联
#   5a-3→5a-9 整条链、反射 stride 静默错位、矩阵/平面约定混用、glTF 越界读）——
#   **全部可在毫秒级离线复现**。
# - 设备原语层（`TEST_SUITE("gpu")`，`just test-all` / `just test-gpu` 显式跑）：
#   后端回读原语 golden——要真设备 + VVL，无设备机器上**必须 FAIL 不 SKIP**
#   （静默跳过 = 假绿）。
#
# 本构建 doctest 2.5.3 的 `TEST_CASE` 宏只接受一个参数（tag 形态不存在），套件过滤是
# 一等公民：`just test` = `--test-suite-exclude=gpu`。
#
# 需要 GPU 的图像/场景级回归不在这里：那条线走 qa/（外部 harness 起 rgl 进程 +
# RPC 驱动 + OpenCV 量化），现状机制描述见 AGENTS.md §测试体系。
#
# ## 复用方式：链全部四层，不重编（批 12.2d 起）
#
# 被测模块来自**引擎四库**（rlbase / rlgfx / rlscene / rlframe，rgl/CMakeLists.txt
# 定义），即**与引擎同一次编译产物**——不是"把源码再编一份"。这一点是硬要求：
# 重编会引入两份可能漂移的构建配置（GLM 深度宏、编译选项），而本项目已经吃过
# "同源漂移"的亏（docs/调试历史.md 2026-09-08 跨日 ±1LSB 条目）。链接面 = 全部
# 四层，理由（"import rlvk" 是编译期依赖 ≠ "需要 VkDevice" 是运行期依赖）见下方
# 链接注释。
#
# ## 边界由库边界直接保证（批 12.2c 起不再靠"闭包判据 + 排除清单"）
#
# 引擎分四库（rlbase / rlgfx / rlscene / rlframe），依赖只向下。**rlbase 的 import
# 闭包不含 rlvk / VmaRaii / QueueFamilyManager**——这条性质由库边界保证，是逐条
# import 边在真实依赖图上验证过的（74 模块 / 442 边，零逆向边、库级无环）。
# 测试层的划分判据只有一个：**这段逻辑需不需要一个 VkDevice 才能跑**。需要 →
# `TEST_SUITE("gpu")` 或 qa/；不需要 → 进本目录无设备子集（不必属于 rlbase：
# render_graph / state_kit 都 import rlvk 却一个设备都不碰，见下方链接注释）。
#
# 头文件约定：doctest 是第三方头，走 GMF include（见各测试文件顶部）。

find_package(doctest CONFIG REQUIRED)
# 设备级用例（[gpu] tag）要真设备：Vulkan / VMA / SDL3 / imgui 的 find_package
# 在这里补一遍（rgl/CMakeLists.txt 里的那些在它自己作用域，不传递到兄弟目录）。
find_package(Vulkan REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

set(RGL_TEST_SOURCES
    test_main.cppm              # DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN + main()
    test_shadow_geometry.cppm   # 级联拟合球/切分/光空间盒/剔除（5a-3→5a-9 回归链）
    test_reflection_layout.cppm # 反射生成物字节布局契约
    test_scene_config.cppm      # 四份配置解析契约
    test_render_scale.cppm      # 批 14.3 渲染倍率纯算术：1.0 档逐像素红线 / 取整 / 钳制 / 非退化
    test_camera.cppm            # 相机数学
    test_cube_geometry.cppm     # 内建 cube 顶点/索引契约
    test_state_kit.cppm         # 十段 RenderState 表 + stateFor 分派（纯逻辑）
    test_render_graph.cppm       # 图编译：校验 / barrier 推导 / 装饰 / RT slot
    test_taa_jitter.cppm         # TAA 纯函数层：Halton / jitter→投影闭式解 / 跳切与历史判据（批 12.6）
    test_scene_spin.cppm         # 帧驱动 spin 角闭式（门 spin_frame_angle 判据本体，跨进程 A/B 对齐的地基）
    test_camera_frame_yaw.cppm   # 帧驱动相机 yaw 闭式（门 frame_yaw 判据本体，temporal-motion 相机运动激励的地基）
    test_aa_target.cppm          # 细线抗锯齿靶几何：数据自洽 / "确实细"载体判据（批 12.6 T4）
    test_gate_lint.cppm          # 门组合自检：38 门申报表 + variant 交叉校验（gate_lint 批）
    test_frisvad_tangent.cppm    # Frisvad 切线兜底：轴向/±Z 奇点/随机有限性（黑面 bug 回归）
    test_render_list_shadow.cppm # fillShadowData 尾区契约：splits 哨兵/VP 拷贝/texel/params 两态（12.3d）
    test_render_list_light.cppm  # fillLightData 落槽契约：按 index 落槽/计数封顶/重复 index（12.3d）
    test_features_gates.cppm     # 能力门真值表：depthClip 双条件/presentTiming 三合一/扩展清单（12.3d）
    test_asset_table.cppm        # AssetTable::expectedGeometrySlot 槽号算术契约（12.3d）
    test_slot_ledger.cppm        # sampler heap 申报制领槽：幂等 / 不同 owner 不同槽 / 超容量不越界（2026-10-04 槽相撞事故护栏）
    test_gltf_texcoord.cppm      # glTF TEXCOORD_0 vec2 步长 + 去索引展开（手构 GLB，2026-08-30 越界读回归，12.3d-C）
    test_shadow_forward.cppm     # shadow.slang 选级 forward 提取式数学恒等（invVP 列叉积，5a-8c 回归，12.3d-C）
    test_evssm_math.cppm         # EVSSM 消费端公式数学契约（快路/零矩/far 矩恒等/单调/LBR，11.6d bug 回归）
    test_device_primitives.cppm      # gpu 套件回读原语：clear 逐像素 / 整数回环 / UNORM 量化 / 拷贝保真
    test_device_primitives_impl.cppm # 设备夹具实现（partition impl unit）
    test_conservative_mesh.cppm      # gpu 套件 11.3-1b 成败门：保守光栅 × mesh shader × RADV（含 EDS3 动态态）
    test_izb_create.cppm             # gpu 套件 11.3-1c：IZB grid-of-lists 原子创建 + early-Z priming
    test_izb_mask.cppm               # gpu 套件 11.3-1d：光空间保守光栅 × IZB 走链 mask 判定
    test_izb_engine_contract.cppm    # gpu 套件 11.3 补账：引擎真 SPV 的 ndc 口径/字节偏移/guard 契约
)

# ─── 11.3-1b 探针 SPIR-V（izb_spike mesh/frag，测试专用不入引擎 SPV 面）──────
# 复用 cmake/RglSlang.cmake（SLANG_FLAGS 唯一声明点，勿在此内联 flag）；本目录
# include 后 GEN_DIR = build/tests/generated_shaders，与引擎产物分目录。
# 产物经 target_sources 挂进 rgl_tests 依赖（与 rgl 可执行挂 SPV 同款做法）。
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/RglSlang.cmake)
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/izb_spike.mesh.slang" IZB_SPIKE_MESH_SPV)
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/izb_spike.frag.slang" IZB_SPIKE_FRAG_SPV)
# 11.3-1c：IZB 创建 + priming compute（grid-of-lists spike）
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/izb_create.comp.slang" IZB_CREATE_SPV)
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/izb_prime.comp.slang" IZB_PRIME_SPV)
# 11.3-1d：光空间遮挡三角（mesh）× IZB 走链 mask（frag）
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/izb_mask.mesh.slang" IZB_MASK_MESH_SPV)
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/izb_mask.frag.slang" IZB_MASK_FRAG_SPV)

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
# 探针 SPV 挂进依赖（add_executable 之后；与 rgl 可执行挂 SPV 同款做法）
target_sources(rgl_tests PRIVATE ${IZB_SPIKE_MESH_SPV} ${IZB_SPIKE_FRAG_SPV}
    ${IZB_CREATE_SPV} ${IZB_PRIME_SPV} ${IZB_MASK_MESH_SPV} ${IZB_MASK_FRAG_SPV})
# 链接面：rlbase 之外**也**链 rlgfx/rlscene/rlframe（批 12.2d）。
#
# 为什么不只链 rlbase（那样测试二进制里 Vulkan 符号恒 0，看起来更干净）：
# **「import rlvk」是编译期依赖，「需要一个 VkDevice」才是运行期依赖**，两者不是一回事。
# render_graph / state_kit / frame_draw_list 都 import rlvk（要 `vk::Format` 等类型），
# 但它们的逻辑是纯计算 —— compileRenderGraph 推导 barrier、StateKit 搭九段状态表，
# 一个设备都不碰。把它们挡在门外等于"因为类型依赖里有 Vulkan，所以不许测"，
# 那正是 12.1→12.2c 一直在纠正的那个混淆。
#
# 代价（明确记录，不假装没有）：测试二进制现在**链接**了 Vulkan/SDL3/VMA/imgui，
# `nm` 不再是 0。保住的是**运行期**性质 —— 默认的 `just test` 只跑无设备子集
# （`--test-suite-exclude=gpu`），那批一条设备路径都不碰，仍是毫秒级；需要设备
# 的那批写在 `TEST_SUITE("gpu")` 里，由 `just test-all` 显式跑。
# 判别 = doctest 测试套件（本构建 TEST_CASE 宏只接受一个参数，tag 形态不存在），
# 不是 tag，也不是链接面。
target_link_libraries(rgl_tests
    PUBLIC rgl_test_support rlbase rlgfx rlscene rlframe
    PRIVATE Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator SDL3::SDL3 imgui::imgui)
# 图像编解码/tinygltf 随 rlbase 传递（静态库 PRIVATE 链接经 $<LINK_ONLY:…> 保序），
# 这里不重复列出。

# 设备级用例的开关：默认 `just test` 跑无 tag 子集（无 GPU、无窗口、毫秒级）；
# `just test-all` 加 --gpu 跑全部，含 [gpu] 用例。
# 刻意**不做**"无设备就 skip"——静默跳过是假绿，正是本项目反复吃亏的那类事
# （"上次跑过"≠"这次验的是同一组输入"）。[gpu] 用例在无设备机器上必须**响亮失败**。
target_compile_definitions(rgl_tests PRIVATE RGL_TESTS_HAVE_GPU_TESTS=1)
# 1b 探针 SPIR-V 绝对路径（构建产物目录；测试读文件建 shader module，spv 缺失 = 响亮失败）
target_compile_definitions(rgl_tests PRIVATE RGL_TEST_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
# 11.3 补账：引擎**真产物**目录（izb_engine_contract 测的就是这份 SPV 本体——测
# spike 副本会重演「两份实现漂移」的 1c 盲区，2026-10-02 ndc 镜像 bug 实证）。
target_compile_definitions(rgl_tests PRIVATE RGL_ENGINE_SHADER_DIR="${CMAKE_BINARY_DIR}/rgl/generated_shaders")
# 引擎 SPV 由 rgl 目标的 slang 规则产出：rgl_tests 直接依赖它，否则 `just test-gpu`
# 单测目标时拿到陈旧 SPV（"改完 shader 没重建就报全通过"的 modules 环境坑同款）。
add_dependencies(rgl_tests rgl)

# 测试代码不纳入 source_checks（tools/checks 扫的是 rgl/）：doctest 宏会触发大量
# 误报（GMF 白名单、行数阈值、函数长度对 TEST_CASE 全是噪声）。测试代码的质量由
# review 保证，不由静态规则保证——这是有意识的取舍，写在这里以免日后有人"顺手加回"。

include(CTest)
add_test(NAME rgl_unit COMMAND rgl_tests)
set_tests_properties(rgl_unit PROPERTIES TIMEOUT 300)
