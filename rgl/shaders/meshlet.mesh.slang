import model;    // ModelVertexOutput 契约（与 model 路同一输出接口 ⟹ frag 原样复用）
import material; // 统一 push constant（DrawArgs 仅 objectIndex）
import scene;    // readObjectMvp / readObjectClusterRange（binding 3 scene data）

// 簇化轴 ★1-a1ii-α：读**簇产物**（meshlet_pack 的 4 段 GPU buffer）的 mesh shader。
//
// 形态 = 影子叠加（docs/mdi_plan.md §10/§11）：与现行 model 路同 objectIndex、同材质
// 绑定，在 opaque 段之后重画一遍恒 L0 簇；正常档与既有深度共面 + eLess ⟹ 一个像素
// 都不写（A1/A2 逐位不变的构造性保证）。`MESHLET_DEBUG_OFFSET` 变体（阳性对照，
// 门 = 选 SPV 变体不是运行时 flag——2026-10-02 slang fp 重排教训）把 clip.z 拉向相机
// 且翻法线，让叠加真正赢得深度测试并留下可见差异。
//
// 段寻址口径 = tools/meshlet_pack/format.h（唯一真相源）：
//   · DiskCluster 44B/条：triCount@8 vertCount@12 triOffset@16（Tris 段**字节**偏移）
//     remapOffset@20（Remap 段**元素**偏移）。
//   · 全局顶点下标 = remap[remapOffset + 局部索引]，恒指向 48B 步长的顶点池
//     （ModelVertex 契约：pos@0 nrm@12 uv@24 tan@32，与 model_schema.lua 同源）。
//   · Tris 是扁平 u8 流（逐簇 triCount×3 个 8bit 局部索引）——ByteAddressBuffer 无
//     字节 Load，用对齐字读 + 拼接（loadU8x3，尾字钳制不越界）。
//
// 派发契约：CPU 端 drawMeshTasksEXT(groupCount = 该 prim 的 l0Count)，groupID.x =
// L0 前缀内簇序 ⟹ 全局簇下标 = ObjectClusterRange[objectIndex].base + groupID.x。
// 恒 L0 档不选级（§10.1 产物不变量：L0 恒为区间前缀）。
//
// 🔴 两条硬前置（§10.3，漏一条 = 图像静默错）：
//   ① tangent.w 逐三角重算（见 main 内注释）；
//   ② triCount/vertCount ≤ 128 的 8bit 契约断言形态 = 越界 → 空组（下方两处早退）。

// 4 个簇 buffer = 全局单例 → 固定 binding 下标（§10.2，像 scene data 那样，
// 不进 per-entry 描述符）。17-20 接在 model 条目已用的 16 之后。
[[vk::binding(17, 0)]]
ByteAddressBuffer meshletVertexPool;  // 48B/顶点（ModelVertex 契约布局）
[[vk::binding(18, 0)]]
ByteAddressBuffer meshletClusters;    // 44B/条 DiskCluster
[[vk::binding(19, 0)]]
ByteAddressBuffer meshletTris;        // 扁平 u8 三角索引流
[[vk::binding(20, 0)]]
ByteAddressBuffer meshletRemap;       // 扁平 u32 顶点重映射流

// 读一个 u8 三元组（Tris 段字节地址处）。ByteAddressBuffer 只能 4B 对齐读：
// 读 base 与 base+4 两个字拼接。尾字地址钳制到 buffer 末字（GetDimensions =
// 描述符 range = 分配字节数，16B 对齐 ⟹ dim ≥ 16、dim−4 恒可读）：r>0 时被钳掉
// 的字节全部落在 & 0xFFFFFF 掩码之外（钳制只可能发生在 buffer 最末字，而 r>0
// 真正需要的字节恒在数据内），r==0 时 hi 直接置 0——两种情形拼接结果都不变。
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

// 簇的第 k 个三角形的 3 个 8bit 局部索引（局部索引 = 本簇 verts 数组下标：
// 顶点线程按 remap 序输出，verts[i] ↔ 局部索引 i）。
uint3 loadLocalTri(uint triOffset, uint k)
{
    const uint v = loadU8x3(meshletTris, triOffset + k * 3u);
    return uint3(v & 0xFFu, (v >> 8u) & 0xFFu, (v >> 16u) & 0xFFu);
}

// 簇内局部顶点的 uv（remap → 顶点池 @24）。
float2 uvOfLocal(uint localIdx, uint remapOffset)
{
    const uint vb = meshletRemap.Load((remapOffset + localIdx) * 4u) * 48u;
    return asfloat(meshletVertexPool.Load2(vb + 24u));
}

// 逐三角 UV Jacobian 手性（loader gltf_loader_impl.cppm::uvTriangleHandedness 同式：
// det = s1x·s2y − s2x·s1y，det < 0 → −1，否则 +1——含 det==0 与 NaN，比较恒假落 +1）。
// meshlet_hand 的 CPU 参考实现与本式逐字符对照；真产物锚 = 2,322,319 负 / 82.00%。
float uvTriangleHandedness(float2 u0, float2 u1, float2 u2)
{
    const float s1x = u1.x - u0.x, s1y = u1.y - u0.y;
    const float s2x = u2.x - u0.x, s2y = u2.y - u0.y;
    return (s1x * s2y - s2x * s1y) < 0.0 ? -1.0 : 1.0;
}

[shader("mesh")]
[numthreads(128, 1, 1)]
[outputtopology("triangle")]
void main(
    uint ti : SV_GroupIndex,
    uint3 groupID : SV_GroupID,
    out vertices ModelVertexOutput verts[128],
    out indices uint3 triangles[128])
{
    // 8bit 局部索引 + out 数组上限 = 产物契约（§10.3 ②）；numthreads 与两者同 128：
    // 每线程至多 1 顶点 + 1 三角形。
    const uint kMaxClusterVerts = 128u;
    const uint kMaxClusterPrims = 128u;
    const uint kClusterStride = 44u;   // DiskCluster（format.h static_assert）
    const uint kVertexStride = 48u;    // ModelVertex（model_schema.lua）

    const ObjectClusterRange range = readObjectClusterRange(drawArgs.objectIndex);
    const uint clusterIdx = range.base + groupID.x;  // 恒 L0 档：group 序 = L0 前缀内簇序
    const uint cBase = clusterIdx * kClusterStride;

    // 断言形态 ①（表坏/groupCount 算错的探测器）：簇条目必须整个落在簇表内 → 否则空组。
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

    // 断言形态 ②（§10.3 ② 的 shader 侧第二道；读取侧 meshlet_asset 已拦第一道）：
    // 越界产物静默画错 → 这里改判空组（宁可少画一块，不画错一块）。
    if (triCount > kMaxClusterPrims || vertCount > kMaxClusterVerts) {
        SetMeshOutputCounts(0, 0);
        return;
    }
    SetMeshOutputCounts(vertCount, triCount);

    if (ti < vertCount) {
        const uint globalVertex = meshletRemap.Load((remapOffset + ti) * 4u);
        const uint vb = globalVertex * kVertexStride;
        float3 pos = asfloat(meshletVertexPool.Load3(vb));         // @0
        float3 nrm = asfloat(meshletVertexPool.Load3(vb + 12u));   // @12
        float2 uv  = asfloat(meshletVertexPool.Load2(vb + 24u));   // @24
        float4 tan = asfloat(meshletVertexPool.Load4(vb + 32u));   // @32（w = +1 占位）

        // 🔴 §10.3 硬前置 1：tangent.w 逐三角重算。产物池存 w=+1 占位而 bistro 逐三角
        // 82.00% 为负；共享池存不下逐三角 w（现行路靠 loader 去索引才逐三角忠实）。
        // 本 shader 取「本簇内**第一个**引用该顶点的三角形的 w」：单写者、确定序、无
        // UB；手性一致的共享顶点（绝大多数）精确，混合手性顶点是已知近似（批次报告
        // 挂号：per-vertex 接口的固有上限，per-triangle 需要去索引 = 3× 顶点预算）。
        float w = tan.w;  // 无三角形引用（孤立顶点）= 保留占位值
        for (uint k = 0; k < triCount; ++k) {
            const uint3 l = loadLocalTri(triOffset, k);
            if (l.x == ti || l.y == ti || l.z == ti) {
                w = uvTriangleHandedness(uvOfLocal(l.x, remapOffset),
                                         uvOfLocal(l.y, remapOffset),
                                         uvOfLocal(l.z, remapOffset));
                break;
            }
        }

#if defined(MESHLET_DEBUG_OFFSET)
        // 阳性对照（★1-a1ii-α 判据 3）：翻法线让叠加像素的着色**必然**不同于底——
        // 否则「画了相同内容」与「什么都没画」在图像上不可区分，判据 3 失去意义。
        nrm = -nrm;
#endif
        float4 clip = clipPos(readObjectMvp(drawArgs.objectIndex), pos);
#if defined(MESHLET_DEBUG_OFFSET)
        // 阳性对照：clip.z 拉向相机 (1−ε) → 与底共面的叠加赢得 eLess 深度测试真正写
        // 像素；xy/w 一个 bit 不动 ⟹ 覆盖集 = 簇足迹本身（ε=0.001：除贴死近平面的
        // 像素外逐位必赢；代价 = 能穿透深度差 <0.1% 远面的遮挡，调试档可接受）。
        clip.z *= 0.999;
#endif
        verts[ti].position = clip;
        verts[ti].normal = nrm;
        verts[ti].uv = uv;
        verts[ti].tangentOS = float4(tan.xyz, w);
        verts[ti].positionOS = pos;  // frag 侧世界变换与 model 路同约定
    }

    if (ti < triCount)
        triangles[ti] = loadLocalTri(triOffset, ti);
}
