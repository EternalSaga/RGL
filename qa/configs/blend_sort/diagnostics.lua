-- 诊断门：只留 headless 出图所需，其余全关（每一项都对应显示链上多出来的一环，
-- 少关一项，闭式解就少一个已知量）。imgui/picking 开着会往像素上叠东西。
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
    rpc = false,            -- 冻结目录保持关；imgqa probe 复制副本后改写此键走 RPC 单启动
                            -- 三拍（后缓冲 + gbuf 通道 0/3，2026-10-18 重立，见 probe.cpp 头注）
    -- 阴影键已迁出（批 N6b）→ 同目录 shadow.lua 的 mode = "none"（= 关键 1：
    -- sampleShadow1Tap 恒 1.0，无遮挡）。旧键 shadow_pass 在本文件出现即启动失败。
    -- 键名坑的教训原样保留：这里曾写 camelCase 的 shadowPass，解析器不认 → 静默回落
    -- 默认 true，靠探针 frag 不采样阴影而恰好无害（两个错误互盖，12.3b 查实改对）。
}
