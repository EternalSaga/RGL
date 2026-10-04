-- 变体 1「带边」诊断门：只留 RPC 连拍，其余全关。
--
-- ⚠ 键名用 **shadow_pass**（snake_case）。analytic 探针踩过的那条坑还在：写成 camelCase 的
--   shadowPass 解析器不认 → 静默回落默认 **true**（qa/configs/analytic/diagnostics.lua 头注
--   记着这次"两个错误互盖"的查实）。本 rig 直射光强度已是 0，阴影门开与关都画不出东西，
--   但"关不掉"和"没有效果"是两件事，写对键名才叫关掉。
--
-- 关阴影 + 关 IZB 的理由：本 rig 唯一的激励是 env 镜面项（light.intensity=0），阴影因子
-- 应当恒 1；留着阴影段只是多两个 pass 和一条"阴影走样"的潜在噪点来源。bistro 那侧的消融
-- 已实测"阴影+IZB 关掉白点 49→30"，说明在 bistro 上阴影确有贡献——但那是**直射光开着**的
-- 场景；本 rig 直射光已归零，这一项不再是混淆源。
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
    shadow_pass = false,
    shadow_pcss = false,
    shadow_evssm = false,
    izb_shadow = false,
    izb_grid = 512,
    izb_dump_frame = 0,
}
