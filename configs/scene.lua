-- 场景配置（启动时加载，字段均可选，缺省回落默认值）
-- model 为空字符串 = 不加载模型，仅渲染 cube
-- pipelines 每条: name（管线表条目名）+ mesh/frag 的 SPV 相对路径（相对 build/vulkan_practice/）
return {
    model = "models/transparent.glb",  -- glTF 模型路径（Phase 2 测试模型）

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
    },

    spin = false,  -- 场景整体时间旋转（true 便于肉眼判断渲染对错；false 静止便于排查遮挡）
    cube = false,  -- 是否渲染硬编码 cube（Phase 1 遗留，原点 ±0.5，与模型互穿时建议关掉）
}
