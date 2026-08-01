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
    -- heapSlot: descriptor heap slot index (used for code generation)
    -- cppName: C++ constant name to export in the generated .cppm
    descriptorSlots = {
        { name = "vertexBuffer", index = 0, heapSlot = 8,  cppName = "HeapSlotVertex",  shape = "byteAddressBuffer" },
        { name = "indexBuffer",  index = 1, heapSlot = 12, cppName = "HeapSlotIndex",   shape = "byteAddressBuffer" },
    },
}
