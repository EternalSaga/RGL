-- material_schema.lua
-- Contract definition for material.slang（Phase 4：统一 push constant 超集 DrawArgs，
-- 所有 shader 风格共享同一布局——renderListSystem 零分支单一写入路径）
-- Used by reflection_tool to verify reflection JSON consistency
-- 和生成 material_types_generated.cppm（DrawArgs，static_assert 96B 钉死布局，scalar 布局零 padding）
return {
    -- Push constant buffer definitions
    -- name matches the slang variable name, not the struct name
    pushConstants = {
        {
            name = "drawArgs",
            fields = {
                { name = "mvp", type = "matrix", rowCount = 4, columnCount = 4 },
                { name = "totalTriangles", type = "uint" },
                { name = "vertexOffset", type = "uint" },
                { name = "baseColor", type = "vec4" },
                { name = "roughnessMetallic", type = "vec2" },
            },
        },
    },
}
