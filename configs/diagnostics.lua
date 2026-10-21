-- configs/diagnostics.lua — 诊断开关（全可选，缺文件/缺段 = 全关，正常路径零影响）
-- 历史排查（4K 黑屏 UAF / 可见窗 dmabuf fault / sRGB 嫌疑）留下的门，2026-09-08 由
-- 环境变量转本配置。启动日志会打一行 "diagnostics: ..." 汇总当前值。
return {
    no_vvl = false,             -- 关 VVL（descriptor heap wrapper 双写排查）
    no_hdr = false,             -- 强制 SDR surface 格式（HDR 后缓冲 fault 排查）
    no_present = false,         -- 渲染但永不 present（KWin dmabuf import 排查）
    vtx_canary = false,         -- 顶点上传读回金丝雀（拼接 buffer 保真度）
    texel_canary = -1,          -- 帧 N 重读白纹理比对（GPU 侧 UAF 签名；-1/缺省 = 关）
    dump_gbuf = -1,             -- deferred 截图直读第 N 个 G-buffer 通道出 PNG（0..4；-1 = 关）
    style_dist = false,         -- per-primitive 风格分布日志
    tex_dist = false,           -- per-primitive 纹理 slot 分布日志
    texture_force_size = 0,     -- 纹理近邻缩放强制 N×N（尺寸阈值实验；0 = 关）
    texture_force_white = false,-- 纹理全白填充（"均匀内容不 fault"假设）
    texture_force_unorm = false,-- 纹理上传强制 UNORM（sRGB 嫌疑隔离）
    profiler = 0,           -- Phase 9 CPU zone profiler：帧窗口 N（0=关；最近 N 帧，退出 dump 到 profiles/，见 docs/archive/phase9_plan.md）
    imgui = false,          -- Phase 10 IMGUI 调试面板总开关（true=帧末渲染 UI + 输入拦截；headless 无窗天然关，见 docs/archive/phase10_plan.md）
    config_reload = false,  -- Phase 10 批 10.3 配置热重载（true=帧边界轮询源 configs/：scene.lua spin/exposure/env 强度、camera.lua 调参、log_level.json；imgui 开关本身需重启，见 docs/archive/phase10_plan.md）
    shader_reload = false,  -- Phase 10 批 10.4 shader 热重载 L1（true=帧边界 fork slangc 全量重编源 rgl/shaders/ 并同签名守卫换入；接口契约变化保留旧管线提示重启；slangc/源目录缺失降级关，见 docs/archive/phase10_plan.md）
    picking = false,  -- Phase 10 批 10.5 GPU id 点选 + 批 10.7 选中高亮（true=编译图期追加 object_id pass（r32ui 写 objectIndex+1，点选回读 1 texel 反查 entity 进面板）+ highlight pass（琥珀高亮叠加成片，按材质过滤=同材质实体一起发光）+ 面板自动切"选中"页 + Esc 取消；默认关=图逐位同，见 docs/archive/phase10_plan.md）
    -- ── 阴影键已迁出（批 N6a 迁出、N6b 断根，docs/shadow_routes_plan.md §2 红线 2）──
    -- shadow_pass / shadow_pcss / shadow_evssm / pcss_sun_degrees / pcss_search_radius /
    -- pcss_max_penumbra / izb_shadow / izb_grid / izb_dump_frame 在本文件**出现即启动失败**
    -- （不静默映射：本项目最怕"配置写了、什么都没发生"；fail-fast 是永久护栏，不随迁移
    -- 批次退役）。新落点 = configs/shadow.lua（mode / izb / pcss.* / evssm.*），N6b 起
    -- 它是阴影门的**唯一**真相源：DiagnosticsConfig 里那九个字段已删除，消费点一律经
    -- shadowConfig() + shadowFormPlan() 读形态。

    -- 债-30：遮挡剔除结果的**驱动方式**（三态互斥，语义 → rgl/diagnostics.cppm `CullReplayMode`；
    -- 定价与方案推导 → docs/cpu_perf_plan.md §4）。为什么默认不是"最忠实"的 gpu_sync：
    -- 读回重放要在帧首同步等上一帧的 cull 结果，而 cull GPU 本身只 ~14µs —— 等的其实是
    -- "上一帧整帧 GPU 排空"（实测 = gpu.frame 的几乎全部，跨 3.6× GPU 跨度成立）。
    --   "cpu"        = 默认。CPU 视锥剔除的结果驱动 draw，GPU 集整条不上 CB（实测交付
    --                   13.29→10.66ms、开发环境 28.17→15.25ms；截图逐位不变）
    --   "gpu_lagged" = GPU 集照跑、结果滞后一帧收割，与「提交那一帧」的 CPU 表逐帧对账；draw 仍
    --                   由 CPU 驱动 ⟹ 画面逐位不变，但 GPU 读回路被真机持续验证（对账不失明）
    --   "gpu_sync"   = 本帧等读回并由 GPU 集驱动 draw（= 本键存在以来的原行为，DGC 对照组）
    -- 未知值 = 启动失败（不静默回落：否则"以为在测 GPU 路其实跑的是 CPU 路"）。
    cull_replay = "cpu",

    -- 债-120：RADV 静态 shader DGC 路（staticGraphicsIndirect 设备的 spike 门，默认关，
    -- spike 全文 → docs/archive/radv_static_dgc_spike.md）。设备有 static 而无
    -- graphicsIndirect、且本键开、且全集非空 ⟹ 构造 DgcStaticCommands：streams buffer
    -- 分配（启动日志「gpu cull: … + streams …」后缀）+ fanout dispatch 进 cull CB +
    -- vkCmdExecuteGeneratedCommandsEXT 驱动 draw（启动日志「DGC path: … DGC static
    -- execute」行）；cull 第 5 类 hazard 边 cull.sequences ↔ stream_fanout.sequences
    -- 自此才有消费者。**要整条路真被走到还需 cull_replay="gpu_sync"**——cpu 档 cull GPU
    -- 集整条不上 CB，gate_lint[cull_replay] 会 warn 点名这一点（设备不支持时另有
    -- gate_lint[dgc_static] warn）。非布尔值 = **静默回落 false**（readBool 走
    -- sol::optional<bool>，类型检查失败即取默认；实测 "yes" 与 1 均无 warn 无 error，
    -- 与 cull_replay 三态键的「未知值 = 启动失败」不同——那是字符串键，这是 bool 族
    -- 的既有共同行为，本刀不修族）。
    dgc_static = false,

    rpc = false,      -- Phase 10b HTTP POST RPC 调试通道（true=帧边界命令队列，仅绑 127.0.0.1；GET /state 发现参数，POST /camera /param /shot；独立于 imgui 门，headless 可用，见 docs/archive/phase10b_plan.md）
    rpc_port = 8788,  -- RPC 监听端口（bind 失败只降级关，不崩）

    -- GPU fault masked 轮询（rgl/device_fault.cppm；查偶发 GPU hang 用，默认关）：
    -- true = 每帧 vkGetDeviceFaultReportsKHR(timeout=0) 轮一次，把驱动**自己内部恢复掉、
    -- 没上报 device lost** 的 masked fault 打成 ERROR。🔴 语义代价：启用该 feature 后驱动
    -- **可以**选择内部恢复而不报 device lost —— 开这道门有可能把"炸给你看"变成"悄悄恢复 +
    -- 一条 ERROR"。关 = 零 Vulkan 调用；device lost 之后那一次抓取**不受本门影响**（恒尝试）。
    -- 生效前置 = 驱动有 VK_KHR_device_fault 且 deviceFaultReportMasked 位为真（本机 RADV
    -- 实测两者都有；探测结果看启动日志 "Device device_fault:" 那行）。
    fault_poll = false,

    -- VVL GPU-AV 兼容模式（rgl/shader.cppm 的 shader object 创建标志；默认关）：
    -- true = 创建 shader object 时**不带** VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT。
    -- 为什么需要这一枚：GPU-AV 装不了带该 bit 的 shader object，会打一条 Internal Error 然后
    -- **自行关闭 GPU-AV**（实测原文："Unable to instrument shader using
    -- VkIndirectExecutionSetEXT validly … GPU-AV's usage of
    -- VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC (If you don't need
    -- VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT, turn it off)"）——于是"开了 GPU-AV"与"没开"
    -- 在日志上几乎同形。摘掉后 GPU-AV 才真的装桩（DescriptorHeapPass / MeshShading 有计数）。
    -- 🔴 代价：该 bit 是进 DGC execution set 的 shader 的硬要求（VUID-11032/11154），所以
    -- **不要与 dgc_static 同开**（gate_lint 会 warn）；且它改变 shader object 创建标志，
    -- 本构建下的像素与性能读数都不代表交付形态。开它的正确姿势（GPU-AV 本体由 VVL 侧配置）：
    --   VK_KHRONOS_VALIDATION_GPUAV_ENABLE=1 just smoke --set gpuav_compat=true
    gpuav_compat = false,

    -- 描述符堆**槽内容**对账（rgl/meshlet_shadow_args.cppm 的 auditSlot；默认关，债-140 第三击 ①）：
    -- true = 每帧在 MDI 行表 upload 之前，把行表绑定的那个 heap 槽读回来与构造期快照逐字节对账。
    -- 抓的是"槽里现在是 0 / 是陈旧值"——devcoredump 实测 fault 地址恰 = 0x0 的直接对应形态。
    -- 为什么两网都替代不了：ASan 不插桩描述符堆（mmap 的设备可见内存），GPU-AV 只查描述符
    -- **索引**越界、不查槽里存的**地址值**。成本 = 每帧一次 ≤64B host 映射 memcmp（实测报数）。
    -- 🔴 生效前置 = MDI 行表载体存在（`meshlet.mdi_shadow=true`）；关着时零调用，gate_lint 会 warn。
    heap_slot_audit = false,

    -- **GPU 内存分配时毒化**（债-140 H3 实验，docs/债140_跨驱动与上游取证.md §13.5；默认关）。
    -- true = 三类 GPU 内存在**分配那一刻**整片填可辨识非零毒值（子集见 poison_scope）：
    --   heap     = bindless 描述符堆（resource + sampler 两个 heap 的**用户槽区**，避开驱动
    --              reserved 槽——那段是驱动的，host 写它是 UB）
    --   rowtable = MDI 行表（persistent-mapped 的 ring×2）
    --   cull     = cull 数据链（candidate_table / sequences / sequence_count / entry_streams）
    -- 判据为什么成立：毒只在**从未被写过**的地方活到 GPU 读取那一刻（CPU 全量写的 buffer 毒会被
    -- 覆盖 = 对它是 no-op；GPU 每帧写满的 buffer 同理）。于是 fault 地址从 `0x0` 变成毒值形态
    -- = 「读了从未写过的槽/行」当场定罪；毒字按四字组下标递增，地址可反解出是哪个四字组。
    -- 🔴 只做**分配时**毒化，不做逐帧重毒（逐帧会改掉 ring 的复用语义，那是 v2）。
    -- 🔴 门开时的任何像素/性能读数都不代表交付形态；门关 = 一条 if 都不进（逐位不变）。
    poison_gpu_mem = false,
    -- 毒化范围（分 buffer 归因用，改这键**不用重编**）："all" 或 heap / rowtable / cull 的
    -- 逗号子集（如 "heap"、"rowtable,cull"）。🔴 未知名 = 启动失败，不静默回落：拼错一个名字
    -- 就少毒一块，而"没毒到"与"毒到了没炸"在 fault 地址读数上**完全同形**（同 cull_replay 纪律）。
    poison_scope = "all",

    -- **A1 黑匣子**（债-140 锁存路线判死后的数据侧摄像头，docs/债140_跨驱动与上游取证.md §13.11；默认关）。
    -- true = 每帧把 MDI 阴影段现场事实（帧号 / 每段已写行数 / 上传的间接计数 COUNT 及其来源 /
    -- 行表首末行六字 + 整段摘要 / heap 槽台账 / DGC 状态）投进环形缓冲，独立线程异步落 ndjson。
    -- 为什么需要：devcoredump 只有复位**之后**的快照（§13.10 实测那里 fault 锁存全是复位值），
    -- 要归因概率性挂死就得自己留一份"案发前最后 N 帧"。
    -- 🔴 观测者效应：本案唯一已证实能改挂率的变量是速度，记录本身会改时序 ⟹ 挂率只能读成
    -- "没抓到"。环形缓冲溢出会覆盖最旧并计数，复盘必须连落盘统计一起读。门关 = 一条 if 都不进。
    blackbox = false,
    -- 黑匣子 ndjson 路径（宜绝对路径，父目录须已存在）。单独一枚键是为了逐轮实验各出一份产物
    -- 而不用重编（同 poison_scope 的加键理由）。🔴 blackbox=true 而此键为空 = 什么都没发生，
    -- gate_lint 会 warn —— "写了但什么都没发生"是本项目最忌的失效形态。
    blackbox_path = "",
    -- **A2 着色器自证**（GPU 侧摄像头，与 A1 同一批；默认关）。
    -- true = 阴影 MDI 的 task 入口换用 -DSHADOW_SELFCHECK 变体 SPV，读行表后自证五条判据
    -- （行号越界 / gcX≠1 / meshGroupCount==0 / objectIndex 越界 / gcY‖gcZ≠1），违例走
    -- printf → SPIR-V NonSemantic.DebugPrintf → RADV 每 submit 打 stderr。两个界经 push
    -- word2（MDI 下的保留位，mesh 侧不读）传入。
    -- 🔴 为什么是第二个 SPV 而不是运行时分支：light.frag IZB_CONSUME 的实测教训（105401 像素
    -- 漂移）—— 代码"出现在编译单元里"本身就会让 slang 重排 fp 融合，门关选原版 = 逐位不变。
    -- ⚠ 还要驱动侧开缓冲才有输出：RADV_PRINTF_BUFFER_SIZE=16777216；不设 = printf 是 no-op，
    --   那时"零输出"分不清"没违例"与"没缓冲"。
    shader_selfcheck = false,

    -- 批 11.3 HFTS/FTIZB 硬阴影（docs/hfts_izb_plan.md）：izb_shadow / izb_grid / izb_dump_frame
    -- 已迁出 → configs/shadow.lua 的 izb = { on, grid, dump_frame }（N6a，见上方迁出注释）。

    -- 脚本化输入探针（窗口模式输入链回归护栏，2026-09-28 "面板吞 WASD" 事故配套；
    -- 同 picking_probe 方法论，默认关 = 零行为变化；帧号 0 起）：
    -- ui_click_probe = { frame = 30, x = 100, y = 120 },          -- 第 N 帧合成一次鼠标左键点击 (x,y)
    -- camera_probe = { frame = 60, key = "W", holdFrames = 8 },  -- 第 N 帧按住移动键 holdFrames 帧，验证
    --                                                             -- eventSystem→InputFilter→cameraSystem 链，打 PASS/FAIL
}
