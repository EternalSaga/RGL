-- 场景配置（启动时加载，字段均可选，缺省回落默认值）
-- model 为空字符串 = 不加载模型，仅渲染 cube
-- pipelines 每条: name（管线表条目名）+ mesh/frag 的 SPV 相对路径（相对 build/vulkan_practice/）
return {
    -- model = "models/grass_variations.glb",  -- 快速排查模型（4 primitive / 1 张内嵌 PNG，全 BLEND）
    model = "models/realistic_wooden_cottage_garden_house_3d_model.glb",  -- glTF 模型路径（Phase 3 测试模型，392 primitive / 11 张 4K 内嵌纹理）
    -- 4K 黑屏 UAF 复验（2026-08-28）：真实 4K 纹理（非合成 TEXTURE_FORCE_SIZE）+ 目标 <2048（llm 1280x720）
    -- = §十 4K 黑屏的精确配置；master（TexturePool 生命周期已修）上复跑，黑 = 独立内核问题，正常 = §十 定性改写为 UAF
    -- model = "models/transparent.glb",  -- Phase 2 回归基线（无纹理，4 primitive）

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
    },

    -- Phase 4 批次 3：风格名→管线规格名映射（loader 输出 unlit 字段派生风格，缺失回落默认）
    styles = {
        unlit = "model",
        pbr = "model_pbr",
    },

    spin = false,  -- 场景整体时间旋转（true 便于肉眼判断渲染对错；false 静止便于排查遮挡）
    cube = false,  -- 是否渲染硬编码 cube（Phase 1 遗留，原点 ±0.5，与模型互穿时建议关掉）
}
