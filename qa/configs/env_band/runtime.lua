-- 变体 1「带边」运行时：headless + RPC 连拍。
--
-- 为什么走 RPC /shotburst 而不是 llm_debug 一次性截图：phase-lock 口径要**同一进程内连续
-- 16 帧**（16 个抖动相位），llm_debug 一张就退。llm_debug 与 /shotburst 还是互斥的
-- （rgl/debug_rpc_exec_impl.cppm:365 直接 409 拒），所以这里必须 llm_debug = false。
-- 帧数与相位对齐由 harness 保证（见 /tmp/opencode/band_probe.py 的帧数自检）。
--
-- 1280×720 = 与 bistro 报障口径一致（画面角分辨率 45°/720 = 0.0625°/px 是整套闭式预测的
-- 分母，换分辨率要连带重算 manifest.extra.probe，别在这里随手改）。
return {
    headless = true,
    headless_size = { 1280, 720 },
    win_size = { 800, 600 },
    show_after = -1,
    present_mode = "immediate",
    ui_font = "",
    ui_font_size = 18.0,
    -- 批 15.2B：湿面 G-buffer 调制**钉死关**。本靶测的是时域带边/相位锁定（闭式预测表
    -- 是整套判据的分母），湿面调制会改材质观测值、与抖动相位无关地污染连拍差分。
    -- strength=0 = 恒等旁路（逐位 no-op）。
    wet = {
        strength = 0.0,
    },
    llm_debug = false,
    llm_shot = {
        file = "screenshots/env_band.png",
        size = { 1280, 720 },
        delay_frames = 8,
    },
}
