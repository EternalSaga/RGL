-- meshlet_cull_schema.lua
-- 逐簇剔除契约（★1-a2-i）：meshlet_cull.slang 的 MeshletCullItem + cullPush 经 C API 反射
-- 生成 C++ struct（generated/meshlet_cull_types_generated.cppm），C++ 侧 import 生成物。
--
-- 🔴 **本表是「提取清单 + 交叉校验期望值」，布局真相源 = meshlet_cull.slang 的字段声明序**。
-- reflection_tool slang 子命令拿编译器实查的 size/字段偏移/字段类型与本表逐项比对，
-- 不等即 [FAIL] 中止生成（见 tools/generator.cppm::validateSlangVertexLayout）。
-- 也就是说：这里的 stride/offset 写错**不会**生成出错位的结构体，只会拒绝生成——
-- 把它当「第二份布局」来维护是多余的，改 shader 后同步本表只为让提取清单继续指得对。
return {
    -- MeshletCullItem 是**平铺字节流条目**（GPU 侧 ByteAddressBuffer 逐条读，不是 uniform），
    -- 故走 vertexLayouts 形态（同 scene_forward_schema.lua 的 ObjectDraw / ObjectClusterRange）。
    vertexLayouts = {
        {
            name = "MeshletCullItem",
            stride = 8,
            attributes = {
                { name = "clusterIdx",  type = "uint", offset = 0 },
                { name = "objectSlot",  type = "uint", offset = 4 },
            },
        },
        -- 簇头运行时视图（★1-a2-i）。offset 逐条来自 meshlet_cull.slang 的声明序；
        -- 写错只会让 reflection_tool 报 [FAIL] 拒生成（API 是实查真值），不会生成错位结构体。
        -- 44B 与 packer 的 `format.h::DiskCluster` 一致，但两侧刻意不共享代码（理由见 slang 侧注释）。
        {
            name = "MeshletClusterHeader",
            stride = 44,
            attributes = {
                { name = "group",       type = "uint",  offset = 0 },
                { name = "refined",     type = "int",   offset = 4 },
                { name = "triCount",    type = "uint",  offset = 8 },
                { name = "vertCount",   type = "uint",  offset = 12 },
                { name = "triOffset",   type = "uint",  offset = 16 },
                { name = "remapOffset", type = "uint",  offset = 20 },
                { name = "center",      type = "vec3",  offset = 24 },
                { name = "radius",      type = "float", offset = 36 },
                { name = "error",       type = "float", offset = 40 },
            },
        },
    },
    -- name = slang 的 **push 变量名**（cullPush），不是 struct 名（material_schema.lua 同规矩）
    pushConstants = {
        {
            name = "cullPush",
            fields = {
                -- "T[N]" 数组形态 = schema_loader.cppm::vertexAttrCppType 的数组分支，
                -- 与 slang_reflect.cppm::typeToCppType 的 Kind::Array 分支产出同一个
                -- std::array<glm::vec4, 6>；本表只声明字段名（push 侧偏移/尺寸取 API 真值）
                { name = "planes",    type = "vec4[6]" },
                { name = "itemCount", type = "uint" },
            },
        },
    },
}
