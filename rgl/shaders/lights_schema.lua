-- lights_schema.lua
-- Phase 7 D3：多灯数据契约（第 4 条反射管线）。lights.slang + 本 schema → lights_types_generated.cppm
--（GpuLight struct + kMaxLights / kGpuLightStride / kSceneDataLightArrayBase 常量）。
-- GpuLight 经 vertexLayouts 机制声明（slangc 不反射非资源 struct，同 ModelVertex 先例）；
-- shader 端字面量（lights.slang kMaxLights / kGpuLightStride / kSceneDataLightArrayBase）必须与本值一致。
return {
    -- lights.slang import scene → 反射带 sceneData（binding 3，ByteAddressBuffer）。
    -- validator 反向校验要求反射里每个 binding 都在 schema 登记（无 heapSlotOffset = 运行时槽，不生成常量）。
    descriptorSlots = {
        { name = "sceneData", index = 3, shape = "byteAddressBuffer" },
    },

    -- 契约常量（shader 端字面量必须一致；kSceneDataLightArrayBase = 头 128 + 矩阵表 4096*64）
    constants = {
        { name = "kMaxLights", value = 8 },
        { name = "kGpuLightStride", value = 64 },
        { name = "kSceneDataLightArrayBase", value = 262272 },
    },

    -- GpuLight struct（stride 64，4×vec4 packed；生成 C++ struct + static_assert(sizeof==64)）
    vertexLayouts = {
        {
            name = "GpuLight",
            stride = 64,
            attributes = {
                { name = "direction", type = "vec4", offset = 0 },
                { name = "position",  type = "vec4", offset = 16 },
                { name = "color",     type = "vec4", offset = 32 },
                { name = "params",    type = "vec4", offset = 48 },  -- .x=range .y=type(0 dir/1 point)
            },
        },
    },
}
