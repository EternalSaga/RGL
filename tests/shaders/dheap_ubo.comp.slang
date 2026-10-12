// dheap_ubo.comp.slang —— VK_EXT_descriptor_heap × ConstantBuffer<UBO> 能力 spike 主判据体
//
// 回答的问题（唯一）：resource heap 里能不能放 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER 的
// descriptor，并被 Slang compute shader 以 ConstantBuffer<T> 正常读取。
//
// 三条臂共用同一份输出布局（outBuf 下标 = 本文件下方 kOut* 常量），差别只在**读哪个成员**：
//   · 本文件 = 主判据（读"正确"成员/索引）
//   · dheap_ubo_wrong.comp.slang = 阳性对照（读"错误"成员/索引，结果必须 ≠ 主判据值）
//
// ⚠️ 本文件**只新增、不改任何现有 .slang**；产物落 build/tests/generated_shaders/，
//    不进引擎 SPV 面（tests/CMakeLists.txt 的 compile_slang 独立管线）。
//
// 输出槽位（RWStructuredBuffer<uint> 下标；每个槽位读**不同**的 magic，
// 任何"整体错位一格"都会同时打红多个槽位）：
//   0 hdr.exposure   1 hdr.roughness  2 hdr.flags  3 hdr.count
//   4 hdr.tint.y     5 hdr.dir.z      6 hdr.scale
//   7 mvps[1].c2.x（元素 1 × stride 64 + 成员偏移 32 = 字节 96）
//   8 raw.Load(4)（ByteAddressBuffer = 字节偏移）
//   9 mvps[0].c0.x（元素 0 基线，与 7 对照出 stride 是否 64）
//  10 raw.Load(0)
//  11 立即数锚点（不来自任何描述符读，见下方 kDispatchAnchor）
//
// ─── Slang 产物的 SPIR-V 形态（本机 slangc + 项目原 flags 实测，非推测）──────────
//   ConstantBuffer<Header> → OpVariable **Uniform** storage class；
//       %Header_natural = OpTypeStruct，OpDecorate **Block**（不是 UniformConstant，
//       不是 BufferBlock）；成员 Offset 装饰实测 = 0 / 4 / 8 / 12 / 16 / 32 / 44
//       （与 C++ offsetof 逐字段相同，见测试里的 static_assert）。
//       ⟹ 恰是 VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT 的定义
//         （"OpTypeStruct in Uniform storage class decorated with Block"）。
//   StructuredBuffer<Mvp>  → StorageBuffer + Block + **NonWritable**，ArrayStride 64
//   ByteAddressBuffer      → StorageBuffer + Block + NonWritable（runtime array of uint）
//   RWStructuredBuffer<uint> → StorageBuffer + Block，ArrayStride 4
//   模块带 OpExtension "SPV_KHR_storage_buffer_storage_class"。

/// ConstantBuffer 的 C 布局：与 SPIR-V 侧 OpMemberDecorate Offset **逐字段对账**
/// （scalar layout 实测 Offset = 0/4/8/12/16/32/44，见测试头注释）。
struct Header {
    float  exposure;  ///< Offset 0
    float  roughness; ///< Offset 4
    uint   flags;     ///< Offset 8
    uint   count;     ///< Offset 12
    float4 tint;      ///< Offset 16
    float3 dir;       ///< Offset 32
    float  scale;     ///< Offset 44
};                     ///< 总大小 48

/// StructuredBuffer 元素：64B（4×float4），ArrayStride 实测 64。
struct Mvp {
    float4 c0;
    float4 c1;
    float4 c2;
    float4 c3;
};

[[vk::binding(0)]] ConstantBuffer<Header>   hdr;    ///< 待验臂：Uniform + Block
[[vk::binding(1)]] StructuredBuffer<Mvp>    mvps;   ///< 退路臂：StorageBuffer + Block + NonWritable
[[vk::binding(2)]] ByteAddressBuffer        raw;    ///< 对照臂：引擎现行唯一验证过的 buffer 形态
[[vk::binding(3)]] RWStructuredBuffer<uint> outBuf; ///< 输出

/// 派发锚点常量（判据 ① 的阳性锚点）：**不来自任何描述符读**，纯立即数。
/// 它把「out buffer 全零」这一现象拆成两种互斥解释：
///   锚点也没写进去 → dispatch / out 描述符本身没生效（通路问题，UBO 结论不成立）
///   锚点写进去了而 0..10 是零 → 通路好，是**读侧**描述符没被消费（真结论）
/// 缺这条时两种解释都表现为"全零红"，spike 会答错问题。
static const uint kDispatchAnchor = 0xBEEFCAFEu;

[shader("compute")]
[numthreads(1, 1, 1)]
void main() {
    outBuf[11] = kDispatchAnchor;  ///< 先写锚点：即使后面的读全部非法也留得下证据
    outBuf[0] = asuint(hdr.exposure);
    outBuf[1] = asuint(hdr.roughness);
    outBuf[2] = hdr.flags;
    outBuf[3] = hdr.count;
    outBuf[4] = asuint(hdr.tint.y);
    outBuf[5] = asuint(hdr.dir.z);
    outBuf[6] = asuint(hdr.scale);
    outBuf[7] = asuint(mvps[1].c2.x);
    outBuf[8] = raw.Load(4);
    outBuf[9] = asuint(mvps[0].c0.x);
    outBuf[10] = raw.Load(0);
}
