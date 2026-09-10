-- scene_forward_schema.lua
-- 前向头契约（forward 单灯头 @0-48）：scene.slang 的 SceneLight struct 经 C API 反射
-- 生成 C++ struct（generated/scene_forward_types_generated.cppm），替换 vkresources.cppm
-- 原手写 LightDataGpu（双源消除，见 docs/slang_api_reflection_research.md）。
-- SceneLight = scene data 头 @0-48（forward 消费，shader 端 readSceneLight Load3 读）。
return {
    -- 前向头 struct（C API 实查布局，逐字段 offsetof static_assert；schema 降为提取清单 +
    -- 交叉校验期望值——shader 端 SceneLight 字段/顺序是布局真相源，本表须与其一致）
    vertexLayouts = {
        {
            name = "SceneLight",
            stride = 48,
            attributes = {
                { name = "direction", type = "vec3", offset = 0 },
                { name = "eyePos",    type = "vec3", offset = 12 },
                { name = "color",     type = "vec3", offset = 24 },
                { name = "ambient",   type = "vec3", offset = 36 },
            },
        },
    },
}
