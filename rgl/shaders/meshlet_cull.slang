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

[[vk::push_constant]] MeshletCullPush cullPush;
