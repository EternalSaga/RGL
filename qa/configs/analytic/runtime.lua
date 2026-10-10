return {
    headless = true,
    headless_size = { 1280, 720 },
    win_size = { 800, 600 },
    show_after = -1,
    present_mode = "immediate",
    ui_font = "",
    ui_font_size = 18.0,
    -- 批 15.2B：湿面 G-buffer 调制**钉死关**。本靶测的是闭式解探针（GGX+ACES 逐式转录的
    -- 独立第二实现），它不认识湿面调制 ⟹ 不钉则 albedo 压暗/粗糙度端点改写材质观测值，
    -- 逐字节对拍必红。strength=0 = 恒等旁路（逐位 no-op）。
    wet = {
        strength = 0.0,
    },
    llm_debug = true,
    llm_shot = {
        file = "screenshots/analytic_probe.png",
        size = { 1280, 720 },
        delay_frames = 8,
    },
}
