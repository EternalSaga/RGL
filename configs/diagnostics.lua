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

    -- 批 11.3 HFTS/FTIZB 硬阴影（docs/hfts_izb_plan.md）：izb_shadow / izb_grid / izb_dump_frame
    -- 已迁出 → configs/shadow.lua 的 izb = { on, grid, dump_frame }（N6a，见上方迁出注释）。

    -- 脚本化输入探针（窗口模式输入链回归护栏，2026-09-28 "面板吞 WASD" 事故配套；
    -- 同 picking_probe 方法论，默认关 = 零行为变化；帧号 0 起）：
    -- ui_click_probe = { frame = 30, x = 100, y = 120 },          -- 第 N 帧合成一次鼠标左键点击 (x,y)
    -- camera_probe = { frame = 60, key = "W", holdFrames = 8 },  -- 第 N 帧按住移动键 holdFrames 帧，验证
    --                                                             -- eventSystem→InputFilter→cameraSystem 链，打 PASS/FAIL
}
