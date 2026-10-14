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
# "同源漂移"的亏（git:调试历史 2026-09-08 跨日 ±1LSB 条目）。链接面 = 全部
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
find_package(nlohmann_json CONFIG REQUIRED)  # test_burst_protocol 的入参类型（rlbase 里 PRIVATE 链接，不传递到测试）

set(RGL_TEST_SOURCES
    test_main.cppm              # DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN + main()
    test_shadow_geometry.cppm   # 级联拟合球/切分/光空间盒/剔除（5a-3→5a-9 回归链）
    test_reflection_layout.cppm # 反射生成物字节布局契约
    test_scene_config.cppm      # 四份配置解析契约
    test_render_scale.cppm      # 批 14.3 渲染倍率纯算术：1.0 档逐像素红线 / 取整 / 钳制 / 非退化
    test_velocity_field.cppm    # 批 14.4 逐像素速度场：显式相机基参考 / 旋转光流闭式 / 天空与无效判据
    test_camera.cppm            # 相机数学
    test_cube_geometry.cppm     # 内建 cube 顶点/索引契约
    test_state_kit.cppm         # 十段 RenderState 表 + stateFor 分派（纯逻辑）
    test_render_graph.cppm       # 图编译：校验 / barrier 推导 / 装饰 / RT slot
    test_offgraph_contract.cppm  # 批次 B1 图外工作申报面：建表三 guard 各配反向 + 位置契约三种偏离
    test_offgraph_sync.cppm      # 批次 B1b 图外同步：词表/边表逐位钉死（迁移没改语义）+ 次数账双向
                                 # （含 §7 判据 2 的变异本体「调用点挪到 pass 循环之后」的单元等价形态）+ 覆盖对账两半边
    test_taa_jitter.cppm         # TAA 纯函数层：Halton / jitter→投影闭式解 / 跳切与历史判据（批 12.6）
    test_timing_util.cppm        # 时间戳/时间统计叶子：toMs 定义值 / percentile 口径逐点手算 / 时钟单调（三处副本收敛的护栏）
    test_scene_spin.cppm         # 帧驱动 spin 角闭式（门 spin_frame_angle 判据本体，跨进程 A/B 对齐的地基）
    test_camera_frame_yaw.cppm   # 帧驱动相机 yaw 闭式（门 frame_yaw 判据本体，temporal-motion 相机运动激励的地基）
    test_burst_protocol.cppm     # /shotburst 协议判据：形状六反向 + 绝对帧号锚点三态（债-89，跨进程逐帧复现的地基）
    test_aa_target.cppm          # 细线抗锯齿靶几何：数据自洽 / "确实细"载体判据（批 12.6 T4）
    test_gate_lint.cppm          # 门组合自检：38 门申报表 + variant 交叉校验（gate_lint 批）
    test_frisvad_tangent.cppm    # Frisvad 切线兜底：轴向/±Z 奇点/随机有限性（黑面 bug 回归）
    test_render_list_shadow.cppm # fillShadowData 尾区契约：splits 哨兵/VP 拷贝/texel/params 两态（12.3d）
    test_shadow_lod_emit.cppm    # §18 批 B2 阴影段 draw 构造：c0 恒旧路径/×2 唯一真相源/条数不变/索引公式（collect+emit 接缝）
    test_meshlet_overlay.cppm    # 债-105 α 叠加段自有门 meshlet.overlay：门关零 draw/真值表四格/四条既有判据反向/×2 同一入口/配置键探针
    test_render_list_light.cppm  # fillLightData 落槽契约：按 index 落槽/计数封顶/重复 index（12.3d）
    test_features_gates.cppm     # 能力门真值表：depthClip 双条件/presentTiming 三合一/扩展清单（12.3d）
    test_asset_table.cppm        # AssetTable::expectedGeometrySlot 槽号算术契约（12.3d）
    test_slot_ledger.cppm        # sampler heap 申报制领槽：幂等 / 不同 owner 不同槽 / 超容量不越界（2026-10-04 槽相撞事故护栏）
    test_gltf_texcoord.cppm      # glTF TEXCOORD_0 vec2 步长 + 去索引展开（手构 GLB，2026-08-30 越界读回归，12.3d-C）
    test_meshlet_asset.cppm      # ★0-b1 meshlet 产物读取侧：合成缓冲正向 + 21 条反向（错误串两两不同 = 可归因）
    test_meshlet_cull.cppm       # ★0.5-a CPU 参考簇剔除器：六面逐条归因 + 保守判据 + 半径缩放上界 + 三角合计
    test_meshlet_ranges.cppm     # ★1-a1i 逐 prim 簇区间扫描：四条产物不变量各配反向用例（错误串两两不同）+ 尾区写入逐字节
    test_scene_sentinel.cppm     # 迁移批 T0 表尾毒化哨兵护栏：毒值三型有毒 / 位置=容量×stride / 容量语义不变 / 写侧不覆盖
    test_meshlet_lod.cppm        # §18 批 A 逐 prim 选级扫描：五条不变量各配反向用例 + 手算 costTable + base-shift 序锚 + Lcg 契约全量钉
    test_mdi_rows.cppm           # ★M-0（§19.3）MDI 行表构建器：行序=候选序 + 前 12B 对账 Vk 结构 + stride 合法性 + 四条不变量反向用例（错误串两两不同）+ 配置键探针
                                 # + ★M-1a（§19.5）尺寸/偏移算术：🔴 argsOffset 与 rowBase 同源 + 07089/07090 合一闭式边界 + 容量少 1 字节必红
    test_mdi_shadow_emit.cppm    # ★M-1b（§19.2 D4 / §19.5）生效档采集判据：constexpr 桥（schema↔mdi_rows）+ 每级联一条 MDI + 同源 pair 闭式字面 + 混合形态/孔洞流/五档回落 + 行序=候选序
                                 # + ★M-1a（§19.5）尺寸/偏移算术：🔴 argsOffset 与 rowBase 同源 + 07089/07090 合一闭式边界 + 容量少 1 字节必红
    test_shader_mapping.cppm     # ★M-3T-a（§19.16）批 M mapping 过滤契约：🔴 无 task stage 不得判成解析失败（实踩：479 条误退全烘而终图逐位不变）+ task 声明进并集 + 三侧坏码流各触发 nullopt + 消费者侧条数与 running index 序
    test_meshlet_hand.cppm       # ★0.5-c UV 手性 CPU 参考 + 簇→三角寻址：闭式五形态锚点 + 跨簇偏移混淆四变异 + 8bit 断言 + Lcg 公式对拍
    test_shadow_forward.cppm     # shadow.slang 选级 forward 提取式数学恒等（invVP 列叉积，5a-8c 回归，12.3d-C）
    test_ssr_viewproj.cppm       # 批 15.1 SSR 尾区 @723712：字节布局互逆 + 抖动口径闭式差量（fillSsrViewProj 接缝）
    test_ssr_params.cppm         # 批 15.1/15.3 SSR 尾区两 vec4 @723776/@723792：两源布局 + fillSsrParams 填值（radius 直传/四系数落槽/空场景透传）+ 出厂默认值 = 批 15.1 常量
    test_wet_params.cppm         # 批 15.2A 湿面尾区两 vec4 @723968/@723984：两源布局 + fillWetParams 填值（六系数落槽/两槽不互换/消融档透传）+ 出厂默认值六枚（strength=1.0 = 「默认开」拍板的钉）
    test_evssm_math.cppm         # EVSSM 消费端公式数学契约（快路/零矩/far 矩恒等/单调/LBR，11.6d bug 回归）
    test_layout_drift.cppm       # 图外布局对账护栏的判定 + 值可读化（批 14.4b 步 2：IZB/FSR 两处护栏收成单点）
    test_gpu_profiler_time.cppm  # 债-87 GPU 时间戳换算口径：tick 差求时长 / 纪元自检平移量与阈值边界（实机 golden）
    test_device_primitives.cppm      # gpu 套件回读原语：clear 逐像素 / 整数回环 / UNORM 量化 / 拷贝保真
    test_device_primitives_impl.cppm # 设备夹具实现（partition impl unit）
    test_conservative_mesh.cppm      # gpu 套件 11.3-1b 成败门：保守光栅 × mesh shader × RADV（含 EDS3 动态态）
    test_izb_create.cppm             # gpu 套件 11.3-1c：IZB grid-of-lists 原子创建 + early-Z priming
    test_izb_mask.cppm               # gpu 套件 11.3-1d：光空间保守光栅 × IZB 走链 mask 判定
    test_izb_engine_contract.cppm    # gpu 套件 11.3 补账：引擎真 SPV 的 ndc 口径/字节偏移/guard 契约
    test_mdi_drawindex.cppm          # gpu 套件 MDI 门：DrawIndex 语义 × stride16 × 零计数不跳号（docs/mdi_plan.md §6）
    test_mdi_task_probe.cppm         # gpu 套件 ★M-2 task 路机制探针：DrawIndex 只在 task 入口 + payload 通路自证 + 守卫 mesh 毒化检测器（docs/mdi_plan.md §19.12）
    test_meshlet_geometry.cppm       # gpu 套件 ★1-a0：簇产物 4 段 GPU 上传的设备级逐字节回读校验
    test_meshlet_shadow_args.cppm    # gpu 套件 ★M-1a（§19.5）：MDI 行表 GPU 载体的**设备侧往返** + 🔴 argsOffset↔rowBase 同源的自证回路（一次真 MDI 展开 + 孔洞行阳性对照）
    test_descriptor_heap_ubo.cppm    # gpu 套件能力 spike：resource heap 能否放 eUniformBuffer 并被 ConstantBuffer<T> 读（+ binding→槽机制证伪）；dheap 4/5 = StructuredBuffer 元素 stride 8/44 设备端往返（R3 撤销条件第 2 条）
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
# MDI DrawIndex 探针（docs/mdi_plan.md §6）：一列一 draw，列号来自 rowBase+DrawIndex、
# 像素值来自 args 行 word[3]——两条信息同源自证（tests/test_mdi_drawindex.cppm）
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/mdi_spike.mesh.slang" MDI_SPIKE_MESH_SPV)
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/mdi_spike.frag.slang" MDI_SPIKE_FRAG_SPV)
# per-draw groupCount 探针（列=row*3+gid.x、值=tag+gid.x）——Phase A 的 1591 draw 各有不同
# groupCount，这条才真正担保"GPU 逐 draw 读到自己那行的 x"
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/mdi_groups.mesh.slang" MDI_GROUPS_MESH_SPV)
# ★M-1a（docs/mdi_plan.md §19.5）行表载体的 DrawIndex↔行 探针：24B 行 × ring×2 一张 buffer ×
# 两个数据字段（word[3]/word[4]）——🔴 判据是「argsOffset 与 rowBase 同源」，截图看不见（tests/test_meshlet_shadow_args.cppm）
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/mdi_shadow_rows.mesh.slang" MDI_SHADOW_ROWS_MESH_SPV)
# ★M-2 task 路机制探针（docs/mdi_plan.md §19.12）：DrawIndex 只在 task 入口声明、行信息经
# payload 交给 mesh（mesh 侧完全不碰 DrawIndex）+ 守卫 mesh（不声明 DrawIndex 的后续直接
# draw = STRIX 毒化检测器）。🔴 task/mesh 刻意分模块编译——slang#10123：同模块 payload 变量
# 复制成两份，spirv-val 放行而 NVIDIA 实测越界读（合并 = 测成编译器缺陷，判据作废）。
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/mdi_task_probe.task.slang" MDI_TASK_PROBE_TASK_SPV)
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/mdi_task_probe.mesh.slang" MDI_TASK_PROBE_MESH_SPV)
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/mdi_task_probe_guard.mesh.slang" MDI_TASK_PROBE_GUARD_MESH_SPV)
# 批 M mapping 过滤用例（tests/test_shader_mapping.cppm）的 task 判据产物：唯一职责 = 声明一个
# tests 里所有 mesh/frag 都不声明的 binding 3，好让"task 的声明漏进/漏出并集"两种实现给出不同集合。
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/mapping_task.task.slang" MAPPING_TASK_TASK_SPV)
# descriptor heap × UBO 能力 spike（tests/test_descriptor_heap_ubo.cppm）：
# 主判据体 + 阳性对照体（同 binding/同 heap，只差读哪个成员/索引）
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/dheap_ubo.comp.slang" DHEAP_UBO_COMP_SPV)
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/dheap_ubo_wrong.comp.slang" DHEAP_UBO_WRONG_COMP_SPV)
# StructuredBuffer 元素 stride 设备端往返（R3 撤销条件第 2 条，docs/constantbuffer_迁移计划.md
# §8.4）：stride 8 与 **44**（非 16 倍数的奇数 stride）；主判据读元素 1/2，阳性对照读元素 0
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/dheap_stride.comp.slang" DHEAP_STRIDE_COMP_SPV)
compile_slang("${CMAKE_CURRENT_SOURCE_DIR}/shaders/dheap_stride_wrong.comp.slang" DHEAP_STRIDE_WRONG_COMP_SPV)

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
    ${IZB_CREATE_SPV} ${IZB_PRIME_SPV} ${IZB_MASK_MESH_SPV} ${IZB_MASK_FRAG_SPV}
    ${MDI_SPIKE_MESH_SPV} ${MDI_SPIKE_FRAG_SPV} ${MDI_GROUPS_MESH_SPV}
    ${MDI_SHADOW_ROWS_MESH_SPV}
    ${MDI_TASK_PROBE_TASK_SPV} ${MDI_TASK_PROBE_MESH_SPV} ${MDI_TASK_PROBE_GUARD_MESH_SPV}
    ${MAPPING_TASK_TASK_SPV}
    ${DHEAP_UBO_COMP_SPV} ${DHEAP_UBO_WRONG_COMP_SPV}
    ${DHEAP_STRIDE_COMP_SPV} ${DHEAP_STRIDE_WRONG_COMP_SPV})
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
    PRIVATE Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator SDL3::SDL3 imgui::imgui
            nlohmann_json::nlohmann_json)  # test_burst_protocol 的入参类型（rlbase 里是 PRIVATE，不传递）
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
