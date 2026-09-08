-- configs/runtime.lua — 运行/窗口模式（2026-09-08 起环境变量全面转配置）
-- 全部字段可选，缺文件/缺段 = 回落默认（交互窗 800x600、不截图）。
-- 注：驱动级 env（RADV_EXPERIMENTAL / VK_DRIVER_FILES / prime offload）不在此列——
-- 由 Justfile run-* 配方注入，非应用配置。
return {
    -- headless：VK_EXT_headless_surface，脱 SDL/compositor（CI/agent 冒烟/截图）
    headless = false,
    headless_size = { 1280, 720 },

    -- 交互窗尺寸（llm 模式用 llm_shot.size、headless 用 headless_size，此值被覆盖）
    win_size = { 800, 600 },

    -- DIAG：隐藏窗启动、第 N 帧后显示（可见窗 fault 排查；-1 = 立即显示）
    show_after = -1,

    -- llm_debug 一次性截图（agent 自主工作：渲染 → 截图 → 退出）：
    -- 截图只改 llm_debug 开关（true/false）；file/size/delay_frames 常驻 llm_shot。
    -- 相机位置不在本文件——编辑 camera.lua 顶层 position/target/fovDegrees。
    -- 注意：agent 冒烟/截图 = 改本文件 build 副本（下次 just build 按源自动复原）
    llm_debug = false,
    llm_shot = {
        file = "screenshots/llm_debug.png",
        size = { 1280, 720 },
        delay_frames = 8,  -- 第 N 帧渲染完成后截图（0 = 首帧）
    },
}
