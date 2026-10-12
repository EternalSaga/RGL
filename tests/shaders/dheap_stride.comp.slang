// dheap_stride.comp.slang —— StructuredBuffer 元素 stride 设备端往返【主判据体】
//
// ## 要回答的唯一问题
//
// 元素 stride **不是 16 的倍数**时（本用例 = 8B 与 44B），`StructuredBuffer<T>` 在真机上
// 读元素 1、元素 2 是否落在 `sizeof(T)` 的整数倍字节上？也就是 SPIR-V 的 `ArrayStride`
// 与 C++ 的 `sizeof` 在设备上是否真的同一个值。
//
// 这是 R3 红线（禁 StructuredBuffer）撤销条件第 2 条（docs/constantbuffer_迁移计划.md
// §8.4）：R3 的理由是一条**驱动/布局事实**，事实只能测，不能靠注释或推理定案。构建期的
// `ArrayStride == sizeof` 对账（tools/check_spirv_stride.py，R4）证的是编译器口径，
// **本用例证的是驱动真按那个 stride 取数**——两条缺一条都还剩一段静默空间。
//
// ## 为什么 44 是关键用例、8 也要留
//
// 44 = `MeshletClusterHeader` 的 sizeof，含 `float3 center` 且**不是 16 的倍数**。
// std430 口径下它会被补到 64（`float3` 占 16B），scalar layout 下才是 44 —— 灰屏那条
// 静默错位的发生地就在这两个数之间。8 = `MeshletCullItem`，另一个实际在用的 stride，
// 顺带钉住"小 stride 不向上取整到 16"。
//
// ## 判据形态：闭式解，不是"读回来跟 CPU 写的比一遍"
//
// buffer 按**字**填充：第 k 个字（字节 4k）= `kWordBase + k`，kWordBase = 0x5A000000
// （全非零、两两不同）。于是"元素 e 的字节偏移 b 处该读到什么"有闭式解：
//
//     期望值 = kWordBase + (e * sizeof(T) + b) / 4
//
// C++ 侧的期望表由 `sizeof(RGL::MeshletClusterHeader)` / `sizeof(RGL::MeshletCullItem)`
// （反射生成物）**算**出来，不是抄来的常数 ⟹ 本文件与测试里的期望值是两条独立路径，
// 只在"stride 相同"这一点上相交。stride 若从 44 漂到 48/64，下面每一个槽位同时打红。
//
// ## 为什么读元素 1 而不是元素 0
//
// 元素 0 恒从字节 0 开始，**stride 错了它也恰好是对的**。只读元素 0 的往返用例等于没测
// stride。本体读元素 1（第一次用到 stride）与元素 2（用到两次 stride，把"步长"与
// "首元素偏移"分开）。阳性对照见 dheap_stride_wrong.comp.slang。
//
// ─── 输出槽位（RWStructuredBuffer<uint> 下标，与测试里的期望表一一对应）──────────
//   0 it[1].a   1 it[1].b   2 it[2].a            （stride 8：字节 8 / 12 / 16）
//   3 h[1].group        4 h[1].remapOffset       （stride 44：字节 44 / 64）
//   5 h[1].center.x   6 h[1].center.y  7 h[1].center.z   （字节 68 / 72 / 76）
//   8 h[1].radius      9 h[1].error              （字节 80 / 84）
//  10 h[2].group                                 （字节 88 = 2×44）
//  11 立即数派发锚点（不来自任何描述符读）
//
// ⚠️ 本文件**只新增、不改任何现有 .slang**；产物落 build/tests/generated_shaders/，
//    不进引擎 SPV 面（tests/CMakeLists.txt 的 compile_slang 独立管线）。
// ⚠️ 下面两个 struct 是 rgl/shaders/meshlet_cull.slang 里**同名契约 struct 的镜像**
//    （tests/shaders 的 compile_slang 只 -I 自己目录，import 不到引擎契约文件）。镜像与
//    C++ 生成物之间的漂移正是本用例要抓的东西之一，刻意不收敛成一份。

/// 8B 元素（镜像 `MeshletCullItem`）。C++ 侧直接用生成物类型，stride 由 sizeof 算。
struct MeshletCullItem {
    uint clusterIdx;   ///< 字节 0
    uint objectSlot;   ///< 字节 4
};

/// 44B 元素（镜像 `MeshletClusterHeader`）。字段序与偏移必须与契约一致；
/// `float3` 在 scalar layout 下占 12B（**不补到 16**）——本用例的判据本体。
struct MeshletClusterHeader {
    uint   group;        ///< 字节 0
    int    refined;      ///< 字节 4
    uint   triCount;     ///< 字节 8
    uint   vertCount;    ///< 字节 12
    uint   triOffset;    ///< 字节 16
    uint   remapOffset;  ///< 字节 20
    float3 center;       ///< 字节 24（12B）
    float  radius;       ///< 字节 36
    float  error;        ///< 字节 40
};

[[vk::binding(0)]] StructuredBuffer<MeshletCullItem>    items;    ///< stride 8
[[vk::binding(1)]] StructuredBuffer<MeshletClusterHeader> headers; ///< stride 44
[[vk::binding(2)]] RWStructuredBuffer<uint>            outBuf;    ///< 输出

/// 派发锚点（与 dheap_ubo.comp.slang 同形态）：把「out 全零」拆成「通路没跑」与
/// 「通路跑了但读侧描述符没被消费」两种互斥解释，缺它会把工具故障答成驱动结论。
static const uint kDispatchAnchor = 0xBEEFCAFEu;

[shader("compute")]
[numthreads(1, 1, 1)]
void main() {
    outBuf[11] = kDispatchAnchor;  ///< 先写锚点：即使后面的读全部非法也留得下证据
    outBuf[0] = items[1].clusterIdx;
    outBuf[1] = items[1].objectSlot;
    outBuf[2] = items[2].clusterIdx;
    outBuf[3] = headers[1].group;
    outBuf[4] = headers[1].remapOffset;
    outBuf[5] = asuint(headers[1].center.x);
    outBuf[6] = asuint(headers[1].center.y);
    outBuf[7] = asuint(headers[1].center.z);
    outBuf[8] = asuint(headers[1].radius);
    outBuf[9] = asuint(headers[1].error);
    outBuf[10] = headers[2].group;
}
