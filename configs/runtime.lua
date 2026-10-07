-- configs/runtime.lua — 运行/窗口模式（2026-09-08 起环境变量全面转配置）
-- 全部字段可选，缺文件/缺段 = 回落默认（交互窗 800x600、不截图）。
-- 注：驱动级 env（VK_DRIVER_FILES / prime offload）不在此列——由 Justfile run-* 配方注入，
-- 非应用配置。Mesa ≥26.2.3 起 RADV 无需 RADV_EXPERIMENTAL。
return {
    -- headless：VK_EXT_headless_surface，脱 SDL/compositor（CI/agent 冒烟/截图）
    headless = false,
    headless_size = { 1280, 720 },

    -- 交互窗尺寸（llm 模式用 llm_shot.size、headless 用 headless_size，此值被覆盖）
    win_size = { 800, 600 },
    -- render_scale（Phase 14 批 14.3）：内部渲染分辨率倍率 = 渲染网格 / 窗口像素。
    --   1.0  = 默认，渲染网格与后缓冲逐像素相同（**交付路径逐位不变**）
    --   0.75 / 0.5 = 内部按 3/4、1/2 分辨率渲染，由 tonemap pass 采样 scene_hdr 时
    --                线性放大到窗口（该 pass 是全屏大三角 + linear sampler，归一化 uv
    --                采样 ⟹ 放大免费，没有额外的 pass 或 RT）
    -- 合法区间 [0.25, 1.0]（越界 = 钳制 + warn，非有限值 = 回落 1.0；上限不给 >1 是
    -- 因为超采样没有任何批次需要，留着只会多一条永不被测的路径）。
    -- **创建期量**：RT / 深度图按它分配，改它要走重启，热重载不生效。
    -- 与 imgui / 点选 / 鼠标**无关**（那些恒在窗口像素空间）。
    -- 与债-37 的 Wayland 分数缩放正交：那条管「逻辑点 → 窗口像素」，本键管「窗口像素 → 渲染像素」。
    -- 生效现场看启动日志 "render scale = ... → render WxH (swapchain WxH)"。
    render_scale = 1.0,

    -- DIAG：隐藏窗启动、第 N 帧后显示（可见窗 fault 排查；-1 = 立即显示）
    show_after = -1,

    -- present_mode：窗口交换链 present 模式（批 9.3 用户追加，headless 恒 eFifo 忽略此键）：
    -- "immediate" = 现状（硬要求 eImmediate，Wayland compositor 管帧同步）
    -- "fifo"      = eFifo（vsync 阻塞、帧率 = 刷新率；present_timing 双模式对比测量用）
    present_mode = "immediate",

    -- imgui 面板字体（Phase 10b 批 11b.1，仅 diagnostics.imgui=true 时生效）：
    -- ui_font = 字体文件绝对路径，空 = 自动探测链（MiSans Light → MiSans → Noto CJK
    -- → 文泉驿 → Windows 微软雅黑/黑体，链表见 imgui_layer_impl kUiFontCandidates）；
    -- 中文渲染需系统字体有对应字形 + imgui[freetype]（CFF 轮廓，如 Noto CJK）。
    ui_font = "",
    ui_font_size = 18.0,   -- 字号 px（imgui 内建 13 太挤；Light 字重发虚就调大或改用 MiSans-Regular）

    -- llm_debug 一次性截图（agent 自主工作：渲染 → 截图 → 退出）：
    -- 截图只改 llm_debug 开关（true/false）；file/size/delay_frames 常驻 llm_shot。
    -- 相机位置不在本文件——编辑 camera.lua 顶层 position/target/fovDegrees。
    -- ⚠ 改本文件 build 副本**不会**自动复原：copy_if_different 只源更新才覆盖，副本比源新
    -- 就永远不覆盖（2026-10-03 实测踩坑：某次冒烟删了 scene.lua 的 environment 块，之后
    -- 每次运行都没天穹，排查时先被当成代码回归）。冒烟完请立刻复原：
    --   cp configs/*.lua build/rgl/configs/     （或 touch configs/<改过的那个源文件>）
    -- 首选别走这条路：`just smoke` / `just shot` 用 RGL_CONFIG_DIR 跑一次性配置
    -- （tools/smoke.py），不碰本文件也不碰副本，因此没有需要复原的状态。
    llm_debug = false,
    llm_shot = {
        file = "screenshots/llm_debug.png",
        size = { 1280, 720 },
        delay_frames = 8,  -- 第 N 帧渲染完成后截图（0 = 首帧）
    },
}
