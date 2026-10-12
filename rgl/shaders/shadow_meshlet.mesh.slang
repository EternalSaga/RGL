import scene;   // sceneData（binding 3）+ readObjectClusterRange / readObjectMatrix / readShadowViewProj
import scene_tables;  // 批 T1：逐对象四张表 StructuredBuffer 读取面（binding 23/25/26/33）

// 簇化轴 ★18-B1：阴影段 depth-only 的**簇路径** mesh shader（docs/mdi_plan.md §18.5 前置清单 /
// §18.6 第一刀形态）。形态 = `meshlet.mesh.slang` 的**段寻址** × `shadow.mesh.slang` 的
// **变换链与输出接口**：读同一份簇产物四段 buffer（binding 17-20，全局单例，装配层
// wireMeshletBindings 写描述符），变换走 `clipPos(readShadowViewProj(cascade), worldPos(...))`
// 两段矩阵（与 shadow.mesh.slang 逐式同——VP 区是每帧 4 槽定长，装不下 per-object 预乘）。
//
// ## 为什么是**第三个** mesh shader，而不是给 meshlet.mesh.slang 加宏
//
// 1. **mesh stage 的输出结构受 frag 入口签名约束**（与 `shadow.mesh.slang` 不复用
//    `model.mesh.slang` 完全同一条理由）：主 pass 的 `ModelVertexOutput` 带 normal/uv/
//    tangentOS/positionOS 五个字段，depth-only pass 一个都不消费；而本管线复用的 frag 恰是
//    `shadow.frag.spv`（只收一个 SV_Position）。给 meshlet.mesh 加宏去裁输出字段 = 同一份
//    SPV 里塞两种输出接口，宏关掉那侧仍要过 frag 入口匹配，裁不干净。
// 2. 🔴 **门 = 选 SPV 变体 / 选管线条目，不是运行时 flag**（2026-10-02 slang fp 重排教训，
//    同 IZB/PCSS/EVSSM/meshlet.debug 纪律）：slang 对源码差异做 fp 重排，「flag=0 的分支」
//    也会让主路逐位漂移。主 pass 的红线 A（`54e40665…`）不能被一个阴影实验碰。
// 3. 两者的**派发契约不同**：主 pass 恒 L0 前缀（`cluster = base + (groupID.x >> 1)`），
//    本管线要插一个选级偏移（`clusterStart`，§18.3 拍板走 push constant，零 scene data 变更）。
//
// ## 本管线**不着色** ⟹ §10.3 两条硬前置里的一条在此不适用（别以为漏了）
//
// `meshlet.mesh.slang` 的 §10.3 硬前置 ① = 「tangent.w 逐三角重算 UV Jacobian 手性」。它是
// **着色输入**（进 TBN 副切线符号），而 depth-only pass 不输出任何逐顶点属性、frag 是空壳
// ⟹ 该前置在本管线**没有消费方**，结构性不适用。硬前置 ②（triCount/vertCount ≤ 128 的
// 8bit 契约）与着色无关，**照样保留**（下方第二道早退）。
//
// ## 🔴 不声明 binding 21（`clusterVisible`）
//
// 阴影段本批**不做逐簇剔除**。descriptor heap 下「有 mapping 未声明」空转无害，而「声明了
// 未 mapping」是 VVL 错误（方向见 shader_registry 的 addMeshletEntry 注释）⟹ 本文件不声明
// 21，条目侧也**不许**给本管线烘 21（两侧同向才安全）。
//
// ## 派发契约（🔴 clusterStart 是**相对 base 的偏移**，不是全局簇下标）
//
// `cluster = readObjectClusterRange(objectIndex).base + args.clusterStart + (groupID.x >> 1)`，
// `half = groupID.x & 1`，本任务三角区间 = `[half*85, min(triCount, half*85 + 85))`。
// `clusterStart` 的口径 = 批 A 的 `MeshletLodPrimRange::start`（相对 `MeshletPrimRange::base`
// 的偏移，meshlet_lod.cppm 头注释）；选级 = 画 `min(D, 本 prim 最深级)` 那一段连续子区间
// （§18.3：每 object 的第 D 级是单一连续区间）。每簇固定 2 槽 = 免前缀和，空半区发空任务早退。
//
// 段寻址口径与规格常量 = `meshlet.mesh.slang` 逐字同源（真相源 tools/meshlet_pack/format.h）：
// DiskCluster 44B/条（triCount@8 vertCount@12 triOffset@16 Tris 段**字节**偏移、remapOffset@20
// Remap 段**元素**偏移）；全局顶点下标 = remap[remapOffset + 局部索引]，恒指向 48B 步长顶点池。
// 五个同名同值常量由 `meshlet_spec_checks` 与 schema 做数值对账（改一侧必两端同改）。

// 4 个簇 buffer = 全局单例 → 固定 binding 下标（§10.2，与主 pass 的 meshlet.mesh **同一批
// 槽号**，装配层 wireMeshletBindings 一次写描述符、两条管线共用）。
[[vk::binding(17, 0)]]
ByteAddressBuffer meshletVertexPool;  // 48B/顶点（ModelVertex 契约布局）
[[vk::binding(18, 0)]]
ByteAddressBuffer meshletClusters;    // 44B/条 DiskCluster
[[vk::binding(19, 0)]]
ByteAddressBuffer meshletTris;        // 扁平 u8 三角索引流
[[vk::binding(20, 0)]]
ByteAddressBuffer meshletRemap;       // 扁平 u32 顶点重映射流

// push constant：**不 import material.slang**，自带 ShadowMeshletDrawArgs（12B，scalar layout）。
// 理由与 shadow.mesh.slang 的自带 push 块同源（共享的 DrawArgs 契约不能为一条管线加字段），
// 本管线再多一个 clusterStart = 阴影 push 的 8B 之上加 4B。三个字段全是**纯索引/偏移**
// （AGENTS bindless 原则）；CPU 侧对偶 = frame_draw_list.cppm::ShadowMeshletDrawArgs（static_assert 钉 12B）。
struct ShadowMeshletDrawArgs
{
    uint objectIndex;   // scene data 各 per-object 表下标 = MeshRef.objectSlot（同 DrawArgs）
    uint cascadeIndex;  // 阴影图分块号，选 readShadowViewProj 槽
    uint clusterStart;  // 🔴 相对 ObjectClusterRange::base 的偏移（批 A MeshletLodPrimRange::start 口径）
};

[[vk::push_constant]] ShadowMeshletDrawArgs shadowMeshletDrawArgs;

// 输出接口 = shadow.mesh.slang 的 ShadowVertexOutput（本管线复用 shadow.frag.spv，签名必须一致）。
struct ShadowVertexOutput
{
    float4 position : SV_Position;
};

// 读一个 u8 三元组（Tris 段字节地址处）。ByteAddressBuffer 只能 4B 对齐读：
// 读 base 与 base+4 两个字拼接。尾字地址钳制到 buffer 末字（GetDimensions =
// 描述符 range = 分配字节数，16B 对齐 ⟹ dim ≥ 16、dim−4 恒可读）：r>0 时被钳掉
// 的字节全部落在 & 0xFFFFFF 掩码之外（钳制只可能发生在 buffer 最末字，而 r>0
// 真正需要的字节恒在数据内），r==0 时 hi 直接置 0——两种情形拼接结果都不变。
// 与 meshlet.mesh.slang 的 loadU8x3 逐式同（同一份段寻址契约，两处必须同步改）。
uint loadU8x3(ByteAddressBuffer buf, uint byteAddr)
{
    uint dim;
    buf.GetDimensions(dim);
    const uint base = byteAddr & ~3u;
    const uint r = byteAddr & 3u;
    const uint w0 = buf.Load(base);
    const uint w1 = buf.Load(min(base + 4u, dim - 4u));
    const uint lo = w0 >> (8u * r);
    const uint hi = (r == 0u) ? 0u : (w1 << (32u - 8u * r));
    return (lo | hi) & 0xFFFFFFu;
}

// 簇的第 k 个三角形的 3 个 8bit 局部索引（局部索引 = 本簇 remap 数组下标；去索引后它
// 只用来定位"这个三角形的这个角"读哪个全局顶点，不再对应输出顶点下标）。
uint3 loadLocalTri(uint triOffset, uint k)
{
    const uint v = loadU8x3(meshletTris, triOffset + k * 3u);
    return uint3(v & 0xFFu, (v >> 8u) & 0xFFu, (v >> 16u) & 0xFFu);
}

[shader("mesh")]
// 256 = 本机 maxMeshWorkGroupInvocations，也是填满顶点输出所需的最小线程数（去索引后
// 顶点输出上限 255 ⟹ 255 条顶点线程）。取 256 的理由与 meshlet.mesh.slang 逐字同。
[numthreads(256, 1, 1)]
[outputtopology("triangle")]
void main(
    uint ti : SV_GroupIndex,
    uint3 groupID : SV_GroupID,
    out vertices ShadowVertexOutput verts[256],
    out indices uint3 triangles[85])
{
    // 产物契约（format.h DiskCluster：triCount ≤ 128、vertCount ≤ 128，8bit 局部索引因此可解）。
    const uint kMaxClusterVerts = 128u;
    const uint kMaxClusterPrims = 128u;
    // 每任务三角形上限：去索引 ⟹ 顶点输出 = 3 × 三角形数 ≤ maxMeshOutputVertices(256)
    // ⟹ 85（3×85 = 255；86 要 258 个顶点，越限）。
    const uint kTaskTris = 85u;
    const uint kClusterStride = 44u;   // DiskCluster（format.h static_assert）
    const uint kVertexStride = 48u;    // ModelVertex（model_schema.lua）

    // 🔴 clusterStart 在 base 之后加：批 A 的 start 是**相对 base 的偏移**，不是全局簇下标。
    const ObjectClusterRange range = readObjectClusterRange(shadowMeshletDrawArgs.objectIndex);
    const uint clusterIdx = range.base + shadowMeshletDrawArgs.clusterStart + (groupID.x >> 1u);
    const uint half = groupID.x & 1u;                         // 半区号：0 = 前 85 三角，1 = 其余
    const uint cBase = clusterIdx * kClusterStride;

    // 断言形态 ①（表坏/groupCount 算错的探测器）：簇条目必须整个落在簇表内 → 否则空组。
    // 阴影段的 groupCount 由「选级后的簇数 × 2」派生（批 B2），比主 pass 多一层选级算术，
    // 这条探测器正是为那一层准备的：算错 = 越界簇，宁可一块不画也不读越界内存。
    uint clustersBytes;
    meshletClusters.GetDimensions(clustersBytes);
    if (cBase + kClusterStride > clustersBytes) {
        SetMeshOutputCounts(0, 0);
        return;
    }
    const uint triCount = meshletClusters.Load(cBase + 8u);
    const uint vertCount = meshletClusters.Load(cBase + 12u);
    const uint triOffset = meshletClusters.Load(cBase + 16u);    // Tris 段**字节**偏移
    const uint remapOffset = meshletClusters.Load(cBase + 20u);  // Remap 段**元素**偏移

    // 断言形态 ②（§10.3 ② 的 shader 侧第二道，与着色无关故本管线**照样保留**）：产物契约
    // triCount/vertCount ≤ 128 没变，但去索引下越界的后果 = 「两槽装不下的三角形根本没有
    // 任务槽」= 少画一块。仍整簇判空（宁可一块不画，也不画一个契约已漂的半吊子簇）。
    if (triCount > kMaxClusterPrims || vertCount > kMaxClusterVerts) {
        SetMeshOutputCounts(0, 0);
        return;
    }

    // 半区切分：本任务负责 [triStart, triStart + 85) ∩ [0, triCount)。
    // triCount ≤ 85 的簇第二槽为空 ⟹ 空任务早退。
    const uint triStart = half * kTaskTris;
    const uint remaining = triStart >= triCount ? 0u : triCount - triStart;  // 无符号回绕守卫
    const uint nTri = min(kTaskTris, remaining);
    if (nTri == 0u) {
        SetMeshOutputCounts(0, 0);
        return;
    }
    SetMeshOutputCounts(3u * nTri, nTri);

    if (ti < 3u * nTri) {
        // 去索引：本线程 = 本任务第 ti/3 个三角形的第 ti%3 个角（与 meshlet.mesh 同法）。
        // 只取位置：depth-only 不着色 ⟹ 法线/uv/切线一个都不读（顶点池里它们就在同一 48B 内，
        // 少读三个向量 = 本管线相对主 pass 的真实成本差）。
        const uint3 local = loadLocalTri(triOffset, triStart + (ti / 3u));
        const uint corner = ti % 3u;
        const uint localIdx = corner == 0u ? local.x : (corner == 1u ? local.y : local.z);
        const uint vb = meshletRemap.Load((remapOffset + localIdx) * 4u) * kVertexStride;
        const float3 pos = asfloat(meshletVertexPool.Load3(vb));  // @0

        // 变换链与 shadow.mesh.slang **逐式同**（两段矩阵：world 在对象矩阵表 @128 已有现成
        // 一份，VP 区是每帧 4 槽定长装不下 per-object 预乘）。列组合走 scene.slang 的
        // worldPos / clipPos（无行/列主序约定歧义）。
        const float3 world = worldPos(readObjectMatrix(shadowMeshletDrawArgs.objectIndex), pos);
        verts[ti].position = clipPos(readShadowViewProj(shadowMeshletDrawArgs.cascadeIndex), world);
    }

    // 去索引的索引自然连续：本任务第 ti 个三角形 = 本组 verts[3ti .. 3ti+2]。
    // out indices 引用的是本组 verts 数组下标，不是全局顶点索引（同 meshlet.mesh.slang 记的
    // 那条坑：写全局索引在 RADV 上被 mod 回绕掩盖、NVIDIA 上直接崩形）。
    if (ti < nTri) {
        const uint base = ti * 3u;
        triangles[ti] = uint3(base, base + 1u, base + 2u);
    }
}
