-- types_schema.lua
-- Contract definition for types.slang
-- Used by schema_validator to verify reflection JSON consistency
return {
    -- push constant 契约归 material_schema.lua（Phase 4 统一 DrawArgs 超集，
    -- material.slang 独立反射 → material_types_generated；本契约只校验 buffer slot 段）

    -- Descriptor table slot definitions
    -- index: slang binding index (validated against reflection JSON)
    -- heapSlotOffset: offset from the start of the heap reserved range (runtime slot = reservedSlots() + heapSlotOffset)
    -- cppName: C++ constant name to export in the generated .cppm
    --
    -- 架构债务 1（方案 B，2026-09-08）：cube 不再是资源内核（Resources）构造期硬编码资产，
    -- 改由 builtin_geometry 在 A1 几何段与普通模型同路上传 → 顶点/索引槽 = **运行时槽**
    --（allocSlot 单调分配，计入 AssetTable::geometrySlotCount）。本表因此不再登记
    -- heapSlotOffset/cppName，只保留 binding index + shape 校验（types.slang 契约仍是
    -- cube.mesh.slang 的两个 ByteAddressBuffer 声明的单一真相源）。
    -- 几何区起点常量见 model_schema.lua 的 HeapSlotGeometryBase（用户槽区首槽 = 几何）。
    descriptorSlots = {
        { name = "vertexBuffer", index = 0, shape = "byteAddressBuffer" },
        { name = "indexBuffer",  index = 1, shape = "byteAddressBuffer" },
    },
}
