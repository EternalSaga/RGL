-- model_schema.lua
-- Contract definition for model.slang（Phase 2：flat unlit + position/normal/uv）
-- Used by reflection_tool to verify reflection JSON consistency
-- 和生成 model_types_generated.cppm（ModelVertex / heap slot 常量 / kTrianglesPerGroup）
-- push constant 契约归 material_schema.lua（Phase 4 统一 DrawArgs 超集，
-- material.slang 独立反射 → material_types_generated；本契约只校验 buffer slot/顶点布局/常量段）
return {
    -- Descriptor table slot definitions
    -- 模型顶点缓冲：运行时 slot = reservedSlots() + heapSlotOffset + primitiveIndex
    -- 基址 2 = cube 顶点 0 / 索引 1 之后（统一 slot 分配器连续分配，见 vkresources.cppm allocSlot）
    -- 纹理槽（Phase 3）：不带 heapSlotOffset/cppName = 运行时 allocSlot 分配（白纹理/每图一槽），
    -- 不生成 C++ 常量（tools/validator + generator 对无 offset 条目跳过常量检查）
    descriptorSlots = {
        { name = "modelVertexBuffer", index = 0, heapSlotOffset = 2, cppName = "HeapSlotModelVertexBase", shape = "byteAddressBuffer" },
        { name = "baseColorTexture", index = 1, shape = "texture2D" },
        { name = "baseColorSampler", index = 2, shape = "samplerState" },
    },

    -- 契约常量（无反射端对应，schema 声明直接生成，同 vertexLayouts 模式）：
    -- 生成的 .cppm 导出 C++ constexpr，shader 端字面量必须与本值一致
    --（见 model.mesh.slang 头注释的契约指向）。
    constants = {
        { name = "kTrianglesPerGroup", value = 10 },
    },

    -- 顶点契约：schema 声明是 source of truth（slangc 不反射顶点 buffer 结构体，
    -- 见 docs/archive/phase2_plan.md Step 0c）。顶点数组是平铺字节流，shader 按这些
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
