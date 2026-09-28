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
    rpc = false,      -- Phase 10b HTTP POST RPC 调试通道（true=帧边界命令队列，仅绑 127.0.0.1；GET /state 发现参数，POST /camera /param /shot；独立于 imgui 门，headless 可用，见 docs/phase10b_plan.md）
    rpc_port = 8788,  -- RPC 监听端口（bind 失败只降级关，不崩）

    -- 脚本化输入探针（窗口模式输入链回归护栏，2026-09-28 "面板吞 WASD" 事故配套；
    -- 同 picking_probe 方法论，默认关 = 零行为变化；帧号 0 起）：
    -- ui_click_probe = { frame = 30, x = 100, y = 120 },          -- 第 N 帧合成一次鼠标左键点击 (x,y)
    -- camera_probe = { frame = 60, key = "W", holdFrames = 8 },  -- 第 N 帧按住移动键 holdFrames 帧，验证
    --                                                             -- eventSystem→InputFilter→cameraSystem 链，打 PASS/FAIL
}
