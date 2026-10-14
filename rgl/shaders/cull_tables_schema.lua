-- cull_tables_schema.lua
-- GPU culling 三张 buffer 的元素契约（cull_tables.slang 的 CullEntry / CullSequence / CullCount）
-- 经 C API 反射生成 C++ struct（generated/cull_tables_types_generated.cppm），两侧共用一份布局。
--
-- 🔴 **本表是「提取清单 + 交叉校验期望值」，布局真相源 = cull_tables.slang 的字段声明序**。
-- reflection_tool slang 子命令拿编译器实查的 size/字段偏移/字段类型与本表逐项比对，不等即
-- [FAIL] 中止生成（tools/generator.cppm::validateSlangVertexLayout）。也就是说：这里的
-- stride/offset 写错**不会**生成出错位的结构体，只会拒绝生成——把它当「第二份布局」维护是
-- 多余的，改 shader 后同步本表只为让提取清单继续指得对。
--
-- 这一条交叉校验就是本刀的**变异拦截点**：改 shader 字段类型（uint→float）或换序而不同步本表
-- ⟹ just reflect 直接 [FAIL]，两侧不可能再各抄各的。
--
-- 期望值（60 / 0,12,16,24 与 120 / 0,4,8,108）的**独立**判据在 tests/test_cull_tables_layout.cppm
-- ——那份不抄本表也不抄生成物，出处是迁移前人工核对过的字节布局。
return {
    vertexLayouts = {
        -- 静态候选表条目：60B，逐字节等于迁移前 gpu_cull.cppm::CullEntry（sizeof 断言 60）。
        -- 中心是三个标量而非 vec3 —— 理由见 cull_tables.slang 文件头【中心】。
        {
            name = "CullEntry",
            stride = 60,
            attributes = {
                { name = "centerX",          type = "float", offset = 0 },
                { name = "centerY",          type = "float", offset = 4 },
                { name = "centerZ",          type = "float", offset = 8 },
                { name = "radius",           type = "float", offset = 12 },
                { name = "objectSlot",       type = "uint",  offset = 16 },
                { name = "pipelineIndex",    type = "uint",  offset = 20 },
                { name = "groupCount",       type = "uint",  offset = 24 },
                { name = "totalTriangles",   type = "uint",  offset = 28 },
                { name = "vertexOffset",     type = "uint",  offset = 32 },
                { name = "baseColor",        type = "vec4",  offset = 36 },
                { name = "roughnessMetallic",type = "vec2",  offset = 52 },
            },
        },
        -- 输出序列行：120B = execSlot 2×u32（@0/@4）+ DrawArgs 槽区 100B（@8 起 4B + 96B 保留
        -- 宽度）+ VkDrawMeshTasksIndirectCommandEXT 12B（@108）。≤ RADV maxIndirectCommandsIndirectStride 2048。
        {
            name = "CullSequence",
            stride = 120,
            attributes = {
                { name = "execSlotFrag",   type = "uint",      offset = 0 },
                { name = "execSlotMesh",   type = "uint",      offset = 4 },
                { name = "drawArgs",       type = "uint",      offset = 8 },
                { name = "drawArgsSlack",  type = "uint[24]",  offset = 12 },
                { name = "meshCmdX",       type = "uint",      offset = 108 },
                { name = "meshCmdY",       type = "uint",      offset = 112 },
                { name = "meshCmdZ",       type = "uint",      offset = 116 },
            },
        },
        -- 序列计数区元素：8B（buffer 本体 64B，余量留白 = 迁移前 kCullCountBytes，本刀不动）。
        {
            name = "CullCount",
            stride = 8,
            attributes = {
                { name = "reserved", type = "uint", offset = 0 },
                { name = "total",    type = "uint", offset = 4 },
            },
        },
    },
}
