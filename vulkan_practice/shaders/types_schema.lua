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
    -- 布局由统一 slot 分配器（vkresources.cppm allocSlot，Phase 3）连续分配，偏移常量钉死约定
    descriptorSlots = {
        { name = "vertexBuffer", index = 0, heapSlotOffset = 0, cppName = "HeapSlotVertexOffset", shape = "byteAddressBuffer" },
        { name = "indexBuffer",  index = 1, heapSlotOffset = 1, cppName = "HeapSlotIndexOffset",  shape = "byteAddressBuffer" },
    },
}
