-- 变体 1「带边」诊断门：只留 RPC 连拍，其余全关。
--
-- ⚠ 阴影键已迁出（批 N6b，docs/shadow_routes_plan.md §2）：shadow_pass / shadow_pcss /
--   shadow_evssm / izb_shadow / izb_grid / izb_dump_frame 在本文件**出现即启动失败**，
--   新落点 = 同目录 shadow.lua（mode = "none" + izb = false）。analytic 探针踩过的键名坑
--   还在：写错键 = 解析器不认 = 静默回落默认，所以旧键的 fail-fast 永久保留。
--
-- 关阴影 + 关 IZB 的理由见同目录 shadow.lua 头注（本探针直射光强度已是 0，阴影因子
-- 应当恒 1）。
return {
    no_vvl = false,
    no_hdr = false,
    no_present = false,
    vtx_canary = false,
    texel_canary = -1,
    dump_gbuf = -1,
    style_dist = false,
    tex_dist = false,
    texture_force_size = 0,
    texture_force_white = false,
    texture_force_unorm = false,
    profiler = 0,
    imgui = false,
    config_reload = false,
    shader_reload = false,
    picking = false,
    rpc = true,          -- ← 唯一开着的调试门（/state /camera /shotburst /quit）
    rpc_port = 8788,
}
