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
    -- ── meshlet 路径规格（★1-a2-i 定案；🔴 这里是**唯一声明点**）──────────────────
    -- 为什么要有这块：这些数原先只以字面量散在 meshlet.mesh.slang（85/128/128/44/48）与
    -- render_list_impl.cppm（×2），两侧靠注释对齐。而 `maxMeshOutputVertices` **全仓只出现在
    -- 注释里、引擎从不运行期查询** ⟹ 85 这个值挂在"设备给 256"这个未验证假设上，换台机器
    -- 就是 mesh shader 越界发射（不报错，画垃圾）。
    -- 🔴 反射**不校验** constants（`tools/generator.cppm`：「反射端无对应数据，直接由 schema
    -- 声明生成」）⟹ 单靠本表只是把 C++ 侧收成一份。真正的机器校验在
    -- `tools/checks/rules_text.py::meshlet_spec_checks()`：它扫 .slang 里的同名字面量并比对
    -- 本表，另验 3×kTaskTris ≤ kMaxOutputVertices 这条**关系**（关系错了比数值错了更致命）。
    constants = {
        -- 设备下限（🔴 取**三机最小值**，不是本机读数；引擎运行期核对，见 checkMeshletDeviceFit）。
        -- 只列**判据真的用到**的两条。`maxMeshOutputPrimitives = 256` 与
        -- `maxMeshOutputMemorySize = 32768` 也是本机读数，但判据用的是 `kTaskTris`
        -- （要求设备 ≥ 85，不是 ≥ 256）⟹ 它们是**设备事实**不是**规格要求**，留在本表会被
        -- meshlet_spec_checks 判成无消费者死常量——事实记在 docs/mdi_plan.md §17.4，别混进来。
        { name = "kMeshletMaxOutputVertices",     value = 256 },
        -- 🔴 128 = **NVIDIA 的硬上限**（`maxMeshWorkGroupInvocations`；声明 256 时 vkCreateShadersEXT
        -- 被 VVL 点名 VUID-RuntimeSpirv-MeshEXT-07295/07298 ⟹ NV 上根本建不出 shader）。本机 RADV
        -- STRIX_HALO 的 maxPreferredMeshWorkGroupInvocations 同为 128（它的 max 给到 256，但 preferred
        -- 才是"不亏性能"那一档）。★M-3T-b 起两枚 meshlet mesh shader 的 `[numthreads]` 都吃本常量
        -- （strided 循环化让覆盖集与线程数解耦 ⟹ 降档逐位不变，实测三锚全中），两侧由
        -- meshlet_spec_checks 的「数值相等」+「numthreads ≤ 本上限」两条对账钉住，别只改一侧。
        { name = "kMeshletMaxWorkGroupInvocations", value = 128 },
        -- 产物侧上限（packer 的 buildMeshlets(128v/128t) 口径；shader 侧越界即整簇判空）
        { name = "kMaxClusterVerts",  value = 128 },
        { name = "kMaxClusterPrims",  value = 128 },
        -- 发射形态：去索引 ⟹ 每任务顶点数 = 3 × 三角数 ≤ kMeshletMaxOutputVertices
        { name = "kTaskTris",         value = 85 },   -- 3×85 = 255 ≤ 256（86 要 258，越限）
        { name = "kSlotsPerCluster",  value = 2 },    -- 每簇固定 2 槽（半区号 = groupID.x & 1）
        -- 段步长（MeshletClusterHeader / ModelVertex 的 sizeof，本表值由反射逐字段布局独立钉住，
        -- 此处再钉一次是为了让 checks 能拿它跟 shader 里的字面量对）
        { name = "kClusterStride",    value = 44 },
        { name = "kVertexStride",     value = 48 },
        -- ★M-1b MDI 行表规格（docs/mdi_plan.md §19.2 D1）。🔴 C++ 侧唯一真相源 = rgl/mdi_rows.cppm
        -- 的 sizeof(MdiShadowRow) + static_assert；本表是它在 checks 射程里的第二拼写：
        -- meshlet_spec_checks 对账 schema ↔ shadow_meshlet.mesh.slang 字面量，
        -- tests/test_mdi_shadow_emit 对账本表 ↔ kMdiShadowRowStride/CommandBytes（constexpr 桥），
        -- tests/test_meshlet_shadow_args 用一次真 MDI 展开钉跨语言字节序（三处合起来 = 无第四形态）。
        -- 名字带 Bytes 后缀而**不叫 kMdiShadowRowStride**：生成 constexpr 与 mdi_rows 导出同名会撞。
        { name = "kMdiShadowRowStrideBytes",      value = 24 },  -- VUID-07088 的 stride（4 的倍数且 ≥12）
        { name = "kMdiShadowObjectIndexByteOffset", value = 12 }, -- 行 word[3]
        { name = "kMdiShadowClusterStartByteOffset", value = 16 }, -- 行 word[4]
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
