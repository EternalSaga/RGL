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
    -- 批 15.1 前半：ssr 门显式写出（出厂默认 false = 行为零变化）。写在这里是为了
    -- `just smoke --ssr --config-base qa/configs/ssr_rig` 能 patch 到它（patch_lua 要求
    -- 键恰好命中 1 次）——靶配置的每个旗标都必须"在场且可核对"，不吃隐式默认。
    ssr = {
        enabled = false,
        -- 批 15.3 子步 A（还 §3.2 未还账④）：五枚画质系数**显式写出**（值 = SsrTuning 出厂
        -- 默认，行为零变化）。18 臂扫描表复跑驱动 tools/ssr_scan.py 经 smoke.py --set 注入
        -- 这五枚键，patch_lua 的「键命中 ≠ 1 即 FAIL」要求键在场——与 `enabled` 写在这里
        -- 同一条理由：靶配置的每个旗标都"在场且可核对"，不吃隐式默认。
        max_distance_radii = 0.52,
        thickness_px = 6.0,
        bias_px = 0.6,
        rough_full = 0.15,
        rough_cutoff = 0.40,
    },
    -- 批 15.2B：湿面 G-buffer 调制**钉死关**。本靶测的是 SSR 闭式凸包，而镜面 roughness
    -- 恒 0.122 = wet=1 满调制 ⟹ 不钉则出厂湿面调制改写 metalrough/albedo，判据 A 的
    -- 闭式覆盖率与判据 B 的泄漏恒等式全被重定义。strength=0 = 恒等旁路（逐位 no-op）。
    wet = {
        strength = 0.0,
    },
    llm_debug = true,
    llm_shot = {
        file = "screenshots/ssr_rig.png",
        size = { 1280, 720 },
        delay_frames = 8,
    },
}
