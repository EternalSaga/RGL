-- scene_forward_schema.lua
-- scene data 头契约（单灯头 @0-48）：scene.slang 的 SceneLight struct 经 C API 反射
-- 生成 C++ struct（generated/scene_forward_types_generated.cppm），替换 vkresources.cppm
-- 原手写 LightDataGpu（双源消除，见 docs/archive/slang_api_reflection_research.md）。
-- SceneLight = scene data 头 @0-48（shader 端 readSceneLight 消费，取共享 eyePos/ambient）。
return {
    -- 前向头 struct（C API 实查布局，逐字段 offsetof static_assert；schema 降为提取清单 +
    -- 交叉校验期望值——shader 端 SceneLight 字段/顺序是布局真相源，本表须与其一致）
    vertexLayouts = {
        {
            name = "SceneLight",
            stride = 48,
            attributes = {
                { name = "direction", type = "vec3", offset = 0 },
                { name = "eyePos",    type = "vec3", offset = 12 },
                { name = "color",     type = "vec3", offset = 24 },
                { name = "ambient",   type = "vec3", offset = 36 },
            },
        },
        -- Phase 10 批 10.2a 单源化：per-object 材质/几何表条目（scene.slang ObjectDraw，
        -- 表基址 @525824 stride 48，见 scene_data.cppm kSceneDataDrawTableOffset）。
        -- 生成 C++ struct 替换手写 ObjectDrawGpu（双源消除）；字段声明序 = 内存序
        --（scalar layout 0/4/8/16/32 无 padding，shader 端 Load<ObjectDraw> 整读）。
        {
            name = "ObjectDraw",
            stride = 48,
            attributes = {
                { name = "totalTriangles",    type = "uint", offset = 0 },
                { name = "vertexOffset",      type = "uint", offset = 4 },
                { name = "roughnessMetallic", type = "vec2", offset = 8 },
                { name = "baseColor",         type = "vec4", offset = 16 },
                { name = "emissive",          type = "vec4", offset = 32 },
            },
        },
        -- 簇化轴 ★1-a1i：per-object 簇区间表条目（scene.slang ObjectClusterRange，
        -- 表基址 @724224 stride 16，见 scene_data.cppm kSceneDataClusterRangeOffset）。
        -- 方案 B（docs/mdi_plan.md §10.1）：**新增独立表**而非给 ObjectDraw 加字段——
        -- 现有结构逐字节不动 ⟹「门关逐位不变」是构造性成立，不是靠"没人读新字段"。
        -- ★1-a2-i：第 4 字段由补齐位 `reserved` 转正为 `cullBase`（逐簇剔除标志数组的
        -- 本 object 首项下标）——口径与「为什么不能用 base+簇内序 当下标」见 scene.slang。
        {
            name = "ObjectClusterRange",
            stride = 16,
            attributes = {
                { name = "base",      type = "uint", offset = 0 },
                { name = "count",     type = "uint", offset = 4 },
                { name = "l0Count",   type = "uint", offset = 8 },
                { name = "cullBase",  type = "uint", offset = 12 },
            },
        },
        -- 迁移批 1（§9.5 binding 32）：湿面区类型化载体（scene.slang WetParamsBlock，
        -- ConstantBuffer<WetParamsBlock> 经子范围描述符指向 scene data 湿面区
        -- kSceneDataWetOffset，32B = 2×vec4）。字段语义 → scene.slang 湿面区注释。
        {
            name = "WetParamsBlock",
            stride = 32,
            attributes = {
                { name = "params",  type = "vec4", offset = 0 },
                { name = "spatial", type = "vec4", offset = 16 },
            },
        },
        -- 批 R（§9.4 推论）：ObjectMatrix 惯用法登记为**契约类型**（scene.slang struct，
        -- 列主序 4×float4，stride 64）。此前它只是 shader-local——契约里的矩阵禁止
        -- float4x4 字段（Slang 对 float4x4 成员发 RowMajor，m[0] ≠ 内存列 0，§9.4 实测），
        -- 矩阵进 UBO 契约只能靠它**作为字段嵌进**别的契约 struct（SceneHeader.skyInvVP /
        -- TAA 两份矩阵 / SSR.viewProj / 阴影 VP 数组），前提是它是可反射的契约类型。
        -- 副产品：R3 契约集合机械取自生成物 ⟹ 本 struct 一进，StructuredBuffer<ObjectMatrix>
        -- 自动放行（后续 MVP 表/对象矩阵表批直接受益）。
        {
            name = "ObjectMatrix",
            stride = 64,
            attributes = {
                { name = "c0", type = "vec4", offset = 0 },
                { name = "c1", type = "vec4", offset = 16 },
                { name = "c2", type = "vec4", offset = 32 },
                { name = "c3", type = "vec4", offset = 48 },
            },
        },
        -- 迁移批 2（§9.5 binding 27）：辐照度区类型化载体（scene.slang IrradianceBlock，
        -- ConstantBuffer<IrradianceBlock> 经子范围描述符指向 scene data 辐照度区
        -- kSceneDataIrradianceOffset，144B = 9×vec4，.xyz=系数 .w 留白）。
        -- 数组字段形态首个消费者：typeToCppType 的 Kind::Array 分支产出 std::array<glm::vec4, 9>。
        {
            name = "IrradianceBlock",
            stride = 144,
            attributes = {
                { name = "sh", type = "vec4[9]", offset = 0 },
            },
        },
        -- 迁移批 S-1（§9.5 binding 28）：阴影光空间 VP 区类型化载体（scene.slang
        -- ShadowViewProjBlock，ConstantBuffer<ShadowViewProjBlock> 经子范围描述符指向 scene
        -- data 阴影 VP 区 kSceneDataShadowMatrixOffset，256B = 4 槽 × ObjectMatrix）。
        -- **契约 struct 的数组字段**首个消费者（批 R 登记的 ObjectMatrix 在此被嵌进契约，
        -- 上面那条注释列的四个用途之一）：typeToCppType 的 Kind::Array 分支取元素 struct 名
        -- → std::array<ObjectMatrix, 4>，与 schema 侧 vertexAttrCppType 的 "T[N]" 分支同源。
        -- 依赖方向：必须排在 ObjectMatrix 之后（发射顺序 = 本表书写顺序，见
        -- generator.cppm::generateCppmFromSlang 的 [FAIL]）。
        {
            name = "ShadowViewProjBlock",
            stride = 256,
            attributes = {
                { name = "viewProj", type = "ObjectMatrix[4]", offset = 0 },
            },
        },
        -- 迁移批 S-2（§9.5 binding 29）：阴影采样参数区类型化载体（scene.slang
        -- ShadowParamsBlock，ConstantBuffer<ShadowParamsBlock> 经子范围描述符指向 scene
        -- data 阴影参数区 kSceneDataShadowParamsOffset，128B = 8×vec4）。
        -- **合并原 7 个各 16B 的独立小区**（splits/texel/params/atlas 连排 + IZB/PCSS/
        -- EVSSM/EVSSM 常量各占一个 256 台阶 = 1024B → 128B），字段序 = 合并前的物理序
        --（前四段本就同区连排，逐字节同；后三段搬家，写端按本 struct 整块 memcpy）。
        {
            name = "ShadowParamsBlock",
            stride = 128,
            attributes = {
                { name = "splits",      type = "vec4", offset = 0 },
                { name = "texelSizes",  type = "vec4", offset = 16 },
                { name = "params",      type = "vec4", offset = 32 },
                { name = "atlas",       type = "vec4", offset = 48 },
                { name = "izb",         type = "vec4", offset = 64 },
                { name = "pcss",        type = "vec4", offset = 80 },
                { name = "evssm",       type = "vec4", offset = 96 },
                { name = "evssmConst",  type = "vec4", offset = 112 },
            },
        },
        -- 迁移批 S-3（§9.5 binding 31）：SSR 区类型化载体（scene.slang SsrParamsBlock，
        -- ConstantBuffer<SsrParamsBlock> 经子范围描述符指向 scene data SSR 区
        -- kSceneDataSsrOffset，96B = mat4 + 2×vec4）。三段本就同区连排 ⟹ **零布局变更**
        --（区容量与合并前逐字节同，其后各区不动）。
        -- 「契约 struct 嵌契约 struct」的**非数组**形态消费者（ObjectMatrix 直接当字段，
        -- 批 R 那条注释列的四个用途之二）。
        {
            name = "SsrParamsBlock",
            stride = 96,
            attributes = {
                { name = "viewProj", type = "ObjectMatrix", offset = 0 },
                { name = "params",   type = "vec4", offset = 64 },
                { name = "gate",     type = "vec4", offset = 80 },
            },
        },
        -- 迁移批 S-5（§9.5 binding 22）：场景**头区**类型化载体（scene.slang
        -- SceneHeaderBlock，ConstantBuffer<SceneHeaderBlock> 经子范围描述符指向 scene data
        -- 头区 kSceneDataHeaderOffset = 0，128B = kSceneDataHeaderBytes）。
        -- 字段序 = 头区物理序：主光 @0（sizeof(SceneLight) = 48）、env 参数 @48
        -- （= kSceneDataEnvOffset 的派生式，本处把它变成**声明**）、skyInvVP @64
        -- （= kSceneDataSkyOffset）。后 16B 是 128B 对齐填充（写入侧不写、读取侧不读）。
        {
            name = "SceneHeaderBlock",
            stride = 128,
            attributes = {
                { name = "light",      type = "SceneLight", offset = 0 },
                { name = "envParams",  type = "vec4",       offset = 48 },
                { name = "skyInvVP",   type = "ObjectMatrix", offset = 64 },
            },
        },
        -- 迁移批 S-4（§9.5 binding 30）：TAA 区类型化载体（scene.slang TaaParamsBlock，
        -- ConstantBuffer<TaaParamsBlock> 经子范围描述符指向 scene data TAA 区
        -- kSceneDataTaaOffset，192B = 2×ObjectMatrix + 4×vec4）。六段自批 13.11 起本就同区
        -- 连排 ⟹ **零布局变更**（区容量与合并前逐字节同，其后 SSR/湿面/簇区间不动）。
        -- 「契约 struct 嵌契约 struct」非数组形态的第二个消费者（两份矩阵 = §9.4 定案：
        -- 契约里的矩阵一律 ObjectMatrix，禁 float4x4 字段）。依赖方向：排在 ObjectMatrix 之后。
        {
            name = "TaaParamsBlock",
            stride = 192,
            attributes = {
                { name = "prevViewProj",     type = "ObjectMatrix", offset = 0 },
                { name = "motion",           type = "ObjectMatrix", offset = 64 },
                { name = "params",           type = "vec4", offset = 128 },
                { name = "prevContentShift", type = "vec4", offset = 144 },
                { name = "inputKernel",      type = "vec4", offset = 160 },
                { name = "clipControl",      type = "vec4", offset = 176 },
            },
        },
    },
}
