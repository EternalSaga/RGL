-- ssr_rig 靶运行时：headless 出图 + **抗锯齿锁 off**。
--
-- `aa = "off"` 是判据前提：FSR/TAA 会改写 tonemap 的色输入来源（`main_scene` 的色输入
-- 优先级链 FSR > ao_applied > taa_out > scene_hdr），而 SSR 将来也要往这条链上插一档。
-- 判据阶段把 aa 锁死在 off，SSR 的影响域才是"只有 SSR"。
return {
    headless = true,
    headless_size = { 1280, 720 },
    win_size = { 800, 600 },
    show_after = -1,
    present_mode = "immediate",
    ui_font = "",
    ui_font_size = 18.0,
    aa = "off",
    llm_debug = true,
    llm_shot = {
        file = "screenshots/ssr_rig.png",
        size = { 1280, 720 },
        delay_frames = 8,
    },
}
