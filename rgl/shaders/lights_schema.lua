-- lights_schema.lua
-- Phase 7 D3：多灯数据契约（第 4 条反射管线）。lights.slang + 本 schema → lights_types_generated.cppm
--（GpuLight / LightArrayBlock struct + kMaxLights / kGpuLightStride / kSceneDataLightArrayBase 常量）。
-- 契约 struct 经 vertexLayouts 机制声明（slangc 不反射非资源 struct，同 ModelVertex 先例）。
return {
    -- 迁移批 3：lights.slang 不再 import scene（灯数组字节读迁到 binding 24 的类型化载体），
    -- 反射全局参数 = lightArray（ConstantBuffer<LightArrayBlock>，运行时槽：无
    -- heapSlotOffset = 不生成 C++ 常量，槽号由 SceneDataService::lightSlot 运行期领取）。
    descriptorSlots = {
        { name = "lightArray", index = 24, shape = "constantBuffer" },
    },

    -- 契约常量（C++ 侧字面量必须一致）。迁移批 3 起 shader 端只剩 kMaxLights 一个消费者
    --（LightArrayBlock.lights 的数组长度）；kGpuLightStride / kSceneDataLightArrayBase 的
    -- shader 字面量已清零，两常量转为 scene_data.cppm 派生式 static_assert +
    -- tests/test_reflection_layout 独立重推导的锚（矩阵容量/头尺寸单边改动 = 本表与
    -- scene_data 公式两侧对账抓，测试侧第三处独立重算）。
    constants = {
        { name = "kMaxLights", value = 8 },
        { name = "kGpuLightStride", value = 64 },
        { name = "kSceneDataLightArrayBase", value = 262656 },
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
        -- 迁移批 3（§9.5 binding 24）：灯数组区类型化载体（lights.slang LightArrayBlock，
        -- ConstantBuffer<LightArrayBlock> 经子范围描述符指向 scene data 灯数组区
        -- kSceneDataLightArrayOffset，528B）。**嵌套 struct 数组首个消费者**（lights 字段 =
        -- std::array<GpuLight, 8>）——批 R 嵌套反射的实战阳性锚。
        -- 🔴 本项必须排在 GpuLight **之后**：发射顺序 = 本名单书写顺序，被依赖者先发射
        --（排错了 generateCppmFromSlang [FAIL] 点名顺序，不会生成错序的编译不过的产物）。
        {
            name = "LightArrayBlock",
            stride = 528,
            attributes = {
                { name = "count",  type = "uint",        offset = 0 },
                { name = "pad",    type = "uint[3]",     offset = 4 },
                { name = "lights", type = "GpuLight[8]", offset = 16 },
            },
        },
    },
}
