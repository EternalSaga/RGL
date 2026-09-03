-- 场景配置（启动时加载，字段均可选，缺省回落默认值）
-- model 为空字符串 = 不加载模型，仅渲染 cube
-- pipelines 每条: name（管线表条目名）+ mesh/frag 的 SPV 相对路径（相对 build/vulkan_practice/）
return {
    -- model = "models/grass_variations.glb",  -- 快速排查模型（4 primitive / 1 张内嵌 PNG，全 BLEND）
    -- model = "models/castle_of_loarre.glb",  -- Loarre 城堡：11 primitive / 1 纹理，包围球 center=(-3.4,0.6,17.7) r=59.7
    model = "models/kyoto_fushimi_inari_taisha_temple.glb",  -- 伏见稻荷大社：18 primitive / 3 纹理，包围球 center≈(0,0,0) r=73.3

    -- Phase 6 批次 5：entities 列表（非空 = 权威场景构成，覆盖 model/cube 旧字段；
    -- 缺省 = 由旧字段合成，行为不变）。type = "model"（file 必填）| "cube"；
    -- position/rotation(欧拉角度 X→Y→Z)/scale(均匀) 全可选，缺省 identity
    -- entities = {
    --     { type = "model", file = "models/kyoto_fushimi_inari_taisha_temple.glb" },
    --     { type = "model", file = "models/castle_of_loarre.glb",
    --       position = { -280, 0, -180 }, rotation = { 0, 90, 0 }, scale = 2.0 },
    --     { type = "cube", position = { -120, 90, 0 }, scale = 40 },
    -- }

    -- 4K 黑屏 UAF 复验（2026-08-28）：真实 4K 纹理（非合成 TEXTURE_FORCE_SIZE）+ 目标 <2048（llm 1280x720）
    -- = §十 4K 黑屏的精确配置；master（TexturePool 生命周期已修）上复跑，黑 = 独立内核问题，正常 = §十 定性改写为 UAF
    -- model = "models/transparent.glb",  -- Phase 5 透明排序测试（无纹理，4 primitive：2 OPAQUE + 2 BLEND α=0.25）
    pipelines = {
        {
            name = "cube",
            mesh = "generated_shaders/cube.mesh.spv",
            frag = "generated_shaders/cube.frag.spv",
        },
        {
            name = "model",
            mesh = "generated_shaders/model.mesh.spv",
            frag = "generated_shaders/model.frag.spv",
        },
        {
            name = "model_pbr",
            mesh = "generated_shaders/model.mesh.spv",  -- mesh 复用 model.mesh.spv（Phase 4 批次 3）
            frag = "generated_shaders/model_pbr.frag.spv",
        },
        {
            name = "model_phong",
            mesh = "generated_shaders/model.mesh.spv",  -- Phase 5 批次 3：基础 Phong 路线，同 mesh
            frag = "generated_shaders/model_phong.frag.spv",
        },
        {
            name = "model_pbr_ibl",
            mesh = "generated_shaders/model.mesh.spv",  -- Phase 5.5：PBR + IBL 环境光照，同 mesh
            frag = "generated_shaders/model_pbr_ibl.frag.spv",
        },
        {
            name = "skybox",
            mesh = "generated_shaders/skybox.mesh.spv",  -- Phase 5.5 批次 4：全屏大三角采 env cubemap
            frag = "generated_shaders/skybox.frag.spv",
        },
    },

    -- Phase 4 批次 3：风格名→管线规格名映射（loader 输出 unlit 字段派生风格，缺失回落默认）
    -- Phase 5 批次 3：pbr 改 "model_phong" 即切基础 Phong 路线（两条光照并存）
    -- Phase 5.5：pbr 改 "model_pbr_ibl" = IBL 环境光照路线（须配 environment，否则启动契约报错）
    styles = {
        unlit = "model",
        pbr = "model_pbr_ibl",
    },

    -- Phase 5.5 环境 cubemap（缺段 = 不启用）。faces 按 Vulkan 层序 0-5 = (+X,−X,+Y,−Y,+Z,−Z)
    -- 排列，素材面顺序定案表见 docs/archive/phase5_5_plan.md（rainbow 方案 B：ft,bk,up,dn,rt,lf；
    -- nightSky 层序 = 1,3,5,6,2,4）。intensity 进 scene data @48；skybox = 背景天穹（批次 4）。
    environment = {
        dir = "skybox/rainbow",
        faces = { "rainbow_ft.png", "rainbow_bk.png", "rainbow_up.png",
                  "rainbow_dn.png", "rainbow_rt.png", "rainbow_lf.png" },
        intensity = 1.0,
        skybox = true,
    },

    -- Phase 5 批次 2：定向光（全字段可选回落 scene.cppm Light 默认值；世界空间）
    light = {
        direction = { 0.5, -0.75, -0.45 },  -- 照射方向（消费时归一化；从相机侧上方照）
        color = { 1.0, 0.98, 0.95 },
        intensity = 3.0,
        ambient = { 0.05, 0.05, 0.06 },
    },

    spin = false,  -- 场景整体时间旋转（true 便于肉眼判断渲染对错；false 静止便于排查遮挡）
    cube = false,  -- 是否渲染硬编码 cube（Phase 1 遗留，原点 ±0.5，与模型互穿时建议关掉）
}
