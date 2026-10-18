-- ssr_rig 靶场景（Phase 15 批 15.0）：**让闭式倒影判据区在 SSR 关时塌成纯黑**。
--
-- 靶本体 = `python3 tools/qa_scenes.py --only ssr_rig`（几何与闭式倒影凸包都在
-- 该文件的「场景 8：ssr_rig」段，manifest 的 probe/closedForm 是判据的权威载体）。
-- 本配置只负责一件事：把反射源之外的每个亮源关掉，使「倒影区变亮」等价于「SSR 在工作」。
--
-- ⚠ 两条**非显然**的引擎事实（本靶实测踩出来的，改本文件前务必读）：
--  ① 黑天靠**整段 environment 缺失**（env.mipRange = 0 ⟹ light.frag 天穹分支不进）。
--     2026-10-18 探针重立起 `intensity = 0` 也能关天穹（sky 分支门控 mipRange>0 &&
--     intensity>0，docs/图变体收敛_plan.md §6.2）；本靶仍用「整段缺失」形态——更硬，
--     连 IBL 漫反射/镜面项一起归零。原 `environment.skybox` 键与 forward skybox pass
--     已随刀②.2 删除（键出现 = 未知键 warn；deferred 的天穹总闸 = intensity）。
--  ② forward 的 IBL 风格契约检查（validateEnvironmentContract：model_pbr_ibl 等风格
--     在 environment 缺失时启动失败）与 styles 表同随刀②.2 删除；deferred 路 env
--     未上传 = light 条目占位槽 + shader 端 mipRange=0 恒不采样，无「声明未映射」形态。
--     本靶镜面靠 metallic=1 + 方向光的 GGX specular（deferred_geometry 写 G-buffer）。
--
-- 其余逐项：spin=false（判据必须与帧时长无关）、ao.strength=0（位级旁路）、
-- exposure=1.0（ACES 前中性倍率）、light.ambient=0（平环境光项归零）、shadow.mode="none"
-- （柱影会落在倒影区里，虽然只会让它更暗、不污染"变亮"判据，但没必要留着当混淆项）。
return {
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
