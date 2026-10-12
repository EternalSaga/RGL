import model;    // ModelVertexOutput 契约（与 model 路同一输出接口 ⟹ frag 原样复用）
import material; // 统一 push constant（DrawArgs 仅 objectIndex）
import scene;    // readObjectMvp / readObjectClusterRange（binding 3 scene data）

// 簇化轴 ★1-a1ii-α″：读**簇产物**（meshlet_pack 的 4 段 GPU buffer）的 mesh shader。
//
// 形态 = 影子叠加（docs/mdi_plan.md §10/§11）：与现行 model 路同 objectIndex、同材质
// 绑定，在 opaque 段之后重画一遍恒 L0 簇；正常档与既有深度共面 + eLess ⟹ 一个像素
// 都不写（A1/A2 逐位不变的构造性保证）。`MESHLET_DEBUG_OFFSET` 变体（阳性对照，
// 门 = 选 SPV 变体不是运行时 flag——2026-10-02 slang fp 重排教训）把 clip.z 拉向相机
// 且翻法线，让叠加真正赢得深度测试并留下可见差异。`MESHLET_TINT` 变体（★0.5-b2 共面
// 双臂的阳性对照）与 DEBUG_OFFSET 正相反：只把着色输入写成常数（逐 primitive 平色块），
// clip 一 bit 不动 ⟹ 深度/覆盖集与底逐位同，专供 eEqual/eNotEqual 双臂做「真在写、
// 真在验色」的非空洞证明（main 侧 coplanar=equal/ne + tint 必须变图）。
//
// 🔴 发射形态 = **去索引**（§13.2 路 C，α″ 用它换掉 α 的共享顶点发射）：每个三角形发射
// 它自己的 3 个私有顶点 ⟹ 顶点输出 = 3 × 三角形数。为什么：逐顶点接口装不下逐三角量
// （§12.2），而 per-primitive 输出撞 slang 上游缺口（FS 侧发不出 PerPrimitiveEXT，§13.1）。
// 去索引后 tangent.w 是"三角形内恒定的逐顶点量"，插值结果恰等于该三角形的值 ⟹ **精确**；
// 代价 = 顶点输出 8,496,360，与现行 model 路（本来就每 group 10 三角 / 32 个私有顶点）
// 的去索引顶点数**逐位相等** ⟹ 相对现状零回归（放弃的是我们本来就没在用的共享顶点）。
//
// 段寻址口径 = tools/meshlet_pack/format.h（唯一真相源）：
//   · DiskCluster 44B/条：triCount@8 vertCount@12 triOffset@16（Tris 段**字节**偏移）
//     remapOffset@20（Remap 段**元素**偏移）。
//   · 全局顶点下标 = remap[remapOffset + 局部索引]，恒指向 48B 步长的顶点池
//     （ModelVertex 契约：pos@0 nrm@12 uv@24 tan@32，与 model_schema.lua 同源）。
//   · Tris 是扁平 u8 流（逐簇 triCount×3 个 8bit 局部索引）——ByteAddressBuffer 无
//     字节 Load，用对齐字读 + 拼接（loadU8x3，尾字钳制不越界）。
//
// 派发契约：CPU 端 drawMeshTasksEXT(groupCount = **2 × 该 prim 的 l0Count**)，**每簇固定 2
// 槽**（去索引把每任务三角形数压到 85，而 L0 簇中位 121 三角、71.2% 的簇 > 85 ⟹ 必须拆；
// 固定 2 槽 = 免前缀和、免额外 buffer，空半区发空任务早退）：
//   cluster = range.base + (groupID.x >> 1)、half = groupID.x & 1、
//   本任务三角形区间 = [half*85, min(triCount, half*85 + 85))。
// 恒 L0 档不选级（§10.1 产物不变量：L0 恒为区间前缀）。
//
// 🔴 两条硬前置（§10.3，漏一条 = 图像静默错）：
//   ① tangent.w 逐三角（见 main 内注释；去索引后才是**精确**解，α 的共享顶点版是近似）；
//   ② triCount/vertCount ≤ 128 的 8bit 契约断言形态 = 越界 → 整簇空组（下方两处早退）。

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

// 簇化轴 ★1-a2-i：逐簇视锥剔除的**可见标志**（`meshlet_cull.comp.slang` 每帧写，1 = 画）。
// 下标 = `range.cullBase + 簇内序`（= **剔除项表**下标，🔴 不是全局簇下标：同资产的簇被多个
// objectSlot 共享，用全局下标会让一个实例的可见性替另一个决定 = 几何凭空消失）。
// 🔴 本 binding **无条件声明**——mesh SPV 只有一份，β-1 取代档与 cull 档共用，而 descriptor heap
// 下「shader 声明了 binding 却无 mapping」是 VVL 错误（反向的「有 mapping 未声明」才空转无害，
// 见 shader.cppm 的 binding 3 注释）。⟹ 门关时装配层照样建 buffer 并**填全 1**，本行恒不成立。
[[vk::binding(21, 0)]]
// 🔴 用 `ByteAddressBuffer` 而非 `StructuredBuffer<uint>`：后者触 `shader_dep_check` **R3 红线**
// （任何 .slang 出现 StructuredBuffer = ERROR，全库零使用、**无豁免清单**，出处 git:调试历史
// 「灰屏」stride 静默错位）。本 binding 一度写成 `StructuredBuffer<uint>` 并留了「不改成
// ByteAddressBuffer」的拍板注——那条拍板**已撤销**，理由见 `docs/constantbuffer_迁移计划.md`
// §8：R3 与「类型化载体」这条线冲突未决前，rgl/ 树内一律走 sanctioned 的 ByteAddressBuffer 形态。
// 唯一的例外通道 = 先按 §8 把 R3 从「一刀切」改成「元素类型未走反射契约才报错」并配 stride
// 实测护栏；在那之前不新增 R3 违规。
ByteAddressBuffer clusterVisible;

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

// 簇的第 k 个三角形的 3 个 8bit 局部索引（局部索引 = 本簇 remap 数组下标；去索引后它
// 只用来定位"这个三角形的这个角"读哪个全局顶点，不再对应输出顶点下标）。
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
// 256 = 本机 maxMeshWorkGroupInvocations，也是填满顶点输出所需的最小线程数（去索引后
// 顶点输出上限 255 ⟹ 255 条顶点线程）。取 256 而非更小：线程数 < 顶点数时高下标顶点
// 永远没人写（未定义输出），且 256 恰好一三角一线程 + 一顶点一线程各跑一遍。
[numthreads(256, 1, 1)]
[outputtopology("triangle")]
void main(
    uint ti : SV_GroupIndex,
    uint3 groupID : SV_GroupID,
    out vertices ModelVertexOutput verts[256],
    out indices uint3 triangles[85])
{
    // 产物契约（format.h DiskCluster：triCount ≤ 128、vertCount ≤ 128，8bit 局部索引因此可解）。
    const uint kMaxClusterVerts = 128u;
    const uint kMaxClusterPrims = 128u;
    // 每任务三角形上限：去索引 ⟹ 顶点输出 = 3 × 三角形数 ≤ maxMeshOutputVertices(256)
    // ⟹ 85（3×85 = 255；86 要 258 个顶点，越限）。out indices 上限同此值：每任务
    // 1 三角形/线程，且 numthreads(256) ≥ 3×85 让两类线程都排得下。
    const uint kTaskTris = 85u;
    const uint kClusterStride = 44u;   // DiskCluster（format.h static_assert）
    const uint kVertexStride = 48u;    // ModelVertex（model_schema.lua）

    const ObjectClusterRange range = readObjectClusterRange(drawArgs.objectIndex);
    const uint clusterIdx = range.base + (groupID.x >> 1u);  // 每簇固定 2 槽（恒 L0 档：group 序 = L0 前缀内簇序）
    const uint half = groupID.x & 1u;                         // 半区号：0 = 前 85 三角，1 = 其余
    const uint cBase = clusterIdx * kClusterStride;

    // 簇化轴 ★1-a2-i **逐簇视锥剔除早退**：mesh shader 没有 discard，「这一簇不画」的唯一手段是
    // 一个输出都不写（`SetMeshOutputCounts(0, 0)` + return，与下方三条断言早退同形）。标志由
    // `meshlet_cull.comp.slang` 每帧在 opaque 段之前的一次 compute 写好（保守球测试，宁可少剔）。
    // 门 `meshlet.cull` 关 = cullBase 恒 0（CPU 侧根本不建剔除项表）+ 整张标志表被装配层填成 1
    // ⟹ 本条件恒不成立 = 逐位不变（不靠「读了但结果一样」，靠门关不写任何非 1 字节）。
    if (clusterVisible.Load((range.cullBase + (groupID.x >> 1u)) * 4u) == 0u) {
        SetMeshOutputCounts(0, 0);
        return;
    }

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

    // 断言形态 ②（§10.3 ② 的 shader 侧第二道；读取侧 meshlet_asset 已拦第一道）：产物契约
    // triCount/vertCount ≤ 128 没变，但去索引改的是**越界的后果**——α 那版越界会拿 8bit 索引
    // 去写越界的 verts 下标 = **画错一块**；现在顶点输出与 vertCount 无关、三角形按
    // [half*85, +85) 取，越界簇最多是"两槽装不下的三角形根本没有任务槽"= **少画一块**。
    // 仍整簇判空（宁可一块不画，也不画一个契约已漂的半吊子簇）。
    if (triCount > kMaxClusterPrims || vertCount > kMaxClusterVerts) {
        SetMeshOutputCounts(0, 0);
        return;
    }

    // 半区切分：本任务负责 [triStart, triStart + 85) ∩ [0, triCount)。
    // triCount ≤ 85 的簇（实测 28.8% 的 L0 簇）第二槽为空 ⟹ 空任务早退。
    const uint triStart = half * kTaskTris;
    const uint remaining = triStart >= triCount ? 0u : triCount - triStart;  // 无符号回绕守卫
    const uint nTri = min(kTaskTris, remaining);
    if (nTri == 0u) {
        SetMeshOutputCounts(0, 0);
        return;
    }
    SetMeshOutputCounts(3u * nTri, nTri);

    if (ti < 3u * nTri) {
        // 去索引：本线程 = 本任务第 ti/3 个三角形的第 ti%3 个角。三角形的 3 个角各是一个
        // 私有输出顶点（局部索引只用来查 remap 读哪个全局顶点，不再共享输出）。
        const uint3 local = loadLocalTri(triOffset, triStart + (ti / 3u));
        const uint corner = ti % 3u;
        const uint localIdx = corner == 0u ? local.x : (corner == 1u ? local.y : local.z);
        const uint vb = meshletRemap.Load((remapOffset + localIdx) * 4u) * kVertexStride;
        float3 pos = asfloat(meshletVertexPool.Load3(vb));         // @0
        float3 nrm = asfloat(meshletVertexPool.Load3(vb + 12u));   // @12
        float2 uv  = asfloat(meshletVertexPool.Load2(vb + 24u));   // @24
        float4 tan = asfloat(meshletVertexPool.Load4(vb + 32u));   // @32（w = +1 占位，本 shader 不消费）

        // 🔴 §10.3 硬前置 1：tangent.w = **本三角形自己的** UV Jacobian 手性，写满该三角形
        // 的全部 3 个输出顶点。它是逐三角量，而 tangentOS.w 是被光栅器插值的逐顶点属性
        // ⟹ 只有"三角形三个角各自私有、都带同一个值"才让插值结果恰等于该三角形的值。
        // α 的共享顶点版只能取「本簇内第一个引用该顶点的三角形」的 w（0.1689% 顶点引用错，
        // §12.2 量化过）；per-primitive 输出撞 slang 的 FS 侧缺口（§13.1）——去索引是第三条路。
        // 本式是三角形三个 uv 的**纯函数** ⟹ 同一三角形的 3 条线程算出逐位相同的值，无需
        // groupshared（且 SetMeshOutputCounts 之后活跃线程数可小于 numthreads，跨线程同步本来就不许用）。
        const float w = uvTriangleHandedness(uvOfLocal(local.x, remapOffset),
                                             uvOfLocal(local.y, remapOffset),
                                             uvOfLocal(local.z, remapOffset));

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
#if defined(MESHLET_TINT)
        // 共面双臂阳性对照（★0.5-b2，门 `meshlet.tint`）：着色输入（normal/uv/切线+手性）
        // 全写死成常数 ⟹ 叠加写回**逐 primitive 的平色块**；position/positionOS 一 bit
        // 不动 ⟹ 深度与覆盖集与底**逐位同**（= 与 DEBUG_OFFSET 正相反：那个动深度动着色，
        // 这个只动着色）。于是 coplanar=equal + 本变体图**必须变**（共面像素经 eEqual 放行
        // 写回不同颜色 = equal 臂的非空洞证明），coplanar=off + 本变体图**必须不变**
        //（eLess 共面全拒 = 反证「变化来自共面写回，不是 tint 本身可见」）。
        // 「常色」的上限说明（拍板记录）：frag 与旧路径共用、albedo = 材质 factor × 贴图
        // 采样，mesh 侧控制不了材质项 ⟹ 全屏单一常色在 mesh-only 变体里数学上做不到；
        // 逐 primitive 平色块（无纹理细节、法线固定）是 mesh-only 能给出的最强可读形态，
        // 且 M 的连通域恰好就是「共面 ∩ 被簇覆盖」像素集。
        verts[ti].normal = float3(0.0, 1.0, 0.0);
        verts[ti].uv = float2(0.5, 0.5);
        verts[ti].tangentOS = float4(1.0, 0.0, 0.0, 1.0);  // w（手性）也是着色输入：进 TBN 副切线符号
#else
        verts[ti].normal = nrm;
        verts[ti].uv = uv;
        verts[ti].tangentOS = float4(tan.xyz, w);
#endif
        verts[ti].positionOS = pos;  // frag 侧世界变换与 model 路同约定
    }

    // 去索引的索引自然连续：本任务第 ti 个三角形 = 本组 verts[3ti .. 3ti+2]。
    // 注意 out indices 引用的是本组 verts 数组下标，不是全局顶点索引（同 model.mesh.slang
    // 踩过的坑：写全局索引在 RADV 上被 mod 回绕掩盖、NVIDIA 上直接崩形）。
    if (ti < nTri) {
        const uint base = ti * 3u;
        triangles[ti] = uint3(base, base + 1u, base + 2u);
    }
}
