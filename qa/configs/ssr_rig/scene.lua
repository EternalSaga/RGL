-- ssr_rig 靶场景（Phase 15 批 15.0）：**让闭式倒影判据区在 SSR 关时塌成纯黑**。
--
-- 靶本体 = `python3 tools/qa_scenes.py --only ssr_rig`（几何与闭式倒影凸包都在
-- 该文件的「场景 8：ssr_rig」段，manifest 的 probe/closedForm 是判据的权威载体）。
-- 本配置只负责一件事：把反射源之外的每个亮源关掉，使「倒影区变亮」等价于「SSR 在工作」。
--
-- ⚠ 两条**非显然**的引擎事实（本靶实测踩出来的，改本文件前务必读）：
--  ① **deferred 下天穹不看 `environment.skybox`，也不乘 `intensity`**——light.frag 的
--     天空分支是 `if (env.mipRange > 0.0) return float4(envSky, 1.0)`，即天穹 = env mip0
--     **原值直出**。所以想要黑天**只能让 mipRange=0**，也就是**整段 environment 缺失**；
--     写 `skybox = false` 或 `intensity = 0` 都关不掉 deferred 的天穹（forward 的 skybox
--     pass 才吃那个键）。已记债 → `docs/phase15_plan.md` §7 债-80。
--  ② 因此本靶**不能**用 IBL 风格：`model_pbr_ibl` / `model_vegetation` / `model_light`
--     三者任一出现在 styleMap 里，`validateEnvironmentContract` 就会在 environment 缺失时
--     启动失败（这是对的——它们真会未映射采样 env）。本靶的件既无植被也无光源，
--     四个风格键全指 `model_pbr` 即可，镜面靠 metallic=1 + 方向光的 GGX specular。
--
-- 其余逐项：spin=false（判据必须与帧时长无关）、ao.strength=0（位级旁路）、
-- exposure=1.0（ACES 前中性倍率）、light.ambient=0（平环境光项归零）、shadow.mode="none"
-- （柱影会落在倒影区里，虽然只会让它更暗、不污染"变亮"判据，但没必要留着当混淆项）。
return {
    styles = {
        unlit = "model",
        pbr = "model_pbr",        -- ← 关键：非 IBL，见文件头 ②
        vegetation = "model_pbr", -- 靶无植被件；指非 env 风格以过 validateEnvironmentContract
        light = "model_pbr",      -- 同上
    },
    entities = {
        { type = "model", file = "models/ssr_rig.glb" },
    },
    -- environment 整段刻意缺失 ⟹ env.mipRange = 0 ⟹ deferred 黑天（见文件头 ①）
    light = {
        direction = { 0.5, -0.75, -0.45 },  -- 从相机侧上方照：柱的 +Z/+X 面受光
        color = { 1.0, 1.0, 1.0 },
        intensity = 12.0,
        ambient = { 0.0, 0.0, 0.0 },        -- ← 平环境光项归零
    },
    spin = false,
    cube = false,
    exposure = 1.0,
    ao = { strength = 0.0 },
}
