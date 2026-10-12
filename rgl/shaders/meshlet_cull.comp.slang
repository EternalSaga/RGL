import meshlet_cull;   // MeshletCullItem / cullPush 契约（反射单一真相源，本 shader 是其消费者）

// 簇化轴 ★1-a2-i：**逐簇**视锥剔除 compute（一线程一剔除项，保守球测试）。
//
// 与 cull.comp.slang 的分工：那个是**逐实例**（per-candidate）剔除，粒度到 objectSlot；本个把粒度
// 下压到**簇**（LOD DAG 的 L0 前缀），产出逐簇可见标志供 mesh shader 早退。两者互不替代：
// 实例级剔掉的是"整个 object 不提交"，簇级剔掉的是"这个 object 的这一块簇不画"。
//
// 🔴 布局契约走反射，不走 cull.comp.slang 的手写字节偏移：MeshletCullItem 与 cullPush 的字段
// 偏移由 meshlet_cull.slang 经 C API 反射生成 C++ struct（generated/meshlet_cull_types_generated.cppm），
// shader 侧则直接 `Load<MeshletCullItem>` 让**编译器自己**决定字段位置（见下）——两侧都没有
// 手抄的偏移。唯一必须写死的字节数是 DiskCluster 的包围球字段（下注其出处）。
//
// 保守性口径（与 CPU 参考实现同式）：球心到某平面距离 < −r 才判不可见，**跨界与内含一律判可见**。
// 误剔没有任何错误信号（图像上就是"少画一块"），所以宁可少剔。
//
// 矩阵约定与 cull.comp.slang 完全一致（无行/列主序歧义）：对象矩阵按"内存 chunk j = 数学列 j"
// 处理（C++ 侧 glm 列主序），世界球心用列组合展开，不依赖 Slang 矩阵构造函数 / operator[] 语义。

[[vk::binding(0, 0)]]
readonly ByteAddressBuffer cullItems;    // MeshletCullItem × cullPush.itemCount
[[vk::binding(1, 0)]]
readonly ByteAddressBuffer clusters;     // 簇头段（DiskCluster，44B/条）
[[vk::binding(2, 0)]]
readonly ByteAddressBuffer sceneData;    // 对象矩阵表 @128 + objectSlot × 64
[[vk::binding(3, 0)]]
RWByteAddressBuffer clusterVisible;      // u32 × itemCount（1 = 可见；mesh shader 按 cullBase+簇内序读）

// 对象矩阵表基址（scene data 契约，scene.slang readObjectMatrix / cull.comp.slang 同值）
static const uint kSceneMatrixBase = 128u;

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    // 尾线程守卫（cull.comp.slang / stream_fanout.comp.slang 同款）：dispatch 按 64 上取整，
    // 尾线程 tid.x ≥ itemCount 会越界读写表/标志 buffer——robustBufferAccess 只保证"丢弃或改写
    // 绑定内存内值"，不可依赖
    if (tid.x >= cullPush.itemCount)
        return;

    // 步长与字段位置**都**取自契约结构体本身（sizeof + Load<T>）：编译器按 meshlet_cull.slang 的
    // 声明算偏移，本文件一个字段偏移都不写（实测产物 = clusterIdx@+0 / objectSlot@+4）
    const MeshletCullItem item =
        cullItems.Load<MeshletCullItem>(tid.x * uint(sizeof(MeshletCullItem)));

    // 簇包围球（源 glb 单位，未乘对象矩阵——世界换算在下面）。步长与字段位置**都**取自契约
    // 结构体本身（与上面 cullItems 同读法）：编译器按 meshlet_cull.slang 的
    // MeshletClusterHeader 定偏移，本文件一个字节偏移都不写（改 format.h 而忘了同步契约
    // = 反射 [FAIL] / 生成物 static_assert 编译红，不再是"读着隔壁字段还像个数"的静默错）
    const MeshletClusterHeader cluster =
        clusters.Load<MeshletClusterHeader>(item.clusterIdx * uint(sizeof(MeshletClusterHeader)));
    const float3 c = cluster.center;
    const float r = cluster.radius;

    // 对象世界矩阵 M（scene data @128 + slot×64，4 列显式读——cull.comp.slang:75-79 同款）
    const uint mb = kSceneMatrixBase + item.objectSlot * 64u;
    const float4 m0 = asfloat(sceneData.Load4(mb));
    const float4 m1 = asfloat(sceneData.Load4(mb + 16u));
    const float4 m2 = asfloat(sceneData.Load4(mb + 32u));
    const float4 m3 = asfloat(sceneData.Load4(mb + 48u));

    // 世界球（与 CPU 同式：中心 = M·(c,1) 列组合，半径 = r × max(三列长度)）。
    // 🔴 缩放必须取 **max**：取小轴或非均匀缩放下会低估半径 ⟹ 把实际可见的簇判成不可见 = 误剔。
    const float3 wc = (m0 * c.x + m1 * c.y + m2 * c.z + m3).xyz;
    const float scale = max(length(m0.xyz), max(length(m1.xyz), length(m2.xyz)));
    const float wr = r * scale;

    // 6 面保守测试（cull.comp.slang:88-97 逐式复用；与 CPU sphereInFrustum 同式同值 = 对账基准一致）
    bool visible = true;
    for (uint p = 0u; p < 6u; ++p)
    {
        const float4 pl = cullPush.planes[p];
        if (dot(pl.xyz, wc) + pl.w < -wr)
        {
            visible = false;
            break;
        }
    }

    clusterVisible.Store(tid.x * 4u, visible ? 1u : 0u);
}
