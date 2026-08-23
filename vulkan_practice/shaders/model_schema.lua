-- model_schema.lua
-- Contract definition for model.slang（Phase 2：flat unlit + position/normal/uv）
-- Used by reflection_tool to verify reflection JSON consistency
-- 和生成 model_types_generated.cppm（ModelDrawArgs / ModelVertex / heap slot 常量）
return {
    -- Push constant buffer definitions
    -- name matches the slang variable name, not the struct name
    pushConstants = {
        {
            name = "modelDrawArgs",
            fields = {
                { name = "mvp", type = "matrix", rowCount = 4, columnCount = 4 },
                { name = "totalTriangles", type = "uint" },
                { name = "baseColor", type = "vec4" },
            },
        },
    },

    -- Descriptor table slot definitions
    -- 模型顶点缓冲：运行时 slot = reservedSlots() + heapSlotOffset + primitiveIndex
    -- 基址 8 避开 cube 占用的 0（顶点）与 4（索引）
    descriptorSlots = {
        { name = "modelVertexBuffer", index = 0, heapSlotOffset = 8, cppName = "HeapSlotModelVertexBase", shape = "byteAddressBuffer" },
    },

    -- 顶点契约：schema 声明是 source of truth（slangc 不反射顶点 buffer 结构体，
    -- 见 docs/phase2_plan.md Step 0c）。顶点数组是平铺字节流，shader 按这些
    -- 偏移用 ByteAddressBuffer 逐字段读取（见 model.mesh.slang 的注释）。
    vertexLayouts = {
        {
            name = "ModelVertex",
            stride = 32,
            attributes = {
                { name = "position", type = "vec3", offset = 0 },
                { name = "normal",   type = "vec3", offset = 12 },
                { name = "uv",       type = "vec2", offset = 24 },
            },
        },
    },
}
