-- types_schema.lua
-- Contract definition for types.slang
-- Used by schema_validator to verify reflection JSON consistency
return {
    -- Push constant buffer definitions
    -- name matches the slang variable name, not the struct name
    pushConstants = {
        {
            name = "drawArgs",
            fields = {
                { name = "mvp", type = "matrix", rowCount = 4, columnCount = 4 },
            },
        },
    },

    -- Descriptor table slot definitions
    -- index: slang binding index (validated against reflection JSON)
    -- heapSlotOffset: offset from the start of the heap reserved range (runtime slot = reservedSlots() + heapSlotOffset)
    -- cppName: C++ constant name to export in the generated .cppm
    descriptorSlots = {
        { name = "vertexBuffer", index = 0, heapSlotOffset = 0, cppName = "HeapSlotVertexOffset", shape = "byteAddressBuffer" },
        { name = "indexBuffer",  index = 1, heapSlotOffset = 4, cppName = "HeapSlotIndexOffset",  shape = "byteAddressBuffer" },
    },
}
