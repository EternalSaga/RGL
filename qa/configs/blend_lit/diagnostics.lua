-- 诊断门：只留「headless 出图 + RPC 读条数」所需，其余全关（每一项都对应显示链上
-- 多出来的一环，少关一项就多一个未知量）。imgui/picking 开着会往像素上叠东西。
--
-- 与 blend_sort 的**唯一实质差别**是 rpc：那边保持关（它的判据只吃 PNG），本靶的判据
-- 有一半是「透明段有几条 draw」，那是引擎侧的事实，**不许**由 harness 数像素反推
-- （反推 = 用被测量证明被测量）。所以条数走 RPC GET /state 的 render.transparentDraws，
-- 冻结目录里直接把门开着写清楚，而不是靠 imgqa 复制副本后偷偷改写。
return {
    no_vvl = false,         -- VVL 必须开：判据第 3 条就是「VVL 干净」
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
    imgui = false,          -- 开着会往像素上叠 UI
    config_reload = false,
    shader_reload = false,
    picking = false,        -- 透明本就不入 picking 段（§4），这里关是为了图逐位同基线形态
    rpc = true,             -- 🔴 承重：透明段条数的唯一读出口（只绑 127.0.0.1:8788）
                            -- 与 runtime.llm_debug=true **共存**：截图走自截，/state 只读；
                            -- 时序与实测窗口 → runtime.lua 的 llm_debug 段注释。
    -- 阴影键已迁出 → 同目录 shadow.lua 的 mode = "none"（那条在本靶是承重的，见该文件）。
    -- 键名坑的教训原样保留：这里曾写 camelCase 的 shadowPass，解析器不认 → 静默回落
    -- 默认 true，靠 frag 不采样阴影而恰好无害（两个错误互盖，12.3b 查实改对）。
}
