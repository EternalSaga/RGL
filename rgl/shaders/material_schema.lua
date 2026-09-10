-- material_schema.lua
-- Contract definition for material.slang（Phase 4：统一 push constant DrawArgs；
-- Phase 7b+ 最终形态：DrawArgs 仅 objectIndex（4B），其余字段全进 scene data per-object 表）
-- Used by reflection_tool to verify reflection JSON consistency
-- 和生成 material_types_generated.cppm（DrawArgs，static_assert 4B 钉死布局）
return {
    -- Push constant buffer definitions
    -- name matches the slang variable name, not the struct name
    pushConstants = {
        {
            name = "drawArgs",
            fields = {
                { name = "objectIndex", type = "uint" },
            },
        },
    },
}
