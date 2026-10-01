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
    profiler = 0,           -- Phase 9 CPU zone profiler：帧窗口 N（0=关；最近 N 帧，退出 dump 到 profiles/，见 docs/phase9_plan.md）
    imgui = false,          -- Phase 10 IMGUI 调试面板总开关（true=帧末渲染 UI + 输入拦截；headless 无窗天然关，见 docs/archive/phase10_plan.md）
    config_reload = false,  -- Phase 10 批 10.3 配置热重载（true=帧边界轮询源 configs/：scene.lua spin/exposure/env 强度、camera.lua 调参、log_level.json；imgui 开关本身需重启，见 docs/archive/phase10_plan.md）
    shader_reload = false,  -- Phase 10 批 10.4 shader 热重载 L1（true=帧边界 fork slangc 全量重编源 rgl/shaders/ 并同签名守卫换入；接口契约变化保留旧管线提示重启；slangc/源目录缺失降级关，见 docs/archive/phase10_plan.md）
    picking = false,  -- Phase 10 批 10.5 GPU id 点选 + 批 10.7 选中高亮（true=编译图期追加 object_id pass（r32ui 写 objectIndex+1，点选回读 1 texel 反查 entity 进面板）+ highlight pass（琥珀高亮叠加成片，按材质过滤=同材质实体一起发光）+ 面板自动切"选中"页 + Esc 取消；默认关=图逐位同，见 docs/archive/phase10_plan.md）
    shadow_pass = true,  -- Phase 11 批 11.2：定向光 CSM 阴影总开关（**5a-9 起默认开**）。true = 图首 4 个 depth-only shadow pass 画投影体进 2048² 图集（4 级各占 1024² 块）+ 采样端 4 抽头 PCF + 末级 shadowFar 淡出（forward/deferred 共用 shadow.slang）。false = 阴影段零 draw + 采样端 enabled=0 直接全亮 → **画面逐位不变**（回归对照用）。开阴影 GPU 成本 +7.1ms（超 Phase 9 余量 3.1ms，画质优先定案，见 scene_data.cppm kActiveShadowCascades 注），见 docs/phase11_plan.md §12.8
    rpc = false,      -- Phase 10b HTTP POST RPC 调试通道（true=帧边界命令队列，仅绑 127.0.0.1；GET /state 发现参数，POST /camera /param /shot；独立于 imgui 门，headless 可用，见 docs/phase10b_plan.md）
    rpc_port = 8788,  -- RPC 监听端口（bind 失败只降级关，不崩）

    -- 批 11.3 HFTS/FTIZB 硬阴影（docs/hfts_izb_plan.md）：**默认关 = 零构造零命令，画面逐位不变**。
    -- izb_shadow=true 仅在 deferred 变体生效：light pass 前图外 dispatch IZB 创建 + priming
    -- （深度源 = 本帧深度图，光空间 = cascade-0 viewProj，近场 v1）；forward 无 drawLight pass
    -- 天然不触发（z-prepass 两路接 = 批 1e）。mask 消费未接 = 开门画面不变（1d 只建数据）。
    izb_shadow = false,
    izb_grid = 512,       -- 光空间网格边长 G（clamp [64, 2048]）
    izb_dump_frame = 0,   -- 第 N 帧（1 起）回读 head 直方图打日志（N+1 帧输出）——冒烟阳性锚点；0 = 关

    -- 脚本化输入探针（窗口模式输入链回归护栏，2026-09-28 "面板吞 WASD" 事故配套；
    -- 同 picking_probe 方法论，默认关 = 零行为变化；帧号 0 起）：
    -- ui_click_probe = { frame = 30, x = 100, y = 120 },          -- 第 N 帧合成一次鼠标左键点击 (x,y)
    -- camera_probe = { frame = 60, key = "W", holdFrames = 8 },  -- 第 N 帧按住移动键 holdFrames 帧，验证
    --                                                             -- eventSystem→InputFilter→cameraSystem 链，打 PASS/FAIL
}
