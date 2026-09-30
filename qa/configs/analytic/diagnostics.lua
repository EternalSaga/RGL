-- 分析探针：把非直接光的一切**逐项关掉**，让着色链塌成闭式可解。
-- 每一项对应链上的一环；少关一项，闭式解就少一个已知量。
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
    picking = false,        -- 不加 picking/highlight pass（图逐位同基线形态）
    rpc = false,            -- llm_debug 截图足够，不需要 RPC
    shadowPass = false,     -- ← 关键 1：sampleShadow1Tap 恒 1.0（无遮挡）
}
