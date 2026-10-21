import scene;   // 结构体与共享助手 + readObjectClusterRange / readObjectMatrix / readShadowViewProj
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
//    同 IZB/PCSS/EVSSM/meshlet.instrument 纪律）：slang 对源码差异做 fp 重排，「flag=0 的分支」
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

// ─── ★M-1b 真 MDI 变体（-DMDI_SHADOW，docs/mdi_plan.md §19.2 D1/D2）────────────────
// 门 = 选 SPV 变体而不是运行时 flag（上方第 2 条理由同一条纪律）。本分支生效时：
//  · push 的 word0（字段名仍叫 objectIndex，12B 尺寸不变 ⟹ 条目 argsSize 与执行器通用
//    push 路径一字节不动）**语义切换成行基址 rowBase**（§3 的统一公式 `slot = rowBase +
//    SV_DrawIndex` 就是这条切换）；
//  · 逐 draw 的身份（objectIndex / clusterStart）由 task 读行表后经 payload 转交。
// 🔴 行表布局的**唯一真相源 = rgl/mdi_rows.cppm**（MdiShadowRow + kMdiShadowRowStride +
//    kMdiShadowCommandBytes）。🔴 **★M-3T-c 起本文件不再拼行布局**：行寻址（步长 24 + 四个字节
//    偏移）整体住在 `shadow_meshlet.task.slang`，本文件只认 payload 的两个字段 ⟹ shader 侧的
//    行表契约从两份收成一份。对账链（`meshlet_spec_checks` 数值对账 +
//    tests/test_meshlet_shadow_args 的设备自证回路 + tests/test_mdi_shadow_emit 的 constexpr 桥）
//    指向那个文件。**别在这里重新拼一份**（手算常量 / 宏等第四份形态一律不要）。
#ifdef MDI_SHADOW
// 🔴 本分支**不声明 binding 34（行表）**，也**不声明** `SV_DrawIndex`：
//  · 行表由 `shadow_meshlet.task.slang` 读（★M-3T-c：行寻址只允许一份拼写，本文件不是那份）；
//  · `SV_DrawIndex` 在 mesh 入口会毒化 gfx1151 的后续 mesh draw（§19.9 实测），声明处已搬进
//    task（§19.15 实测 task 侧不触发）。
// 于是 MDI 变体与出厂那份的差异**只剩身份来源**：非 MDI 从 push 取 objectIndex/clusterStart，
// MDI 从 payload 取（同一份行表里的同两个值）。
// 🔴 与 task 侧那份**逐字段同构**（跨模块无法共享声明，两份 .slang 各编各的；改一侧必同改另一侧）。
// 历史：binding 34 的号源 = 定案号 22 被 sceneHeader UBO 占用（§19.2 D2，实测 vkCreateShadersEXT
// conflicts），全树 0..33 已满 ⟹ 落第一个空闲号 34。声明方从 mesh 换成 task 不改条目级烘法
//（条目 mapping 取各 stage 声明的并集）。
struct MdiShadowPayload
{
    uint objectIndex;
    uint clusterStart;
    // 债-143：行 mesh 组数上界（task 从行 word[5] 原样转交）。mesh 侧的钳制用它，
    // 🔴 与 task 那份**逐字段同构**（字段序即契约，改一侧必同改另一侧）。
    uint meshGroupCount;
};
#endif

// push constant：**不 import material.slang**，自带 ShadowMeshletDrawArgs（12B，scalar layout）。
// 理由与 shadow.mesh.slang 的自带 push 块同源（共享的 DrawArgs 契约不能为一条管线加字段），
// 本管线再多一个 clusterStart = 阴影 push 的 8B 之上加 4B。三个字段全是**纯索引/偏移**
// （AGENTS bindless 原则）；CPU 侧对偶 = frame_draw_list.cppm::ShadowMeshletDrawArgs（static_assert 钉 12B）。
struct ShadowMeshletDrawArgs
{
    uint objectIndex;   // scene data 各 per-object 表下标 = MeshRef.objectSlot（同 DrawArgs）；
                        // 🔴 MDI_SHADOW 变体下**语义切换 = 行基址 rowBase**（§19.2 D1），
                        //    尺寸与字段序不变（word1 = cascadeIndex、word2 在 MDI 下不用）
    uint cascadeIndex;  // 阴影图分块号，选 readShadowViewProj 槽
    uint clusterStart;  // 🔴 相对 ObjectClusterRange::base 的偏移（批 A MeshletLodPrimRange::start 口径）；
                        //    MDI_SHADOW 变体下此位 = reserved（clusterStart 进行表 word[4]）
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

// ★M-3T-b：本管线 mesh 线程数降档 256 → **128**（docs/mdi_plan.md §19.16 末「★M-3T-b 的输入事实」）。
// 🔴 单一拼写：`numthreads` 与下方两段 strided 循环的步长**共用这一个常量** ⟹ 以后改线程数只改
// 这里 + schema 一处，不用回来改循环。
//
// 本常量 = schema 常量 `kMeshletMaxWorkGroupInvocations`（meshlet_cull_schema.lua）的 **shader 侧
// 拼写**，由 `meshlet_spec_checks` **规则 1** 对账：它把 `.slang` 里以 `const uint <schema 常量名>
// = <值>u;` 出现的字面量与 schema 数值钉成**相等**，改一侧不改另一侧即被构建期点名。同一个 schema
// 常量还兼作两条管线共用的**运行期设备下限闸门**（main_meshlet_tables.cppm::checkMeshletDeviceFit）。
//
// 128 的出处（三机口径，2026-10-13 现读，别抄旧注释里的"本机上限 256"）：
//  · **NVIDIA `maxMeshWorkGroupInvocations = 128` 是硬上限**：声明 256 时 `vkCreateShadersEXT`
//    被 VVL 点名 VUID-RuntimeSpirv-MeshEXT-07295/07298 ⟹ MDI 路在 NV 上根本建不出 shader。
//  · 本机 AMD Radeon 8060S（RADV STRIX_HALO）`maxPreferredMeshWorkGroupInvocations = 128`
//    （它的 maxMeshWorkGroupInvocations 给到 256，但 preferred 才是"不亏性能"的那一档）。
//  · 三机最小值 = NV 的 128 ⟹ schema 降到 128 之后，"numthreads 吃 schema 常量"的单一真相源才
//    真的成立（本批之前 schema 还是 256，故本文件一度只能另起一名，见下）。
//
// 🔴 教训（2026-10-13 实测，防在别的场景重犯）：本常量曾与 schema 那个**同名不同值**（本管线先降到
//    128、schema 当时仍是 256），`meshlet_spec_checks` 规则 1 当场把这一警告打出来、判成规格漂移。
//    规则 1 的语义是"同名必须等值" ⟹ 两个量**只要此刻不是同一个量就必须分开名字**，别"顺手统一"；
//    反过来，确认它们已是同一个量之后就应当并回同一个名字，否则留下一份没人对账的平行拼写。
//
// 线程数 128 < 顶点输出上限 255 **不再**意味着"高下标的顶点没人写"——那是"一线程一元素 + if
// 守卫"时代的论证（旧注释正是那么写的），现在是 strided 循环，覆盖集 = [0, 3nTri) 与线程数解耦。
static const uint kMeshletMaxWorkGroupInvocations = 128u;

[shader("mesh")]
[numthreads(kMeshletMaxWorkGroupInvocations, 1, 1)]
[outputtopology("triangle")]
void main(
    uint ti : SV_GroupIndex,
    uint3 groupID : SV_GroupID,
#ifdef MDI_SHADOW
    // 逐 draw 的身份经 payload 进来（task 读行表填的）。🔴 mesh 侧**必须真读一次** payload：
    // slang 会把没读过的 payload 从入口接口整个裁掉（§19.15 实测），裁掉就等于 task 有 payload
    // 输出、mesh 无输入 = 两侧不同向。
    payload MdiShadowPayload mdiPayload,
#endif
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

#ifdef MDI_SHADOW
    // 🔴 身份来源 = payload（★M-3T-c）：task 已按 row = rowBase + SV_DrawIndex 从 24B 行表里
    // 读出这两个值，并把本批的 mesh 组数一次发完。本文件因此**不拼行布局**（口径 → 文件头）。
    // 🔴 clusterStart 在 base 之后加：批 A 的 start 是**相对 base 的偏移**，不是全局簇下标。
    // 🔴 派发序号 = groupID.x：task 一次发 meshGroupCount 组 ⟹ mesh 的 WorkgroupId（spec 语义
    //    = "position within the current mesh task"）取值域 [0, meshGroupCount)，**逐值等于**旧的
    //    mesh 组号。★M-3T-a 那版每 task 只发 1 组、groupID.x 恒 0，才不得不从 payload 拿 slot；
    //    本批起两条路径算式**完全同形**（只差身份从 push 还是 payload 来）。
# ifdef SHADOW_SELFCHECK
    // 🔴 **latch-once**（本批补的纪律，与 task 侧同源）：payload 三个字段与 `groupID.x` 各读
    //    **一次**进局部，之后**判据、地址算术、以及真正交出去的计算全部只用这几份拷贝**。
    //    原版（#else 那支）逐字节不动。
    //
    //    为什么必须这样：本自证的全部价值是"我验的就是我要用的那个值"。若判据读一份、
    //    地址算术再读一份，两次读之间值变了（编译器重 materialize、或上游本身在被并发写），
    //    摄像头就是在**校验一个没人用的值** —— 它永远不响，而"零违例"会被读成"已排除"。
    //    这类失效不报错、不崩溃，是本项目最忌的"看起来已完工的半吊子"。
    const uint latchedObjectIndex = mdiPayload.objectIndex;
    const uint latchedClusterStart = mdiPayload.clusterStart;
    const uint latchedMeshGroupCount = mdiPayload.meshGroupCount;  // 债-143 钳制上界（latch 一次）
    const uint latchedSlot = groupID.x;
    const uint latchedCascade = shadowMeshletDrawArgs.cascadeIndex;
    const ObjectClusterRange range = readObjectClusterRange(latchedObjectIndex);
    const uint latchedRangeBase = range.base;
    const uint latchedRangeCount = range.count;
    const uint clusterIdx = latchedRangeBase + latchedClusterStart + (latchedSlot >> 1u);
# else
    const ObjectClusterRange range = readObjectClusterRange(mdiPayload.objectIndex);
    const uint clusterIdx = range.base + mdiPayload.clusterStart + (groupID.x >> 1u);
# endif
#else
    // 🔴 clusterStart 在 base 之后加：批 A 的 start 是**相对 base 的偏移**，不是全局簇下标。
    // 🟰 非 MDI 这一支与改前**逐 token 同**（🔴 红线：门关/出厂的**交付图像**逐位不变 = 出厂锚
    //    `95edc8cc…`）。⚠ ★M-3T-b 起这条红线的钉法从「对拍本 SPV 的 sha256」换成「对拍截图哈希」：
    //    线程数 256→128 与两段 strided 循环**必然改 SPV 字节**（LocalSize 与循环结构），但每个输出
    //    元素仍由恰好一个线程用同一表达式算一次 ⟹ 图像逐位不变（实测 2026-10-13，见交付报告）。
    const ObjectClusterRange range = readObjectClusterRange(shadowMeshletDrawArgs.objectIndex);
    const uint clusterIdx = range.base + shadowMeshletDrawArgs.clusterStart + (groupID.x >> 1u);
#endif
    // 半区号：0 = 前 85 三角，1 = 其余。★M-3T-c 起两条路径**共用这一行**（序号来源同为
    // groupID.x，理由见 clusterIdx 处）。非 MDI 这一支的表达式与出厂那份逐 token 同（未动）。
#if defined(MDI_SHADOW) && defined(SHADOW_SELFCHECK)
    const uint half = latchedSlot & 1u;  // 🔴 latch-once：用上面那份拷贝，不再读第二次 groupID.x
#else
    const uint half = groupID.x & 1u;
#endif
    const uint cBase = clusterIdx * kClusterStride;

    uint clustersBytes;
    meshletClusters.GetDimensions(clustersBytes);

#if defined(MDI_SHADOW) && defined(SHADOW_SELFCHECK)
    // ---- 债-140 **mesh 取址链自证**（只在 MDI_SHADOW + SHADOW_SELFCHECK 双定义变体里存在）----
    //
    // 判据的**唯一真相源** = `rgl/blackbox.cppm::meshChainViolations`（CPU 镜像，进 just test
    // 可变异）。这里必须是同一条判据的第二份拼写；改一侧必须同改另一侧。
    //
    // 🔴 为什么装在 mesh 而不是继续加在 task：task 只把行表三个 word 抄进 payload，它**读不到**
    //    ObjectClusterRange（没声明 binding 33）⟹ "clusterStart 的真上界"在 task 里判不了；
    //    而真正拿这些值解引用的是 mesh。上一批的摄像头装在链的**首端**，故障发生在链的**末端**。
    //
    // 🔴 本段刻意放在引擎那道簇表守卫**之前**：守卫命中时 `SetMeshOutputCounts(0,0)` 静默早退
    //    = "一块不画但没有任何信号"，正是本项目最忌的形态。自证不新增防线，是把已有防线接上声音。
    uint trisBytes = 0u;
    uint remapBytes = 0u;
    uint poolBytes = 0u;
    meshletTris.GetDimensions(trisBytes);
    meshletRemap.GetDimensions(remapBytes);
    meshletVertexPool.GetDimensions(poolBytes);
    const uint selfWordM = shadowMeshletDrawArgs.clusterStart;  // word2：MDI 下的保留位，task 侧同源
    const uint objectCapacityM = (selfWordM >> 16u) & 0xFFFFu;
    uint mbits = 0u;
    if (latchedObjectIndex >= objectCapacityM)
        mbits |= 1u;    // P0：mesh 在这里就解引用对象表（range 表 + 后面的矩阵表）
    if (latchedClusterStart >= latchedRangeCount)
        mbits |= 2u;    // P1：🔴 clusterStart 的**真**上界（上一批覆盖面清单第 1 条，本批补上）
    else if (latchedClusterStart + (latchedSlot >> 1u) >= latchedRangeCount)
        mbits |= 4u;    // P2：起点在区间内，是**派发序号**把它推出去的（×2/选级算术错）
    if (uint64_t(clusterIdx) * uint64_t(kClusterStride) > uint64_t(0xFFFFFFFF))
        mbits |= 256u;  // P8：32 位回绕 ⟹ 引擎那道守卫判不出来（防线自身失效）
    if (uint64_t(clusterIdx) * uint64_t(kClusterStride) +
            uint64_t(kClusterStride) >
        uint64_t(clustersBytes))
        mbits |= 8u;    // P3：簇条目越过簇表尾（引擎已守，但原本静默）
    // 🔴 `ti == 0u`：这一段 14 个被验值全是 **线程无关** 的 uniform 量（payload / 表读来的
    //    索引与 buffer 尺寸），而 mesh 入口有 128 线程 ⟹ 实测 RADV 会把同格式的输出按线程
    //    展开成一行 64 份重复值（标定实测：每字段 n=64 且值全同，5145 字符/行）。限定单线程
    //    打印**零覆盖损失**，行宽降到 1/64，命中时才读得出数值。
    if (mbits != 0u && ti == 0u)
    {
        printf("MESH_SELFCHECK stage=entry bits=%u cascade=%u obj=%u obj_cap=%u cstart=%u "
               "range_base=%u range_count=%u slot=%u cluster_idx=%u cbase=%u clusters_bytes=%u "
               "tris_bytes=%u remap_bytes=%u pool_bytes=%u\n",
               mbits, latchedCascade, latchedObjectIndex, objectCapacityM, latchedClusterStart,
               latchedRangeBase, latchedRangeCount, latchedSlot, clusterIdx, cBase, clustersBytes,
               trisBytes, remapBytes, poolBytes);
    }
#endif

    // 🔴 **债-143 钳制**（仅 MDI 两支；非 MDI 出厂支一个字节不碰）：`groupID.x ≥ 行 meshGroupCount`
    // 的组**本行根本没要**——spec 承诺 WorkgroupId ∈ [0, EmitMeshTasksEXT 的 X)，但 gfx1151 的
    // task→mesh 派发实测偶发超发（三探针定案 → docs/债140_跨驱动与上游取证.md §17）。不钳制时
    // 这种组拿 `base + clusterStart + (slot>>1)` 读到**隔壁对象的簇条目**去画 = P2 命中。
    // 钳制 = 它们按"没要过"空转，图像不再被驱动超发污染；引擎不再裸信那条 spec 承诺。
    // 🔴 自证变体里本钳制刻意装在摄像头**之后**：P2 照响，那是"上游缺陷仍在"的活体证据——
    //    防线可以加，摄像头不许被修复弄盲（"修了 = 看不见了"正是本项目最忌的形态）。
#ifdef MDI_SHADOW
# if defined(SHADOW_SELFCHECK)
    if (latchedSlot >= latchedMeshGroupCount) {
        SetMeshOutputCounts(0, 0);
        return;
    }
# else
    if (groupID.x >= mdiPayload.meshGroupCount) {
        SetMeshOutputCounts(0, 0);
        return;
    }
# endif
#endif

    // 断言形态 ①（表坏/groupCount 算错的探测器）：簇条目必须整个落在簇表内 → 否则空组。
    // 阴影段的 groupCount 由「选级后的簇数 × 2」派生（批 B2），比主 pass 多一层选级算术，
    // 这条探测器正是为那一层准备的：算错 = 越界簇，宁可一块不画也不读越界内存。
    if (cBase + kClusterStride > clustersBytes) {
        SetMeshOutputCounts(0, 0);
        return;
    }
    const uint triCount = meshletClusters.Load(cBase + 8u);
    const uint vertCount = meshletClusters.Load(cBase + 12u);
    const uint triOffset = meshletClusters.Load(cBase + 16u);    // Tris 段**字节**偏移
    const uint remapOffset = meshletClusters.Load(cBase + 20u);  // Remap 段**元素**偏移

#if defined(MDI_SHADOW) && defined(SHADOW_SELFCHECK)
    // 🔴 段级两判：条目里**存的**那两个偏移，原码从头到尾没验过 —— 条目本身在表内 ≠ 条目里的
    //    段偏移合法。一个属于别的对象/陈旧的簇条目，能顺利通过守卫①（它在同一张全局簇表里），
    //    却把 triOffset/remapOffset 指到 buffer 外 ⟹ 后面那些无守卫的 Load 直接踩到 0x0。
    //    这是 fault@0 最顺的一条通路，也是上一批"零违例"里**根本没被覆盖**的那一段。
    {
        uint sbits = 0u;
        if (uint64_t(triOffset) + uint64_t(3u) * uint64_t(triCount) >
            uint64_t(trisBytes))
            sbits |= 16u;  // P4
        if ((uint64_t(remapOffset) + uint64_t(vertCount)) * uint64_t(4u) >
            uint64_t(remapBytes))
            sbits |= 32u;  // P5
        // 🔴 同上：段级两判的被验值（triOffset/triCount/remapOffset/vertCount 与四个
        //    GetDimensions）也是线程无关的，单线程打印。
        if (sbits != 0u && ti == 0u)
        {
            printf("MESH_SELFCHECK stage=section bits=%u cascade=%u obj=%u cluster_idx=%u "
                   "tri_off=%u tri_count=%u vert_count=%u remap_off=%u tris_bytes=%u "
                   "remap_bytes=%u pool_bytes=%u\n",
                   sbits, latchedCascade, latchedObjectIndex, clusterIdx, triOffset, triCount,
                   vertCount, remapOffset, trisBytes, remapBytes, poolBytes);
        }
    }
#endif

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

    // ★M-3T-b：128 线程要填满 3×85 = 255 个顶点输出 ⟹ 每线程最多 2 个元素，故 if 改 strided 循环。
    // 🔴 逐位不变的论证（且本批**实测**，不靠论证）：每个输出元素仍由**恰好一个**线程用**同一个
    // 表达式**计算一次，迭代之间无累积量、体内无跨线程依赖、无 groupshared（`SetMeshOutputCounts`
    // 之后活跃线程数可小于 numthreads，跨线程同步本来就不许用）⟹ 不存在可重排的结合序。
    // 步长取线程数常量而非写死 128，是为了让"覆盖集 = [0, 3nTri)"与线程数解耦（以后改线程数不用
    // 回来改这里）。
#if defined(MDI_SHADOW) && defined(SHADOW_SELFCHECK)
    // 🔴 逐顶点两判各**只报一次**：一次 draw 有 1530 行 × 每行若干 mesh 组 × 每组最多 255 个
    //    顶点，若逐顶点打，printf 缓冲会被同一类违例灌满、把**首条**（信息量最大的那条）挤掉。
    bool vertPoolReported = false;
    bool localIdxReported = false;
#endif
    for (uint v = ti; v < 3u * nTri; v += kMeshletMaxWorkGroupInvocations) {
        // 去索引：本元素 = 本任务第 v/3 个三角形的第 v%3 个角（与 meshlet.mesh 同法）。
        // 只取位置：depth-only 不着色 ⟹ 法线/uv/切线一个都不读（顶点池里它们就在同一 48B 内，
        // 少读三个向量 = 本管线相对主 pass 的真实成本差）。
        const uint3 local = loadLocalTri(triOffset, triStart + (v / 3u));
        const uint corner = v % 3u;
        const uint localIdx = corner == 0u ? local.x : (corner == 1u ? local.y : local.z);
#if defined(MDI_SHADOW) && defined(SHADOW_SELFCHECK)
        // 🔴 P7 必须在**这次 remap 读之前**：`localIdx` 是从 Tris 段读来的 u8，它直接决定
        //    `(remapOffset + localIdx) * 4` 落在哪。产物契约要求它 < 本簇 vertCount。
        if (!localIdxReported && localIdx >= vertCount)
        {
            printf("MESH_SELFCHECK stage=localidx bits=128 cascade=%u obj=%u cluster_idx=%u "
                   "local_idx=%u vert_count=%u remap_off=%u remap_bytes=%u v=%u\n",
                   latchedCascade, latchedObjectIndex, clusterIdx, localIdx, vertCount,
                   remapOffset, remapBytes, v);
            localIdxReported = true;
        }
#endif
        const uint vb = meshletRemap.Load((remapOffset + localIdx) * 4u) * kVertexStride;
#if defined(MDI_SHADOW) && defined(SHADOW_SELFCHECK)
        // 🔴 P6 就是 fault@0 的**前一次读**：`vb` 来自 remap 里的一个 u32 乘 48，原码对它的
        //    全部防线是"remap 读回来的值恰好合法"。这一判是整条链上最接近案发现场的摄像头。
        if (!vertPoolReported && uint64_t(vb) + uint64_t(kVertexStride) >
                                     uint64_t(poolBytes))
        {
            printf("MESH_SELFCHECK stage=pool bits=64 cascade=%u obj=%u cluster_idx=%u vb=%u "
                   "vstride=%u pool_bytes=%u local_idx=%u remap_off=%u v=%u\n",
                   latchedCascade, latchedObjectIndex, clusterIdx, vb, kVertexStride, poolBytes,
                   localIdx, remapOffset, v);
            vertPoolReported = true;
        }
#endif
        const float3 pos = asfloat(meshletVertexPool.Load3(vb));  // @0

        // 变换链与 shadow.mesh.slang **逐式同**（两段矩阵：world 在对象矩阵表 @128 已有现成
        // 一份，VP 区是每帧 4 槽定长装不下 per-object 预乘）。列组合走 scene.slang 的
        // worldPos / clipPos（无行/列主序约定歧义）。
#ifdef MDI_SHADOW
        // 🔴 latch-once：自证变体里这两次解引用用**上面那份拷贝**（对象矩阵表按 objectIndex 取，
        //    VP 槽按 cascadeIndex 取），原版仍走原表达式。
# ifdef SHADOW_SELFCHECK
        const float3 world = worldPos(readObjectMatrix(latchedObjectIndex), pos);
        verts[v].position = clipPos(readShadowViewProj(latchedCascade), world);
# else
        const float3 world = worldPos(readObjectMatrix(mdiPayload.objectIndex), pos);
        verts[v].position = clipPos(readShadowViewProj(shadowMeshletDrawArgs.cascadeIndex), world);
# endif
#else
        const float3 world = worldPos(readObjectMatrix(shadowMeshletDrawArgs.objectIndex), pos);
        verts[v].position = clipPos(readShadowViewProj(shadowMeshletDrawArgs.cascadeIndex), world);
#endif
    }

    // 去索引的索引自然连续：本任务第 p 个三角形 = 本组 verts[3p .. 3p+2]。
    // out indices 引用的是本组 verts 数组下标，不是全局顶点索引（同 meshlet.mesh.slang 记的
    // 那条坑：写全局索引在 RADV 上被 mod 回绕掩盖、NVIDIA 上直接崩形）。
    // ★M-3T-b：今天 nTri ≤ 85 < 128 ⟹ 恒跑一轮（相对改前零成本）；改循环是为了让"线程数 <
    // 输出元素数"这一类未来漂移不会静默少写（改 if 的形态下它会画出一个悄悄少一批三角形的图）。
    for (uint p = ti; p < nTri; p += kMeshletMaxWorkGroupInvocations) {
        const uint base = p * 3u;
        triangles[p] = uint3(base, base + 1u, base + 2u);
    }
}
