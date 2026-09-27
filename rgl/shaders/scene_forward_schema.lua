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
        -- Phase 10 批 10.2a 单源化：per-object 材质/几何表条目（scene.slang ObjectDraw，
        -- 表基址 @524944 stride 48，见 scene_data.cppm kSceneDataDrawTableOffset）。
        -- 生成 C++ struct 替换手写 ObjectDrawGpu（双源消除）；字段声明序 = 内存序
        --（scalar layout 0/4/8/16/32 无 padding，shader 端 Load<ObjectDraw> 整读）。
        {
            name = "ObjectDraw",
            stride = 48,
            attributes = {
                { name = "totalTriangles",    type = "uint", offset = 0 },
                { name = "vertexOffset",      type = "uint", offset = 4 },
                { name = "roughnessMetallic", type = "vec2", offset = 8 },
                { name = "baseColor",         type = "vec4", offset = 16 },
                { name = "emissive",          type = "vec4", offset = 32 },
            },
        },
    },
}
