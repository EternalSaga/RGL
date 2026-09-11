-- 场景配置（启动时加载，字段均可选，缺省回落默认值）
-- model 为空字符串 = 不加载模型，仅渲染 cube
-- pipelines 每条: name（管线表条目名）+ mesh/frag 的 SPV 相对路径（相对 build/rgl/）
return {
    -- model = "models/grass_variations.glb",  -- 快速排查模型（4 primitive / 1 张内嵌 PNG，全 BLEND）
    -- model = "models/castle_of_loarre.glb",  -- Loarre 城堡：11 primitive / 1 纹理，包围球 center=(-3.4,0.6,17.7) r=59.7
    -- model = "models/kyoto_fushimi_inari_taisha_temple.glb",  -- 伏见稻荷大社：18 primitive / 3 纹理，包围球 center≈(0,0,0) r=73.3
    model = "models/bistro_exterior.glb",  -- Phase 7 D1 测试主场景：2.8M 三角 / 1591 primitive / 395 纹理

    -- Phase 6 批次 5：entities 列表（非空 = 权威场景构成，覆盖 model/cube 旧字段；
    -- 缺省 = 由旧字段合成，行为不变）。type = "model"（file 必填）| "cube"；
    -- position/rotation(欧拉角度 X→Y→Z)/scale(均匀) 全可选，缺省 identity
    entities = {
        { type = "model", file = "models/bistro_exterior.glb", scale = 0.01 },
    },

    -- Phase 6 批次 5：entities 列表（非空 = 权威场景构成，覆盖 model/cube 旧字段；
    -- 缺省 = 由旧字段合成，行为不变）。type = "model"（file 必填）| "cube"；
    -- position/rotation(欧拉角度 X→Y→Z)/scale(均匀) 全可选，缺省 identity
    -- entities = {
    --     { type = "model", file = "models/kyoto_fushimi_inari_taisha_temple.glb" },
    --     { type = "model", file = "models/grass_variations.glb", position = { 47.3, 0, -169.3 }, rotation = { 0, 174.6, 0 }, scale = 0.32 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 163.0, 0, 0.5 }, rotation = { 0, 169.3, 0 }, scale = 0.44 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -119.9, 0, 122.7 }, rotation = { 0, 98.2, 0 }, scale = 0.45 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -18.1, 0, -142.0 }, rotation = { 0, 193.8, 0 }, scale = 0.42 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 54.1, 0, 144.5 }, rotation = { 0, 289.8, 0 }, scale = 0.32 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 54.2, 0, -155.7 }, rotation = { 0, 303.9, 0 }, scale = 0.33 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 145.1, 0, 96.1 }, rotation = { 0, 289.7, 0 }, scale = 0.36 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 104.6, 0, 69.9 }, rotation = { 0, 228.6, 0 }, scale = 0.32 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 149.8, 0, -47.2 }, rotation = { 0, 72.3, 0 }, scale = 0.41 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -118.0, 0, 142.0 }, rotation = { 0, 327.4, 0 }, scale = 0.38 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -102.0, 0, -130.8 }, rotation = { 0, 290.0, 0 }, scale = 0.49 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 136.7, 0, 24.7 }, rotation = { 0, 216.6, 0 }, scale = 0.33 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -99.3, 0, -62.3 }, rotation = { 0, 316.8, 0 }, scale = 0.38 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 121.0, 0, 109.2 }, rotation = { 0, 112.4, 0 }, scale = 0.3 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -120.4, 0, 12.3 }, rotation = { 0, 75.6, 0 }, scale = 0.47 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -35.5, 0, -105.2 }, rotation = { 0, 280.1, 0 }, scale = 0.25 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -90.8, 0, 158.2 }, rotation = { 0, 223.3, 0 }, scale = 0.33 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -101.3, 0, 98.3 }, rotation = { 0, 45.7, 0 }, scale = 0.5 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 135.7, 0, 47.7 }, rotation = { 0, 268.7, 0 }, scale = 0.35 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 128.7, 0, -15.5 }, rotation = { 0, 352.5, 0 }, scale = 0.47 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -87.1, 0, -158.9 }, rotation = { 0, 161.0, 0 }, scale = 0.46 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 43.2, 0, -145.0 }, rotation = { 0, 169.5, 0 }, scale = 0.43 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -98.3, 0, 63.2 }, rotation = { 0, 268.7, 0 }, scale = 0.32 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -172.2, 0, 13.4 }, rotation = { 0, 62.0, 0 }, scale = 0.26 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 112.2, 0, 110.1 }, rotation = { 0, 130.0, 0 }, scale = 0.31 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -175.7, 0, 55.2 }, rotation = { 0, 91.7, 0 }, scale = 0.35 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 115.4, 0, -109.3 }, rotation = { 0, 96.5, 0 }, scale = 0.37 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -120.3, 0, 16.3 }, rotation = { 0, 135.9, 0 }, scale = 0.33 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 45.8, 0, -127.9 }, rotation = { 0, 237.3, 0 }, scale = 0.49 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 124.0, 0, -65.1 }, rotation = { 0, 217.1, 0 }, scale = 0.33 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -119.6, 0, 88.9 }, rotation = { 0, 285.6, 0 }, scale = 0.48 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -28.0, 0, 106.9 }, rotation = { 0, 161.8, 0 }, scale = 0.26 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 168.8, 0, 32.3 }, rotation = { 0, 40.4, 0 }, scale = 0.43 },
    -- { type = "model", file = "models/grass_variations.glb", position = { -119.8, 0, 141.1 }, rotation = { 0, 307.3, 0 }, scale = 0.27 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 65.5, 0, 173.9 }, rotation = { 0, 220.8, 0 }, scale = 0.36 },
    -- { type = "model", file = "models/grass_variations.glb", position = { 35.3, 0, -171.5 }, rotation = { 0, 177.0, 0 }, scale = 0.37 },
    -- },

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
    -- Phase 8 批次 ②b：hdr 非空 = 优先走 Radiance .hdr 环境（CPU 等距柱状→RGBA16F cubemap
    -- 重投影，官方 Bistro 素材 san_giuseppe_bridge）；素材 gitignore，文件缺失自动告警回落
    -- faces PNG 路径。hdr_size = 重投影面边长（2 的幂，默认 2048）。
    -- intensity：官方 Falcor 配方为 10，但那是对其 irradiance 卷积归一后的校准值；本工程
    -- 简化 IBL 直接采 cubemap top-mip 平均（env 线性均值本就 ≈0.56），×10 必过曝。实测
    -- 1.0 时 forward/deferred 建筑与背景曝光正常（docs/phase8_plan.md ②b/③a 记录）。
    environment = {
        hdr = "env/san_giuseppe_bridge_4k.hdr",
        hdr_size = 2048,
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
        intensity = 4.0,
        ambient = { 0.25, 0.25, 0.28 },
    },

    spin = false,  -- 场景整体时间旋转（true 便于肉眼判断渲染对错；false 静止便于排查遮挡）
    cube = false,  -- 是否渲染硬编码 cube（Phase 1 遗留，原点 ±0.5，与模型互穿时建议关掉）
}
