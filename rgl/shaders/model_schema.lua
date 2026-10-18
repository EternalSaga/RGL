-- model_schema.lua
-- Contract definition for model.slang（Phase 2：flat unlit + position/normal/uv）
-- Used by reflection_tool to verify reflection JSON consistency
-- 和生成 model_types_generated.cppm（ModelVertex / heap slot 常量 / kTrianglesPerGroup）
-- push constant 契约归 material_schema.lua（Phase 4 统一 DrawArgs 超集，
-- material.slang 独立反射 → material_types_generated；本契约只校验 buffer slot/顶点布局/常量段）
return {
    -- Descriptor table slot definitions
    -- 几何区基址：用户槽区（reserved 之后）第一个槽 = 第一个几何槽，几何 = 内建几何
    -- （cube 顶点/索引）+ 每模型一个的拼接顶点 buffer（架构债务 1 方案 B 后无"cube 占位 0/1"
    -- 语义——基址与内容无关，几何按上传序连续排布，计数与漂移校验见 AssetTable::allocGeometrySlot）
    -- 纹理槽（Phase 3）/ scene data / RT 采样槽：不带 heapSlotOffset/cppName = 运行时 allocSlot
    -- 分配（白纹理/每图一槽），不生成 C++ 常量（tools/validator + generator 对无 offset 条目跳过）
    descriptorSlots = {
        { name = "modelVertexBuffer", index = 0, heapSlotOffset = 0, cppName = "HeapSlotGeometryBase", shape = "byteAddressBuffer" },
        { name = "baseColorTexture", index = 1, shape = "texture2D" },
        { name = "baseColorSampler", index = 2, shape = "samplerState" },
        -- Phase 8 批次 ③a：normal 贴图（DirectX 约定，flip-G 解码见 normalmap.slang）
        -- + ORM 贴图（R=Occlusion/G=Roughness/B=Metalness），运行时 allocSlot（同
        -- baseColorTexture，无 heapSlotOffset/cppName 不生成 C++ 常量）
        { name = "normalTexture", index = 6, shape = "texture2D" },
        { name = "ormTexture", index = 7, shape = "texture2D" },
        -- Phase 8 批次 ③：emissive 贴图（binding 8，运行时 allocSlot，同其它纹理；无
        -- heapSlotOffset/cppName 不生成 C++ 常量；复用 baseColorSampler，无独立 sampler 槽）
        { name = "emissiveTexture", index = 8, shape = "texture2D" },
    },

    -- 契约常量（无反射端对应，schema 声明直接生成，同 vertexLayouts 模式）：
    -- 生成的 .cppm 导出 C++ constexpr，shader 端字面量必须与本值一致
    --（见 model.mesh.slang 头注释的契约指向）。
    constants = {
        { name = "kTrianglesPerGroup", value = 10 },
    },

    -- 顶点契约：schema 声明是 source of truth（slangc 不反射顶点 buffer 结构体，
    -- 见 git:phase2_plan.md Step 0c）。顶点数组是平铺字节流，shader 按这些
    -- 偏移用 ByteAddressBuffer 逐字段读取（见 model.mesh.slang 的注释）。
    vertexLayouts = {
        {
            -- Phase 8（方案 A）：tangent vec4 @32 —— 切线 xyz + 手性 w（loader 按 uv
            -- Jacobian 符号逐三角形推）。bistro 由 bistro_pack.py CPU 烘 Lengyel 平滑
            -- 切线（glTF core TANGENT）；无切线资产 loader 以 Frisvad 基兜底（w=+1）。
            name = "ModelVertex",
            stride = 48,
            attributes = {
                { name = "position", type = "vec3", offset = 0 },
                { name = "normal",   type = "vec3", offset = 12 },
                { name = "uv",       type = "vec2", offset = 24 },
                { name = "tangent",  type = "vec4", offset = 32 },
            },
        },
    },
}
