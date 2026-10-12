// 逐簇剔除（★1-a2-i）的 CPU↔shader 共享契约。布局真相源 = 本文件经 C API 反射
// → generated/meshlet_cull_types_generated.cppm；C++ 侧 import 生成物，🔴 禁止再手写第二份。
//
// 为什么这条契约走反射而 cull.comp.slang 不走：后者是 Phase 7b 的历史遗留（头注释自陈
// 「首版不接反射管线，ByteAddressBuffer 手写字节偏移」），C++ 侧 gpu_cull.cppm 与 shader 各写
// 一份偏移、靠 static_assert + 人肉对齐兜底。本项目已定案：**compute 的布局契约同样必须走反射
// 单一真相源**，新契约一律照本文件形态（slang struct + schema 提取清单 + 生成物），别再照抄遗留样板。
//
// 布局口径：Scalar Block Layout（cmake/RglSlang.cmake 的 SLANG_FLAGS 统一 -fvk-use-scalar-layout，
// 反射与 SPV 两处同源）⟹ 所有类型对齐 4、零 padding：MeshletCullItem = 8B、
// MeshletCullPush = 6×16 + 4 = 100B，与 C++（glm 自然对齐 4）逐字段偏移天然一致。
//
// 消费者：meshlet_cull.comp.slang（import 本文件）；C++ 侧 = meshlet_ranges.cppm 的
// MeshletCullItemTable（import 生成物）。

// 一个**逐簇剔除项** = 一个 (簇, 实例) 对。为什么不是只存簇下标：同一资产的簇被多个 objectSlot
// 共享，而剔除判据里的世界变换来自**实例**（对象矩阵表按 objectSlot 索引）——只存簇下标
// ⟹ 剔除器不知道该用哪个实例的矩阵。字段序 = meshlet_ranges_impl.cppm::buildClusterCullItems
// 的产出序（clusterIdx 在前），两边顺序对不上是**静默**的错剔，故由反射逐字段 offsetof 钉住。
struct MeshletCullItem
{
    uint clusterIdx;   // 全局簇下标（读簇包围球用）
    uint objectSlot;   // 实例矩阵槽 = shader objectIndex（读对象矩阵用）
};

// 每帧 push 常量：视锥 + 剔除项数。为什么 6 个面走 push 而不是 scene data：它是**每帧变**且
// 逐 dispatch 唯一的少量数据（100B），而 scene data 的 bindless 预算留给逐对象表。
struct MeshletCullPush
{
    float4 planes[6];  // 视锥 6 面（left/right/bottom/top/near/far，法线朝内且已归一化）
    uint itemCount;    // 剔除项数（= 标志数组长度）
};

// 簇头（= 打包工具产物 `tools/meshlet_pack/format.h::DiskCluster` 的**运行时视图**）。
//
// 🔴 为什么它进本契约而不是让 shader 写死字节偏移：这个布局此前有 4 份手写副本（packer 的
// format.h / 引擎 `meshlet_asset` 的镜像 struct / meshlet.mesh.slang 的 44+8/12/16/20 字面量 /
// 本 compute 的 24/36 字面量）。副本越多，**差一个字段就静默错剔一块几何**的面积越大——
// 产品级 tangent.w off-by-one（把 tangent.z 写成 tangent.w）就是同型错。本结构把 shader 侧
// 与引擎侧收敛成一份：shader 用 `Load<MeshletClusterHeader>` 让编译器定偏移，C++ 用生成物。
//
// ⚠ 与 packer 的 `DiskCluster` 之间**刻意**不共享代码：项目红线「引擎不 include tools/」
// （`tests/test_meshlet_asset.cppm` 头注释），且该测试刻意手抄 format.h 常量当**独立**第二出处
// ——两侧同时漂才被抓到，合并成一份反而失去这个交叉验证。本结构的作用是把**引擎内部**的
// 多份副本收成一份。
struct MeshletClusterHeader
{
    uint   group;       // 所属 group 下标（选级判据住在 group，不住簇）
    int    refined;     // 产出本簇的**更细** group 下标；-1 = 原始几何（L0 判据）
    uint   triCount;    // 三角数（≤ 128，8bit 局部索引因此可解）
    uint   vertCount;   // 簇内顶点数（≤ 128）
    uint   triOffset;   // Tris 段内**字节**偏移（triCount×3 个 u8）
    uint   remapOffset; // Remap 段内**元素**偏移（vertCount 个 u32 全局顶点下标）
    float3 center;      // 包围球球心（**源 glb 单位**，未应用对象矩阵）
    float  radius;      // 包围球半径（同上，源单位）
    float  error;       // 跨 DAG 累积误差（剔除不消费，声明在此只为把 44B 布局钉死）
};

[[vk::push_constant]] MeshletCullPush cullPush;
