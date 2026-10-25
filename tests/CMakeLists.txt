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
    test_cull_tables_layout.cppm # GPU cull 三张表元素契约（cull_tables 管线产物）：60/120/8 逐字段锚 + 旧三条 assert 的不变式接棒 + 总数与反向锚
    test_compute_push_layout.cppm # compute push 契约（compute-push 管线产物）：八枚逐字段布局 + 总数锚 + 反向锚
    test_compute_bindings.cppm    # B2-1″ compute binding 名→号 接口表：调用点七枚逐条阳性锚 + 反向 + 覆盖锚（9 枚）+ 三类报错可归因
    test_compute_hazard.cppm      # B2-2 compute hazard 机器派生 + 双源对账：判据四条各配反向 + 按类去重 + 🔴 人工半边↔反射互锁 + 数量锚 5 + 全产品面复算
    test_scene_config.cppm      # 四份配置解析契约
    test_gpu_poison.cppm        # 债-140 H3 毒化实验的纯函数面：毒值恒非零 + 逐字节小端 + 尾部不留洞不越界 + scope fail-fast + 🔴 门关三目标全 false（"门关=逐位不变"的唯一机器判据）
    test_blackbox.cppm          # 债-140 A1/A2：FNV 摘要公开向量 + 首末行摘取 hasRows 反向 + ndjson 里 COUNT 与 rows 各自在场 + 自证 push 超限不截断 + 🔴 五条自证谓词的 CPU 镜像（每条含相邻合法界；shader 侧判据唯一可单测的那一份）
    test_render_scale.cppm      # 批 14.3 渲染倍率纯算术：1.0 档逐像素红线 / 取整 / 钳制 / 非退化
    test_velocity_field.cppm    # 批 14.4 逐像素速度场：显式相机基参考 / 旋转光流闭式 / 天空与无效判据
    test_coordinate_rebase.cppm # 债-85 批 1 护栏：触发常数钉值 / 距离余量标定（625→≥3、1e6→<0）/ 粒度取最粗级（最大值居中反向）+ 相机 ULP 错法对照 / 非法输入不出 0 / snap 格点闭式 + 嵌套；批 3 toRelative：门关恒等 / 顺序承重阳性锚点（先减后收窄≠先收窄后减）/ 只动平移列四列断言 / 提升无损 / 组合恒等
    test_render_origin.cppm     # 相对坐标批 3 立、批 5 第 1 刀改写（originSystem 已删，原点生产者 = projectionSystem）：RenderOrigin 默认 = 门关画像 / 门关铺垃圾→恰为逐位零 + view 逐位不变 / 🔴 门开原点闭式 + 每分量落在 G 格上 / 🔴 精度收益（ULP 比值，非「非零」）/ 🔴 两条粒度路径一致 + G·2、G/2 区分力守卫 / 🔴 粒度 NaN 必须抛且消息点名 coordinate.rebase（反向半边：粒度正常不许抛）/ 幂等（= 第 2 刀跨进程复现的进程内对应物）
    test_taa_cross_frame.cppm   # 相对坐标批 4 第一刀 taaSystem **接线**判据：门关 motion 逐位 = 改前纯旋转比 / 🔴 ΔO 方向（两原点传反=变号）/ prevOrigin 帧尾推进 / 原点差不被 historyValid·motion_compensation 门掉（velocity 链不吃门）/ 自旋+移动原点对上按定义算的两帧世界坐标
    test_view_light_rebase.cppm # 相对坐标批 4 第二刀 view/eyePos/灯位置三出口接线判据：门关逐位 = 改前 lookAt/收窄式 / 🔴 精度收益锚（1e6 处平移列塌缩 + sub-ULP 区分力守卫钉「先减后收窄」）/ 三出口同空间（闭式 + view·eyePos 跨出口）/ 不漏改（origin≠0 三处都变）/ projectionSystem 接线正反两半（view 吃原点、near/far 不吃）
    test_shadow_texel_grid.cppm # 相对坐标批 5 第 0 刀（只测量）：出厂四级纹素与吸附格距 G 的整除性 / 光基是否退化 / 🔴 原点跨一格后图集相对世界的跳变量（三假设对照 H0 世界轴、H1 光基、H2 光基+每级纹素抬 2 的幂）/ 比较算子区分力三半边
    test_shadow_texel_absolute.cppm # 相对坐标批 5 第 0.5 刀（只测量，零生产改动）：🔴 把 texel 吸附的输入换成**绝对**球心（H3）后原点跨格的图集跳变量（吸附项残余 vs 顶点路径残余分开报）/ 姿态变化时盒心仍格点量化（不量化臂反向半边）/ |O|≈1e6 处 double 域吸附的可行性边界 / 门关态逐位是否被改（决定要不要门关早退）/ H3 vs H2 取舍表 / 批 5 阈值推导
    test_rebase_consumers.cppm  # 相对坐标批 4 第三刀「自动跟/不用改」表 → 判据：🔴 texelSizes 整链（computeCascadeSpheres→buildShadowVolume→fillShadowData→rebaseSnapGranularity）与相机姿态/位置逐位无关（批 5 粒度不循环定义的唯一凭据）+ 跨档必须变的反向半边 / CPU 剔除可见集逐球平移不变（闭式六面对账）/ meshlet 簇可见集逐元素平移不变 + 两侧不同空间的错位锚点 / 透明排序键顺序平移不变 + 混空间锚点
    test_camera.cppm            # 相机数学
    test_cube_geometry.cppm     # 内建 cube 顶点/索引契约
    test_state_kit.cppm         # 十段 RenderState 表 + stateFor 分派（纯逻辑）
    test_render_graph.cppm       # 图编译：校验 / barrier 推导 / 装饰 / RT slot
    test_attachment_kind.cppm   # RgAttachmentKind 分派点穷尽性：kindName 拼写 + 四 validator 消息逐字节
    test_attachment_buffer.cppm  # 批次 B2-3 本体：buffer 档按名注册拒绝面 + storage×kind 双向规则
                                 # + plan-only 做成类型（buffer 不进 barrier 计划/渲染条目）+ Lua 词表
                                 # + 组 8（B2-4③′）：注册字节 >0 拒绝面 + 同名替换正常路径
    test_dynamic_render_graph.cppm  # B2-4③′（D5 第一刀）：按名结转（结转≠重置）+ 逐出台账正反
                                 # + declareBufferProductions 幂等/末尾追加/试编译 fail-fast + 计数
    test_attachment_sync.cppm      # B2-4④′：declareBufferConsumptions 语义 + 图声明面↔附件↔边表
                                 # 双源对账（正向缺边/反向半申报/休眠）+ 名字钉死 + EVSSM 实测锚点
    test_izb_sizes.cppm          # B2-4③′ IZB 迁入：尺寸公式闭式期望 + 写上界配对对账（判据 5）
                                 # + extent 漂移同名替换的机器可观测面 + 附件名常量钉死
    test_offgraph_contract.cppm  # 批次 B1 图外工作申报面：建表三 guard 各配反向 + 位置契约三种偏离
    test_offgraph_sync.cppm      # 批次 B1b 图外同步：词表/边表逐位钉死（迁移没改语义）+ 次数账双向
                                 # （含 §7 判据 2 的变异本体「调用点挪到 pass 循环之后」的单元等价形态）+ 覆盖对账两半边
    test_taa_jitter.cppm         # TAA 纯函数层：Halton / jitter→投影闭式解 / 跳切与历史判据（批 12.6）
    test_timing_util.cppm        # 时间戳/时间统计叶子：toMs 定义值 / percentile 口径逐点手算 / 时钟单调（三处副本收敛的护栏）
    test_scene_spin.cppm         # 帧驱动 spin 角闭式（门 spin_frame_angle 判据本体，跨进程 A/B 对齐的地基）
    test_camera_frame_yaw.cppm   # 帧驱动相机 yaw 闭式（门 frame_yaw 判据本体，temporal-motion 相机运动激励的地基）
    test_burst_protocol.cppm     # /shotburst 协议判据：形状六反向 + 绝对帧号锚点三态（债-89，跨进程逐帧复现的地基）
    test_rpc_contract.cppm       # RPC 路由单源生成物内部契约：唯一性 + 查表可逆 + 回包逐条同序（批 1）
    test_rpc_capabilities.cppm   # 能力观测批 1：/state.capabilities 拼装——内容只吃编译图（脏窗口判据 ①）
                                 # + rtBuilt 两半边（恒 true 判据 ②）+ 门关无 ssr_*（判据 ③）+ kind 矩阵 + 空图退化
    test_debug_rpc_decorate.cppm # 能力观测批 4：/decorate 探针分类（决策顺序 + 词表覆盖双向 = 变异④的常驻判据）+ 快照差异四半边 + 回包条件性
    test_frame_capture_miss.cppm # 帧捕获未命中分类：永久/可重试两半边各钉 + 边界逐格 + 闭式对账 + 默认预算双向（修「拼错附件名打死 /shot」）
    test_aa_target.cppm          # 细线抗锯齿靶几何：数据自洽 / "确实细"载体判据（批 12.6 T4）
    test_gate_lint.cppm          # 门组合自检：申报表覆盖率对账 + variant 交叉校验 + committed 金标准校验
                                 # （🔴 门数不在这里抄——真相源 = `kGateRules` / `kGateBlocks`，抄一次漂一次）
    test_frisvad_tangent.cppm    # Frisvad 切线兜底：轴向/±Z 奇点/随机有限性（黑面 bug 回归）
    test_render_list_shadow.cppm # fillShadowData 尾区契约：splits 哨兵/VP 拷贝/texel/params 两态（12.3d）
    test_shadow_lod_emit.cppm    # §18 批 B2 阴影段 draw 构造：c0 恒旧路径/×2 唯一真相源/条数不变/索引公式（collect+emit 接缝）
    test_meshlet_overlay.cppm    # 债-105 α 叠加段自有档 meshlet.mode="overlay"（债-129 S2 合并）：门关零 draw/真值表四格/四条既有判据反向/×2 同一入口/配置键探针
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
    test_shadow_proxy_types.cppm  # N10 P2 阴影代理钉段表：五条 invariant 各配反向用例（错误串两两不同）+ radius 闭式锚 + v1/零条空表 + R2 签名与阶跃闭式
    test_shadow_proxy_emit.cppm   # N10 P3 钉段 draw 的采集与发射：c0 恒一条 + c1..c3 两条 / push 独立块指针身份 / texel 门两态 / collect 宿主嵌套三态 / 门关与空表形状相等
    test_device_fault.cppm       # GPU fault 诊断通路的纯函数面：后端优先级真值表 / 枚举翻译两两不同 / 地址区间 spec 闭式 / 空报告与查询失败两条出口可分 / EXT 两段式容量决策
    test_shutdown_ledger.cppm      # 诊断账注册表：幂等（两条路只结算一次）/ 顺序=登记顺序 / 空表不印 / 置位先于调用
    test_heap_slot_audit.cppm   # 债-140 第三击①：描述符堆槽内容对账（恒等 / 清零 / 快照本身是零 / 陈旧地址 / 长度不符 五种现场两两可分）+ 第四击Ⓐ：烘入 mapping 对账（六种现场两两可分）
    test_mdi_rows.cppm           # ★M-0（§19.3）MDI 行表构建器：行序=候选序 + 前 12B 对账 Vk 结构 + stride 合法性 + 四条不变量反向用例（错误串两两不同）+ 配置键探针
                                 # + ★M-1a（§19.5）尺寸/偏移算术：🔴 argsOffset 与 rowBase 同源 + 07089/07090 合一闭式边界 + 容量少 1 字节必红
    test_mdi_shadow_emit.cppm    # ★M-1b（§19.2 D4 / §19.5）生效档采集判据：constexpr 桥（schema↔mdi_rows）+ 每级联一条 MDI + 同源 pair 闭式字面 + 混合形态/孔洞流/五档回落 + 行序=候选序
    test_mdi_direct_emu.cppm     # 债-140 §27 直派模拟（gfx1151 workaround，Mesa#16670）：三态裁决全表 + 保守上界网格 + 容量边界 + 组号→行号同源对账 + 空组早退 + 配置键
                                 # + ★M-1a（§19.5）尺寸/偏移算术：🔴 argsOffset 与 rowBase 同源 + 07089/07090 合一闭式边界 + 容量少 1 字节必红
    test_shader_mapping.cppm     # ★M-3T-a（§19.16）批 M mapping 过滤契约：🔴 无 task stage 不得判成解析失败（实踩：479 条误退全烘而终图逐位不变）+ task 声明进并集 + 三侧坏码流各触发 nullopt + 消费者侧条数与 running index 序
    test_shader_output_sig.cppm  # VUID-11148 判据面（2026-10-21 DGC execution set 修复）：fragment 输出签名只收 Output+Location + 声明序无关 + depth-only 空签名 + 坏码流哨兵 + 单/五输出必不同（合成 SPV，不读构建产物）
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
    test_mip_gen_chain.cppm          # gpu 套件 债-137：引擎真 mip_gen.comp.spv 的降采样采样点（0.5 而非 0.25）
                                     # —— 棋盘/水平梯度/垂直梯度三条（后两条是对称对，只改对一轴必红）
    test_mdi_drawindex.cppm          # gpu 套件 MDI 门：DrawIndex 语义 × stride16 × 零计数不跳号（docs/mdi_plan.md §6）
    test_mdi_task_probe.cppm         # gpu 套件 ★M-2 task 路机制探针：DrawIndex 只在 task 入口 + payload 通路自证 + 守卫 mesh 毒化检测器（docs/mdi_plan.md §19.12）
    test_meshlet_geometry.cppm       # gpu 套件 ★1-a0：簇产物 4 段 GPU 上传的设备级逐字节回读校验
    test_meshlet_shadow_args.cppm    # gpu 套件 ★M-1a（§19.5）：MDI 行表 GPU 载体的**设备侧往返** + 🔴 argsOffset↔rowBase 同源的自证回路（一次真 MDI 展开 + 孔洞行阳性对照）
    test_descriptor_heap_ubo.cppm    # gpu 套件能力 spike：resource heap 能否放 eUniformBuffer 并被 ConstantBuffer<T> 读（+ binding→槽机制证伪）；dheap 4/5 = StructuredBuffer 元素 stride 8/44 设备端往返（R3 撤销条件第 2 条）
    test_render_target_cache.cppm    # gpu 套件 RT 缓存多描述符：跨重建槽复用（EVSSM 矩图迁入的结构性收益）+ 逐项独立领号 + 预发号不重复领 + 规格漂移报错不动已有附件
)
# ── 测试归属完整性闸门（与 rgl/CMakeLists.txt 的引擎模块闸门同款理由）────────────
# 磁盘上每个 `test_*.cppm` 必须**恰好登记一次**。漏登记**不会有人红**：那个文件只是不再
# 编译，而 "663 passed" 与 "680 passed" 在门禁眼里一样绿 —— 少了东西同样让判据失效而
# 不出声。2026-10-20 实测踩到（债-140 第四击批）：一次批量编辑把已存在的条目整行吞掉
#（替换而非追加），configure 与 `just gates` 四道全绿，17 条用例静默消失，
# 直到对比测试总数才发现。反向（清单有、磁盘无）CMake 自己会报 "Cannot find source
# file"，不必重复。
file(GLOB RGL_TEST_FILES_ON_DISK CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cppm")
# 非用例 TU：支撑模块挂 rgl_test_support（见下方 target_sources），不进 RGL_TEST_SOURCES
set(RGL_TEST_NON_CASE_FILES test_support.cppm)
set(RGL_TEST_REGISTERED_ABS "")
# 🔴 `IN LISTS` 后面接的是**变量名**而不是值：写成 ${RGL_TEST_NON_CASE_FILES} 会去找一个
# 名叫 `test_support.cppm` 的变量（恒空）⟹ 闸门反过来误报（本批实测踩过）。
foreach(_t IN LISTS RGL_TEST_SOURCES RGL_TEST_NON_CASE_FILES)
    list(APPEND RGL_TEST_REGISTERED_ABS "${CMAKE_CURRENT_SOURCE_DIR}/${_t}")
endforeach()
set(_test_unregistered "")
foreach(_f IN LISTS RGL_TEST_FILES_ON_DISK)
    if(NOT _f IN_LIST RGL_TEST_REGISTERED_ABS)
        list(APPEND _test_unregistered "${_f}")
    endif()
endforeach()
if(_test_unregistered)
    string(REPLACE ";" "\n    " _test_pretty "${_test_unregistered}")
    message(FATAL_ERROR
        "以下测试 TU 未登记进 tests/CMakeLists.txt 的 RGL_TEST_SOURCES：\n    ${_test_pretty}\n"
        "它现在**根本不编译**，而 `just test` 依旧全绿 —— 用例数会静默变少。"
        "加进去时顺手写一行「测什么」的说明（本清单每条都带）。")
endif()


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
# 🔴 必须带 GLM 宏（清单在顶层 `RGL_GLM_DEFINES`，债-147）：本库 link 的是 **rlstd**，
# 而宏挂在 **rlbase** 上 ⟹ 不显式加就**不继承**。实测漏掉时本库是唯一一个「真 include glm
# 却不受管」的引擎侧 TU，而 `eyePlanes` / `frustumCorners` 这些共用几何 helper 的产出正是
# 各测试的**期望值来源**——helper 里若静默收窄，测试会跟着一起错而不报。
target_compile_definitions(rgl_test_support PUBLIC ${RGL_GLM_DEFINES})

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
# compute-push 生成物的**源树路径**（tests/test_compute_push_layout.cppm 的总数锚与反向锚要数
# 产物文本里的 `export struct`）。「恰好 8 个 struct」「没有 MeshletCullPush」这两条在 C++ 类型
# 层面写不出来——探测一个未声明的名字是**硬错误**而不是 SFINAE（`requires { typename RGL::X; }`
# 里 X 不依赖模板参数 ⟹ 名字查找失败落在概念定义点，clang 22 实测直接报 "no type named"），
# 所以只能读文本。生成物是已提交文件，源树路径稳定。
target_compile_definitions(rgl_tests PRIVATE
    RGL_COMPUTE_PUSH_CPPM="${CMAKE_SOURCE_DIR}/rgl/generated/compute_push_types_generated.cppm")
# cull_tables 生成物的**源树路径**（tests/test_cull_tables_layout.cppm 的总数锚与反向锚同样要数
# 产物文本里的 `export struct`，理由与上面那条完全相同：探测未声明的名字是硬错误而非 SFINAE）。
target_compile_definitions(rgl_tests PRIVATE
    RGL_CULL_TABLES_CPPM="${CMAKE_SOURCE_DIR}/rgl/generated/cull_tables_types_generated.cppm")
# gate_lint 金标准目录的**源树绝对路径**（tests/test_gate_lint.cppm 的默认校验模式靠它定位
# committed golden，🔴 **无需任何 env 就跑**——旧形态「env 未设 = no-op」是实测咬过人的假绿：
# d5cb02b 改门名 izb→izb.on 没重生成，committed golden 静默陈旧无人验。env
# RGL_GATE_LINT_GOLDEN=<dir> 保留为重生成模式。定位机制先例 = 上面两条源树路径宏）。
target_compile_definitions(rgl_tests PRIVATE RGL_GATE_LINT_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
# 债-140 mesh 自证的 **latch-once 结构护栏**要读 .slang 源本体（shader 里的"验的就是用的那一份"
# 是源码级性质，编译产物判不了）。定位机制与上面源树路径宏同源。
# 🔴 测试**运行时**读源文件 ⟹ 变异 .slang 不需要重编就能生效（也正因为如此，变异 harness 对这条
#    必须跳过构建，否则改的是 SPV 生效前的源、量的却是重编后的产物，读数会自相矛盾）。
target_compile_definitions(rgl_tests PRIVATE RGL_SHADER_SRC_DIR="${CMAKE_SOURCE_DIR}/rgl/shaders")
# 引擎 SPV 由 rgl 目标的 slang 规则产出：rgl_tests 直接依赖它，否则 `just test-gpu`
# 单测目标时拿到陈旧 SPV（"改完 shader 没重建就报全通过"的 modules 环境坑同款）。
add_dependencies(rgl_tests rgl)

# 测试代码不纳入 source_checks（tools/checks 扫的是 rgl/）：doctest 宏会触发大量
# 误报（GMF 白名单、行数阈值、函数长度对 TEST_CASE 全是噪声）。测试代码的质量由
# review 保证，不由静态规则保证——这是有意识的取舍，写在这里以免日后有人"顺手加回"。

include(CTest)
add_test(NAME rgl_unit COMMAND rgl_tests)
set_tests_properties(rgl_unit PROPERTIES TIMEOUT 300)
